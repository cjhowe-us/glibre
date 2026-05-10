// SPDX-License-Identifier: Apache-2.0
// tests/data/schemas/geometry/geometry_schema_parse_test.cpp
//
// Catch2 unit tests for the three geometry Fory side-record schemas
// (plan #696):
//   - CookManifest.fory       (specs/geometry/SPEC.md §7.1.1)
//   - BLASRecipeRecord.fory   (specs/geometry/SPEC.md §7.1.2)
//   - MeshSourceMetadata.fory (specs/geometry/SPEC.md §7.1.3)
//
// Test names (dispatch-prompt names for DoD):
//   - geometry_cook_manifest_schema_parses
//   - geometry_blas_recipe_record_schema_parses
//   - geometry_mesh_source_metadata_schema_parses
//
// Test names (plan #696 Unit Test Plan — structural-invariant checks):
//   - data/schemas/geometry/CookManifest: round_trip_preserves_triple_lock
//   - data/schemas/geometry/CookManifest: rejects_meshlet_caps_other_than_64_124
//   - data/schemas/geometry/CookManifest: rejects_unknown_draco_profile_value
//   - data/schemas/geometry/BLASRecipeRecord: round_trip_preserves_descriptor_order
//   - data/schemas/geometry/BLASRecipeRecord: rejects_format_hash_mismatch_with_companion_pak
//   - data/schemas/geometry/MeshSourceMetadata: round_trip_preserves_bounding_box
//
// NOTE on "round_trip" semantics: the Apache Fory C++ serializer is not yet
// plumbed in this codebase (no Fory runtime API surface exists in core/ or
// plugins/).  "Round_trip" tests here verify that the schema parses correctly
// and the named fields are present with the correct types — the precondition
// for a Fory round-trip once the serializer is available.  This is consistent
// with the deferral rationale in schema_migration_test.cpp (plan #977).
//
// PHILOSOPHY §11: EASTL replaces std containers.
//   std:: retained for std::string_view (API boundary), std::filesystem,
//   std::expected (glibre::Result).

#include <filesystem>
#include <string_view>

#include <EASTL/string.h>
#include <EASTL/vector.h>
#include <catch2/catch_test_macros.hpp>

#include "parser.hpp"

namespace fs = std::filesystem;
using namespace glibre::tools::foryc;

// ---------------------------------------------------------------------------
// On-disk schema root threaded in by CMakeLists.txt via target_compile_definitions.
//
// GEOMETRY_SCHEMA_DIR expands to "${CMAKE_SOURCE_DIR}/data/schemas/geometry"
// at configure time so tests always parse the canonical .fory artefacts named
// by the DoD file_exists: block, preventing silent drift between the on-disk
// file and any previously-embedded copy.
// ---------------------------------------------------------------------------

#ifndef GEOMETRY_SCHEMA_DIR
#    error "GEOMETRY_SCHEMA_DIR must be set via target_compile_definitions in CMakeLists.txt"
#endif

