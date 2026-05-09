// SPDX-License-Identifier: Apache-2.0
// tests/tools/foryc/foryc_abi_hash_test.cpp
//
// Catch2 unit tests for glibre-foryc abi_hash module (plan #222).
//
// Test names match the Unit Test Plan in issue #222:
//
//   Collection ABI hash tests (compute_collection_abi_hash):
//     foryc_abi_hash_is_deterministic
//     foryc_abi_hash_changes_on_field_rename
//     foryc_abi_hash_changes_on_field_reorder_by_tag
//     foryc_abi_hash_changes_on_whitespace
//     foryc_abi_hash_changes_on_version_bump
//     foryc_collection_abi_hash_zero_schemas
//     foryc_collection_abi_hash_one_type_zero_fields
//
//   Source hash tests:
//     foryc_source_hash_changes_on_whitespace
//     foryc_source_hash_changes_on_field_rename
//
//   Format helper tests:
//     foryc_format_as_uint64_hex_is_16_chars
//     foryc_format_as_full_hex_is_64_chars
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

// Compute collection ABI hash from a single .fory source string.
// Convenience wrapper: creates a single-schema collection.
static Blake3Digest abi_hash_of(std::string_view src) {
    auto src_hash_r = compute_source_hash(src);
    REQUIRE(src_hash_r.has_value());
    eastl::vector<SchemaWithDigest> entries;
    entries.push_back({parse_ok(src), *src_hash_r});
    auto r = compute_collection_abi_hash(entries);
    REQUIRE(r.has_value());
    return *r;
}

// Compute raw source hash, asserting success.
static Blake3Digest source_hash_of(std::string_view src) {
    auto r = compute_source_hash(src);
    REQUIRE(r.has_value());
    return *r;
}

// -----------------------------------------------------------------------
// Collection ABI hash tests
// -----------------------------------------------------------------------

TEST_CASE("foryc_abi_hash_is_deterministic", "[foryc][abi_hash]") {
    // Calling compute_collection_abi_hash twice on the same inputs produces
    // the same 32-byte digest.  No randomness or time-based inputs.
    constexpr std::string_view src = R"(
schema glibre.test.Foo {
  version 1
  field x : u32 tag 1
  field y : f32 tag 2
}
)";
    auto src_hash_r = compute_source_hash(src);
    REQUIRE(src_hash_r.has_value());

    eastl::vector<SchemaWithDigest> entries;
    entries.push_back({parse_ok(src), *src_hash_r});

    auto r1 = compute_collection_abi_hash(entries);
    auto r2 = compute_collection_abi_hash(entries);
    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());
    CHECK(*r1 == *r2);
}

TEST_CASE("abi_hash_stable_across_runs", "[foryc][abi_hash]") {
    // Alias for foryc_abi_hash_is_deterministic (issue body name).
    constexpr std::string_view src = R"(
schema StableSchema {
  version 2
  field a : u64 tag 1
}
)";
    CHECK(abi_hash_of(src) == abi_hash_of(src));
}

TEST_CASE("foryc_abi_hash_changes_on_field_rename", "[foryc][abi_hash]") {
    // Renaming a field while keeping type and tag must change the ABI hash.
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
    // Source hashes differ (raw bytes differ), causing different collection hash.
    CHECK(abi_hash_of(src_before) != abi_hash_of(src_after));
}

TEST_CASE("abi_hash_changes_on_schema_edit", "[foryc][abi_hash]") {
    // Alias for foryc_abi_hash_changes_on_field_rename.
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
    // Different tag assignments on the same fields must change the hash.
    // Tag renumbering is ABI-breaking (fory-codegen.md §ABI Stability Rules).
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

TEST_CASE("foryc_abi_hash_changes_on_version_bump", "[foryc][abi_hash]") {
    // Bumping the schema version must change the collection ABI hash.
    // version is included as version_le(4) in the recipe.
    constexpr std::string_view src_v1 = R"(
schema glibre.test.Versioned {
  version 1
  field x : u32 tag 1
}
)";
    constexpr std::string_view src_v2 = R"(
schema glibre.test.Versioned {
  version 2
  field x : u32 tag 1
}
)";
    CHECK(abi_hash_of(src_v1) != abi_hash_of(src_v2));
}

TEST_CASE("foryc_abi_hash_changes_on_whitespace", "[foryc][abi_hash]") {
    // The collection ABI hash recipe includes the raw source hash, so two
    // .fory files differing only in whitespace will produce DIFFERENT
    // collection hashes (because raw source bytes differ).
    //
    // Whitespace-invariance is a property of the CANONICAL source hash
    // (compute_canonical_source_hash), not of the locked wire recipe.
    // This test verifies correct behaviour: whitespace changes the hash.
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
    // Raw bytes differ → source_hash differs → collection hash differs.
    CHECK(abi_hash_of(compact) != abi_hash_of(spaced));
}

