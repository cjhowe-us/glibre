// SPDX-License-Identifier: Apache-2.0
// tests/tools/foryc/foryc_emit_header_test.cpp
//
// Catch2 unit tests for glibre-foryc header emission (plan #220).
//
// Test names match the dispatch prompt's Unit Test Plan:
//   - foryc_emits_minimal_header_from_schema
//   - foryc_emits_header_for_nested_namespace
//   - foryc_emit_header_rejects_unknown_type
//
// Additional coverage tests:
//   - foryc_emit_header_tag_sorted_fields
//   - foryc_map_builtin_scalars
//   - foryc_map_generic_list_type
//   - foryc_map_generic_option_type
//   - foryc_map_generic_map_type
//   - foryc_emits_generated_file_comment
//   - foryc_emits_pragma_once_guard
//
// ABI-shape tests (fory-codegen.md §"ABI Stability Rules" rule 2):
//   - foryc_emit_header_marks_struct_final
//   - foryc_emit_header_emits_default_ctor
//
// Empty-schema guard:
//   - foryc_emit_header_rejects_empty_schema
//
// Builtin include tests:
//   - foryc_emit_header_includes_builtins_for_math_type
//   - foryc_emit_header_no_builtins_include_for_scalar_only_schema
//
// libc++ stdlib migration guard (plan #1055, eastl-removal.md):
//   - tools/foryc: emits_with_std_string_buffers

#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "emit_header.hpp"
#include "parser.hpp"

using namespace glibre::tools::foryc;

// -----------------------------------------------------------------------
// Helper: parse a schema string and return the Schema IR.
// Fails the test if parsing fails.
// -----------------------------------------------------------------------

static Schema parse_ok(std::string_view src, std::string_view vpath = "<test>") {
    auto r = parse_string(src, vpath);
    REQUIRE(r.has_value());
    return std::move(*r);
}

static bool has_tools_error(const glibre::Error& e, glibre::tools::Error code) noexcept {
    const auto* te = std::get_if<glibre::tools::Error>(&e.code());
    return te && (*te == code);
}

// -----------------------------------------------------------------------
// Dispatch-prompt named tests
// -----------------------------------------------------------------------

TEST_CASE("foryc_emits_minimal_header_from_schema", "[foryc][emit_header]") {
    // Parse `schema Foo { version 1  field x : u32 tag 1 }` and emit.
    // Assert that the output contains `struct Foo` and `uint32_t x`.
    constexpr std::string_view src = R"(
schema Foo {
  version 1
  field x : u32 tag 1
}
)";

    const auto schema = parse_ok(src);
    auto result = emit_header(schema);
    REQUIRE(result.has_value());

    const std::string& text = *result;

    // Must contain the struct declaration.
    CHECK(text.find("struct Foo") != std::string::npos);

    // Must contain the uint32_t field.
    CHECK(text.find("uint32_t x") != std::string::npos);
}

TEST_CASE("foryc_emits_header_for_nested_namespace", "[foryc][emit_header]") {
    // Schema with FQN `data.shapes.Circle` → expect nested namespaces.
    constexpr std::string_view src = R"(
schema data.shapes.Circle {
  version 1
  field radius : f32 tag 1
}
)";

    const auto schema = parse_ok(src);
    auto result = emit_header(schema);
    REQUIRE(result.has_value());

    const std::string& text = *result;

    // Expect `namespace data {` and `namespace shapes {` blocks.
    CHECK(text.find("namespace data {") != std::string::npos);
    CHECK(text.find("namespace shapes {") != std::string::npos);
    CHECK(text.find("struct Circle") != std::string::npos);
    // Closing namespace comments.
    CHECK(text.find("}  // namespace data") != std::string::npos);
    CHECK(text.find("}  // namespace shapes") != std::string::npos);
}

