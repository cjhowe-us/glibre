// plugins/shader/src/reflection/blob_builder.cpp
//
// build_reflection_blob — project a SlangcReflectionRecord into a canonical ReflectionBlob.
//
// Authority: specs/shader/SPEC.md §4.4, §6.3.
// Plan: #514 — feat(shader): slangc reflection record ingester.
//
// ## Canonicalization algorithm
//
// 1. Translate each RawEntryPoint → EntryPoint (name preserved verbatim,
//    RawStage → Stage).  Deduplicate identical (name, stage) pairs; reject
//    same-name-different-stage conflicts.
//
// 2. Translate each RawBinding → BindingSlot (name preserved verbatim,
//    RawBindingKind → BindingKind, field copies).  The frequency field is
//    left at DescriptorFrequencyGroup::PerDraw (zero-value default); the
//    downstream frequency_tagger pass (SPEC §6.3) is responsible for
//    assigning the correct group.  At this stage we are only projecting
//    the raw record into the canonical struct.
//
// 3. Translate RawVertexInput[] → VertexIOLayout::elements[].
//
// 4. Copy RawPushConstantRange[] → PushConstantRange[].
//
// 5. Copy RawSpecializationConstant[] → SpecializationConstantSlot[].
//
// 6. MaterialParameterBlock is left empty (default-constructed with mr).
//    The SPEC §4.4 material parameter block is populated by the frequency
//    tagger pass once the ConstantBuffer binding named "MaterialParameters"
//    (or engine-conventional equivalent) is identified.  At ingest time we
//    do not know which binding is the material block; that classification
//    lives in the tagger pass (#515, already shipped).
//
// 7. rt_payload_bytes is forwarded verbatim.

#include "blob_builder.hpp"

#include <algorithm>
#include <cstdint>
#include <memory_resource>
#include <string_view>

#include <glibre/error.hpp>
#include <glibre/shader/shader.hpp>

#include "slangc_reflection_record.hpp"

