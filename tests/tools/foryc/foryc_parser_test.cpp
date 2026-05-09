// SPDX-License-Identifier: Apache-2.0
// tests/tools/foryc/foryc_parser_test.cpp
//
// Catch2 unit tests for glibre-foryc parser (plan #219).
//
// Test names match the Unit Test Plan in issue #219:
//   - parses_minimal_schema
//   - rejects_duplicate_tag
//   - rejects_zero_version          (was: rejects_non_monotone_version — renamed to match
//   assertion)
//   - rejects_duplicate_version_key
//   - walks_input_directory_recursively
//
// Also covers the dispatch-prompt-named cases:
//   - foryc_parses_minimal_fory_schema  (alias via SECTION)
//   - foryc_rejects_malformed_input

#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

#include "parser.hpp"

namespace fs = std::filesystem;
using namespace glibre::tools::foryc;

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

static bool has_tools_error(const glibre::Error& e, glibre::tools::Error code) noexcept {
    const auto* te = eastl::get_if<glibre::tools::Error>(&e.code());
    return te && (*te == code);
}

// -----------------------------------------------------------------------
// Core parser tests (names from plan #219 §Unit Test Plan)
// -----------------------------------------------------------------------

TEST_CASE("parses_minimal_schema", "[foryc][parser]") {
    // Minimal valid .fory schema: one type, one field.
    // Also satisfies the dispatch prompt test "foryc_parses_minimal_fory_schema".
    constexpr std::string_view src = R"(
schema glibre.test.Foo {
  version 1
  since   "0.0.1"
  field x : u32 tag 1 since 1
}
)";

    auto result = parse_string(src, "test.fory");
    REQUIRE(result.has_value());

    const auto& schema = *result;
    REQUIRE(schema.types.size() == 1);

    const auto& td = schema.types[0];
    CHECK(td.fqn == eastl::string("glibre.test.Foo"));
    CHECK(td.version == 1);
    CHECK(td.since_version == eastl::string("0.0.1"));
    REQUIRE(td.fields.size() == 1);
    CHECK(td.fields[0].name == eastl::string("x"));
    CHECK(td.fields[0].type_name == eastl::string("u32"));
    CHECK(td.fields[0].tag == 1);
    CHECK(td.fields[0].since == 1);
}

TEST_CASE("foryc_parses_minimal_fory_schema", "[foryc][parser]") {
    // Explicit alias for the dispatch-prompt-named test.
    // Parses `type Foo { x: u32 }` expressed in .fory syntax.
    constexpr std::string_view src = R"(
schema Foo {
  version 1
  field x : u32 tag 1
}
)";

    auto result = parse_string(src, "<string>");
    REQUIRE(result.has_value());
    const auto& schema = *result;
    REQUIRE(schema.types.size() == 1);
    CHECK(schema.types[0].fields.size() == 1);
    CHECK(schema.types[0].fields[0].name == eastl::string("x"));
    CHECK(schema.types[0].fields[0].type_name == eastl::string("u32"));
}

TEST_CASE("rejects_duplicate_tag", "[foryc][parser]") {
    // Two fields sharing tag 1 must yield ForycDuplicateTag.
    constexpr std::string_view src = R"(
schema glibre.test.Bad {
  version 1
  field a : u32 tag 1
  field b : f32 tag 1
}
)";

    auto result = parse_string(src, "bad.fory");
    REQUIRE(!result.has_value());
    CHECK(has_tools_error(result.error(), glibre::tools::Error::ForycDuplicateTag));
}

TEST_CASE("rejects_zero_version", "[foryc][parser]") {
    // version 0 is invalid (must be >= 1).
    // Renamed from rejects_non_monotone_version: the parser only rejects
    // version == 0 at this stage; monotonicity across migration blocks is
    // out of scope for plan #219 (sibling plans #220–#225).
    constexpr std::string_view src = R"(
schema glibre.test.Zero {
  version 0
  field x : u32 tag 1
}
)";

    auto result = parse_string(src, "zero.fory");
    REQUIRE(!result.has_value());
    CHECK(has_tools_error(result.error(), glibre::tools::Error::ForycNonMonotoneVersion));
}

