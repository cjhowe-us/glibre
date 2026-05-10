// plugins/shader/src/descriptor/descriptor_layout.cpp
//
// DescriptorLayout::derive implementation (§4.5 of specs/shader/SPEC.md).
//
// Responsibility: translate a fully-tagged ReflectionBlob into the
// backend-neutral RootSignatureSchema, threading the supplied PMR memory
// resource through every allocation so all output bytes are tracked under
// ContextTag::shader (perf-budget.md §Allocator Rules #1).
//
// Plan: #1087 — iterate-shader-descriptor-allocator-threading.
// Authority: specs/shader/descriptor-layout-design.md §3.2.
//
// ## Algorithm (8-pass pipeline)
//
//   Pass 1 — pre-condition check: verify every BindingSlot carries a known
//             DescriptorFrequencyGroup (DescriptorFrequencyMissing on fail).
//   Pass 2 — partition: bucket each slot into the matching DescriptorTable.
//   Pass 3 — static-sampler extraction: Sampler-kind slots marked immutable are
//             moved to the side list (skipped in MVP; all Samplers stay in table).
//   Pass 4 — push-constant lowering: copy reflection.push_constants into the
//             schema side list; no Metal synthetic slot at MVP (deferred).
//   Pass 5 — vertex-IO forwarding: pass through (vertex hash deferred to
//             sub-epic #69 amendment).
//   Pass 6 — per-table sort by (register_space, register_index, stage_mask).
//   Pass 7 — per-table cap check: ≤ 31 slots per group (Metal baseline).
//   Pass 8 — cross-table completeness: Σ len(per_*.slots) + len(static_samplers)
//             == len(reflection.bindings); DescriptorFrequencyAmbiguous on mismatch.
//
// Passes 3, 5 are MVP stubs (documented inline).  Full semantics are deferred to
// the sub-epic #69 amendment plan that adds BindingOverflow / SamplerLimitExceeded
// / IncompatibleVertexLayout / PushConstantTooLarge error arms.

#include <algorithm>
#include <memory_resource>

#include <glibre/shader/shader.hpp>

