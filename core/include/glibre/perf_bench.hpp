#pragma once
// core/include/glibre/perf_bench.hpp
//
// Per-context CI gate macro and constants for Catch2 benchmark tests.
//
// Design (perf-budget.md §CI Gate Spec #1, plan #242):
//   Each bounded context's SPEC §9 lists at least one micro-benchmark that
//   exercises its phase work under the S1 fixture.  The benchmark asserts
//   `time <= cell_budget_ns` (perf-budget.md §CI Gate Spec #1).  PR fails
//   if any assertion fails.
//
//   BENCHMARK_CELL_BODY is the inner assertion macro used inside a named
//   TEST_CASE.  Test files write the TEST_CASE with a literal name so that
//   the DoD verifier's grep can find "BENCHMARK_CELL_<tag>" as a literal
//   string inside a TEST_CASE call:
//
//     TEST_CASE("BENCHMARK_CELL_core_phase_under_budget", "[perf][core]") {
//         BENCHMARK_CELL_BODY(glibre::perf_bench::kCoreCpuBudgetNs,
//                             glibre::perf_bench::ContextTag::Core,
//                             (void)0);
//     }
//
//   The naming convention "BENCHMARK_CELL_<context>_<description>" ties
//   each test case to its context's budget cell in perf-budget.md.
//
// Budget constants (perf-budget.md §Per-Context Budget Table):
//   CPU sim ceilings converted to nanoseconds.  Single-phase micro-
//   benchmarks use the sim ceiling (tighter of sim vs. submit).
//
// Dependency note (plan #241 / PR #988):
//   This header is intentionally standalone.  It does NOT include
//   glibre/perf_budget.hpp so it can land before plan #241 merges.
//   The local ContextTag enum mirrors the definition in perf_budget.hpp;
//   when plan #243 wires a live PerfBudget, it will replace this copy
//   with a single include and remove the local definition.
//
// Note on -fno-exceptions:
//   The perf gate test executable does NOT compile with -fno-exceptions.
//   Catch2 BENCHMARK requires exception support for its internal error
//   handling (CATCH_TRY / CATCH_CATCH_ALL in catch_benchmark.hpp).
//   Test executables are not on the engine ABI boundary; the flag is not
//   mandated for test-only binaries.

#include <chrono>
#include <cstdint>

// Catch2 benchmark + test macros.
#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

// NOLINTBEGIN(cppcoreguidelines-macro-usage)

namespace glibre::perf_bench {

// ---------------------------------------------------------------------------
// ContextTag — bounded-context identifiers matching perf-budget.md table.
//
// Mirrors glibre::ContextTag defined in core/include/glibre/perf_budget.hpp
// (plan #241, PR #988).  This local copy keeps perf_bench.hpp standalone
// so the CI gate scaffolding can land before plan #241 merges.
//
// Plan #243 will wire a live PerfBudget and collapse this into a single
// #include "glibre/perf_budget.hpp".  The enum values and ordering MUST
// match the definition in perf_budget.hpp.
// ---------------------------------------------------------------------------
enum class ContextTag : std::uint8_t {
    Core = 0,
    Platform,
    Data,
    Shader,
    Render,
    Geometry,
    Physics,
    Content,
    Tools,
    E2E,  // test-only; no shipping-build budget ceiling
};

// ---------------------------------------------------------------------------
// CPU budget ceilings per context — nanoseconds
//
// Source: perf-budget.md §Per-Context Budget Table.
// These are the CPU-sim ceilings (the tighter of sim vs. submit) in ns.
// Single-phase micro-benchmarks are compared against the sim ceiling.
//
// A no-op workload always passes; the gate fires only when a real S1
// workload (plan #243) overshoots the ceiling.
// ---------------------------------------------------------------------------

// core: 0.40 ms sim → 400_000 ns
inline constexpr std::uint64_t kCoreCpuBudgetNs = 400'000u;

// platform: 0.20 ms sim → 200_000 ns
inline constexpr std::uint64_t kPlatformCpuBudgetNs = 200'000u;

// data: 0.15 ms sim → 150_000 ns
inline constexpr std::uint64_t kDataCpuBudgetNs = 150'000u;

// render: combined sim (0.10 ms) + submit (1.40 ms) → 1.50 ms → 1_500_000 ns
// Render macro-benchmarks test the combined phase 6+7 slice (not sim-only).
inline constexpr std::uint64_t kRenderCpuBudgetNs = 1'500'000u;

// geometry: 0.30 ms sim → 300_000 ns
inline constexpr std::uint64_t kGeometryCpuBudgetNs = 300'000u;

// physics: 2.00 ms sim → 2_000_000 ns
inline constexpr std::uint64_t kPhysicsCpuBudgetNs = 2'000'000u;

// content: 0.20 ms sim → 200_000 ns
inline constexpr std::uint64_t kContentCpuBudgetNs = 200'000u;

// tools: 0.80 ms sim → 800_000 ns
inline constexpr std::uint64_t kToolsCpuBudgetNs = 800'000u;

// ---------------------------------------------------------------------------
// detail::time_body_ns — time a single invocation of `body`, return ns.
//
// Uses std::chrono::steady_clock (PHILOSOPHY §11 retained std:: utility).
// The volatile sink prevents dead-code elimination of the body result.
// ---------------------------------------------------------------------------

namespace detail {

template<typename F>
[[nodiscard]] std::uint64_t time_body_ns(F&& body) {
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    [[maybe_unused]] volatile auto sink = body();
    const auto t1 = Clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()
    );
}

}  // namespace detail

}  // namespace glibre::perf_bench