TEST_CASE("foryc_emit_header_rejects_unknown_type", "[foryc][emit_header]") {
    // Schema with an unknown type should fail at emit time.
    // Note: the parser rejects unknown types (ForycUnknownType) before
    // emit_header is called in normal operation.  However, emit_header must
    // also be robust against a hand-crafted Schema IR containing an unknown
    // type (e.g. if the parser builtins and emit_header's mapping table
    // diverge in future).  Since both now share builtin_map.hpp, drift is
    // prevented at the source; this test guards the emit path independently.
    //
    // We construct the IR manually to bypass the parser's type check.
    Schema schema;
    schema.source_path = "<test>";

    TypeDecl td;
    td.fqn = "test.Bad";
    td.version = 1;

    FieldDecl fd;
    fd.name = "x";
    fd.type_name = "NotABuiltin";
    fd.tag = 1;
    td.fields.push_back(std::move(fd));

    schema.types.push_back(std::move(td));

    auto result = emit_header(schema);
    REQUIRE(!result.has_value());
    CHECK(has_tools_error(result.error(), glibre::tools::Error::ForycUnknownType));
}

// -----------------------------------------------------------------------
// Additional tests
// -----------------------------------------------------------------------

TEST_CASE("foryc_emit_header_tag_sorted_fields", "[foryc][emit_header]") {
    // Fields declared in non-tag order must appear tag-sorted in the output.
    // Schema declares tag 3 first, then tag 1, then tag 2.
    constexpr std::string_view src = R"(
schema glibre.test.Sorted {
  version 1
  field z : u32 tag 3
  field a : f32 tag 1
  field b : bool tag 2
}
)";

    const auto schema = parse_ok(src);
    auto result = emit_header(schema);
    REQUIRE(result.has_value());

    const std::string& text = *result;

    // Locate each field; they must appear in tag order: a (tag1), b (tag2), z (tag3).
    const std::size_t pos_a = text.find("float a");
    const std::size_t pos_b = text.find("bool b");
    const std::size_t pos_z = text.find("uint32_t z");

    REQUIRE(pos_a != std::string::npos);
    REQUIRE(pos_b != std::string::npos);
    REQUIRE(pos_z != std::string::npos);

    CHECK(pos_a < pos_b);
    CHECK(pos_b < pos_z);
}

TEST_CASE("foryc_map_builtin_scalars", "[foryc][emit_header][mapping]") {
    // Verify the complete scalar mapping table from fory-codegen.md.
    struct Case {
        std::string_view fory;
        std::string_view cpp;
    };

    // clang-format off
    static constexpr Case k_cases[] = {
        {"u8",     "uint8_t"},
        {"u16",    "uint16_t"},
        {"u32",    "uint32_t"},
        {"u64",    "uint64_t"},
        {"i8",     "int8_t"},
        {"i16",    "int16_t"},
        {"i32",    "int32_t"},
        {"i64",    "int64_t"},
        {"f32",    "float"},
        {"f64",    "double"},
        {"bool",   "bool"},
        {"vec2f",  "glibre::math::Vec2f"},
        {"vec3f",  "glibre::math::Vec3f"},
        {"vec4f",  "glibre::math::Vec4f"},
        {"vec2i",  "glibre::math::Vec2i"},
        {"vec3i",  "glibre::math::Vec3i"},
        {"vec4i",  "glibre::math::Vec4i"},
        {"quatf",  "glibre::math::Quatf"},
        {"entity", "glibre::core::EntityId"},
        {"string", "std::string"},
        {"bytes",  "std::vector<std::byte>"},
    };
    // clang-format on

    for (const auto& c : k_cases) {
        auto r = map_builtin_to_cpp(c.fory);
        INFO("fory type: " << c.fory);
        REQUIRE(r.has_value());
        CHECK(*r == std::string(c.cpp.data(), c.cpp.size()));
    }
}

TEST_CASE("foryc_map_generic_list_type", "[foryc][emit_header][mapping]") {
    auto r = map_builtin_to_cpp("list<u32>");
    REQUIRE(r.has_value());
    CHECK(*r == "std::vector<uint32_t>");
}