namespace glibre::shader {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

// copy_slot_with_mr — copy a BindingSlot from the ReflectionBlob, allocating
// the name string under mr.
//
// std::pmr::vector<BindingSlot>::push_back copies the struct but
// copy-constructs BindingSlot::name using the *source* string's allocator
// (the blob's resource), not the vector's resource.  To ensure the name bytes
// land under the target mr, construct the string explicitly from the source
// data using the target mr.
BindingSlot copy_slot_with_mr(const BindingSlot& src, std::pmr::memory_resource* mr) {
    BindingSlot dst;
    dst.kind           = src.kind;
    dst.register_space = src.register_space;
    dst.register_index = src.register_index;
    dst.array_size     = src.array_size;
    dst.stages         = src.stages;
    dst.frequency      = src.frequency;
    dst.name           = std::pmr::string{src.name.data(), src.name.size(), mr};
    return dst;
}

// slot_less — ordering key for Pass 6 sort: (register_space, register_index, stage_mask bits).
struct SlotLess {
    bool operator()(const BindingSlot& a, const BindingSlot& b) const noexcept {
        if (a.register_space != b.register_space) return a.register_space < b.register_space;
        if (a.register_index != b.register_index) return a.register_index < b.register_index;
        return a.stages.bits < b.stages.bits;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// DescriptorLayout::derive
// ---------------------------------------------------------------------------

glibre::Result<DescriptorLayout>
DescriptorLayout::derive(
    const ReflectionBlob& blob, CompileTarget /*target*/, std::pmr::memory_resource* mr
) noexcept {
    // Construct layout with all vectors wired to mr.
    DescriptorLayout layout{mr};
    RootSignatureSchema& schema = layout.schema_;

    // ------------------------------------------------------------------
    // Pass 1 — pre-condition: every binding must carry a recognised
    // DescriptorFrequencyGroup.  Unknown values (future enum extensions
    // not in this build) fail with DescriptorFrequencyMissing.
    // ------------------------------------------------------------------
    for (const BindingSlot& slot : blob.bindings) {
        switch (slot.frequency) {
            case DescriptorFrequencyGroup::PerFrame:
            case DescriptorFrequencyGroup::PerPass:
            case DescriptorFrequencyGroup::PerMaterial:
            case DescriptorFrequencyGroup::PerDraw:
                break;
            default:
                return std::unexpected(glibre::Error{Error::DescriptorFrequencyMissing});
        }
    }

    // ------------------------------------------------------------------
    // Pass 2 — partition each binding into the matching DescriptorTable.
    // All BindingSlot copies allocate name under mr (see copy_slot_with_mr).
    // ------------------------------------------------------------------
    for (const BindingSlot& slot : blob.bindings) {
        switch (slot.frequency) {
            case DescriptorFrequencyGroup::PerFrame:
                schema.per_frame.slots.push_back(copy_slot_with_mr(slot, mr));
                break;
            case DescriptorFrequencyGroup::PerPass:
                schema.per_pass.slots.push_back(copy_slot_with_mr(slot, mr));
                break;
            case DescriptorFrequencyGroup::PerMaterial:
                schema.per_material.slots.push_back(copy_slot_with_mr(slot, mr));
                break;
            case DescriptorFrequencyGroup::PerDraw:
                schema.per_draw.slots.push_back(copy_slot_with_mr(slot, mr));
                break;
            default:
                // Unreachable: Pass 1 already validated all groups.
                return std::unexpected(glibre::Error{Error::DescriptorFrequencyMissing});
        }
    }

    // ------------------------------------------------------------------
    // Pass 3 — static-sampler extraction (MVP stub).
    // Full semantics deferred to sub-epic #69 (SamplerLimitExceeded arm).
    // All Sampler-kind slots remain in their frequency table for now.
    // static_samplers stays empty.
    // ------------------------------------------------------------------

    // ------------------------------------------------------------------
    // Pass 4 — push-constant side list: forward reflection.push_constants.
    // No synthetic PerDraw slot at MVP (deferred to sub-epic #69).
    // PushConstantRange is POD (no string members) so a direct copy suffices.
    // ------------------------------------------------------------------
    schema.push_constants.reserve(blob.push_constants.size());
    for (const PushConstantRange& pcr : blob.push_constants) {
        schema.push_constants.push_back(pcr);
    }

    // ------------------------------------------------------------------
    // Pass 5 — vertex-IO forwarding (MVP stub).
    // vertex_layout_hash deferred to sub-epic #69 (IncompatibleVertexLayout arm).
    // RootSignatureSchema carries no VertexIOLayout field at this revision;
    // the vertex_io information lives in the ReflectionBlob and is forwarded
    // to the Metal backend directly via ShaderArtifact (future plan).
    // ------------------------------------------------------------------

    // ------------------------------------------------------------------
    // Pass 6 — per-table sort by (register_space, register_index, stage_mask).
    // §4.5 inv. 3 requires slots be ordered for deterministic ordinal assignment.
    // ------------------------------------------------------------------
    SlotLess less{};
    std::ranges::sort(schema.per_frame.slots,    less);
    std::ranges::sort(schema.per_pass.slots,     less);
    std::ranges::sort(schema.per_material.slots, less);
    std::ranges::sort(schema.per_draw.slots,     less);

    // ------------------------------------------------------------------
    // Pass 7 — per-table cap check: Metal 4 baseline ≤ 31 slots per group.
    // Full BindingOverflow error arm deferred to sub-epic #69 amendment.
    // At MVP return DescriptorFrequencyAmbiguous as the closest existing arm.
    // ------------------------------------------------------------------
    constexpr std::size_t kMaxSlotsPerGroup = 31;
    if (schema.per_frame.slots.size()    > kMaxSlotsPerGroup ||
        schema.per_pass.slots.size()     > kMaxSlotsPerGroup ||
        schema.per_material.slots.size() > kMaxSlotsPerGroup ||
        schema.per_draw.slots.size()     > kMaxSlotsPerGroup)
    {
        return std::unexpected(glibre::Error{Error::DescriptorFrequencyAmbiguous});
    }

    // ------------------------------------------------------------------
    // Pass 8 — cross-table completeness check.
    // Σ len(per_*.slots) + len(static_samplers) must equal len(blob.bindings).
    // Any mismatch indicates a bug in passes 2–3 (double-count or silent drop).
    // ------------------------------------------------------------------
    const std::size_t total_placed =
        schema.per_frame.slots.size() +
        schema.per_pass.slots.size() +
        schema.per_material.slots.size() +
        schema.per_draw.slots.size() +
        schema.static_samplers.size();

    if (total_placed != blob.bindings.size()) {
        return std::unexpected(glibre::Error{Error::DescriptorFrequencyAmbiguous});
    }

    return layout;
}

// ---------------------------------------------------------------------------
// DescriptorLayout accessors
// ---------------------------------------------------------------------------

const DescriptorTable& DescriptorLayout::table(DescriptorFrequencyGroup g) const noexcept {
    switch (g) {
        case DescriptorFrequencyGroup::PerFrame:    return schema_.per_frame;
        case DescriptorFrequencyGroup::PerPass:     return schema_.per_pass;
        case DescriptorFrequencyGroup::PerMaterial: return schema_.per_material;
        case DescriptorFrequencyGroup::PerDraw:     return schema_.per_draw;
    }
    // Unreachable on well-formed inputs; all DescriptorFrequencyGroup values covered above.
    return schema_.per_frame;
}

const RootSignatureSchema& DescriptorLayout::schema() const noexcept { return schema_; }

}  // namespace glibre::shader
