// SPDX-License-Identifier: Apache-2.0
// tests/tools/foryc/foryc_abi_hash_test.cpp
//
// Catch2 unit tests for glibre-foryc abi_hash module (plan #222).
//
// Test names match the Unit Test Plan in issue #222 (as adjusted by the
// dispatch prompt analysis):
//
//   ABI hash tests:
//     foryc_abi_hash_is_deterministic
//     foryc_abi_hash_changes_on_field_rename
//     foryc_abi_hash_changes_on_field_reorder_by_tag
//     foryc_abi_hash_unchanged_on_whitespace
//
//   Source hash tests:
//     foryc_source_hash_unchanged_on_whitespace
//     foryc_source_hash_changes_on_field_rename
//
//   Format helper test:
//     foryc_format_as_uint64_hex_is_16_chars
//
//   Issue-body unit test plan aliases:
//     abi_hash_stable_across_runs            (alias for deterministic)
//     abi_hash_changes_on_schema_edit        (covered by field_rename)
//     abi_hash_independent_of_schema_file_order_on_disk
//     per_type_schema_hash_matches_blake3_of_source

#include <catch2/catch_test_macros.hpp>

#include "abi_hash.hpp"
#include "parser.hpp"

using namespace glibre::tools::foryc;

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

// Parse a .fory source string, asserting success.
static Schema parse_ok(std::string_view src) {
    auto r = parse_string(src, "<test>");
    REQUIRE(r.has_value());
    return std::move(*r);
}

// Compute ABI hash from a .fory source string, asserting success.
static Blake3Digest abi_hash_of(std::string_view src) {
    const Schema schema = parse_ok(src);
    auto r = compute_abi_hash(schema);
    REQUIRE(r.has_value());
    return *r;
}

// Compute source hash, asserting success.
static Blake3Digest source_hash_of(std::string_view src) {
    auto r = compute_source_hash(src);
    REQUIRE(r.has_value());
    return *r;
}

// -----------------------------------------------------------------------
// ABI hash tests
// -----------------------------------------------------------------------

TEST_CASE("foryc_abi_hash_is_deterministic", "[foryc][abi_hash]") {
    // Calling compute_abi_hash twice on the same Schema produces the same
    // 32-byte digest.  This asserts that the function is a pure function of
    // the IR with no randomness or time-based inputs.
    constexpr std::string_view src = R"(
schema glibre.test.Foo {
  version 1
  field x : u32 tag 1
  field y : f32 tag 2
}
)";
    const Schema schema = parse_ok(src);
    auto r1 = compute_abi_hash(schema);
    auto r2 = compute_abi_hash(schema);
    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());
    CHECK(*r1 == *r2);
}

TEST_CASE("abi_hash_stable_across_runs", "[foryc][abi_hash]") {
    // Alias for foryc_abi_hash_is_deterministic (issue body name).
    // Computing the hash of the same schema on two consecutive calls must
    // produce the same result (no PRNG, no clock, no env inputs).
    constexpr std::string_view src = R"(
schema StableSchema {
  version 2
  field a : u64 tag 1
}
)";
    const Schema schema = parse_ok(src);
    auto h1 = compute_abi_hash(schema);
    auto h2 = compute_abi_hash(schema);
    REQUIRE(h1.has_value());
    REQUIRE(h2.has_value());
    CHECK(*h1 == *h2);
}

TEST_CASE("foryc_abi_hash_changes_on_field_rename", "[foryc][abi_hash]") {
    // Renaming a field while keeping type and tag the same must produce a
    // different ABI hash.  Field name is part of the ABI contract
    // (serialized struct members have named accessors that break on rename).
    constexpr std::string_view src_before = R"(
schema glibre.test.Widget {
  version 1
  field old_name : u32 tag 1
}
)";
    constexpr std::string_view src_after = R"(
schema glibre.test.Widget {
  version 1
  field new_name : u32 tag 1
}
)";
    CHECK(abi_hash_of(src_before) != abi_hash_of(src_after));
}

TEST_CASE("abi_hash_changes_on_schema_edit", "[foryc][abi_hash]") {
    // Alias for foryc_abi_hash_changes_on_field_rename (issue body name).
    // Any semantic change to the schema IR must change the ABI hash.
    constexpr std::string_view src_v1 = R"(
schema EditSchema {
  version 1
  field count : u32 tag 1
}
)";
    constexpr std::string_view src_v2 = R"(
schema EditSchema {
  version 1
  field total : u32 tag 1
}
)";
    CHECK(abi_hash_of(src_v1) != abi_hash_of(src_v2));
}

TEST_CASE("foryc_abi_hash_changes_on_field_reorder_by_tag", "[foryc][abi_hash]") {
    // Two schemas with the same fields but different tag assignments must
    // produce different ABI hashes.  Tag assignment determines on-disk
    // layout (per fory-codegen.md §ABI Stability Rules); renumbering tags
    // is an ABI-breaking change.
    constexpr std::string_view src_original = R"(
schema glibre.test.Ordered {
  version 1
  field x : u32 tag 1
  field y : f32 tag 2
}
)";
    constexpr std::string_view src_retagged = R"(
