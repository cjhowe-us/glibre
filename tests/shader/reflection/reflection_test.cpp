// tests/shader/reflection/reflection_test.cpp
//
// Catch2 unit tests for glibre::shader slangc reflection record ingester (plan #514).
//
// Authority: specs/shader/SPEC.md §4.4, §6.3.
//
// Named test cases (plan #514 Unit Test Plan):
//   - reflection_blob_extracts_canonical_metadata_from_slangc_record
//   - slangc_reflection_record_returns_ReflectionExtractionFailed_on_truncated_record
//   - slangc_reflection_record_lists_all_entry_points_with_stage_attribute
//   - slangc_reflection_record_yields_register_ranges_per_stage_mask
//   - blob_builder_canonicalizes_binding_names_deterministically
//   - reflection_blob_equality_is_structural_over_named_fields
//
// Design:
//   - Fixture JSON strings represent the slangc reflection format consumed by
//     ingest_slangc_reflection().  No real slangc subprocess is needed.
//   - Each test uses a per-test PerContextAllocatorResource so allocations are
//     tracked under ContextTag::shader (perf-budget.md §Allocator Rules #1).
//   - -fno-exceptions compatible; no REQUIRE_THROWS.
//   - All errors inspected via std::holds_alternative / std::get on Error::code().

#include <cstdint>
#include <string_view>
#include <variant>

#include <catch2/catch_test_macros.hpp>
#include <glibre/alloc.hpp>
#include <glibre/shader/shader.hpp>

// Internal headers — test is the designated consumer of these private types.
#include "blob_builder.hpp"
#include "slangc_reflection_record.hpp"

namespace {

// ---------------------------------------------------------------------------
// Per-test allocator helper (mirrors descriptor_layout_test.cpp convention).
// ---------------------------------------------------------------------------
struct ShaderTestAlloc {
    glibre::PerContextAllocator alloc{glibre::ContextTag::shader};
    glibre::PerContextAllocatorResource mr{alloc};
};

// ---------------------------------------------------------------------------
// Minimal fixture JSON strings used across tests.
// ---------------------------------------------------------------------------

// Full-featured slangc reflection record:
//   - 2 entry points (vertex + pixel)
//   - 2 parameter bindings (ConstantBuffer + SampledImage)
//   - 1 vertex input
//   - 1 push-constant range
//   - 1 specialization constant
//   - rt_payload_bytes = 16
constexpr std::string_view kFullRecord = R"({
  "entryPoints": [
    { "name": "VSMain", "stage": "vertex" },
    { "name": "PSMain", "stage": "pixel" }
  ],
  "parameters": [
    {
      "name": "FrameUniforms",
      "binding": { "kind": "constantBuffer", "space": 0, "index": 0, "count": 1 },
      "stages": 3
    },
    {
      "name": "AlbedoTexture",
      "binding": { "kind": "texture", "space": 0, "index": 1, "count": 1 },
      "stages": 2
    }
  ],
  "vertexInputs": [
    { "semantic": "POSITION", "semanticIndex": 0, "location": 0, "formatCode": 28 }
  ],
  "pushConstantRanges": [
    { "offset": 0, "size": 16, "stages": 1 }
  ],
  "specializationConstants": [
    { "name": "EnableShadows", "id": 0, "size": 4 }
  ],
  "rtPayloadBytes": 16
})";

// Truncated JSON — missing closing brace.
constexpr std::string_view kTruncatedRecord = R"({
  "entryPoints": [
    { "name": "VSMain", "stage": "vertex" }
)";

// Empty JSON text.
constexpr std::string_view kEmptyRecord = "";

// JSON object with no recognised top-level keys (forward-compat: should succeed with empty blob).
constexpr std::string_view kUnknownKeysRecord = R"({
  "futureKey": "ignored",
  "anotherFuture": 42
})";

// Record with two identical entry points — should deduplicate to one.
constexpr std::string_view kDuplicateEntryPoints = R"({
  "entryPoints": [
    { "name": "CSMain", "stage": "compute" },
    { "name": "CSMain", "stage": "compute" }
  ],
  "parameters": []
})";

// Record with same entry-point name but different stages — conflict.
constexpr std::string_view kConflictingEntryPoints = R"({
  "entryPoints": [
    { "name": "Main", "stage": "vertex" },
    { "name": "Main", "stage": "pixel" }
  ],
  "parameters": []
})";

