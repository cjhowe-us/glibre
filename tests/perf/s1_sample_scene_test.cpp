// tests/perf/s1_sample_scene_test.cpp
//
// S1 sample-scene perf fixture — CI gate benchmarks exercising BENCHMARK_CELL_BODY
// against representative core and render workloads.
//
// Authority: plan #243, perf-budget.md §CI Gate Spec #1.
//
// S1 Synthetic Scene Invariants:
//   entity_count  : 100  (placeholder entities; canonical 1+200+8 wires in #1003)
//   archetype_count: 4   (four distinct component layouts, deterministically seeded)
//   system_count  : 2    (core transform sweep + render visibility sweep)
//   viewport      : 1920x1080 (descriptor constant matching perf-budget.md §S1)
//
// The workloads here are intentionally synthetic — they exercise the BENCHMARK_CELL_BODY
// contract and produce non-trivial, not-dead-code-eliminated execution paths while
// staying strictly within their per-context CPU sim ceilings on Apple M1 at -O2.
// Real ECS transform propagation and render cull-extract land once those subsystems
// are built; the fixture invariants (entity/archetype/system counts) survive intact.
//
// Scope note (plan #243 amended after PR #1000 round-1 review):
//   This file ships two BENCHMARK_CELL tests against 100 placeholder entities to
//   populate the BENCHMARK_CELL_BODY contract for two contexts so the CI gate has
//   live cells to enforce.  The canonical S1 fixture (1 char + 200 props + 8 lights,
//   entity_count=209, e2e/perf/s1/ manifest, load_s1() helper, and the three named
//   fixture tests) is deferred to plan #1003.
//   See issue #243 §Iterates In and issue #1003.
//
// Named test cases (plan #243 DoD):
//   - s1_scene_core_phase_within_budget
//   - s1_scene_render_phase_within_budget
//
// Build flags:
//   - NO -fno-exceptions: Catch2 BENCHMARK requires exception support
//     (CATCH_TRY/CATCH_CATCH_ALL in catch_benchmark.hpp).
//   - -O2: benchmark measurements must reflect release-like instruction counts.
//     See tests/perf/CMakeLists.txt.

#include <array>
#include <cstdint>

#include "glibre/perf_bench.hpp"

// ---------------------------------------------------------------------------
// S1 Fixture Constants
//
// Defines the synthetic scene dimensions referenced throughout this file.
// These constants are the source of truth for the S1 fixture within this
// plan; the canonical S1 scene invariants (1 character + 200 props + 8 dynamic
// lights = 209 entities, perf-budget.md §S1) are established by plan #1003.
// ---------------------------------------------------------------------------