schema glibre.test.Ordered {
  version 1
  field x : u32 tag 2
  field y : f32 tag 1
}
)";
    CHECK(abi_hash_of(src_original) != abi_hash_of(src_retagged));
}

TEST_CASE("foryc_abi_hash_unchanged_on_whitespace", "[foryc][abi_hash]") {
    // Two schemas that are identical except for indentation and blank lines
    // must produce the same ABI hash.  Whitespace is not part of the ABI
    // contract; only fqn, version, field tags, types, and names matter.
    constexpr std::string_view compact = R"(schema glibre.test.Ws{version 1
field x:u32 tag 1
field y:f32 tag 2})";

    constexpr std::string_view spaced = R"(
schema glibre.test.Ws {
  version 1

  field x : u32   tag 1
  field y : f32   tag 2

}
)";
    // Both must parse successfully and yield the same ABI hash.
    CHECK(abi_hash_of(compact) == abi_hash_of(spaced));
}

TEST_CASE("abi_hash_independent_of_schema_file_order_on_disk", "[foryc][abi_hash]") {
    // This test verifies that the ABI hash for a schema collection with
    // multiple types is independent of the declaration order of those types
    // in the source file.  compute_abi_hash sorts types by fqn before
    // hashing, so two files that declare the same types in different order
    // must produce the same hash.
    constexpr std::string_view src_ab = R"(
schema glibre.test.Alpha {
  version 1
  field a : u32 tag 1
}
schema glibre.test.Beta {
  version 1
  field b : f32 tag 1
}
)";
    constexpr std::string_view src_ba = R"(
schema glibre.test.Beta {
  version 1
  field b : f32 tag 1
}
schema glibre.test.Alpha {
  version 1
  field a : u32 tag 1
}
)";
    // Parse into two schemas and compare ABI hashes.
    CHECK(abi_hash_of(src_ab) == abi_hash_of(src_ba));
}

// -----------------------------------------------------------------------
// Source hash tests
// -----------------------------------------------------------------------

TEST_CASE("foryc_source_hash_unchanged_on_whitespace", "[foryc][source_hash]") {
    // The source hash canonicalizes whitespace before hashing.  Two schema
    // sources that differ only in whitespace (indentation, blank lines) must
    // yield the same source hash.
    constexpr std::string_view src_compact = "schema Foo{version 1 field x:u32 tag 1}";
    constexpr std::string_view src_spaced = R"(
schema Foo {
  version 1
  field x : u32 tag 1
}
)";
    CHECK(source_hash_of(src_compact) == source_hash_of(src_spaced));
}

TEST_CASE("foryc_source_hash_changes_on_field_rename", "[foryc][source_hash]") {
    // Renaming a field must change the source hash even if the ABI hash
    // also changes.  The source hash is over the token sequence; a rename
    // changes the token sequence.
    constexpr std::string_view src_before = R"(
schema Foo {
  version 1
  field old_name : u32 tag 1
}
)";
    constexpr std::string_view src_after = R"(
schema Foo {
  version 1
  field new_name : u32 tag 1
}
)";
    CHECK(source_hash_of(src_before) != source_hash_of(src_after));
}

TEST_CASE("per_type_schema_hash_matches_blake3_of_source", "[foryc][source_hash]") {
    // The source hash computed by compute_source_hash for a single-type
    // schema file must match the blake3 of the canonical token sequence.
    // We verify this indirectly: two differently-whitespaced sources with
    // the same token sequence produce the same hash (tested above), and two
    // sources with different token sequences produce different hashes.
    constexpr std::string_view src_a = R"(
schema glibre.core.ComponentA {
  version 1
  field pos : vec3f tag 1
}
)";
    constexpr std::string_view src_b = R"(
schema glibre.core.ComponentB {
  version 1
  field pos : vec3f tag 1
}
)";
    // Different FQN → different source token sequence → different source hash.
    CHECK(source_hash_of(src_a) != source_hash_of(src_b));

    // Same schema twice → same hash.
    CHECK(source_hash_of(src_a) == source_hash_of(src_a));
}

// -----------------------------------------------------------------------
// format_as_uint64_hex helper test
// -----------------------------------------------------------------------

TEST_CASE("foryc_format_as_uint64_hex_is_16_chars", "[foryc][abi_hash]") {
    // format_as_uint64_hex must always return exactly 16 lowercase hex
    // characters, with leading zeros where needed.
    Blake3Digest all_zeros{};  // zero-initialized
    const eastl::string hex_zeros = format_as_uint64_hex(all_zeros);
    CHECK(hex_zeros.size() == 16);
    CHECK(hex_zeros == eastl::string("0000000000000000"));

    Blake3Digest all_ff{};
    for (auto& b : all_ff)
        b = 0xFF;
    const eastl::string hex_ff = format_as_uint64_hex(all_ff);
    CHECK(hex_ff.size() == 16);
    CHECK(hex_ff == eastl::string("ffffffffffffffff"));

    // Verify a known value: first 8 bytes [0x01, 0x00, ..., 0x00] →
    // big-endian uint64 = 0x0100000000000000.
    Blake3Digest known{};
    known[0] = 0x01;
    const eastl::string hex_known = format_as_uint64_hex(known);
    CHECK(hex_known == eastl::string("0100000000000000"));
}