TEST_CASE("foryc_map_generic_option_type", "[foryc][emit_header][mapping]") {
    auto r = map_builtin_to_cpp("option<entity>");
    REQUIRE(r.has_value());
    CHECK(*r == "std::optional<glibre::core::EntityId>");
}

TEST_CASE("foryc_map_generic_map_type", "[foryc][emit_header][mapping]") {
    auto r = map_builtin_to_cpp("map<u32,string>");
    REQUIRE(r.has_value());
    CHECK(*r == "std::unordered_map<uint32_t, std::string>");
}

TEST_CASE("foryc_emits_generated_file_comment", "[foryc][emit_header]") {
    // The emitted header must start with a "GENERATED FILE" comment and
    // include a TODO citing plan #222.
    constexpr std::string_view src = R"(
schema glibre.test.Fwd {
  version 1
  field x : i32 tag 1
}
)";

    const auto schema = parse_ok(src, "test/Fwd.fory");
    auto result = emit_header(schema);
    REQUIRE(result.has_value());

    const std::string& text = *result;
    CHECK(text.find("GENERATED FILE") != std::string::npos);
    CHECK(text.find("#222") != std::string::npos);
    CHECK(text.find("glibre-foryc") != std::string::npos);
}

TEST_CASE("foryc_emits_pragma_once_guard", "[foryc][emit_header]") {
    constexpr std::string_view src = R"(
schema glibre.test.Guard {
  version 1
  field y : f64 tag 1
}
)";

    const auto schema = parse_ok(src);
    auto result = emit_header(schema);
    REQUIRE(result.has_value());
    CHECK(result->find("#pragma once") != std::string::npos);
}

// -----------------------------------------------------------------------
// ABI shape tests — fory-codegen.md §"ABI Stability Rules" rule 2.
//
// Generated structs MUST be marked `final` and have `= default` ctor only.
// These tests guard against future refactors silently dropping either
// requirement.
// -----------------------------------------------------------------------

TEST_CASE("foryc_emit_header_marks_struct_final", "[foryc][emit_header][abi]") {
    // The emitted struct must be declared `struct <Name> final {`.
    constexpr std::string_view src = R"(
schema glibre.abi.Foo {
  version 1
  field x : u32 tag 1
}
)";

    const auto schema = parse_ok(src);
    auto result = emit_header(schema);
    REQUIRE(result.has_value());

    const std::string& text = *result;
    // Exact token sequence mandated by ABI rule 2.
    CHECK(text.find("struct Foo final {") != std::string::npos);
}

TEST_CASE("foryc_emit_header_emits_default_ctor", "[foryc][emit_header][abi]") {
    // The emitted struct must contain `<Name>() = default;` and nothing else
    // in the way of constructors/destructors (ABI rule 2: no user-defined ctors).
    constexpr std::string_view src = R"(
schema glibre.abi.Bar {
  version 1
  field y : f32 tag 1
}
)";

    const auto schema = parse_ok(src);
    auto result = emit_header(schema);
    REQUIRE(result.has_value());

    const std::string& text = *result;
    // Exact ctor line mandated by ABI rule 2.
    CHECK(text.find("Bar() = default;") != std::string::npos);
    // Negative guard: no user-defined ctor body (would contain a real '{').
    // `Bar() = default;` ends with ';', not '{'. The struct body's opening brace
    // appears only once, on the `struct Bar final {` line.
    // If a body-ctor appeared it would look like `Bar(...) {` or `Bar() {`.
    // We rely on the absence of "Bar() {" as the negative signal.
    CHECK(text.find("Bar() {") == std::string::npos);
}

// -----------------------------------------------------------------------
// Empty-schema guard
// -----------------------------------------------------------------------

