// tests/perf/perf_budget_gate_test.cpp
//
// CI gate benchmarks for per-context perf-budget assertions.
//
// Authority: plan #242, perf-budget.md §CI Gate Spec #1.
//
// Named test cases (plan #242 DoD):
//   - BENCHMARK_CELL_core_phase_under_budget
//   - BENCHMARK_CELL_render_phase_under_budget
//
// Design:
//   Each TEST_CASE uses BENCHMARK_CELL_BODY which runs:
//     (a) A Catch2 BENCHMARK block for statistical reporting.
//     (b) A chrono-timed single run + REQUIRE(elapsed < ceiling_ns).
//
//   Test case names use the literal "BENCHMARK_CELL_<ctx>_<desc>" convention
//   so that the DoD verifier grep (TEST_CASE|SCENARIO)\("BENCHMARK_CELL_..."
//   finds each case in source.
//
//   This scaffolding plan ships trivial no-op workloads demonstrating the
//   contract.  Real S1 workloads (plan #243) will replace the no-op bodies.
//   The CI gate fires only when a real workload overshoots its cell ceiling.
//
// Scope (plan #242):
//   Two contexts demonstrated: core and render.  Additional per-context
//   benchmarks are added as each context's phase work lands in plan #243.
//
// Out of scope:
//   - Real S1 workloads (#243).
//   - Frame-loop integration of the gate.
//   - GPU timing (MTLCounterSampleBuffer — separate render plan).

#include "glibre/perf_bench.hpp"

// ---------------------------------------------------------------------------
// BENCHMARK_CELL_core_phase_under_budget
//
// Demonstrates the CI gate contract for the core context.
// Budget: 0.40 ms CPU sim (400_000 ns) from perf-budget.md §core row.
//
// No-op workload: a no-op always satisfies the budget.
// Plan #243 replaces the body with a real transform-propagation fixture.
// ---------------------------------------------------------------------------
TEST_CASE("BENCHMARK_CELL_core_phase_under_budget", "[perf][core]") {
    BENCHMARK_CELL_BODY(
        glibre::perf_bench::kCoreCpuBudgetNs, glibre::perf_bench::ContextTag::Core, (void)0
    );
}

// ---------------------------------------------------------------------------
// BENCHMARK_CELL_render_phase_under_budget
//
// Demonstrates the CI gate contract for the render context.
// Budget: 1.50 ms combined CPU (sim 0.10 ms + submit 1.40 ms = 1_500_000 ns)
// from perf-budget.md §render row.
//
// No-op workload: a no-op always satisfies the budget.
// Plan #243 replaces the body with a real cull-extract / render-submit stub.
// ---------------------------------------------------------------------------
TEST_CASE("BENCHMARK_CELL_render_phase_under_budget", "[perf][render]") {
    BENCHMARK_CELL_BODY(
        glibre::perf_bench::kRenderCpuBudgetNs, glibre::perf_bench::ContextTag::Render, (void)0
    );
}