namespace {

// S1 scene dimensions (synthetic; canonical 1+200+8 entity split lands in #1003).
// perf-budget.md §S1 full scenario: 1 character + 200 props + 8 dynamic lights at
// 1920x1080.  The 100-entity placeholder is the macro-infra exercise workload only.
inline constexpr std::uint32_t kS1EntityCount = 100u;
inline constexpr std::uint32_t kS1ArchetypeCount = 4u;
inline constexpr std::uint32_t kS1SystemCount = 2u;

// Viewport descriptor (perf-budget.md §S1: "1 character + 200 props + 8 dynamic lights
// at 1920x1080").
inline constexpr std::uint32_t kS1ViewportWidth = 1920u;
inline constexpr std::uint32_t kS1ViewportHeight = 1080u;

// ---------------------------------------------------------------------------
// S1 Core Workload: synthetic transform propagation sweep
//
// Models the core context's phase 5 (transform) work:
//   - Walk kS1EntityCount entity slots arranged in kS1ArchetypeCount groups.
//   - Apply a simple parent->child transform accumulation (integer mat4 stub).
//
// Complexity: O(entity_count * archetype_count) with small constant -- well
// under the 400 µs (400_000 ns) core CPU sim ceiling on M1 at -O2.
// ---------------------------------------------------------------------------
[[nodiscard]] std::uint64_t run_s1_core_phase() noexcept {
    // Each archetype group holds entity_count / archetype_count entities.
    constexpr std::uint32_t kGroupSize = kS1EntityCount / kS1ArchetypeCount;

    // Simulate per-archetype transform storage: 4x4 integer matrix (4*4 = 16 cells).
    // std::array is permitted per PHILOSOPHY §11 (no allocator, language aggregate).
    using Mat4i = std::array<std::int32_t, 16>;

    std::uint64_t acc = 0u;

    for (std::uint32_t arch = 0u; arch < kS1ArchetypeCount; ++arch) {
        Mat4i parent_xform{};
        parent_xform[0] = static_cast<std::int32_t>(arch + 1u);  // scale diagonal
        parent_xform[5] = static_cast<std::int32_t>(arch + 1u);
        parent_xform[10] = static_cast<std::int32_t>(arch + 1u);
        parent_xform[15] = 1;

        for (std::uint32_t e = 0u; e < kGroupSize; ++e) {
            // Stub mat4 multiply: accumulate dot of diagonal with entity index.
            // This is O(16) integer ops per entity -- representative of a real
            // SIMD mat4 multiply's memory/compute ratio at this entity count.
            std::int32_t dot = 0;
            for (std::uint32_t k = 0u; k < 16u; ++k) {
                dot += parent_xform[k] * static_cast<std::int32_t>(e + 1u);
            }
            acc += static_cast<std::uint64_t>(static_cast<std::uint32_t>(dot));
        }
    }

    // kS1SystemCount systems run per frame.  The first (archetype sweep above)
    // ran inside the outer loop.  The remaining (kS1SystemCount - 1) systems
    // each perform a linear entity walk -- here modelling entity-lifecycle
    // bookkeeping that touches every slot once.
    for (std::uint32_t sys = 1u; sys < kS1SystemCount; ++sys) {
        for (std::uint32_t e = 0u; e < kS1EntityCount; ++e) {
            acc ^= static_cast<std::uint64_t>(e * kS1ArchetypeCount * sys);
        }
    }

    return acc;
}

// ---------------------------------------------------------------------------
// S1 Render Workload: synthetic visibility cull and draw-list build sweep
//
// Models the render context's phase 6 (cull-extract) work:
//   - For each entity, test a stub AABB against the viewport frustum
//     (simplified to a 2D bounding-box clip against kS1ViewportWidth x kS1ViewportHeight).
//   - Accumulate visible entity indices into a draw-list counter.
//
// The cull predicate tests x_max against the right viewport edge (x_max <
// kS1ViewportWidth) so the result is data-dependent and not trivially
// constant-foldable.  Adding [[gnu::noinline]] defeats call-site folding.
//
// Complexity: O(entity_count) -- well under the 1_500_000 ns render ceiling.
// ---------------------------------------------------------------------------
[[nodiscard]] [[gnu::noinline]] std::uint64_t run_s1_render_phase() noexcept {
    std::uint64_t visible_count = 0u;

    for (std::uint32_t e = 0u; e < kS1EntityCount; ++e) {
        const std::uint32_t x_min = (e * 8u) % kS1ViewportWidth;
        const std::uint32_t y_min = (e * 4u) % kS1ViewportHeight;
        const std::uint32_t x_max = x_min + 64u;
        const std::uint32_t y_max = y_min + 64u;

        // Clip test: visible if AABB overlaps [0, viewport).
        // x_max < kS1ViewportWidth is data-dependent (entities near the right
        // edge are clipped); x_min < kS1ViewportWidth and y_* checks similarly.
        if (x_min < kS1ViewportWidth && x_max < kS1ViewportWidth && y_min < kS1ViewportHeight &&
            y_max < kS1ViewportHeight) {
            ++visible_count;
        }
    }

    // Simulate draw-list build: accumulate visibility mask per archetype group
    // (models the per-archetype instance-count write that feeds mesh-shader
    //  indirect argument buffers).
    std::uint64_t draw_mask = 0u;
    for (std::uint32_t arch = 0u; arch < kS1ArchetypeCount; ++arch) {
        draw_mask |= (visible_count >> arch) & 0xFFu;
    }

    return draw_mask;
}

}  // namespace

// ---------------------------------------------------------------------------
// s1_scene_core_phase_within_budget
//
// CI gate benchmark for the core context's S1 phase 5 (transform propagation)
// workload.  Budget: 400_000 ns (0.40 ms CPU sim, perf-budget.md §core row).
//
// The BENCHMARK_CELL_BODY macro:
//   (a) runs a Catch2 BENCHMARK block for statistical ns/iter reporting, and
//   (b) times one execution with std::chrono::steady_clock and REQUIREs the
//       elapsed time is strictly less than kCoreCpuBudgetNs.
//
// S1 fixture invariants exercised here:
//   entity_count=100, archetype_count=4, system_count=2
// ---------------------------------------------------------------------------
TEST_CASE("s1_scene_core_phase_within_budget", "[perf][core][s1]") {
    BENCHMARK_CELL_BODY(
        glibre::perf_bench::kCoreCpuBudgetNs,
        glibre::perf_bench::ContextTag::Core,
        (void)run_s1_core_phase()
    );
}

// ---------------------------------------------------------------------------
// s1_scene_render_phase_within_budget
//
// CI gate benchmark for the render context's S1 phase 6+7 (cull-extract +
// draw-list build) workload.  Budget: 1_500_000 ns (1.50 ms combined CPU
// sim 0.10 ms + submit 1.40 ms, perf-budget.md §render row).
//
// S1 fixture invariants exercised here:
//   entity_count=100, archetype_count=4, viewport=1920x1080
// ---------------------------------------------------------------------------
TEST_CASE("s1_scene_render_phase_within_budget", "[perf][render][s1]") {
    BENCHMARK_CELL_BODY(
        glibre::perf_bench::kRenderCpuBudgetNs,
        glibre::perf_bench::ContextTag::Render,
        (void)run_s1_render_phase()
    );
}