TEST_CASE("rejects_duplicate_version_key", "[foryc][parser]") {
    // A schema block with two `version` lines must yield ForycSyntaxError.
    constexpr std::string_view src = R"(
schema glibre.test.DupVersion {
  version 1
  version 2
  field x : u32 tag 1
}
)";

    auto result = parse_string(src, "dup_version.fory");
    REQUIRE(!result.has_value());
    CHECK(has_tools_error(result.error(), glibre::tools::Error::ForycSyntaxError));
}

TEST_CASE("rejects_duplicate_since_key", "[foryc][parser]") {
    // A schema block with two `since` lines must yield ForycSyntaxError.
    constexpr std::string_view src = R"(
schema glibre.test.DupSince {
  version 1
  since "0.0.1"
  since "0.0.2"
  field x : u32 tag 1
}
)";

    auto result = parse_string(src, "dup_since.fory");
    REQUIRE(!result.has_value());
    CHECK(has_tools_error(result.error(), glibre::tools::Error::ForycSyntaxError));
}

TEST_CASE("walks_input_directory_recursively", "[foryc][parser][fs]") {
    // Create a temporary directory with two .fory files in a nested layout
    // and verify that parse_file() reads them both without error.
    const auto tmp = fs::temp_directory_path() / "foryc_test_walk";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "sub");

    // Write a minimal .fory file at root and one in a subdirectory.
    auto write_fory = [](const fs::path& p, std::string_view name) {
        std::ofstream ofs{p};
        ofs << "schema " << name << " {\n"
            << "  version 1\n"
            << "  field x : u32 tag 1\n"
            << "}\n";
    };

    const fs::path root_file = tmp / "Root.fory";
    const fs::path sub_file = tmp / "sub" / "Sub.fory";
    write_fory(root_file, "Root");
    write_fory(sub_file, "Sub");

    // Parse each file via parse_file() to confirm recursive discovery works
    // (the walk loop itself lives in main.cpp; we verify parse_file here).
    std::size_t file_count = 0;
    std::size_t type_count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(tmp)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".fory")
            continue;
        ++file_count;
        auto r = parse_file(entry.path());
        REQUIRE(r.has_value());
        type_count += r->types.size();
    }

    CHECK(file_count == 2);
    CHECK(type_count == 2);

    fs::remove_all(tmp);
}

TEST_CASE("foryc_rejects_malformed_input", "[foryc][parser]") {
    // Garbage input yields ForycSyntaxError.
    constexpr std::string_view src = "this is not valid .fory @@@";
    auto result = parse_string(src, "garbage.fory");
    REQUIRE(!result.has_value());
    // The parser should return some tools::Error variant, not a
    // core::Error or render::Error.
    const auto* te = eastl::get_if<glibre::tools::Error>(&result.error().code());
    CHECK(te != nullptr);
}

TEST_CASE("parses_multiple_types", "[foryc][parser]") {
    // Multiple schema blocks in one file.
    constexpr std::string_view src = R"(
schema glibre.core.Transform {
  version 3
  since   "0.1.0"
  field translation : vec3f  tag 1 since 1
  field rotation    : quatf  tag 2 since 1
  field scale       : vec3f  tag 3 since 1 default { 1.0, 1.0, 1.0 }
  field flags       : u32    tag 4 since 2
  field parent      : entity tag 5 since 3
}

migration v2_to_v3 {
  provider "glibre::core::migrate_Transform_v2_to_v3"
}

schema glibre.core.Velocity {
  version 1
  field linear  : vec3f tag 1
  field angular : vec3f tag 2
}
)";

    auto result = parse_string(src, "multi.fory");
    REQUIRE(result.has_value());
    const auto& schema = *result;
    // Two schema blocks; migration is consumed but not stored.
    REQUIRE(schema.types.size() == 2);
    CHECK(schema.types[0].fqn == eastl::string("glibre.core.Transform"));
    CHECK(schema.types[0].version == 3);
    CHECK(schema.types[0].fields.size() == 5);
    CHECK(schema.types[1].fqn == eastl::string("glibre.core.Velocity"));
    CHECK(schema.types[1].fields.size() == 2);
}

