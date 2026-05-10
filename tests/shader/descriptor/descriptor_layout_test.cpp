// tests/shader/descriptor/descriptor_layout_test.cpp
//
// Catch2 unit tests for glibre::shader::DescriptorLayout::derive (plan #1087).
//
// Authority: specs/shader/SPEC.md §4.5, specs/shader/descriptor-layout-design.md §3.2.
//
// Named test cases (plan #1087 Unit Test Plan):
//   - descriptor_derive_uses_pmr_resource
//
// Design constraints:
//   - -fno-exceptions compatible (error-model.md §Decision 3).
//   - No REQUIRE_THROWS.
//   - Each test constructs a per-test PerContextAllocatorResource so that
//     DescriptorLayout::derive() allocations are tracked under ContextTag::shader
//     (perf-budget.md §Allocator Rules #1).
//   - ReflectionBlob test fixtures are constructed with a separate "blob_mr" so
//     that blob-side allocations are counted independently.  Only the derive()
//     output must land under "derive_mr"; the test verifies this by probing
//     bytes_used() on the derive allocator before and after the call.

#include <cstdint>

#include <catch2/catch_test_macros.hpp>
#include <glibre/alloc.hpp>
#include <glibre/shader/shader.hpp>

namespace {

// Per-test allocator pair helper — mirrors the ShaderTestAlloc pattern from
// tests/shader/source/shader_source_test.cpp.
//
// Declare the PerContextAllocator BEFORE the resource (RAII order: resource
// must destruct before allocator per alloc.hpp lifetime contract).
struct ShaderTestAlloc {
    glibre::PerContextAllocator alloc{glibre::ContextTag::shader};
    glibre::PerContextAllocatorResource mr{alloc};
};

// make_binding_slot — construct a BindingSlot with all fields set, using the
// supplied memory resource for the name string.
glibre::shader::BindingSlot make_binding_slot(
    std::pmr::memory_resource* mr,
    glibre::shader::BindingKind kind,
    std::uint32_t reg_space,
    std::uint32_t reg_index,
    glibre::shader::DescriptorFrequencyGroup freq,
    std::string_view name
) {
    glibre::shader::BindingSlot slot;
    slot.kind = kind;
    slot.register_space = reg_space;
    slot.register_index = reg_index;
    slot.array_size = 1;
    slot.stages = glibre::shader::StageMask{0x01};  // Vertex stage bit
    slot.frequency = freq;
    slot.name = std::pmr::string{name.data(), name.size(), mr};
    return slot;
}

}  // namespace

// ===========================================================================
// Test: descriptor_derive_uses_pmr_resource
//
// Confirms that DescriptorLayout::derive(blob, target, &derive_mr) routes
// ALL output DescriptorTable::slots string and vector allocations through
// the supplied PMR memory resource.
//
// Probe: bytes_used() on the derive_alloc must increase after derive() on a
// blob with at least one BindingSlot carrying a non-empty name string.
//
// The blob is constructed with a SEPARATE blob_alloc to ensure that blob-side
// allocations do not contaminate the derive-side byte count.
//
// After the call, the derive_alloc byte count must be strictly greater than
// zero — confirming allocations went to derive_mr, not to std::pmr::get_default_resource().
// ===========================================================================