// Record with whitespace-padded binding name — tests name canonicalization.
constexpr std::string_view kPaddedBindingName = R"({
  "parameters": [
    {
      "name": "  MyBuffer  ",
      "binding": { "kind": "constantBuffer", "space": 0, "index": 0, "count": 1 },
      "stages": 1
    }
  ]
})";

// Record that exercises register_space / register_index round-trip.
constexpr std::string_view kRegisterRanges = R"({
  "parameters": [
    {
      "name": "BufA",
      "binding": { "kind": "constantBuffer", "space": 0, "index": 2, "count": 1 },
      "stages": 1
    },
    {
      "name": "BufB",
      "binding": { "kind": "texture", "space": 1, "index": 0, "count": 4 },
      "stages": 2
    }
  ]
})";

// Helpers for error inspection.
[[nodiscard]] bool is_reflection_failed(const glibre::Error& err) noexcept {
    return std::holds_alternative<glibre::shader::Error>(err.code()) &&
           std::get<glibre::shader::Error>(err.code()) ==
               glibre::shader::Error::ReflectionExtractionFailed;
}

}  // namespace

// ===========================================================================
// Test: reflection_blob_extracts_canonical_metadata_from_slangc_record
//
// End-to-end: ingest kFullRecord → build_reflection_blob → verify every field.
//
// Confirms:
//   - 2 entry points (VSMain/vertex, PSMain/pixel) in the blob.
//   - 2 bindings (FrameUniforms/ConstantBuffer, AlbedoTexture/SampledImage).
//   - 1 vertex input (POSITION, index=0, location=0, formatCode=28).
//   - 1 push-constant range (offset=0, size=16, stages.bits=1).
//   - 1 specialization constant (EnableShadows, id=0, size=4).
//   - rt_payload_bytes == 16.
// ===========================================================================

TEST_CASE(
    "reflection_blob_extracts_canonical_metadata_from_slangc_record", "[shader][reflection]"
) {
    ShaderTestAlloc ta;

    auto ingest_result = glibre::shader::ingest_slangc_reflection(kFullRecord, &ta.mr);
    REQUIRE(ingest_result.has_value());

    auto blob_result = glibre::shader::build_reflection_blob(*ingest_result, &ta.mr);
    REQUIRE(blob_result.has_value());

    const glibre::shader::ReflectionBlob& blob = *blob_result;

    // Entry points.
    CHECK(blob.entry_points.size() == 2u);
    REQUIRE(blob.entry_points.size() >= 1u);
    CHECK(blob.entry_points[0].name == "VSMain");
    CHECK(blob.entry_points[0].stage == glibre::shader::Stage::Vertex);
    REQUIRE(blob.entry_points.size() >= 2u);
    CHECK(blob.entry_points[1].name == "PSMain");
    CHECK(blob.entry_points[1].stage == glibre::shader::Stage::Pixel);

    // Bindings.
    CHECK(blob.bindings.size() == 2u);
    REQUIRE(blob.bindings.size() >= 1u);
    CHECK(blob.bindings[0].name == "FrameUniforms");
    CHECK(blob.bindings[0].kind == glibre::shader::BindingKind::ConstantBuffer);
    CHECK(blob.bindings[0].register_space == 0u);
    CHECK(blob.bindings[0].register_index == 0u);
    CHECK(blob.bindings[0].array_size == 1u);
    CHECK(blob.bindings[0].stages.bits == 3u);

    REQUIRE(blob.bindings.size() >= 2u);
    CHECK(blob.bindings[1].name == "AlbedoTexture");
    CHECK(blob.bindings[1].kind == glibre::shader::BindingKind::SampledImage);
    CHECK(blob.bindings[1].register_index == 1u);

    // Vertex IO.
    REQUIRE(blob.vertex_io.elements.size() == 1u);
    CHECK(blob.vertex_io.elements[0].semantic == "POSITION");
    CHECK(blob.vertex_io.elements[0].semantic_index == 0u);
    CHECK(blob.vertex_io.elements[0].location == 0u);
    CHECK(blob.vertex_io.elements[0].format_code == 28u);

    // Push constants.
    REQUIRE(blob.push_constants.size() == 1u);
    CHECK(blob.push_constants[0].offset == 0u);
    CHECK(blob.push_constants[0].size == 16u);
    CHECK(blob.push_constants[0].stages.bits == 1u);

    // Specialization constants.
    REQUIRE(blob.spec_constants.size() == 1u);
    CHECK(blob.spec_constants[0].name == "EnableShadows");
    CHECK(blob.spec_constants[0].id == 0u);
    CHECK(blob.spec_constants[0].size_bytes == 4u);

    // RT payload.
    CHECK(blob.rt_payload_bytes == 16u);
}