namespace glibre::shader {

namespace {

// ---------------------------------------------------------------------------
// RawStage → Stage mapping
// ---------------------------------------------------------------------------

[[nodiscard]] glibre::Result<Stage> translate_stage(RawStage rs) noexcept {
    switch (rs) {
    case RawStage::Vertex:
        return Stage::Vertex;
    case RawStage::Pixel:
        return Stage::Pixel;
    case RawStage::Compute:
        return Stage::Compute;
    case RawStage::Mesh:
        return Stage::Mesh;
    case RawStage::Amplification:
        return Stage::Amplification;
    case RawStage::Library:
        return Stage::Library;
    case RawStage::Unknown:
        return std::unexpected(glibre::Error{shader::Error::ReflectionExtractionFailed});
    }
    return std::unexpected(glibre::Error{shader::Error::ReflectionExtractionFailed});
}

// ---------------------------------------------------------------------------
// RawBindingKind → BindingKind mapping
// ---------------------------------------------------------------------------

[[nodiscard]] glibre::Result<BindingKind> translate_binding_kind(RawBindingKind rk) noexcept {
    switch (rk) {
    case RawBindingKind::ConstantBuffer:
        return BindingKind::ConstantBuffer;
    case RawBindingKind::SampledImage:
        return BindingKind::SampledImage;
    case RawBindingKind::StorageImage:
        return BindingKind::StorageImage;
    case RawBindingKind::Sampler:
        return BindingKind::Sampler;
    case RawBindingKind::StructuredBuffer:
        return BindingKind::StructuredBuffer;
    case RawBindingKind::RWStructuredBuffer:
        return BindingKind::RWStructuredBuffer;
    case RawBindingKind::AccelerationStructure:
        return BindingKind::AccelerationStructure;
    case RawBindingKind::PushConstant:
        return BindingKind::PushConstant;
    case RawBindingKind::Unknown:
        return std::unexpected(glibre::Error{shader::Error::ReflectionExtractionFailed});
    }
    return std::unexpected(glibre::Error{shader::Error::ReflectionExtractionFailed});
}

// ---------------------------------------------------------------------------
// Canonicalize name: lower-case the first character to normalize the common
// slangc variation where some fields are TitleCase vs camelCase in different
// slangc versions.  The name is otherwise preserved verbatim; the goal is
// deterministic round-tripping (§4.4 invariant 2), not human readability.
//
// Canonicalization rule: trim leading and trailing ASCII whitespace, then
// apply no further transformation.  slangc binding names are Slang identifiers
// (letters, digits, underscore only); whitespace trimming is defense-in-depth
// for any JSON-encoder that pads strings.
// ---------------------------------------------------------------------------

[[nodiscard]] std::string_view canonical_name(std::string_view raw) noexcept {
    // Trim leading whitespace.
    std::size_t start = 0;
    while (start < raw.size() && (raw[start] == ' ' || raw[start] == '\t'))
        ++start;
    raw = raw.substr(start);
    // Trim trailing whitespace.
    while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t')) {
        raw = raw.substr(0, raw.size() - 1);
    }
    return raw;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// build_reflection_blob — public implementation
// ---------------------------------------------------------------------------

glibre::Result<ReflectionBlob>
build_reflection_blob(const SlangcReflectionRecord& rec, std::pmr::memory_resource* mr) noexcept {
    ReflectionBlob blob{
        .entry_points = std::pmr::vector<EntryPoint>{mr},
        .bindings = std::pmr::vector<BindingSlot>{mr},
        .vertex_io = VertexIOLayout{std::pmr::vector<VertexInputElement>{mr}},
        .push_constants = std::pmr::vector<PushConstantRange>{mr},
        .material_parameters =
            MaterialParameterBlock{std::pmr::string{mr}, 0, std::pmr::vector<BindingSlot>{mr}},
        .spec_constants = std::pmr::vector<SpecializationConstantSlot>{mr},
        .rt_payload_bytes = 0,
    };

    // -----------------------------------------------------------------------
    // Step 1: entry points
    // Deduplicate identical (name, stage) pairs; reject same-name different-stage.
    // -----------------------------------------------------------------------
    for (const RawEntryPoint& rep : rec.entry_points) {
        GLIBRE_TRY(stage, translate_stage(rep.stage));
        const std::string_view canon =
            canonical_name(std::string_view{rep.name.data(), rep.name.size()});
        // Check for existing entry with the same name.
        bool found_dup = false;
        for (const EntryPoint& existing : blob.entry_points) {
            if (std::string_view{existing.name.data(), existing.name.size()} == canon) {
                if (existing.stage != stage) {
                    // Same name, different stage → conflict.
                    return std::unexpected(
                        glibre::Error{shader::Error::ReflectionExtractionFailed}
                    );
                }
                // Exact duplicate — skip silently.
                found_dup = true;
                break;
            }
        }
        if (!found_dup) {
            blob.entry_points.push_back(
                EntryPoint{
                    std::pmr::string{canon.data(), canon.size(), mr},
                    stage,
                }
            );
        }
    }

    // -----------------------------------------------------------------------
    // Step 2: bindings → BindingSlot[]
    // frequency is left at PerDraw (default) — frequency_tagger classifies.
    // -----------------------------------------------------------------------
    for (const RawBinding& rb : rec.bindings) {
        GLIBRE_TRY(kind, translate_binding_kind(rb.kind));
        const std::string_view canon =
            canonical_name(std::string_view{rb.name.data(), rb.name.size()});
        blob.bindings.push_back(
            BindingSlot{
                .kind = kind,
                .register_space = rb.register_space,
                .register_index = rb.register_index,
                .array_size = rb.array_size,
                .stages = StageMask{rb.stage_mask},
                .frequency = DescriptorFrequencyGroup::PerDraw,  // tagger will classify
                .name = std::pmr::string{canon.data(), canon.size(), mr},
            }
        );
    }

    // -----------------------------------------------------------------------
    // Step 3: vertex inputs → VertexIOLayout
    // -----------------------------------------------------------------------
    for (const RawVertexInput& rvi : rec.vertex_inputs) {
        const std::string_view canon =
            canonical_name(std::string_view{rvi.semantic.data(), rvi.semantic.size()});
        blob.vertex_io.elements.push_back(
            VertexInputElement{
                std::pmr::string{canon.data(), canon.size(), mr},
                rvi.semantic_index,
                rvi.location,
                rvi.format_code,
            }
        );
    }

    // -----------------------------------------------------------------------
    // Step 4: push-constant ranges
    // -----------------------------------------------------------------------
    for (const RawPushConstantRange& rpc : rec.push_constant_ranges) {
        blob.push_constants.push_back(
            PushConstantRange{
                .offset = rpc.offset,
                .size = rpc.size,
                .stages = StageMask{rpc.stage_mask},
            }
        );
    }

    // -----------------------------------------------------------------------
    // Step 5: specialization-constant slots
    // -----------------------------------------------------------------------
    for (const RawSpecializationConstant& rsc : rec.spec_constants) {
        const std::string_view canon =
            canonical_name(std::string_view{rsc.name.data(), rsc.name.size()});
        blob.spec_constants.push_back(
            SpecializationConstantSlot{
                std::pmr::string{canon.data(), canon.size(), mr},
                rsc.id,
                rsc.size_bytes,
            }
        );
    }

    // -----------------------------------------------------------------------
    // Step 6: rt_payload_bytes
    // -----------------------------------------------------------------------
    blob.rt_payload_bytes = rec.rt_payload_bytes;

    return blob;
}

}  // namespace glibre::shader
