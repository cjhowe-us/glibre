#pragma once
// plugins/shader/src/reflection/slangc_reflection_record.hpp
//
// SlangcReflectionRecord — parsed representation of slangc's native reflection JSON.
//
// Responsibility: parse the JSON string that slangc emits alongside the
// compiled bytecode (-reflection-json flag) and represent the result as typed
// C++ structs.  This is the first stage of the two-stage ingest pipeline:
//
//   Stage 1 (this file): JSON text → SlangcReflectionRecord  (SlangcReflectionRecord.ingest)
//   Stage 2 (blob_builder.hpp/.cpp): SlangcReflectionRecord → ReflectionBlob (BlobBuilder.build)
//
// Authority: specs/shader/SPEC.md §4.4, §6.3.
// Plan: #514 — feat(shader): slangc reflection record ingester.
//
// ## slangc JSON reflection schema (stable subset consumed here)
//
// slangc emits a JSON object with the following top-level keys:
//
//   {
//     "entryPoints": [ { "name": string, "stage": string, ... }, ... ],
//     "parameters": [
//       {
//         "name": string,
//         "binding": { "kind": string, "space": int, "index": int, "count": int },
//         "stages": int,                        // bitmask
//         "type": { "kind": string, ... }
//       },
//       ...
//     ],
//     "vertexInputs": [ { "semantic": string, "semanticIndex": int, "location": int,
//                          "formatCode": int }, ... ],
//     "pushConstantRanges": [ { "offset": int, "size": int, "stages": int }, ... ],
//     "specializationConstants": [ { "name": string, "id": int, "size": int }, ... ],
//     "rtPayloadBytes": int
//   }
//
// Fields not listed above are silently ignored.  The parser is lenient on
// unknown keys and strict on required keys (returns ReflectionExtractionFailed
// when a required key is absent or has an unexpected type).
//
// ## Design constraints
//
// - No external JSON library (not in vcpkg manifest).  Hand-rolled minimal
//   parser that covers only the subset of JSON needed for the reflection record.
// - No exceptions; no RTTI.  All errors returned via glibre::Result.
// - No dynamic linkage to slangc; this module is dependency-free beyond
//   <cstdint>, <span>, <string_view>, <vector>, and glibre/error.hpp.
// - Deterministic: ingesting the same text twice yields structurally-equal
//   SlangcReflectionRecord values (§4.4 invariant 2).

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include <glibre/error.hpp>

namespace glibre::shader {

// ---------------------------------------------------------------------------
// Stage string → Stage enum mapping
// ---------------------------------------------------------------------------
// slangc emits stage as a string: "vertex", "pixel", "compute", "mesh",
// "amplification", "library".  We store a raw integer matching our Stage enum
// rather than importing shader.hpp here (avoiding a circular include).
// BlobBuilder translates the integer back to Stage before writing the
// ReflectionBlob entry_points field.
// ---------------------------------------------------------------------------

enum class RawStage : std::uint8_t {
    Vertex = 0,
    Pixel = 1,
    Compute = 2,
    Mesh = 3,
    Amplification = 4,
    Library = 5,
    Unknown = 0xFF,  // Unrecognised stage string → fail during ingest
};

// ---------------------------------------------------------------------------
// Binding kind string → BindingKind enum mapping
// ---------------------------------------------------------------------------
// slangc emits binding kind as a string matching these names.
// Unknown kind strings result in ReflectionExtractionFailed.
enum class RawBindingKind : std::uint8_t {
    ConstantBuffer = 0,
    SampledImage = 1,
    StorageImage = 2,
    Sampler = 3,
    StructuredBuffer = 4,
    RWStructuredBuffer = 5,
    AccelerationStructure = 6,
    PushConstant = 7,
    Unknown = 0xFF,
};

// ---------------------------------------------------------------------------
// Parsed entry-point record
// ---------------------------------------------------------------------------
struct RawEntryPoint {
    std::pmr::string name;
    RawStage stage{RawStage::Unknown};
};

// ---------------------------------------------------------------------------
// Parsed binding record
// ---------------------------------------------------------------------------
struct RawBinding {
    std::pmr::string name;
    RawBindingKind kind{RawBindingKind::Unknown};
    std::uint32_t register_space{0};
    std::uint32_t register_index{0};
    std::uint32_t array_size{1};
    std::uint8_t stage_mask{0};  // bitmask matching StageMask::bits
};

// ---------------------------------------------------------------------------
// Parsed vertex-input record
// ---------------------------------------------------------------------------
struct RawVertexInput {
    std::pmr::string semantic;
    std::uint32_t semantic_index{0};
    std::uint32_t location{0};
    std::uint32_t format_code{0};
};

// ---------------------------------------------------------------------------
// Parsed push-constant range
// ---------------------------------------------------------------------------
struct RawPushConstantRange {
    std::uint32_t offset{0};
    std::uint32_t size{0};
    std::uint8_t stage_mask{0};
};

// ---------------------------------------------------------------------------
// Parsed specialization-constant slot
// ---------------------------------------------------------------------------
struct RawSpecializationConstant {
    std::pmr::string name;
    std::uint32_t id{0};
    std::uint32_t size_bytes{0};
};

// ---------------------------------------------------------------------------
// Top-level parsed reflection record
// ---------------------------------------------------------------------------
struct SlangcReflectionRecord {
    std::pmr::vector<RawEntryPoint> entry_points;
    std::pmr::vector<RawBinding> bindings;
    std::pmr::vector<RawVertexInput> vertex_inputs;
    std::pmr::vector<RawPushConstantRange> push_constant_ranges;
    std::pmr::vector<RawSpecializationConstant> spec_constants;
    std::uint32_t rt_payload_bytes{0};
};

// ---------------------------------------------------------------------------
// ingest — parse slangc JSON reflection text into a SlangcReflectionRecord.
//
// json_text  — UTF-8 text of the reflection JSON emitted by slangc.  Need not
//              be NUL-terminated.
// mr         — PMR memory resource used for all string and vector allocations
//              inside the returned SlangcReflectionRecord.  Must outlive the
//              returned record.
//
// Returns shader::Error::ReflectionExtractionFailed if:
//   - The text is empty.
//   - The top-level value is not a JSON object.
//   - A required key is absent or carries the wrong JSON type.
//   - An entry point carries an unrecognised stage string.
//   - A binding carries an unrecognised kind string.
//   - Any integer field overflows uint32_t.
// ---------------------------------------------------------------------------
glibre::Result<SlangcReflectionRecord>
ingest_slangc_reflection(std::string_view json_text, std::pmr::memory_resource* mr) noexcept;

}  // namespace glibre::shader
