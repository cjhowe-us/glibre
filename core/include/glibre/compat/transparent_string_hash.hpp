#pragma once
// core/include/glibre/compat/transparent_string_hash.hpp
//
// glibre::TransparentStringHash — heterogeneous hash functor for string-keyed
// associative containers (std::pmr::unordered_map, std::unordered_set, …).
//
// Authority: reviews/decisions/eastl-removal.md §1 (matrix row 27:
//   eastl::transparent_string_hash → glibre::TransparentStringHash) + §1 compat/
//   creation rule ("The core/include/glibre/compat/ directory is created by the
//   first migration PLAN that needs it.").
//
// ## Purpose
//
//   std::unordered_map<std::pmr::string, V> normally requires a
//   std::pmr::string key argument for every lookup — constructing a temporary
//   std::pmr::string from a string literal or std::string_view on each call.
//   TransparentStringHash enables heterogeneous lookup:
//
//     std::pmr::unordered_map<
//         std::pmr::string, V,
//         glibre::TransparentStringHash,
//         std::equal_to<>          // transparent equality
//     > map;
//
//     map.find(std::string_view{"hello"});   // no std::pmr::string allocation
//     map.find("hello");                     // ditto (const char* → string_view)
//
//   The `using is_transparent = void;` tag is the C++14 mechanism that opts the
//   hash type into heterogeneous lookup for unordered containers. Containers
//   require BOTH hash and equality to be transparent; use std::equal_to<> (C++14)
//   as the equality predicate.
//
// ## Status / deletion gate
//
//   This is a LOCAL UTILITY, not a future standard library feature. There is no
//   `__cpp_lib_transparent_string_hash` feature macro to gate on — the struct
//   stays until explicitly superseded by a std:: type that provides the same
//   functionality, at which point it should be deleted and call sites updated.
//   See eastl-removal.md §1 matrix row 27: "Locked toolchain ships? yes
//   (header-only)".
//
//   If a future C++ standard introduces std::transparent_string_hash or
//   equivalent, a deletion-gate #error should be added here mirroring the
//   pattern in glibre/compat/move_only_function.hpp.
//
// ## -fno-exceptions clean
//   std::hash<std::string_view> is noexcept; the operator() overloads below
//   are noexcept. No exceptions thrown or propagated.
//
// ## SRP
//   This header's sole responsibility: expose a transparent hash + equality
//   pair for std::pmr::string-keyed heterogeneous unordered lookup. One reason
//   to change: a superior stdlib primitive supersedes it.

#include <functional>
#include <string>
#include <string_view>

namespace glibre {

// ---------------------------------------------------------------------------
// TransparentStringHash — transparent hash for std::pmr::string and friends.
//
// Accepts std::string_view, std::string, std::pmr::string, and const char*
// without key materialisation. Delegates hashing to std::hash<std::string_view>
// after implicit conversion (all string types convert to string_view).
//
// Usage with std::pmr::unordered_map:
//   std::pmr::unordered_map<
//       std::pmr::string, Value,
//       glibre::TransparentStringHash,
//       std::equal_to<>
//   > map{&memory_resource};
//   map.find(std::string_view{"key"});  // heterogeneous lookup, no allocation
// ---------------------------------------------------------------------------

struct TransparentStringHash {
    // Required tag that activates heterogeneous lookup in unordered containers.
    using is_transparent = void;

    // Hash any string-like type by delegating to std::hash<std::string_view>.
    // std::string_view is the minimal common denominator: std::string,
    // std::pmr::string, const char*, and std::string_view all convert to it
    // implicitly (or via the const char* overload below).
    [[nodiscard]] std::size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
};

}  // namespace glibre
