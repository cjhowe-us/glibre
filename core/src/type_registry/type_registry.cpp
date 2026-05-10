// core/src/type_registry/type_registry.cpp
//
// TypeRegistry implementation — immutable-after-init TypeId → ColumnDescriptor.
//
// Authority: specs/core/SPEC.md §4.9, §6.9; plan #597.
//
// ## Storage layout
//
//   entries_ is a std::pmr::vector<ColumnDescriptor> grown strictly by
//   push_back.  Slot i holds the descriptor for TypeId{i}.  The invariant
//   "id.value == entries_.size() before each registration" ensures no gaps
//   and no out-of-order insertions; the codegen pipeline guarantees this
//   ordering by emitting type registrations in TypeId-ascending order from
//   glibre-types.dylib's _registry.cpp.
//
// ## Allocator wiring (HIGH-1 round-1 review fix; SRP lift MED-2 round-2 review)
//
//   The constructor requires a PerContextAllocator& stamped with
//   ContextTag::core.  glibre::PerContextAllocatorResource (from
//   glibre/alloc.hpp) adapts the allocator to the std::pmr::memory_resource
//   interface required by std::pmr::vector so all TypeRegistry storage is
//   tracked under the 64 MiB core heap ceiling (perf-budget.md Allocator
//   Rule #1).  The adaptor class was lifted from a TypeRegistry nested class
//   (round-2 review MED-2 fix) so that other per-context PMR consumers
//   (PhaseRegistry, plugin-owned-type tables) can share it without pulling in
//   type_registry.hpp.  std::pmr::get_default_resource() is no longer used.
//
// ## Seal semantics
//
//   sealed_ is a plain bool (not atomic) because all operations before
//   seal() happen on the loader thread and all operations after seal()
//   during the World lifetime are pure reads from any thread; the
//   happens-before from World construction to first use on worker threads
//   is provided by the OS thread-creation/join barrier (SPEC §6.10 MVP
//   single-thread note), so no additional synchronisation is needed here.
//
// ## is_loading_ latch (MED-4 round-1 review fix)
//
//   extend_during_load() returns core::Error::TypeRegistryClosed when
//   is_loading_ is false (release-safe always-on check).  The latch is set
//   by PluginLoader (friend) via set_loading_(true) at entry to the loading
//   critical section and cleared at exit.
//
// ## -fno-exceptions clean
//   No exceptions thrown or propagated.

#include "glibre/core/type_registry.hpp"

namespace glibre {
namespace core {

// ---------------------------------------------------------------------------
// TypeRegistry implementation
// ---------------------------------------------------------------------------

TypeRegistry::TypeRegistry(PerContextAllocator& alloc) noexcept
    : alloc_resource_{alloc},
      entries_(&alloc_resource_) {}

Result<const ColumnDescriptor*> TypeRegistry::lookup(TypeId id) const noexcept {
    if (id.value >= entries_.size()) {
        return std::unexpected(
            glibre::Error{
                core::Error::TypeUnregistered, ErrorContext{__FILE__, __LINE__, "id out of range"}
            }
        );
    }
    return &entries_[id.value];
}

bool TypeRegistry::is_registered(TypeId id) const noexcept { return id.value < entries_.size(); }

std::size_t TypeRegistry::count() const noexcept { return entries_.size(); }

Result<void> TypeRegistry::register_type(TypeId id, ColumnDescriptor desc) noexcept {
    if (sealed_) {
        return std::unexpected(
            glibre::Error{
                core::Error::TypeRegistryClosed,
                ErrorContext{__FILE__, __LINE__, "register_type called after seal()"}
            }
        );
    }
    // Enforce contiguous assignment: id.value must equal the next empty slot.
    if (id.value != entries_.size()) {
        return std::unexpected(
            glibre::Error{
                core::Error::TypeRegistryGap,
                ErrorContext{__FILE__, __LINE__, "id.value != count() — codegen contract violation"}
            }
        );
    }
    entries_.push_back(desc);
    return {};
}

void TypeRegistry::seal() noexcept { sealed_ = true; }

Result<void> TypeRegistry::extend_during_load(TypeId id, ColumnDescriptor desc) noexcept {
    // is_loading_ latch: extend_during_load() is only valid inside
    // PluginLoader's loading critical section (round-1 review MED-4 fix,
    // strengthened to always-on typed error in round-2 review MED-3 fix).
    //
    // Returns TypeRegistryClosed when is_loading_ is false, in both debug and
    // release builds.  A debug-assert-only approach (the prior state) silently
    // mutated a sealed registry in release when a late caller arrived outside
    // the load window (SPEC §4.9 invariant 1 violation).
    if (!is_loading_) {
        return std::unexpected(
            glibre::Error{
                core::Error::TypeRegistryClosed,
                ErrorContext{
                    __FILE__,
                    __LINE__,
                    "extend_during_load() called outside PluginLoader critical section"
                }
            }
        );
    }

    // Bypasses sealed_ check — loader privilege (friend class PluginLoader).
    // Enforce contiguous assignment same as register_type.
    if (id.value != entries_.size()) {
        return std::unexpected(
            glibre::Error{
                core::Error::TypeRegistryGap,
                ErrorContext{__FILE__, __LINE__, "id.value != count() — codegen contract violation"}
            }
        );
    }
    entries_.push_back(desc);
    return {};
}

}  // namespace core
}  // namespace glibre
