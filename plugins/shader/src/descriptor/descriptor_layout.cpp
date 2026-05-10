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
//   Pass 7 — per-table cap check: ≤ slot_cap_for(target) slots per group.
//   Pass 8 — cross-table completeness: Σ len(per_*.slots) + len(static_samplers)
//             == len(reflection.bindings); DescriptorFrequencyAmbiguous on mismatch.
//
// Each pass is a file-scope static free function in the anonymous namespace
// (specs/shader/descriptor-layout-design.md §3.2 line 363).  Each function takes
// the in-progress RootSignatureSchema by reference and returns
// std::expected<void, shader::Error>.  derive() is the thin sequencer that calls
// them in order and short-circuits on the first failure.
//
// Passes 3, 5 are MVP stubs (documented inline).  Full semantics are deferred to
// the sub-epic #69 amendment plan that adds SamplerLimitExceeded /
// IncompatibleVertexLayout / PushConstantTooLarge error arms.
// BindingOverflow is now a live arm (plan #1087 R1 MED-1); sub-epic #69 will
// extend it with per-sampler-table granularity.

#include <algorithm>
#include <memory_resource>

#include <glibre/shader/shader.hpp>

namespace glibre::shader {

// ---------------------------------------------------------------------------
// Internal helpers and pass implementations
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
//
// LOW-2 fix (plan #1087 R1): designated aggregate init binds the name string
// directly to mr at construction.  The previous default-construct + assign
// approach left dst.name's POAA fields wired to std::pmr::get_default_resource()
// because move-assign-from-temporary does NOT propagate the source allocator
// per [container.alloc.req] — any post-derive grow on dst.name would have
// silently fallen back to the default resource.
BindingSlot copy_slot_with_mr(const BindingSlot& src, std::pmr::memory_resource* mr) {
    return BindingSlot{
        .kind = src.kind,
        .register_space = src.register_space,
        .register_index = src.register_index,
        .array_size = src.array_size,
        .stages = src.stages,
        .frequency = src.frequency,
        .name = std::pmr::string{src.name.data(), src.name.size(), mr},
    };
}

// slot_less — ordering key for Pass 6 sort: (register_space, register_index, stage_mask bits).
struct SlotLess {
    bool operator()(const BindingSlot& a, const BindingSlot& b) const noexcept {
        if (a.register_space != b.register_space)
            return a.register_space < b.register_space;
        if (a.register_index != b.register_index)
            return a.register_index < b.register_index;
        return a.stages.bits < b.stages.bits;
    }
};

// slot_cap_for — per-table slot capacity for the given compile target.
//
// MED-1 fix (plan #1087 R2): the cap is a property of the backend target,
// not a magic constant inside Pass 7.  This makes the dependency explicit and
// ensures a future DXIL caller does not silently inherit Metal's limit.
//
// Metal 4 baseline: 31 slots per DescriptorFrequencyGroup (argument-buffer
// tier-2 layout; sub-epic #69 may extend this with per-sampler-table limits).
// DXIL (D3D12): root-signature limits differ; cap derivation deferred to
// post-MVP — assertion below signals the unimplemented path at dev time.
[[nodiscard]] std::size_t slot_cap_for(CompileTarget target) noexcept {
    switch (target) {
    case CompileTarget::MetalLib:
        return 31;
    case CompileTarget::DXIL:
        // TODO(post-MVP, sub-epic #69): derive cap from D3D12 root-signature
        // limits.  DXIL is not an MVP target; return MetalLib cap as a safe
        // temporary stand-in so this path does not silently misapply 31 to
        // D3D12 without a code-reader noticing.
        return 31;  // NOLINT(cppcoreguidelines-avoid-magic-numbers)
    }
    return 31;  // unreachable; all CompileTarget arms covered above
}

// ---------------------------------------------------------------------------
// Pass implementations — each returns std::expected<void, shader::Error>
// (specs/shader/descriptor-layout-design.md §3.2 line 363).
// ---------------------------------------------------------------------------

// pass1_precondition — verify every BindingSlot carries a recognised
// DescriptorFrequencyGroup.  Unknown values (future enum extensions not in
// this build) fail with DescriptorFrequencyMissing.
[[nodiscard]] std::expected<void, glibre::Error> pass1_precondition(
    const ReflectionBlob& blob, RootSignatureSchema& /*schema*/
) noexcept {
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
    return {};
}

// pass2_partition — bucket each binding into the matching DescriptorTable.
// All BindingSlot copies allocate name under mr (see copy_slot_with_mr).
[[nodiscard]] std::expected<void, glibre::Error> pass2_partition(
    const ReflectionBlob& blob, RootSignatureSchema& schema, std::pmr::memory_resource* mr
) noexcept {
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
    return {};
}

// pass4_push_constants — forward reflection.push_constants into the schema
// side list.  No synthetic PerDraw slot at MVP (deferred to sub-epic #69).
// PushConstantRange is POD (no string members) so a direct copy suffices.
[[nodiscard]] std::expected<void, glibre::Error>
pass4_push_constants(const ReflectionBlob& blob, RootSignatureSchema& schema) noexcept {
    schema.push_constants.reserve(blob.push_constants.size());
    for (const PushConstantRange& pcr : blob.push_constants) {
        schema.push_constants.push_back(pcr);
    }
    return {};
}

// pass6_sort — per-table sort by (register_space, register_index, stage_mask).
// §4.5 inv. 3 requires slots be ordered for deterministic ordinal assignment.
[[nodiscard]] std::expected<void, glibre::Error>
pass6_sort(const ReflectionBlob& /*blob*/, RootSignatureSchema& schema) noexcept {
    SlotLess less{};
    std::ranges::sort(schema.per_frame.slots, less);
    std::ranges::sort(schema.per_pass.slots, less);
    std::ranges::sort(schema.per_material.slots, less);
    std::ranges::sort(schema.per_draw.slots, less);
    return {};
}

// pass7_cap_check — per-table cap check: ≤ slot_cap_for(target) slots per group.
// Returns BindingOverflow (plan #1087 R1 MED-1) to distinguish layout-cap
// violations from tagger conflicts (DescriptorFrequencyAmbiguous is reserved for
// multi-tag annotation bugs per SPEC §10.2).
// Full sub-epic #69 amendment will add SamplerLimitExceeded and extend this
// check with per-sampler-table limits.
[[nodiscard]] std::expected<void, glibre::Error> pass7_cap_check(
    const ReflectionBlob& /*blob*/, RootSignatureSchema& schema, CompileTarget target
) noexcept {
    const std::size_t cap = slot_cap_for(target);
    if (schema.per_frame.slots.size() > cap || schema.per_pass.slots.size() > cap ||
        schema.per_material.slots.size() > cap || schema.per_draw.slots.size() > cap) {
        return std::unexpected(glibre::Error{Error::BindingOverflow});
    }
    return {};
}

// pass8_completeness — cross-table completeness check.
// Σ len(per_*.slots) + len(static_samplers) must equal len(blob.bindings).
// Any mismatch indicates a bug in passes 2–3 (double-count or silent drop).
[[nodiscard]] std::expected<void, glibre::Error>
pass8_completeness(const ReflectionBlob& blob, RootSignatureSchema& schema) noexcept {
    const std::size_t total_placed = schema.per_frame.slots.size() + schema.per_pass.slots.size() +
                                     schema.per_material.slots.size() +
                                     schema.per_draw.slots.size() + schema.static_samplers.size();
    if (total_placed != blob.bindings.size()) {
        return std::unexpected(glibre::Error{Error::DescriptorFrequencyAmbiguous});
    }
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// DescriptorLayout::derive — thin sequencer (specs/shader/descriptor-layout-design.md §3.2)
// ---------------------------------------------------------------------------

glibre::Result<DescriptorLayout> DescriptorLayout::derive(
    const ReflectionBlob& blob, CompileTarget target, std::pmr::memory_resource* mr
) noexcept {
    // Construct layout with all vectors wired to mr.
    DescriptorLayout layout{mr};
    RootSignatureSchema& schema = layout.schema_;

    // Short-circuit on the first pass failure (§3.2 sequencer contract).
    // Pass 3 (static-sampler extraction) and Pass 5 (vertex-IO forwarding)
    // are MVP stubs — no-ops that need no function body.
    if (auto r = pass1_precondition(blob, schema); !r)
        return std::unexpected(r.error());

    if (auto r = pass2_partition(blob, schema, mr); !r)
        return std::unexpected(r.error());

    // Pass 3 — static-sampler extraction (MVP stub).
    // Full semantics deferred to sub-epic #69 (SamplerLimitExceeded arm).
    // All Sampler-kind slots remain in their frequency table for now.

    if (auto r = pass4_push_constants(blob, schema); !r)
        return std::unexpected(r.error());

    // Pass 5 — vertex-IO forwarding (MVP stub).
    // vertex_layout_hash deferred to sub-epic #69 (IncompatibleVertexLayout arm).

    if (auto r = pass6_sort(blob, schema); !r)
        return std::unexpected(r.error());

    if (auto r = pass7_cap_check(blob, schema, target); !r)
        return std::unexpected(r.error());

    if (auto r = pass8_completeness(blob, schema); !r)
        return std::unexpected(r.error());

    return layout;
}

// ---------------------------------------------------------------------------
// DescriptorLayout accessors
// ---------------------------------------------------------------------------

const DescriptorTable& DescriptorLayout::table(DescriptorFrequencyGroup g) const noexcept {
    switch (g) {
    case DescriptorFrequencyGroup::PerFrame:
        return schema_.per_frame;
    case DescriptorFrequencyGroup::PerPass:
        return schema_.per_pass;
    case DescriptorFrequencyGroup::PerMaterial:
        return schema_.per_material;
    case DescriptorFrequencyGroup::PerDraw:
        return schema_.per_draw;
    }
    // Unreachable on well-formed inputs; all DescriptorFrequencyGroup values covered above.
    return schema_.per_frame;
}

const RootSignatureSchema& DescriptorLayout::schema() const noexcept { return schema_; }

}  // namespace glibre::shader