TEST_CASE("descriptor_derive_uses_pmr_resource", "[shader][descriptor_layout]") {
    // Blob-side allocator: used only for constructing the input ReflectionBlob.
    ShaderTestAlloc blob_ta;

    // Derive-side allocator: must receive ALL output allocations from derive().
    ShaderTestAlloc derive_ta;

    // Record derive allocator byte count before the call.
    const std::uint64_t bytes_before = derive_ta.alloc.bytes_used();

    // Build a ReflectionBlob with one BindingSlot per frequency group so
    // the partition pass exercises all four tables.  The name strings are
    // allocated under blob_ta.mr to keep blob allocations separate.
    glibre::shader::ReflectionBlob blob{
        .entry_points = std::pmr::vector<glibre::shader::EntryPoint>{&blob_ta.mr},
        .bindings = std::pmr::vector<glibre::shader::BindingSlot>{&blob_ta.mr},
        .vertex_io =
            glibre::shader::VertexIOLayout{
                std::pmr::vector<glibre::shader::VertexInputElement>{&blob_ta.mr}
            },
        .push_constants = std::pmr::vector<glibre::shader::PushConstantRange>{&blob_ta.mr},
        .material_parameters =
            glibre::shader::MaterialParameterBlock{
                std::pmr::string{&blob_ta.mr},
                0,
                std::pmr::vector<glibre::shader::BindingSlot>{&blob_ta.mr}
            },
        .spec_constants = std::pmr::vector<glibre::shader::SpecializationConstantSlot>{&blob_ta.mr},
        .rt_payload_bytes = 0,
    };

    // Add one ConstantBuffer binding in each frequency group.  Each has a
    // non-empty name so that the name allocation is observable in bytes_used().
    blob.bindings.push_back(make_binding_slot(
        &blob_ta.mr,
        glibre::shader::BindingKind::ConstantBuffer,
        0,
        0,
        glibre::shader::DescriptorFrequencyGroup::PerFrame,
        "frame_uniforms"
    ));
    blob.bindings.push_back(make_binding_slot(
        &blob_ta.mr,
        glibre::shader::BindingKind::ConstantBuffer,
        0,
        1,
        glibre::shader::DescriptorFrequencyGroup::PerPass,
        "pass_uniforms"
    ));
    blob.bindings.push_back(make_binding_slot(
        &blob_ta.mr,
        glibre::shader::BindingKind::SampledImage,
        0,
        0,
        glibre::shader::DescriptorFrequencyGroup::PerMaterial,
        "albedo_texture"
    ));
    blob.bindings.push_back(make_binding_slot(
        &blob_ta.mr,
        glibre::shader::BindingKind::ConstantBuffer,
        0,
        2,
        glibre::shader::DescriptorFrequencyGroup::PerDraw,
        "draw_data"
    ));

    // Call derive with the derive-side memory resource.
    auto result = glibre::shader::DescriptorLayout::derive(
        blob, glibre::shader::CompileTarget::MetalLib, &derive_ta.mr
    );

    // derive must succeed (all bindings are frequency-tagged).
    REQUIRE(result.has_value());

    // Record derive allocator byte count after the call.
    const std::uint64_t bytes_after = derive_ta.alloc.bytes_used();

    // PRIMARY ASSERTION: the derive allocator must have received allocations.
    // If derive() silently fell back to std::pmr::get_default_resource(),
    // bytes_after would equal bytes_before.
    CHECK(bytes_after > bytes_before);

    // SECONDARY ASSERTIONS: structural correctness of the derived layout.
    // One slot per frequency group.
    const auto& schema = result->schema();
    CHECK(schema.per_frame.slots.size() == 1u);
    CHECK(schema.per_pass.slots.size() == 1u);
    CHECK(schema.per_material.slots.size() == 1u);
    CHECK(schema.per_draw.slots.size() == 1u);

    // Verify slot names were copied correctly (content, not just pointer).
    CHECK(schema.per_frame.slots[0].name == "frame_uniforms");
    CHECK(schema.per_pass.slots[0].name == "pass_uniforms");
    CHECK(schema.per_material.slots[0].name == "albedo_texture");
    CHECK(schema.per_draw.slots[0].name == "draw_data");

    // Verify table() accessor agrees with schema().
    CHECK(result->table(glibre::shader::DescriptorFrequencyGroup::PerFrame).slots.size() == 1u);
    CHECK(result->table(glibre::shader::DescriptorFrequencyGroup::PerPass).slots.size() == 1u);
    CHECK(result->table(glibre::shader::DescriptorFrequencyGroup::PerMaterial).slots.size() == 1u);
    CHECK(result->table(glibre::shader::DescriptorFrequencyGroup::PerDraw).slots.size() == 1u);
}

// ===========================================================================
// Test: descriptor_derive_returns_frequency_missing_for_untagged_binding
//
// Confirms that derive() returns DescriptorFrequencyMissing when a binding
// carries a DescriptorFrequencyGroup value that is outside the known range.
// This is a defense-in-depth check (Pass 1 of the 8-pass pipeline).
//
// We use static_cast to produce an out-of-range enum value (simulating a
// future protocol extension the current build does not understand).
// ===========================================================================

