// tests/core/world/perf/pmc_sampler.cpp
//
// macOS-only PMC sampler — implementation of PmcSampler::start_sampling()
// and PmcSampler::stop_sampling().
//
// Authority: plan #944.
//
// ---------------------------------------------------------------------------
// Integration Notes — kperf / KPC private API (follow-up task)
//
// Apple Silicon exposes hardware PMC events via the private kperf / KPC
// framework that lives in:
//   /System/Library/PrivateFrameworks/kperf.framework/
//   /System/Library/PrivateFrameworks/kperfdata.framework/
//
// Key entry points (from open-source Darwin XNU + kdebug research):
//   kpc_force_all_ctrs_set(int)          — acquire exclusive counter control
//   kpc_set_thread_counting(uint32_t)    — enable per-thread counting
//   kpc_get_thread_counters(uint32_t, uint32_t, uint64_t*)  — read counters
//   kpc_set_config(uint32_t, kpc_config_t*)  — configure event selectors
//   kpc_get_config_count(uint32_t)       — number of configurable counters
//   kpc_get_counter_count(uint32_t)      — total counter slots
//
// The framework requires the entitlement com.apple.private.kpc.user which
// is not available to un-signed or ad-hoc-signed developer builds.  Calling
// kpc_force_all_ctrs_set() without the entitlement returns EPERM.
//
// Approach for the follow-up integration spike:
//   1. Add a dedicated signed test runner target with the entitlement
//      (requires a provisioning profile or Apple Developer Program membership
//      with a custom entitlement set).
//   2. dlopen() the kperf framework at runtime and resolve the symbols
//      dynamically so the test binary does not hard-link the private
//      framework (avoids App Store / notarization rejection for normal
//      builds).
//   3. Fall back gracefully to zero-count if dlopen() or symbol resolution
//      fails (GLIBRE_ENABLE_PMC path unchanged from this stub).
//
// Apple PMU event selector values (ARM64 core v8.5 / Apple Silicon):
//   L1D_CACHE_REFILL         0x03   — L1D miss / refill
//   LD_RETIRED               0x06   — loads retired
//   L2D_CACHE_REFILL         0x17   — L2 unified miss
//   INST_RETIRED             0x08   — instructions retired
// (Source: ARM Architecture Reference Manual + Apple WWDC 2023 Instruments
//  engineering session; vendor-specific event codes may differ per SoC rev.)
//
// Until this spike completes, start_sampling() and stop_sampling() are
// zero-stubs on all platforms including macOS.
// ---------------------------------------------------------------------------

#include "pmc_sampler.hpp"

#ifdef __APPLE__
// macOS implementation — currently a zero-stub pending KPC entitlement
// integration (see §Integration Notes above).
//
// Thread-local storage is reserved for the snapshot that start_sampling() will
// capture and stop_sampling() will diff against once the KPC entitlement
// integration spike (follow-up to #944) lands.  Using TLS avoids heap
// allocation and keeps start/stop on the same OS thread without synchronisation.
//
// In the current zero-stub the variable is intentionally inert (start_sampling
// zeros it, stop_sampling returns {} directly) and exists solely to document
// the storage contract for the integration spike.  It will become live once
// kpc_get_thread_counters() calls are added to start_sampling()/stop_sampling().

#include <cstring>

namespace {

// Reserved storage for KPC spike integration — currently inert (zero-stub).
// start_sampling() will populate this; stop_sampling() will compute the delta.
// See §Integration Notes above for the kperf/KPC API plan.
thread_local glibre::testing::PmcCounters g_snapshot{};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace

namespace glibre::testing {

void PmcSampler::start_sampling() noexcept {
    // TODO (follow-up spike): call kpc_force_all_ctrs_set(1), configure
    // event selectors via kpc_set_config(), enable per-thread counting via
    // kpc_set_thread_counting(), then read initial counter values via
    // kpc_get_thread_counters() into g_snapshot.
    //
    // For now: zero the snapshot so stop_sampling() always returns {0,0,0,0}.
    g_snapshot = {};
}

PmcCounters PmcSampler::stop_sampling() noexcept {
    // TODO (follow-up spike): read final counter values via
    // kpc_get_thread_counters(), compute delta vs g_snapshot, disable
    // per-thread counting via kpc_set_thread_counting(0).
    //
    // For now: return {0,0,0,0} (all fields zero-initialised by default ctor).
    return {};
}

}  // namespace glibre::testing

#else  // !__APPLE__

// Non-Apple stub — zero-count unconditionally.
namespace glibre::testing {

void PmcSampler::start_sampling() noexcept {}

PmcCounters PmcSampler::stop_sampling() noexcept { return {}; }

}  // namespace glibre::testing

#endif  // __APPLE__
