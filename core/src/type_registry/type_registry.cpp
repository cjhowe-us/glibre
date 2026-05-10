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
// ## Seal semantics
//
//   sealed_ is a plain bool (not atomic) because all operations before
//   seal() happen on the loader thread and all operations after seal()
//   during the World lifetime are pure reads from any thread; the
//   happens-before from World construction to first use on worker threads
//   is provided by the OS thread-creation/join barrier (SPEC §6.10 MVP
//   single-thread note), so no additional synchronisation is needed here.
//
// ## -fno-exceptions clean
//   No exceptions thrown or propagated.

#include "glibre/core/type_registry.hpp"

#include <memory_resource>

namespace glibre {
namespace core {

TypeRegistry::TypeRegistry(std::pmr::memory_resource* resource) noexcept
    : entries_(resource != nullptr ? resource : std::pmr::get_default_resource()) {}

Result<const ColumnDescriptor*>
TypeRegistry::lookup(TypeId id) const noexcept {
    if (id.value >= entries_.size()) {
        return std::unexpected(Error{core::Error::TypeUnregistered});
    }
    return &entries_[id.value];
}

bool TypeRegistry::is_registered(TypeId id) const noexcept {
    return id.value < entries_.size();
}

std::size_t TypeRegistry::count() const noexcept {
    return entries_.size();
}

Result<void>
TypeRegistry::register_type(TypeId id, ColumnDescriptor desc) noexcept {
    if (sealed_) {
        return std::unexpected(Error{core::Error::TypeRegistryClosed});
    }
    // Enforce contiguous assignment: id.value must equal the next empty slot.
    if (id.value != entries_.size()) {
        return std::unexpected(Error{core::Error::TypeUnregistered});
    }
    entries_.push_back(desc);
    return {};
}

void TypeRegistry::seal() noexcept {
    sealed_ = true;
}

Result<void>
TypeRegistry::extend_during_load(TypeId id, ColumnDescriptor desc) noexcept {
    // Bypasses sealed_ check — loader privilege (friend class PluginLoader).
    // Enforce contiguous assignment same as register_type.
    if (id.value != entries_.size()) {
        return std::unexpected(Error{core::Error::TypeUnregistered});
    }
    entries_.push_back(desc);
    return {};
}

}  // namespace core
}  // namespace glibre