// ---------------------------------------------------------------------------
// BENCHMARK_CELL_BODY(ceiling_ns, context_tag, ...)
//
// Inner assertion body for a per-context CI gate benchmark.  Used INSIDE a
// TEST_CASE whose name follows the "BENCHMARK_CELL_<tag>" convention so that
// the DoD verifier can grep for the literal test-case name.
//
// Parameters:
//   ceiling_ns  — ceiling in nanoseconds (use kCoreCpuBudgetNs etc.).
//   context_tag — glibre::perf_bench::ContextTag value; reserved for plan
//                 #243 PerfBudget wiring.  Not used in this scaffolding.
//   ...         — body expression(s) forming the workload; must be void-
//                 evaluatable (e.g. a function call, `(void)0`).
//
// The macro:
//   1. Calls BENCHMARK with a lambda wrapping `body`.
//      Catch2 runs the lambda many times and reports mean/median/SD.
//   2. Times one call to `body` with steady_clock and REQUIREs the
//      result is < ceiling_ns.  This REQUIRE is the CI gate assertion.
//
// Usage:
//   // The TEST_CASE name is a literal so the DoD verifier grep succeeds:
//   TEST_CASE("BENCHMARK_CELL_core_phase_under_budget", "[perf][core]") {
//       BENCHMARK_CELL_BODY(glibre::perf_bench::kCoreCpuBudgetNs,
//                           glibre::perf_bench::ContextTag::Core,
//                           (void)0);
//   }
// ---------------------------------------------------------------------------

// NOLINTBEGIN(bugprone-macro-parentheses)
#define BENCHMARK_CELL_BODY(ceiling_ns, context_tag, ...)                                          \
    do {                                                                                           \
        /* context_tag reserved for plan #243 PerfBudget wiring. */                                \
        [[maybe_unused]] constexpr glibre::perf_bench::ContextTag GLIBRE_BENCH_CTX_ =              \
            (context_tag);                                                                         \
                                                                                                   \
        /* 1. Catch2 BENCHMARK block — statistical reporting. */                                   \
        BENCHMARK("workload") {                                                                    \
            __VA_ARGS__;                                                                           \
            return 0;                                                                              \
        };                                                                                         \
                                                                                                   \
        /* 2. Chrono-timed single run — hard CI gate assertion. */                                 \
        const auto GLIBRE_BENCH_NS_ = glibre::perf_bench::detail::time_body_ns([] {                \
            __VA_ARGS__;                                                                           \
            return 0;                                                                              \
        });                                                                                        \
        REQUIRE(GLIBRE_BENCH_NS_ < static_cast<std::uint64_t>(ceiling_ns));                        \
    } while (false)
// NOLINTEND(bugprone-macro-parentheses)

// NOLINTEND(cppcoreguidelines-macro-usage)
