// SPDX-License-Identifier: Apache-2.0
// tools/foryc/src/builtin_map.hpp
//
// Single source of truth for the .fory builtin → C++ type mapping.
//
// Consumed by:
//   parser.cpp   — membership check (`is_builtin(name)`)
//   emit_header.cpp — type translation (`map_builtin_to_cpp`)
//
// Authority: reviews/decisions/fory-codegen.md §"Schema File Format" bullet
// list of builtin scalar names and §"Rules".
//
// The table below lists only scalar (non-generic) builtins.  Generic types
// (list<T>, map<K,V>, option<T>) are handled structurally in emit_header.cpp
// and are recognised by the parser via the k_generic_prefixes sentinel below.
//
// Adding a new builtin:
//   1. Add a row to k_builtin_scalar_map.
//   2. If the C++ type is not from the stdlib, declare it in
//      core/include/glibre/types/_builtins.hpp and mark it as needing the
//      builtins include (set needs_builtins_include = true in its row).
//   3. Rebuild; the static_assert at the bottom will fire if k_builtin_scalar_map
//      and k_builtin_scalar_keys diverge.

#pragma once

#include <string_view>

namespace glibre::tools::foryc {

// -----------------------------------------------------------------------
// BuiltinEntry — one row in the scalar builtin mapping table
// -----------------------------------------------------------------------

struct BuiltinEntry {
    std::string_view fory;               // keyword in the .fory grammar
    std::string_view cpp;                // corresponding C++ type expression
    bool needs_builtins_include{false};  // true → generated header must
                                         //   #include <glibre/types/_builtins.hpp>
};

// -----------------------------------------------------------------------
// Scalar builtin mapping table
//
// Order: unsigned int, signed int, float, bool, math, entity, variable-length.
// -----------------------------------------------------------------------

// clang-format off
static constexpr BuiltinEntry k_builtin_scalar_map[] = {
    // Unsigned integers
    {"u8",     "uint8_t",                    false},
    {"u16",    "uint16_t",                   false},
    {"u32",    "uint32_t",                   false},
    {"u64",    "uint64_t",                   false},
    // Signed integers
    {"i8",     "int8_t",                     false},
    {"i16",    "int16_t",                    false},
    {"i32",    "int32_t",                    false},
    {"i64",    "int64_t",                    false},
    // Floating-point
    {"f32",    "float",                      false},
    {"f64",    "double",                     false},
    // Boolean
    {"bool",   "bool",                       false},
    // Math aggregates — declared in glibre/types/_builtins.hpp
    {"vec2f",  "glibre::math::Vec2f",        true},
    {"vec3f",  "glibre::math::Vec3f",        true},
    {"vec4f",  "glibre::math::Vec4f",        true},
    {"vec2i",  "glibre::math::Vec2i",        true},
    {"vec3i",  "glibre::math::Vec3i",        true},
    {"vec4i",  "glibre::math::Vec4i",        true},
    {"quatf",  "glibre::math::Quatf",        true},
    // ECS handle — declared in glibre/types/_builtins.hpp
    {"entity", "glibre::core::EntityId",     true},
    // Variable-length primitives — libc++ per PHILOSOPHY §11
    {"string", "std::string",              false},
    {"bytes",  "std::vector<std::byte>",   false},
};
// clang-format on

// Sentinel count — static_assert users can reference this.
static constexpr std::size_t k_builtin_scalar_count =
    sizeof(k_builtin_scalar_map) / sizeof(k_builtin_scalar_map[0]);

// -----------------------------------------------------------------------
// Generic type prefixes (recognised structurally, not in the table above)
// -----------------------------------------------------------------------

static constexpr std::string_view k_generic_prefixes[] = {
    "list",    // list<T>   → std::vector<T_cpp>
    "map",     // map<K,V>  → std::unordered_map<K_cpp, V_cpp>
    "option",  // option<T> → std::optional<T_cpp>
};

}  // namespace glibre::tools::foryc