TEST_CASE("foryc_emit_header_rejects_empty_schema", "[foryc][emit_header]") {
    // A Schema with zero TypeDecls must return ForycEmptySchema.
    // This guards against migration-only .fory files producing misleading
    // preamble-only output.
    Schema schema;
    schema.source_path = "<empty>";
    // schema.types is empty by default.

    auto result = emit_header(schema);
    REQUIRE(!result.has_value());
    CHECK(has_tools_error(result.error(), glibre::tools::Error::ForycEmptySchema));
}

// -----------------------------------------------------------------------
// _builtins.hpp conditional include tests
// -----------------------------------------------------------------------

TEST_CASE("foryc_emit_header_includes_builtins_for_math_type", "[foryc][emit_header]") {
    // A schema with a vec3f field must emit #include <glibre/types/_builtins.hpp>.
    constexpr std::string_view src = R"(
schema glibre.core.Transform {
  version 1
  field position : vec3f tag 1
  field rotation : quatf tag 2
  field owner    : entity tag 3
}
)";

    const auto schema = parse_ok(src);
    auto result = emit_header(schema);
    REQUIRE(result.has_value());

    const std::string& text = *result;
    CHECK(text.find("#include <glibre/types/_builtins.hpp>") != std::string::npos);
    // Must also contain the resolved C++ type names.
    CHECK(text.find("glibre::math::Vec3f") != std::string::npos);
    CHECK(text.find("glibre::math::Quatf") != std::string::npos);
    CHECK(text.find("glibre::core::EntityId") != std::string::npos);
}

TEST_CASE("foryc_emit_header_no_builtins_include_for_scalar_only_schema", "[foryc][emit_header]") {
    // A schema using only stdlib-mapped types must NOT include _builtins.hpp.
    constexpr std::string_view src = R"(
schema glibre.example.Widget {
  version 1
  field id     : u32    tag 1
  field weight : f32    tag 2
  field name   : string tag 3
  field active : bool   tag 4
}
)";

    const auto schema = parse_ok(src);
    auto result = emit_header(schema);
    REQUIRE(result.has_value());

    const std::string& text = *result;
    CHECK(text.find("#include <glibre/types/_builtins.hpp>") == std::string::npos);
}

// -----------------------------------------------------------------------
// libc++ stdlib buffer guard — plan #1048 / eastl-removal.md
//
// Verifies that emit_header() produces a std::string buffer whose content
// is accessible through std::string_view.  Migration to std::string
// completed by plan #1048; this test uses std::string directly.
//
// Policy: eastl-removal.md §Consequences/Test-fixtures — Catch2 built-in
// matchers work on std::string_view slices; no EASTL matchers needed.
// -----------------------------------------------------------------------

TEST_CASE("tools/foryc: emits_with_std_string_buffers", "[foryc][emit_header][stdlib]") {
    // Parse a schema with scalar fields only (no math/entity builtins).
    // The emitted header must be accessible as a std::string buffer.
    constexpr std::string_view src = R"(
schema glibre.example.Counter {
  version 1
  field count  : u32  tag 1
  field active : bool tag 2
}
)";

    const auto schema = parse_ok(src, "Counter.fory");
    auto result = emit_header(schema);
    REQUIRE(result.has_value());

    // Bind via std::string_view — plan #1048 migrated API to std::string.
    const std::string_view text{result->data(), result->size()};

    // --- Content assertions via std::string_view::find ---
    // Struct present.
    CHECK(text.find("struct Counter") != std::string_view::npos);
    // Fields present (tag-sorted ascending: count tag 1, active tag 2).
    CHECK(text.find("uint32_t count") != std::string_view::npos);
    CHECK(text.find("bool active") != std::string_view::npos);
    // ABI rule 2: struct is final, default ctor only.
    CHECK(text.find("struct Counter final {") != std::string_view::npos);
    CHECK(text.find("Counter() = default;") != std::string_view::npos);
    // pragma once present.
    CHECK(text.find("#pragma once") != std::string_view::npos);
    // Namespace wrapping from FQN "glibre.example.Counter".
    CHECK(text.find("namespace glibre {") != std::string_view::npos);
    CHECK(text.find("namespace example {") != std::string_view::npos);
}