TEST_CASE(
    "descriptor_derive_returns_frequency_missing_for_untagged_binding",
    "[shader][descriptor_layout]"
) {
    ShaderTestAlloc blob_ta;
    ShaderTestAlloc derive_ta;

    glibre::shader::ReflectionBlob blob{
        .entry_points = std::pmr::vector<glibre::shader::EntryPoint>{&blob_ta.mr},
        .bindings = std::pmr::vector<glibre::shader::BindingSlot>{&blob_ta.mr},
        .vertex_io =
            glibre::shader::VertexIOLayout{
                std::pmr::vector<glibre::shader::VertexInputElement>{&blob_ta.mr}
            },
        .push_constants = std::pmr::vector<glibre::shader::PushConstantRange>{&blob_ta.mr},
        .material_parameters =
            glibre::shader::MaterialParameterBlock{
                std::pmr::string{&blob_ta.mr},
                0,
                std::pmr::vector<glibre::shader::BindingSlot>{&blob_ta.mr}
            },
        .spec_constants = std::pmr::vector<glibre::shader::SpecializationConstantSlot>{&blob_ta.mr},
        .rt_payload_bytes = 0,
    };

    // Add a slot with an out-of-range frequency group value.
    glibre::shader::BindingSlot bad_slot;
    bad_slot.kind = glibre::shader::BindingKind::ConstantBuffer;
    bad_slot.register_space = 0;
    bad_slot.register_index = 0;
    bad_slot.array_size = 1;
    bad_slot.stages = glibre::shader::StageMask{0x01};
    bad_slot.frequency = static_cast<glibre::shader::DescriptorFrequencyGroup>(0xFF);
    bad_slot.name = std::pmr::string{"bad_slot", &blob_ta.mr};
    blob.bindings.push_back(std::move(bad_slot));

    auto result = glibre::shader::DescriptorLayout::derive(
        blob, glibre::shader::CompileTarget::MetalLib, &derive_ta.mr
    );

    REQUIRE_FALSE(result.has_value());

    const auto& err = result.error();
    const bool is_missing = std::holds_alternative<glibre::shader::Error>(err.code()) &&
                            std::get<glibre::shader::Error>(err.code()) ==
                                glibre::shader::Error::DescriptorFrequencyMissing;
    CHECK(is_missing);
}

// ===========================================================================
// Test: descriptor_derive_empty_blob_succeeds_with_no_allocations
//
// An empty ReflectionBlob (no bindings) must produce a valid DescriptorLayout
// with all four tables empty, and must not allocate any bytes under derive_mr
// (nothing to partition).  This pins the zero-allocation baseline.
// ===========================================================================

TEST_CASE(
    "descriptor_derive_empty_blob_succeeds_with_no_allocations", "[shader][descriptor_layout]"
) {
    ShaderTestAlloc blob_ta;
    ShaderTestAlloc derive_ta;

    glibre::shader::ReflectionBlob blob{
        .entry_points = std::pmr::vector<glibre::shader::EntryPoint>{&blob_ta.mr},
        .bindings = std::pmr::vector<glibre::shader::BindingSlot>{&blob_ta.mr},
        .vertex_io =
            glibre::shader::VertexIOLayout{
                std::pmr::vector<glibre::shader::VertexInputElement>{&blob_ta.mr}
            },
        .push_constants = std::pmr::vector<glibre::shader::PushConstantRange>{&blob_ta.mr},
        .material_parameters =
            glibre::shader::MaterialParameterBlock{
                std::pmr::string{&blob_ta.mr},
                0,
                std::pmr::vector<glibre::shader::BindingSlot>{&blob_ta.mr}
            },
        .spec_constants = std::pmr::vector<glibre::shader::SpecializationConstantSlot>{&blob_ta.mr},
        .rt_payload_bytes = 0,
    };

    const std::uint64_t bytes_before = derive_ta.alloc.bytes_used();

    auto result = glibre::shader::DescriptorLayout::derive(
        blob, glibre::shader::CompileTarget::MetalLib, &derive_ta.mr
    );

    REQUIRE(result.has_value());

    // All four tables must be empty.
    const auto& schema = result->schema();
    CHECK(schema.per_frame.slots.empty());
    CHECK(schema.per_pass.slots.empty());
    CHECK(schema.per_material.slots.empty());
    CHECK(schema.per_draw.slots.empty());
    CHECK(schema.static_samplers.empty());
    CHECK(schema.push_constants.empty());

    // An empty blob produces no allocations under derive_mr.
    const std::uint64_t bytes_after = derive_ta.alloc.bytes_used();
    CHECK(bytes_after == bytes_before);
}