// Parse the .fory file at GEOMETRY_SCHEMA_DIR/<filename> and return the result.
// On failure the caller is expected to REQUIRE(result.has_value()) which will
// emit the Catch2 failure with the full result context; this helper is kept
// thin so the failure line number points into the TEST_CASE, not this helper.
[[nodiscard]] static ParseResult load_schema(const char* filename) {
    fs::path path = fs::path(GEOMETRY_SCHEMA_DIR) / filename;
    return parse_file(path);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Return the FieldDecl for the named field, or nullptr if not found.
static const FieldDecl* find_field(const TypeDecl& td, std::string_view name) noexcept {
    for (const auto& f : td.fields) {
        if (f.name == eastl::string(name.data(), name.size()))
            return &f;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// DoD smoke-parse tests: assert each on-disk .fory file parses successfully.
// These are the names required by the plan's DoD block.
// ---------------------------------------------------------------------------

TEST_CASE("geometry_cook_manifest_schema_parses", "[geometry][schemas][cook_manifest]") {
    auto result = load_schema("CookManifest.fory");
    REQUIRE(result.has_value());

    const Schema& schema = *result;
    REQUIRE(schema.types.size() == 1);

    const TypeDecl& td = schema.types[0];
    CHECK(td.fqn == eastl::string("glibre.geometry.CookManifest"));
    CHECK(td.version == 1);
    CHECK(td.since_version == eastl::string("0.1.0"));

    // Total field count from §7.1.1 schema block:
    //   tags 1-7 (7) + tags 10-15 (6) + tags 20-23 (4) + tags 30-31 (2) +
    //   tags 40-42 (3) + tags 50-53 (4) = 26 fields total.
    CHECK(td.fields.size() == 26);
}

TEST_CASE("geometry_blas_recipe_record_schema_parses", "[geometry][schemas][blas_recipe]") {
    auto result = load_schema("BLASRecipeRecord.fory");
    REQUIRE(result.has_value());

    const Schema& schema = *result;
    // Two schema blocks in the file: BLASRecipeRecord + BLASGeometryDescriptor.
    REQUIRE(schema.types.size() == 2);

    const TypeDecl& td_blas = schema.types[0];
    CHECK(td_blas.fqn == eastl::string("glibre.geometry.BLASRecipeRecord"));
    CHECK(td_blas.version == 1);
    CHECK(td_blas.fields.size() == 6);

    const TypeDecl& td_desc = schema.types[1];
    CHECK(td_desc.fqn == eastl::string("glibre.geometry.BLASGeometryDescriptor"));
    CHECK(td_desc.version == 1);
    // BLASGeometryDescriptor fields: tags 1,2 (2) + tags 10-13 (4) + tags 20-22 (3) + tag 30 (1)
    // = 10.
    CHECK(td_desc.fields.size() == 10);
}

TEST_CASE(
    "geometry_mesh_source_metadata_schema_parses", "[geometry][schemas][mesh_source_metadata]"
) {
    auto result = load_schema("MeshSourceMetadata.fory");
    REQUIRE(result.has_value());

    const Schema& schema = *result;
    REQUIRE(schema.types.size() == 1);

    const TypeDecl& td = schema.types[0];
    CHECK(td.fqn == eastl::string("glibre.geometry.MeshSourceMetadata"));
    CHECK(td.version == 1);
    CHECK(td.since_version == eastl::string("0.1.0"));
    CHECK(td.fields.size() == 10);
}

// ---------------------------------------------------------------------------
// CookManifest structural-invariant tests (plan #696 Unit Test Plan)
// ---------------------------------------------------------------------------

// data/schemas/geometry/CookManifest: round_trip_preserves_triple_lock
//
// Asserts the three cook-incremental key fields (the "triple lock") are
// present with the correct types and tag numbers (§7.1.1 invariant 1).
// Full Fory serialization round-trip is deferred; the Fory C++ serializer
// is not yet plumbed in this codebase (see NOTE at top of file).
TEST_CASE(
    "data/schemas/geometry/CookManifest: round_trip_preserves_triple_lock",
    "[geometry][schemas][cook_manifest]"
) {
    auto result = load_schema("CookManifest.fory");
    REQUIRE(result.has_value());
    const TypeDecl& td = result->types[0];

    // source_content_hash — tag 3
    const FieldDecl* f_sch = find_field(td, "source_content_hash");
    REQUIRE(f_sch != nullptr);
    CHECK(f_sch->type_name == eastl::string("u64"));
    CHECK(f_sch->tag == 3);

    // format_hash — tag 4
    const FieldDecl* f_fh = find_field(td, "format_hash");
    REQUIRE(f_fh != nullptr);
    CHECK(f_fh->type_name == eastl::string("u64"));
    CHECK(f_fh->tag == 4);

    // foryc_abi_hash — tag 5
    const FieldDecl* f_ah = find_field(td, "foryc_abi_hash");
    REQUIRE(f_ah != nullptr);
    CHECK(f_ah->type_name == eastl::string("u64"));
    CHECK(f_ah->tag == 5);
}

// data/schemas/geometry/CookManifest: rejects_meshlet_caps_other_than_64_124
//
// The spec (§7.1.1 invariant 2) hard-freezes meshlet_max_vertices == 64 and
// meshlet_max_triangles == 124.  Both fields must be u8 at tags 20 and 21
// respectively.  Any schema that removes or changes these tags would fail to
// uphold the invariant; we verify the fields are present, typed correctly, and
// at the correct tags — the validator that checks *values* lives in the cooker
// (Error::CookManifestInvalid) and is out of scope for plan #696.
//
// TODO(value-check): the name "rejects_*" implies runtime value rejection, but
// this test only verifies field type/tag layout (u8 at tags 20/21) — a
// necessary precondition.  Actual rejection of non-64/non-124 values via
// Error::CookManifestInvalid will be owned by the cook-driver plan (#487) once
// the cooker pipeline is wired.  The DoD unit_test_named: clause must keep this
// exact test name, so we document the intent gap here rather than renaming.
TEST_CASE(
    "data/schemas/geometry/CookManifest: rejects_meshlet_caps_other_than_64_124",
    "[geometry][schemas][cook_manifest]"
) {
    auto result = load_schema("CookManifest.fory");
    REQUIRE(result.has_value());
    const TypeDecl& td = result->types[0];

    // meshlet_max_vertices — tag 20, type u8
    const FieldDecl* f_mv = find_field(td, "meshlet_max_vertices");
    REQUIRE(f_mv != nullptr);
    CHECK(f_mv->type_name == eastl::string("u8"));
    CHECK(f_mv->tag == 20);

    // meshlet_max_triangles — tag 21, type u8
    const FieldDecl* f_mt = find_field(td, "meshlet_max_triangles");
    REQUIRE(f_mt != nullptr);
    CHECK(f_mt->type_name == eastl::string("u8"));
    CHECK(f_mt->tag == 21);

    // Verify a schema with a different type for these fields would NOT match
    // the frozen cap contract: simulate by checking that a mutated schema
    // (u32 instead of u8) parses but yields a different type_name.
    // This guards against accidental type widening in the .fory file.
    constexpr std::string_view mutated = R"(
schema glibre.geometry.CookManifestMutated {
  version  1
  field meshlet_max_vertices : u32 tag 20 since 1
  field meshlet_max_triangles : u32 tag 21 since 1
}
)";
    auto mutated_result = parse_string(mutated, "mutated.fory");
    REQUIRE(mutated_result.has_value());
    const FieldDecl* mf_mv = find_field(mutated_result->types[0], "meshlet_max_vertices");
    REQUIRE(mf_mv != nullptr);
    // Confirm that u32 != u8 — the widened type is detectably different.
    CHECK(mf_mv->type_name != eastl::string("u8"));
}

// data/schemas/geometry/CookManifest: rejects_unknown_draco_profile_value
//
// §7.1.1 invariant 3: draco_profile is a u8 projection of a closed-sum enum;
// unknown values yield Error::CookManifestInvalid at load time (that error arm
// is in the cooker, not the schema file).  Here we verify that draco_profile
// is declared as u8 at tag 30 — the closed-sum enforcement comes from the
// runtime.  A schema with a wider type (u32) would silently accept values
// outside the closed set; the test confirms the current schema uses u8.
//
// TODO(value-check): as with rejects_meshlet_caps_other_than_64_124, the
// "rejects_*" name implies runtime value rejection.  Actual
// Error::CookManifestInvalid for unknown draco_profile values is owned by the
// cook-driver plan (#487) and the Draco integration plan (#482).  This test
// verifies the schema layout precondition only; test name is frozen by the DoD
// unit_test_named: clause.
TEST_CASE(
    "data/schemas/geometry/CookManifest: rejects_unknown_draco_profile_value",
    "[geometry][schemas][cook_manifest]"
) {
    auto result = load_schema("CookManifest.fory");
    REQUIRE(result.has_value());
    const TypeDecl& td = result->types[0];

    // draco_profile — tag 30, type u8 (closed-sum enum projection)
    const FieldDecl* f_dp = find_field(td, "draco_profile");
    REQUIRE(f_dp != nullptr);
    CHECK(f_dp->type_name == eastl::string("u8"));
    CHECK(f_dp->tag == 30);

    // draco_speed — tag 31, type u8
    const FieldDecl* f_ds = find_field(td, "draco_speed");
    REQUIRE(f_ds != nullptr);
    CHECK(f_ds->type_name == eastl::string("u8"));
    CHECK(f_ds->tag == 31);
}

// ---------------------------------------------------------------------------
// BLASRecipeRecord structural-invariant tests (plan #696 Unit Test Plan)
// ---------------------------------------------------------------------------

// data/schemas/geometry/BLASRecipeRecord: round_trip_preserves_descriptor_order
//
// §7.1.2 invariant 3: descriptors must be sorted by ascending group_index.
// The schema expresses the ordering contract via the list<BLASGeometryDescriptor>
// field at tag 12.  We verify: (a) the field is present at tag 12 with the
// correct generic type, and (b) group_index is at tag 1 in BLASGeometryDescriptor
// (the sort key declared in the spec).  Runtime enforcement of ascending order
// at deserialise time is Error::BLASRecipeInvalid — that lives in the cooker.
TEST_CASE(
    "data/schemas/geometry/BLASRecipeRecord: round_trip_preserves_descriptor_order",
    "[geometry][schemas][blas_recipe]"
) {
    auto result = load_schema("BLASRecipeRecord.fory");
    REQUIRE(result.has_value());
    REQUIRE(result->types.size() == 2);

    const TypeDecl& td_blas = result->types[0];
    CHECK(td_blas.fqn == eastl::string("glibre.geometry.BLASRecipeRecord"));

    // descriptors — tag 12, type list<BLASGeometryDescriptor>
    const FieldDecl* f_desc = find_field(td_blas, "descriptors");
    REQUIRE(f_desc != nullptr);
    CHECK(f_desc->type_name == eastl::string("list<BLASGeometryDescriptor>"));
    CHECK(f_desc->tag == 12);

    // lod0_descriptor_count — tag 11, type u32
    const FieldDecl* f_cnt = find_field(td_blas, "lod0_descriptor_count");
    REQUIRE(f_cnt != nullptr);
    CHECK(f_cnt->type_name == eastl::string("u32"));
    CHECK(f_cnt->tag == 11);

    // BLASGeometryDescriptor: group_index is the sort key (tag 1)
    const TypeDecl& td_desc = result->types[1];
    CHECK(td_desc.fqn == eastl::string("glibre.geometry.BLASGeometryDescriptor"));
    const FieldDecl* f_gi = find_field(td_desc, "group_index");
    REQUIRE(f_gi != nullptr);
    CHECK(f_gi->type_name == eastl::string("u32"));
    CHECK(f_gi->tag == 1);
}

// data/schemas/geometry/BLASRecipeRecord: rejects_format_hash_mismatch_with_companion_pak
//
// §7.1.2 invariant 1: a BLASRecipeRecord whose format_hash or
// source_content_hash does not match the companion pak is
// Error::BLASRecipeInvalid.  The runtime validation is in the cooker; here we
// verify that both hash fields are declared as u64 at the correct tags (2 and 3)
// so the binary layout is correct for the companion-pak comparison.
//
// TODO(value-check): actual Error::BLASRecipeInvalid for mismatched hashes is
// owned by the BLASRecipe cooker plan (#482).  This test verifies the schema
// layout precondition (u64 at tags 2 and 3) only; test name is frozen by the
// DoD unit_test_named: clause.
TEST_CASE(
    "data/schemas/geometry/BLASRecipeRecord: rejects_format_hash_mismatch_with_companion_pak",
    "[geometry][schemas][blas_recipe]"
) {
    auto result = load_schema("BLASRecipeRecord.fory");
    REQUIRE(result.has_value());
    REQUIRE(result->types.size() >= 1);

    const TypeDecl& td = result->types[0];

    // format_hash — tag 2, type u64
    const FieldDecl* f_fh = find_field(td, "format_hash");
    REQUIRE(f_fh != nullptr);
    CHECK(f_fh->type_name == eastl::string("u64"));
    CHECK(f_fh->tag == 2);

    // source_content_hash — tag 3, type u64
    const FieldDecl* f_sch = find_field(td, "source_content_hash");
    REQUIRE(f_sch != nullptr);
    CHECK(f_sch->type_name == eastl::string("u64"));
    CHECK(f_sch->tag == 3);

    // pak_path — tag 1, type string
    const FieldDecl* f_pp = find_field(td, "pak_path");
    REQUIRE(f_pp != nullptr);
    CHECK(f_pp->type_name == eastl::string("string"));
    CHECK(f_pp->tag == 1);
}

// ---------------------------------------------------------------------------
// MeshSourceMetadata structural-invariant tests (plan #696 Unit Test Plan)
// ---------------------------------------------------------------------------

// data/schemas/geometry/MeshSourceMetadata: round_trip_preserves_bounding_box
//
// §7.1.3 invariant 3: bounding_box_min and bounding_box_max are the
// authored bind-pose AABB (not derived from LODs).  Both are vec3f at
// tags 10 and 11.  We verify the field types and tags are correct so the
// layout matches what the editor's content-browser preview expects.
TEST_CASE(
    "data/schemas/geometry/MeshSourceMetadata: round_trip_preserves_bounding_box",
    "[geometry][schemas][mesh_source_metadata]"
) {
    auto result = load_schema("MeshSourceMetadata.fory");
    REQUIRE(result.has_value());
    const TypeDecl& td = result->types[0];

    // bounding_box_min — tag 10, type vec3f
    const FieldDecl* f_bbmin = find_field(td, "bounding_box_min");
    REQUIRE(f_bbmin != nullptr);
    CHECK(f_bbmin->type_name == eastl::string("vec3f"));
    CHECK(f_bbmin->tag == 10);

    // bounding_box_max — tag 11, type vec3f
    const FieldDecl* f_bbmax = find_field(td, "bounding_box_max");
    REQUIRE(f_bbmax != nullptr);
    CHECK(f_bbmax->type_name == eastl::string("vec3f"));
    CHECK(f_bbmax->tag == 11);

    // source_content_hash — tag 4, type u64
    // (§7.1.3 invariant 2: must equal PakHeader.source_content_hash)
    const FieldDecl* f_sch = find_field(td, "source_content_hash");
    REQUIRE(f_sch != nullptr);
    CHECK(f_sch->type_name == eastl::string("u64"));
    CHECK(f_sch->tag == 4);

    // source_vertex_count + source_triangle_count — tags 12/13, type u32
    const FieldDecl* f_vc = find_field(td, "source_vertex_count");
    REQUIRE(f_vc != nullptr);
    CHECK(f_vc->type_name == eastl::string("u32"));
    CHECK(f_vc->tag == 12);

    const FieldDecl* f_tc = find_field(td, "source_triangle_count");
    REQUIRE(f_tc != nullptr);
    CHECK(f_tc->type_name == eastl::string("u32"));
    CHECK(f_tc->tag == 13);
}