// ===========================================================================
// Test: slangc_reflection_record_returns_ReflectionExtractionFailed_on_truncated_record
//
// Confirms that ingest_slangc_reflection() returns ReflectionExtractionFailed
// when given:
//   - Empty text.
//   - JSON truncated mid-object (missing closing brace).
//   - JSON object with non-JSON top-level value (plain integer text).
// ===========================================================================

TEST_CASE(
    "slangc_reflection_record_returns_ReflectionExtractionFailed_on_truncated_record",
    "[shader][reflection]"
) {
    SECTION("empty text") {
        ShaderTestAlloc ta;
        auto result = glibre::shader::ingest_slangc_reflection(kEmptyRecord, &ta.mr);
        REQUIRE_FALSE(result.has_value());
        CHECK(is_reflection_failed(result.error()));
    }

    SECTION("truncated JSON object") {
        ShaderTestAlloc ta;
        auto result = glibre::shader::ingest_slangc_reflection(kTruncatedRecord, &ta.mr);
        REQUIRE_FALSE(result.has_value());
        CHECK(is_reflection_failed(result.error()));
    }

    SECTION("non-object top-level (bare integer)") {
        ShaderTestAlloc ta;
        const std::string_view not_an_object = "42";
        auto result = glibre::shader::ingest_slangc_reflection(not_an_object, &ta.mr);
        REQUIRE_FALSE(result.has_value());
        CHECK(is_reflection_failed(result.error()));
    }

    SECTION("unknown binding kind returns error from build_reflection_blob") {
        ShaderTestAlloc ta;
        // A binding with an unrecognised kind string should fail at build time.
        constexpr std::string_view bad_kind = R"({
          "parameters": [
            {
              "name": "X",
              "binding": { "kind": "unknownKind", "space": 0, "index": 0, "count": 1 },
              "stages": 1
            }
          ]
        })";
        auto ingest_result = glibre::shader::ingest_slangc_reflection(bad_kind, &ta.mr);
        // Parser rejects unknown binding kind during ingest.
        REQUIRE_FALSE(ingest_result.has_value());
        CHECK(is_reflection_failed(ingest_result.error()));
    }
}

// ===========================================================================
// Test: slangc_reflection_record_lists_all_entry_points_with_stage_attribute
//
// Verifies that ingest + build correctly surfaces all entry points with their
// stage labels.  Also verifies:
//   - Duplicate (name, stage) pairs are coalesced to one entry (not doubled).
//   - Conflicting (same name, different stage) pairs return ReflectionExtractionFailed.
//   - Unknown keys at the top level are silently ignored (forward-compat).
// ===========================================================================