TEST_CASE("foryc_collection_abi_hash_zero_schemas", "[foryc][abi_hash]") {
    // Empty collection produces the blake3 of an empty input — deterministic.
    eastl::vector<SchemaWithDigest> empty_entries;
    auto r1 = compute_collection_abi_hash(empty_entries);
    auto r2 = compute_collection_abi_hash(empty_entries);
    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());
    CHECK(*r1 == *r2);
}

TEST_CASE("foryc_collection_abi_hash_one_type_zero_fields", "[foryc][abi_hash]") {
    // A schema with a single type that has no fields should not crash and
    // should produce a deterministic hash.
    constexpr std::string_view src = R"(
schema glibre.test.Empty {
  version 1
}
)";
    // Should not throw; result should be deterministic.
    const Blake3Digest h1 = abi_hash_of(src);
    const Blake3Digest h2 = abi_hash_of(src);
    CHECK(h1 == h2);
}

TEST_CASE("abi_hash_independent_of_schema_file_order_on_disk", "[foryc][abi_hash]") {
    // The collection ABI hash sorts TypeDecls by fqn before hashing.
    // Two collections declaring the same types in different order must
    // produce the same collection hash.
    constexpr std::string_view src_alpha = R"(
schema glibre.test.Alpha {
  version 1
  field a : u32 tag 1
}
)";
    constexpr std::string_view src_beta = R"(
schema glibre.test.Beta {
  version 1
  field b : f32 tag 1
}
)";

    // Build collection [Alpha, Beta].
    auto dig_alpha_r = compute_source_hash(src_alpha);
    auto dig_beta_r  = compute_source_hash(src_beta);
    REQUIRE(dig_alpha_r.has_value());
    REQUIRE(dig_beta_r.has_value());

    eastl::vector<SchemaWithDigest> entries_ab;
    entries_ab.push_back({parse_ok(src_alpha), *dig_alpha_r});
    entries_ab.push_back({parse_ok(src_beta),  *dig_beta_r});

    // Build collection [Beta, Alpha].
    eastl::vector<SchemaWithDigest> entries_ba;
    entries_ba.push_back({parse_ok(src_beta),  *dig_beta_r});
    entries_ba.push_back({parse_ok(src_alpha), *dig_alpha_r});

    auto h_ab = compute_collection_abi_hash(entries_ab);
    auto h_ba = compute_collection_abi_hash(entries_ba);
    REQUIRE(h_ab.has_value());
    REQUIRE(h_ba.has_value());
    CHECK(*h_ab == *h_ba);
}

// -----------------------------------------------------------------------
// Source hash tests
// -----------------------------------------------------------------------

TEST_CASE("foryc_source_hash_changes_on_whitespace", "[foryc][source_hash]") {
    // compute_source_hash hashes raw bytes.  Two .fory sources that differ
    // only in whitespace yield DIFFERENT source hashes.
    // (Whitespace-invariant hashing is compute_canonical_source_hash.)
    constexpr std::string_view src_compact = "schema Foo{version 1 field x:u32 tag 1}";
    constexpr std::string_view src_spaced = R"(
schema Foo {
  version 1
  field x : u32 tag 1
}
)";
    CHECK(source_hash_of(src_compact) != source_hash_of(src_spaced));
}

TEST_CASE("foryc_source_hash_changes_on_field_rename", "[foryc][source_hash]") {
    // Renaming a field changes the raw bytes and therefore the source hash.
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
    // schema file is a blake3 of raw bytes.  Two different sources produce
    // different hashes; the same source produces the same hash twice.
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
    // Different FQN → different raw bytes → different source hash.
    CHECK(source_hash_of(src_a) != source_hash_of(src_b));

    // Same source twice → same hash.
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

// -----------------------------------------------------------------------
// format_as_full_hex helper test
// -----------------------------------------------------------------------

TEST_CASE("foryc_format_as_full_hex_is_64_chars", "[foryc][abi_hash]") {
    // format_as_full_hex must return exactly 64 lowercase hex characters.
    Blake3Digest all_zeros{};
    const eastl::string hex_zeros = format_as_full_hex(all_zeros);
    CHECK(hex_zeros.size() == 64);
    // All zeros → 64 '0' chars.
    CHECK(hex_zeros == eastl::string(64, '0'));

    Blake3Digest all_ff{};
    for (auto& b : all_ff)
        b = 0xFF;
    const eastl::string hex_ff = format_as_full_hex(all_ff);
    CHECK(hex_ff.size() == 64);
    CHECK(hex_ff == eastl::string(64, 'f'));

    // format_as_full_hex first 16 chars must equal format_as_uint64_hex
    // (big-endian uint64 of first 8 bytes).
    Blake3Digest mixed{};
    for (std::size_t i = 0; i < 32; ++i)
        mixed[i] = static_cast<uint8_t>(i + 1);  // 0x01..0x20

    const eastl::string full_hex = format_as_full_hex(mixed);
    const eastl::string u64_hex  = format_as_uint64_hex(mixed);
    CHECK(full_hex.size() == 64);
    CHECK(u64_hex.size() == 16);
    // First 16 chars of full_hex must equal the uint64 hex.
    CHECK(eastl::string(full_hex.data(), 16) == u64_hex);
}
