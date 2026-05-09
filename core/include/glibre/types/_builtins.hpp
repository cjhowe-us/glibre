// SPDX-License-Identifier: Apache-2.0
// core/include/glibre/types/_builtins.hpp
//
// Hand-audited C++ type declarations for every .fory builtin that maps to a
// non-stdlib type.  Generated headers produced by glibre-foryc
// (tools/foryc, plan #220) unconditionally include this file whenever any
// field in the schema resolves to one of the "math" or "entity" builtins:
//   vec2f  vec3f  vec4f  vec2i  vec3i  vec4i  quatf  entity
//
// PHILOSOPHY §11: EASTL replaces std containers; std:: is retained only for
// language/runtime utilities EASTL does not own.
//
// Audit status: hand-authored, reviewed.  Do NOT machine-generate this file.
//   Every type here is trivially copyable, has no virtuals, and satisfies
//   std::is_trivially_copyable_v so that it can appear in fory-generated
//   structs marked `final` with `= default` ctor only (ABI rule 2 from
//   reviews/decisions/fory-codegen.md §"ABI Stability Rules").
//
// New types MUST be added here BEFORE the corresponding fory builtin keyword
// is accepted by the parser; the two must stay in sync with builtin_map.hpp
// (tools/foryc/src/builtin_map.hpp).

#pragma once

#include <cstdint>

// -----------------------------------------------------------------------
// glibre::math — POD math aggregates
//
// Layout is tag-sorted field order per ABI rule 1.  All members are
// value-initialised to zero by the = default constructor.
// -----------------------------------------------------------------------

namespace glibre::math {

struct Vec2f {
    float x{};
    float y{};
    Vec2f() = default;
};

struct Vec3f {
    float x{};
    float y{};
    float z{};
    Vec3f() = default;
};

struct Vec4f {
    float x{};
    float y{};
    float z{};
    float w{};
    Vec4f() = default;
};

struct Vec2i {
    std::int32_t x{};
    std::int32_t y{};
    Vec2i() = default;
};

struct Vec3i {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t z{};
    Vec3i() = default;
};

struct Vec4i {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t z{};
    std::int32_t w{};
    Vec4i() = default;
};

// Unit quaternion (w, x, y, z) — identity = {1,0,0,0}.
struct Quatf {
    float w{1.0f};
    float x{};
    float y{};
    float z{};
    Quatf() = default;
};

}  // namespace glibre::math

// -----------------------------------------------------------------------
// glibre::core — ECS handle
// -----------------------------------------------------------------------

namespace glibre::core {

// EntityId is a 64-bit opaque handle.  The upper 32 bits are the
// archetype-generation counter; the lower 32 bits are the slot index.
// Null entity = 0.
struct EntityId {
    std::uint64_t value{};
    EntityId() = default;
    [[nodiscard]] constexpr bool is_null() const noexcept { return value == 0; }
};

}  // namespace glibre::core