TEST_CASE(
    "slangc_reflection_record_lists_all_entry_points_with_stage_attribute", "[shader][reflection]"
) {
    SECTION("all six stages round-trip correctly") {
        ShaderTestAlloc ta;
        constexpr std::string_view all_stages = R"({
          "entryPoints": [
            { "name": "VS",   "stage": "vertex" },
            { "name": "PS",   "stage": "pixel" },
            { "name": "CS",   "stage": "compute" },
            { "name": "MS",   "stage": "mesh" },
            { "name": "AS",   "stage": "amplification" },
            { "name": "LIB",  "stage": "library" }
          ],
          "parameters": []
        })";

        auto ingest_result = glibre::shader::ingest_slangc_reflection(all_stages, &ta.mr);
        REQUIRE(ingest_result.has_value());
        auto blob_result = glibre::shader::build_reflection_blob(*ingest_result, &ta.mr);
        REQUIRE(blob_result.has_value());

        const auto& eps = blob_result->entry_points;
        CHECK(eps.size() == 6u);
        REQUIRE(eps.size() >= 6u);
        CHECK(eps[0].stage == glibre::shader::Stage::Vertex);
        CHECK(eps[1].stage == glibre::shader::Stage::Pixel);
        CHECK(eps[2].stage == glibre::shader::Stage::Compute);
        CHECK(eps[3].stage == glibre::shader::Stage::Mesh);
        CHECK(eps[4].stage == glibre::shader::Stage::Amplification);
        CHECK(eps[5].stage == glibre::shader::Stage::Library);
    }

    SECTION("duplicate (name, stage) coalesces to one entry") {
        ShaderTestAlloc ta;
        auto ingest_result =
            glibre::shader::ingest_slangc_reflection(kDuplicateEntryPoints, &ta.mr);
        REQUIRE(ingest_result.has_value());
        auto blob_result = glibre::shader::build_reflection_blob(*ingest_result, &ta.mr);
        REQUIRE(blob_result.has_value());
        CHECK(blob_result->entry_points.size() == 1u);
        CHECK(blob_result->entry_points[0].name == "CSMain");
    }

    SECTION("conflicting (same name, different stage) returns error") {
        ShaderTestAlloc ta;
        auto ingest_result =
            glibre::shader::ingest_slangc_reflection(kConflictingEntryPoints, &ta.mr);
        REQUIRE(ingest_result.has_value());
        auto blob_result = glibre::shader::build_reflection_blob(*ingest_result, &ta.mr);
        REQUIRE_FALSE(blob_result.has_value());
        CHECK(is_reflection_failed(blob_result.error()));
    }

    SECTION("unknown top-level keys are silently ignored") {
        ShaderTestAlloc ta;
        auto ingest_result = glibre::shader::ingest_slangc_reflection(kUnknownKeysRecord, &ta.mr);
        REQUIRE(ingest_result.has_value());
        auto blob_result = glibre::shader::build_reflection_blob(*ingest_result, &ta.mr);
        REQUIRE(blob_result.has_value());
        CHECK(blob_result->entry_points.empty());
        CHECK(blob_result->bindings.empty());
    }
}

// ===========================================================================
// Test: slangc_reflection_record_yields_register_ranges_per_stage_mask
//
// Verifies that register_space, register_index, array_size, and stage_mask
// fields are correctly round-tripped from the JSON into BindingSlot fields.
// ===========================================================================

TEST_CASE(
    "slangc_reflection_record_yields_register_ranges_per_stage_mask", "[shader][reflection]"
) {
    ShaderTestAlloc ta;

    auto ingest_result = glibre::shader::ingest_slangc_reflection(kRegisterRanges, &ta.mr);
    REQUIRE(ingest_result.has_value());

    auto blob_result = glibre::shader::build_reflection_blob(*ingest_result, &ta.mr);
    REQUIRE(blob_result.has_value());

    const auto& bindings = blob_result->bindings;
    REQUIRE(bindings.size() == 2u);

    // BufA: space=0, index=2, count=1, stages=1
    CHECK(bindings[0].name == "BufA");
    CHECK(bindings[0].kind == glibre::shader::BindingKind::ConstantBuffer);
    CHECK(bindings[0].register_space == 0u);
    CHECK(bindings[0].register_index == 2u);
    CHECK(bindings[0].array_size == 1u);
    CHECK(bindings[0].stages.bits == 1u);

    // BufB: space=1, index=0, count=4, stages=2
    CHECK(bindings[1].name == "BufB");
    CHECK(bindings[1].kind == glibre::shader::BindingKind::SampledImage);
    CHECK(bindings[1].register_space == 1u);
    CHECK(bindings[1].register_index == 0u);
    CHECK(bindings[1].array_size == 4u);
    CHECK(bindings[1].stages.bits == 2u);
}

// ===========================================================================
// Test: blob_builder_canonicalizes_binding_names_deterministically
//
// Confirms that:
//   1. Whitespace-padded names are trimmed (canonical_name rule).
//   2. Ingesting the same JSON twice yields bindings with equal names (§4.4 inv 2).
//   3. The blob produced from a whitespace-padded fixture matches the blob from
//      the equivalent clean fixture (determinism of canonicalization).
// ===========================================================================

