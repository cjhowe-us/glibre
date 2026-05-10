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
// ## Allocator wiring (HIGH-1 round-1 review fix)
//
//   The constructor requires a PerContextAllocator& stamped with
//   ContextTag::core.  TypeRegistry::PerContextAllocatorResource adapts
//   the allocator to the std::pmr::memory_resource interface required by
//   std::pmr::vector so all TypeRegistry storage is tracked under the 64 MiB
//   core heap ceiling (perf-budget.md Allocator Rule #1).
//   std::pmr::get_default_resource() is no longer used here.
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
//   extend_during_load() asserts is_loading_ == true.  The latch is set
//   by PluginLoader (friend) via set_loading_(true) at entry to the loading
//   critical section and cleared at exit.  This ensures that
//   extend_during_load() cannot be called outside that window.
//
// ## -fno-exceptions clean
//   No exceptions thrown or propagated.

#include "glibre/core/type_registry.hpp"

#include <cassert>
#include <memory_resource>

namespace glibre {
namespace core {

// ---------------------------------------------------------------------------
// TypeRegistry::PerContextAllocatorResource — virtual method bodies
// ---------------------------------------------------------------------------

void* TypeRegistry::PerContextAllocatorResource::do_allocate(
    std::size_t bytes, std::size_t alignment
) {
    auto result = alloc_.allocate(bytes, alignment);
    if (!result) {
        // Ceiling breach in GLIBRE_ALLOC_STRICT builds.  std::pmr::vector
        // expects a valid pointer or a thrown exception.  Since the engine
        // compiles with -fno-exceptions, abort — the caller in a diagnostic
        // build should have budgeted enough memory.  This matches the
        // PerContextAllocator OOM contract (alloc.hpp: OOM → std::abort()).
        std::abort();
    }
    return *result;
}

void TypeRegistry::PerContextAllocatorResource::do_deallocate(
    void* p, std::size_t bytes, std::size_t /*alignment*/
) noexcept {
    alloc_.deallocate(p, bytes);
}

bool TypeRegistry::PerContextAllocatorResource::do_is_equal(
    const std::pmr::memory_resource& other
) const noexcept {
    // dynamic_cast is unavailable under -fno-rtti.  Identity equality: two
    // PerContextAllocatorResource instances wrapping the same PerContextAllocator
    // are equal if they are the same object (pointer equality on *this).
    // For TypeRegistry, there is exactly one alloc_resource_ per instance and
    // the vector never transfers resources across allocators, so self-equality
    // is the only meaningful case.
    return this == &other;
}

// ---------------------------------------------------------------------------
// TypeRegistry implementation
// ---------------------------------------------------------------------------

TypeRegistry::TypeRegistry(PerContextAllocator& alloc) noexcept
    : alloc_resource_{alloc},
      entries_(&alloc_resource_) {}

Result<const ColumnDescriptor*> TypeRegistry::lookup(TypeId id) const noexcept {
    if (id.value >= entries_.size()) {
        return std::unexpected(
            Error{
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
            Error{
                core::Error::TypeRegistryClosed,
                ErrorContext{__FILE__, __LINE__, "register_type called after seal()"}
            }
        );
    }
    // Enforce contiguous assignment: id.value must equal the next empty slot.
    if (id.value != entries_.size()) {
        return std::unexpected(
            Error{
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
    // PluginLoader's loading critical section (round-1 review MED-4 fix).
    assert(is_loading_ && "extend_during_load() called outside loader critical section");

    // Bypasses sealed_ check — loader privilege (friend class PluginLoader).
    // Enforce contiguous assignment same as register_type.
    if (id.value != entries_.size()) {
        return std::unexpected(
            Error{
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
