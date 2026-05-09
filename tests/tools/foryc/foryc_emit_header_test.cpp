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
    const auto* te = eastl::get_if<glibre::tools::Error>(&e.code());
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

    const eastl::string& text = *result;

    // Must contain the struct declaration.
    CHECK(text.find("struct Foo") != eastl::string::npos);

    // Must contain the uint32_t field.
    CHECK(text.find("uint32_t x") != eastl::string::npos);
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

    const eastl::string& text = *result;

    // Expect `namespace data {` and `namespace shapes {` blocks.
    CHECK(text.find("namespace data {") != eastl::string::npos);
    CHECK(text.find("namespace shapes {") != eastl::string::npos);
    CHECK(text.find("struct Circle") != eastl::string::npos);
    // Closing namespace comments.
    CHECK(text.find("}  // namespace data") != eastl::string::npos);
    CHECK(text.find("}  // namespace shapes") != eastl::string::npos);
}

TEST_CASE("foryc_emit_header_rejects_unknown_type", "[foryc][emit_header]") {
    // Schema with an unknown type should fail at emit time.
    // Note: the parser rejects unknown types (ForycUnknownType) before
    // emit_header is called in normal operation.  However, emit_header must
    // also be robust against a hand-crafted Schema IR containing an unknown
    // type (e.g. if the parser builtins and emit_header's mapping table
    // diverge in future).
    //
    // We construct the IR manually to bypass the parser's type check.
    Schema schema;
    schema.source_path = eastl::string("<test>");

    TypeDecl td;
    td.fqn = eastl::string("test.Bad");
    td.version = 1;

    FieldDecl fd;
    fd.name = eastl::string("x");
    fd.type_name = eastl::string("NotABuiltin");
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

    const eastl::string& text = *result;

    // Locate each field; they must appear in tag order: a (tag1), b (tag2), z (tag3).
    const std::size_t pos_a = text.find("float a");
    const std::size_t pos_b = text.find("bool b");
    const std::size_t pos_z = text.find("uint32_t z");

    REQUIRE(pos_a != eastl::string::npos);
    REQUIRE(pos_b != eastl::string::npos);
    REQUIRE(pos_z != eastl::string::npos);

    CHECK(pos_a < pos_b);
    CHECK(pos_b < pos_z);
}

TEST_CASE("foryc_map_builtin_scalars", "[foryc][emit_header][mapping]") {
    // Verify the complete scalar mapping table from fory-codegen.md.
    struct Case { std::string_view fory; std::string_view cpp; };
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
        {"string", "eastl::string"},
        {"bytes",  "eastl::vector<std::byte>"},
    };

    for (const auto& c : k_cases) {
        auto r = map_builtin_to_cpp(c.fory);
        INFO("fory type: " << c.fory);
        REQUIRE(r.has_value());
        CHECK(*r == eastl::string(c.cpp.data(), c.cpp.size()));
    }
}

TEST_CASE("foryc_map_generic_list_type", "[foryc][emit_header][mapping]") {
    auto r = map_builtin_to_cpp("list<u32>");
    REQUIRE(r.has_value());
    CHECK(*r == eastl::string("eastl::vector<uint32_t>"));
}

TEST_CASE("foryc_map_generic_option_type", "[foryc][emit_header][mapping]") {
    auto r = map_builtin_to_cpp("option<entity>");
    REQUIRE(r.has_value());
    CHECK(*r == eastl::string("eastl::optional<glibre::core::EntityId>"));
}

TEST_CASE("foryc_map_generic_map_type", "[foryc][emit_header][mapping]") {
    auto r = map_builtin_to_cpp("map<u32,string>");
    REQUIRE(r.has_value());
    CHECK(*r == eastl::string("eastl::unordered_map<uint32_t, eastl::string>"));
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

    const eastl::string& text = *result;
    CHECK(text.find("GENERATED FILE") != eastl::string::npos);
    CHECK(text.find("#222") != eastl::string::npos);
    CHECK(text.find("glibre-foryc") != eastl::string::npos);
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
    CHECK(result->find("#pragma once") != eastl::string::npos);
}