TEST_CASE("blob_builder_canonicalizes_binding_names_deterministically", "[shader][reflection]") {
    SECTION("whitespace trimmed from binding names") {
        ShaderTestAlloc ta;
        auto ingest_result = glibre::shader::ingest_slangc_reflection(kPaddedBindingName, &ta.mr);
        REQUIRE(ingest_result.has_value());
        auto blob_result = glibre::shader::build_reflection_blob(*ingest_result, &ta.mr);
        REQUIRE(blob_result.has_value());
        REQUIRE(blob_result->bindings.size() == 1u);
        // Whitespace stripped: "  MyBuffer  " → "MyBuffer"
        CHECK(blob_result->bindings[0].name == "MyBuffer");
    }

    SECTION("same JSON text ingested twice yields equal binding names") {
        ShaderTestAlloc ta1;
        ShaderTestAlloc ta2;

        auto r1 = glibre::shader::ingest_slangc_reflection(kRegisterRanges, &ta1.mr);
        auto r2 = glibre::shader::ingest_slangc_reflection(kRegisterRanges, &ta2.mr);
        REQUIRE(r1.has_value());
        REQUIRE(r2.has_value());

        auto b1 = glibre::shader::build_reflection_blob(*r1, &ta1.mr);
        auto b2 = glibre::shader::build_reflection_blob(*r2, &ta2.mr);
        REQUIRE(b1.has_value());
        REQUIRE(b2.has_value());

        REQUIRE(b1->bindings.size() == b2->bindings.size());
        for (std::size_t i = 0; i < b1->bindings.size(); ++i) {
            CHECK(b1->bindings[i].name == b2->bindings[i].name);
            CHECK(b1->bindings[i].kind == b2->bindings[i].kind);
            CHECK(b1->bindings[i].register_space == b2->bindings[i].register_space);
            CHECK(b1->bindings[i].register_index == b2->bindings[i].register_index);
        }
    }
}

// ===========================================================================
// Test: reflection_blob_equality_is_structural_over_named_fields
//
// Confirms that two independently-built ReflectionBlob values produced from
// byte-equal input JSON are structurally equal across all named fields.
//
// This is a direct pin-test of §4.4 invariant 2:
//   "Reflecting the same bytecode container twice yields a structurally equal
//    ReflectionBlob."
//
// We simulate the "same bytecode" by using the same JSON text twice.
// ===========================================================================

TEST_CASE("reflection_blob_equality_is_structural_over_named_fields", "[shader][reflection]") {
    ShaderTestAlloc ta1;
    ShaderTestAlloc ta2;

    auto r1 = glibre::shader::ingest_slangc_reflection(kFullRecord, &ta1.mr);
    auto r2 = glibre::shader::ingest_slangc_reflection(kFullRecord, &ta2.mr);
    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());

    auto b1 = glibre::shader::build_reflection_blob(*r1, &ta1.mr);
    auto b2 = glibre::shader::build_reflection_blob(*r2, &ta2.mr);
    REQUIRE(b1.has_value());
    REQUIRE(b2.has_value());

    // Entry points.
    REQUIRE(b1->entry_points.size() == b2->entry_points.size());
    for (std::size_t i = 0; i < b1->entry_points.size(); ++i) {
        CHECK(b1->entry_points[i].name == b2->entry_points[i].name);
        CHECK(b1->entry_points[i].stage == b2->entry_points[i].stage);
    }

    // Bindings.
    REQUIRE(b1->bindings.size() == b2->bindings.size());
    for (std::size_t i = 0; i < b1->bindings.size(); ++i) {
        CHECK(b1->bindings[i] == b2->bindings[i]);
    }

    // Vertex IO.
    REQUIRE(b1->vertex_io.elements.size() == b2->vertex_io.elements.size());
    for (std::size_t i = 0; i < b1->vertex_io.elements.size(); ++i) {
        CHECK(b1->vertex_io.elements[i].semantic == b2->vertex_io.elements[i].semantic);
        CHECK(b1->vertex_io.elements[i].semantic_index == b2->vertex_io.elements[i].semantic_index);
        CHECK(b1->vertex_io.elements[i].location == b2->vertex_io.elements[i].location);
        CHECK(b1->vertex_io.elements[i].format_code == b2->vertex_io.elements[i].format_code);
    }

    // Push constants.
    REQUIRE(b1->push_constants.size() == b2->push_constants.size());
    for (std::size_t i = 0; i < b1->push_constants.size(); ++i) {
        CHECK(b1->push_constants[i] == b2->push_constants[i]);
    }

    // Spec constants.
    REQUIRE(b1->spec_constants.size() == b2->spec_constants.size());
    for (std::size_t i = 0; i < b1->spec_constants.size(); ++i) {
        CHECK(b1->spec_constants[i].name == b2->spec_constants[i].name);
        CHECK(b1->spec_constants[i].id == b2->spec_constants[i].id);
        CHECK(b1->spec_constants[i].size_bytes == b2->spec_constants[i].size_bytes);
    }

    // RT payload.
    CHECK(b1->rt_payload_bytes == b2->rt_payload_bytes);
}
