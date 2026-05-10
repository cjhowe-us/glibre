// tests/core/world/perf/pmc_sampler.hpp
//
// macOS-only PMC (Performance Monitor Counter) sampler helper.
//
// Authority: plan #944, reviews/decisions/perf-budget.md §CI Gate Spec #2.
//
// Provides PmcSampler::measure([&]{...}) -> PmcCounters, a thin RAII wrapper
// over the Apple Silicon kperf / KPC private API that captures hardware
// performance counter deltas around a callable.
//
// Build flag:
//   GLIBRE_ENABLE_PMC (default ON for tests/core/world/perf/** targets)
//   When OFF, measure() delegates to the zero-stub overload that returns a
//   zero-initialised PmcCounters.  This allows alarm tests to compile on
//   platforms or CI environments that lack KPC access.
//
// Platform guard:
//   The implementation TU uses #ifdef __APPLE__ throughout.  On non-Apple
//   platforms the stub (zero-count) path is compiled unconditionally.
//
// kperf / KPC integration status:
//   The Apple kperf/KPC API (kpc_get_counters, kpc_set_thread_counting,
//   kpc_force_all_ctrs_set, kpc_config_t, kpc_countable_t, etc.) is an
//   undocumented private framework that requires the
//   com.apple.private.kpc.user entitlement.  On un-entangled developer
//   builds kpc_force_all_ctrs_set() returns EPERM.  The implementation
//   therefore stubs all macOS bodies to zero-count and documents this as
//   a follow-up integration task (see pmc_sampler.cpp §Integration Notes).
//
// Usage:
//   #include "tests/core/world/perf/pmc_sampler.hpp"
//
//   auto counters = glibre::testing::PmcSampler::measure([&] {
//       workload();
//   });
//   // counters.l1d_misses, counters.loads_retired
//
// Dependencies: none (header-only declaration; no EASTL, no glibre::core).
// The sampler is test-infrastructure only and must not be included from
// engine or plugin code.

#pragma once

#include <cstdint>
#include <type_traits>

// Guard: only compile active PMC sampling when explicitly requested.
#ifndef GLIBRE_ENABLE_PMC
#define GLIBRE_ENABLE_PMC 1
#endif

namespace glibre::testing {

// ---------------------------------------------------------------------------
// PmcCounters — hardware counter snapshot delta around a measured region.
//
// All fields represent DELTA values (end - start) accumulated over the
// duration of a single PmcSampler::measure() call.  Zero-initialised by
// default; fields are zero on non-Apple platforms or when GLIBRE_ENABLE_PMC
// is 0, or when the KPC entitlement is absent.
//
// Field semantics (Apple Silicon A-class PMU events):
//   l1d_misses        — L1 data-cache refills (misses that went to L2+).
//   loads_retired     — all retired load micro-ops.
//   l2_misses         — L2 unified cache misses (approximate; PMU may share
//                       the counter with instruction misses on some cores).
//   instructions      — retired instruction count (if counter slot free).
//
// Note: Apple Silicon's fixed-function PMU exposes 8-10 configurable
// counter slots.  The exact availability depends on the core cluster
// (P-core vs E-core), macOS version, and concurrent profiling sessions.
// When a counter slot is unavailable its field is 0.
// ---------------------------------------------------------------------------
struct PmcCounters {
    std::uint64_t l1d_misses{0};
    std::uint64_t loads_retired{0};
    std::uint64_t l2_misses{0};
    std::uint64_t instructions{0};
};

// ---------------------------------------------------------------------------
// PmcSampler — stateless sampler; all state is on the stack inside measure().
//
// measure() wraps a callable [&]{...} with PMC read/write brackets and
// returns the counter delta.  It is not thread-safe with respect to other
// concurrent calls on the same OS thread (KPC counters are per-thread on
// Apple Silicon when KPERF_THREAD_COUNTING is used).
//
// Template parameter F must be Callable and must return void.
// ---------------------------------------------------------------------------
class PmcSampler {
public:
    // measure() — capture PMC delta around the supplied callable.
    //
    // Returns a PmcCounters whose fields are the delta from before to after
    // the callable invocation on the current OS thread.  On platforms or
    // environments where KPC is unavailable all fields are 0.
    template <typename F>
        requires std::is_invocable_v<F>
    [[nodiscard]] static PmcCounters measure(F &&fn);

private:
    // Internal helpers declared in pmc_sampler.cpp; forward-declared here so
    // the measure() template body can call them without exposing the
    // implementation detail in the header.
    static void start_sampling() noexcept;
    static PmcCounters stop_sampling() noexcept;
};

} // namespace glibre::testing

// ---------------------------------------------------------------------------
// Inline template definition — must appear after the class definition.
// ---------------------------------------------------------------------------

#include <utility>

namespace glibre::testing {

template <typename F>
    requires std::is_invocable_v<F>
[[nodiscard]] PmcCounters PmcSampler::measure(F &&fn) {
    PmcSampler::start_sampling();
    std::forward<F>(fn)();
    return PmcSampler::stop_sampling();
}

} // namespace glibre::testing