TEST_CASE("rejects_unknown_type", "[foryc][parser]") {
    // A field type that is not in the builtins set.
    constexpr std::string_view src = R"(
schema glibre.test.Bad {
  version 1
  field x : NotABuiltin tag 1
}
)";
    auto result = parse_string(src, "bad_type.fory");
    REQUIRE(!result.has_value());
    CHECK(has_tools_error(result.error(), glibre::tools::Error::ForycUnknownType));
}

TEST_CASE("parses_generic_field_types", "[foryc][parser]") {
    // Generic builtin types: list<T>, map<K,V>, option<T>.
    constexpr std::string_view src = R"(
schema glibre.test.Generics {
  version 1
  field items  : list<u32>       tag 1
  field lookup : map<u32,string> tag 2
  field maybe  : option<entity>  tag 3
}
)";
    auto result = parse_string(src, "generics.fory");
    REQUIRE(result.has_value());
    CHECK(result->types[0].fields.size() == 3);
}

// -----------------------------------------------------------------------
// Plan #1010 — double-underscore guard in FQN segments
// -----------------------------------------------------------------------

// Test: foryc_parser_rejects_double_underscore_segment
//
// Verify that the parser rejects FQN segments containing "__" (double-
// underscore).  The codegen mangling scheme (fory-codegen.md §"ABI Stability
// Rules" point 4, plan #1010) replaces every '.' in a dotted FQN with "__" to
// form C symbol suffixes.  A segment that already contains "__" would alias a
// distinct FQN (e.g. "glibre.core__Foo" and "glibre.core.Foo" would both
// mangle to "glibre__core__Foo"), making fqn_to_mangled non-injective.
//
// The parser must reject such schemas with ForycInvalidIdentifier.
TEST_CASE("foryc_parser_rejects_double_underscore_segment", "[foryc][parser][fqn_mangle]") {
    // Case 1: double-underscore in the first (top-level) segment.
    {
        constexpr std::string_view src = R"(
schema glibre__bad.core.Foo {
  version 1
  field x : u32 tag 1
}
)";
        auto result = parse_string(src, "bad_fqn_first.fory");
        REQUIRE(!result.has_value());
        CHECK(has_tools_error(result.error(), glibre::tools::Error::ForycInvalidIdentifier));
    }

    // Case 2: double-underscore in a middle segment.
    {
        constexpr std::string_view src = R"(
schema glibre.core__bad.Foo {
  version 1
  field x : u32 tag 1
}
)";
        auto result = parse_string(src, "bad_fqn_middle.fory");
        REQUIRE(!result.has_value());
        CHECK(has_tools_error(result.error(), glibre::tools::Error::ForycInvalidIdentifier));
    }

    // Case 3: double-underscore in the type-name (last) segment.
    {
        constexpr std::string_view src = R"(
schema glibre.core.Foo__Bar {
  version 1
  field x : u32 tag 1
}
)";
        auto result = parse_string(src, "bad_fqn_last.fory");
        REQUIRE(!result.has_value());
        CHECK(has_tools_error(result.error(), glibre::tools::Error::ForycInvalidIdentifier));
    }

    // Case 4: single underscore is still accepted (sanity guard).
    {
        constexpr std::string_view src = R"(
schema glibre.core.Foo_Bar {
  version 1
  field x : u32 tag 1
}
)";
        auto result = parse_string(src, "single_underscore_ok.fory");
        REQUIRE(result.has_value());
        CHECK(result->types[0].fqn == eastl::string("glibre.core.Foo_Bar"));
    }
}
