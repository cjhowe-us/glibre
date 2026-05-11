// plugins/shader/src/permutation/enumeration_table.cpp
//
// Build-time cross-product enumerator — implementation translation unit.
//
// Authority: specs/shader/SPEC.md §6.1.
//
// EnumerationTable::walk() is fully inlined in enumeration_table.hpp because
// the walk body is parameterized over a Pruner and a Visitor concept.
// This TU exists to satisfy the CMakeLists source-list convention for the
// permutation/ directory (each header with non-trivial semantics has a
// paired .cpp) and to provide a compile-time instantiation check of the
// two most common call patterns (accept-all and pointer-based pruner).

#include "enumeration_table.hpp"

namespace glibre::shader::permutation {

// Explicit instantiation smoke-check: ensure the two most common walk()
// variants compile without linker-visible symbols.

namespace {

// accept-all visitor — compile-instantiation guard.
[[maybe_unused]] void smoke_walk_all() {
    EnumerationTable t;
    std::uint32_t count = 0;
    t.walk_all([&count](const PermutationKey&) noexcept { ++count; });
    (void)count;
}

// accept-none pruner — compile-instantiation guard.
[[maybe_unused]] void smoke_walk_pruned() {
    EnumerationTable t;
    t.walk(
        [](const PermutationKey&) noexcept { return false; },
        [](const PermutationKey&) noexcept {}
    );
}

}  // namespace

}  // namespace glibre::shader::permutation
