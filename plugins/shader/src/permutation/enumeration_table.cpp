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
// paired .cpp) and to provide compile-time instantiation checks of the
// two most common call patterns (accept-all and pointer-based pruner).

#include <functional>

#include "enumeration_table.hpp"

namespace glibre::shader::permutation {

// Compile-time instantiation guard: verify both primary walk() call patterns
// are well-formed (Pruner + Visitor concepts satisfied).  These static_asserts
// replace the former dead-code [[maybe_unused]] functions; they produce no
// object-file symbols and express intent directly to the compiler.

// accept-all pattern: walk_all with a counting lambda visitor.
static_assert(
    std::is_invocable_v<
        decltype(&EnumerationTable::walk_all<std::function<void(const PermutationKey&)>>),
        const EnumerationTable&,
        std::function<void(const PermutationKey&)>>,
    "EnumerationTable::walk_all must be invocable with a void(const PermutationKey&) visitor."
);

// pruned pattern: walk with a bool pruner and a void visitor.
static_assert(
    std::is_invocable_v<
        decltype(&EnumerationTable::walk<
                 std::function<bool(const PermutationKey&)>,
                 std::function<void(const PermutationKey&)>>),
        const EnumerationTable&,
        std::function<bool(const PermutationKey&)>,
        std::function<void(const PermutationKey&)>>,
    "EnumerationTable::walk must be invocable with a bool pruner and a void visitor."
);

}  // namespace glibre::shader::permutation
