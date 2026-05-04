# platform — Detailed Design: clock aggregate

> Detailed design for the `Clock` / `Instant` / `WallTime` / `Duration`
> aggregate declared in `specs/platform/SPEC.md` §4.4. Refines §4.4,
> §5.9 (public surface), §6.6 (`mach_absolute_time` seam), §7 (the
> non-serializability rule for `Instant`), §8.1 (clock as a
> peer-plugin pass-through; platform-self-reload re-acquisition rule),
> §9.1 / §9.2 (per-frame budget + sub-arena), §10.3.4 (total-function
> failure model). Adds the **frame-tick** + **fixed-step accumulator**
> value-objects mandated by the spike brief; both are pure value
> primitives that *use* `Clock` rather than embedding scheduling
> policy.
>
> Cites `reviews/decisions/error-model.md`,
> `reviews/decisions/frame-phases.md`,
> `reviews/decisions/perf-budget.md`,
> `reviews/decisions/plugin-abi.md`,
> `reviews/decisions/hot-reload-protocol.md`,
> `reviews/decisions/fory-codegen.md`. Does not introduce any new
> public surface beyond the §5 stubs in `specs/platform/SPEC.md`
> except for the additive `FrameTick` / `FixedStepAccumulator` value
> objects co-located on `glibre::platform::Clock`'s seam — both are
> pure header-side computations on `Instant` / `Duration` and
> introduce no new ABI symbols beyond those derivable from the
> existing `Clock::now` / `wall` / `native_tick` triplet.
>
> Harmonius prior art (`harmonius/docs/requirements/platform/
> threading-async.md`, `harmonius/docs/design/core-runtime/
> game-loop.md` §Fixed Timestep Accumulator) cited as research input
> only — every conclusion below was independently re-derived per
> `PHILOSOPHY.md`.

Refs: spike #723 — `[SPIKE] design-platform-clock-detailed`.
Parent: #714. Sibling task-breakdown spike blocked-by this
deliverable.

## 1. Purpose

The `clock` aggregate is the single component in the platform
context permitted to call the OS time-source primitives
(`mach_absolute_time` / `mach_timebase_info` for the monotonic
side; `std::chrono::system_clock::now` for the wall side). Its one
responsibility is **owning the OS time seam**: returning a
monotonic, never-decreasing, never-wrapping `Instant`; returning a
calendar-bearing `WallTime`; advertising the underlying hardware
tick so deterministic step sizing can land on an integer multiple
of it; and offering two pure-function value-object companions —
`FrameTick` (per-frame delta computation) and
`FixedStepAccumulator` (deterministic fixed-step substep counting)
— that turn raw `Instant` deltas into the form simulation /
physics / animation / present need.

What this aggregate explicitly refuses to own:

- **Phase scheduling.** The decision to *run* a system at a
  particular phase boundary is owned by `core` (specs/core, schedule
  + frame loop, sibling design `schedule-frame-design.md`). The
  clock returns numbers; `core` decides what to do with them.
- **Fixed-step driving.** `FixedStepAccumulator` is a value-object
  utility: it consumes a frame delta, returns "how many substeps to
  run" + "interpolation alpha". It does **not** invoke physics, AI,
  or any other consumer; the consumer (physics, sibling sub-epic
  #132) calls `accumulator.consume()` and runs its own loop. Per
  spec §1, platform refuses to schedule.
- **Time-of-day / calendar logic.** Day / night cycles, in-game
  clocks, save-game wall stamps used as gameplay state are not
  platform's concern. Platform exposes `WallTime` for log
  correlation only (§4.4 inv #3); gameplay code that needs a
  game-clock builds it on top.
- **Event pumping.** SDL3 event drain (§4.2, sibling #717) is the
  `Pump` aggregate. The clock has no opinion about events.
- **VBlank pacing / present timing.** `CAMetalDisplayLink` callback
  handling and the swap-interval policy live in render's present
  half (sibling sub-epic #97). The clock contributes the **wall
  read** at present time (§9.1 phase-9 row) and nothing else.
- **NTP / clock-skew detection / time-zone handling.** The wall
  clock is consumed at face value; we report what the OS reports.
  Drift detection is the e2e long-run test (§11.3.4), not a
  per-frame surface.
- **Process / signals / argv.** `Process` (§4.5) is sibling #725.
- **File watching / file I/O.** `FileWatcher` (§4.3, sibling #719)
  / `FileIo` (§4.6, sibling #721).
- **Window / surface.** Sibling #715.
- **Platform error policy.** Closed sum `platform::Error` and the
  POSIX → arm translation (§4.7, §10) is sibling #727. This
  design surfaces failures into pre-existing arms; it does not
  invent or rename them. (In practice, §10.3.4 already says
  `Clock::now` / `wall` / `native_tick` are total functions and
  return no `Result<T>` — see §10 below.)

The aggregate's SRP boundary is sharp: if the OS monotonic source's
resolution / API / guarantees shift (e.g. `mach_absolute_time`
deprecation in a future macOS, or the M-series timebase rate
changes across SoC generations), this design changes. Anything
else is out of scope.

## 2. Requirements coverage

Mapping of the harmonius requirement / design clauses that pertain
to time / clock / frame-pacing onto MVP coverage in this aggregate.
Every entry is independently re-derived; coverage sites refer to
sections of `specs/platform/SPEC.md` and to the design sections
below.

| Harmonius clause                                                                       | Glibre disposition (MVP)                                                                                                                                                                                                                          |
|----------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `threading-async.md` *monotonic time source for scheduling, never wraps, never slews*  | **Covered.** `Clock::now() -> Instant` backed by `mach_absolute_time` (§3.2). §4.4 inv #1 (monotonic non-decreasing) is the load-bearing rule; abort on regression rather than clamp.                                                              |
| `threading-async.md` *wall-clock for log timestamps and crash dumps*                   | **Covered.** `Clock::wall() -> WallTime` is the only sanctioned source (§4.4 inv #3). Log helper (`glibre::log_error`) calls `Clock::wall()`; engine code never calls `time(nullptr)` / `gettimeofday`.                                              |
| `core-runtime/game-loop.md` §Fixed Timestep Accumulator (substeps, alpha)              | **Covered, ownership split.** The numeric primitive (`FixedStepAccumulator`) lives here; the *use* (driving phase 3 physics-fixed substeps) lives in sibling `physics` (#132) and core's schedule-frame loop. §3.5.                                |
| `core-runtime/game-loop.md` *monotonic frame counter*                                  | **Covered, ownership split.** `Clock` provides `Instant`; the `frame_index : std::uint64_t` counter itself is core's concern (incremented in phase 9 per `frame-phases.md`). Platform contributes the time, not the index.                          |
| `core-runtime/game-loop.md` *delta-time per frame*                                     | **Covered.** `FrameTick::tick(Clock&) -> FrameDelta` returns `{ now, delta, wall_now }` (§3.4). Caller threads the `FrameTick` through frames; platform owns no global `last_now_` field — that would couple to scheduling.                          |
| `core-runtime/game-loop.md` *max-ticks-per-frame cap (spiral-of-death prevention)*     | **Covered.** `FixedStepAccumulator::consume(max_ticks)` clamps and returns the carry-over; remaining accumulated time is preserved for the next frame so alpha stays continuous. §3.5.                                                              |
| `core-runtime/game-loop.md` *interpolation alpha in `[0, 1]`*                          | **Covered.** `FixedStepAccumulator::alpha() -> float` returns the residual divided by step duration, clamped to `[0, 1]`. §3.5.                                                                                                                     |
| `tools/profiler.md` *high-precision time deltas for span measurement*                  | **Owned by sibling `tools` profiler context.** The profiler reads `Clock::now()` like any consumer; building the span tree is not platform's concern. We guarantee `now()` is O(1) and `Instant` resolution is at least 1 ns (§9).                  |
| `crash-reporting.md` *correlate monotonic frame-time budget overrun with wall stamp*   | **Covered.** §4.4 inv #3 makes `WallTime` the single sanctioned correlation source. The crash dump captures one `Clock::wall()` plus the most recent `Instant`; pairing these is the call site's responsibility (`obs` context, post-MVP).         |
| `core-runtime/game-loop.md` *VBlank-driven present pacing*                             | **Refused (routed to `render` / SDL3).** `CAMetalDisplayLink` callback handling lives in render's present path. Platform's only contribution at present is the second `Clock::wall()` read (§9.1 phase-9 row). No VBlank-sync handle on `Clock`.    |
| `core-runtime/game-loop.md` *deterministic replay-from-trace*                          | **Covered, by refusal.** §4.4 inv #2: `Instant` is not serializable. Replay frameworks that need recorded time use a recorded `Duration` stream from the e2e harness, not `Clock`. This design exposes no surface for replay-driven time injection. |
| `tools/profiler-test-cases.md` *per-thread monotonic-source coherence*                 | **Covered.** §4.4 inv #1 + §6 (concurrency): all reads come from the same OS counter, so any two threads' `Instant` values are comparable. No per-thread offset.                                                                                    |
| harmonius `Duration` arithmetic (signed delta type)                                    | **Covered.** `Duration = std::chrono::nanoseconds` (signed `int64_t`). `Instant - Instant -> Duration` is the only signed arithmetic at the seam (§4.4). No raw `int64_t` substitutes; the type system prevents accidental nanosecond / millisecond / tick mixing. |
| harmonius `Clock::native_tick` advertisement                                           | **Covered.** §4.4 inv #4: `Clock::native_tick() -> Duration` returns the smallest representable step on the host. On macOS / M1 this is `1 ns` (`mach_absolute_time` is sub-µs; rounding to integer ns is below noise; see §3.2).                  |
| harmonius "one global clock per process"                                               | **Covered.** §4.4 inv #5: Meyer's singleton accessor `Clock::get()`. Aggregates that need time take `Clock&`. §3.6.                                                                                                                                  |

Glibre-native requirements added beyond harmonius:

- **`FrameTick` is stateless w.r.t. `Clock`.** The previous-instant
  field lives on `FrameTick` (a value object the *caller* owns),
  not on `Clock`. This keeps `Clock::now()` re-entrant and keeps
  the singleton's mutable state at zero (matters for §6
  concurrency and §8 hot-reload).
- **`FixedStepAccumulator` is a value, not an entity.** It has no
  identity; copying it is meaningful (e.g. for branchless lookahead
  / what-if rollback in net-code). Keeping it value-typed lets the
  consumer (physics / animation) place one per fixed-step domain
  without coordinating through a global registry.
- **`Instant` is `int64_t` nanoseconds since an arbitrary epoch.**
  The internal layout is fixed (§3.3) so the `glibre-types` ABI
  hash is stable: any peer plugin that holds an `Instant` across
  hot-reload (e.g. last-known frame time) sees the same byte layout
  before and after the swap.
- **`WallTime` is `std::chrono::system_clock::time_point`.** No
  custom calendar type; the seam value is the standard library
  type so `spdlog`'s formatter can take it directly.

Coverage rule: every harmonius clause above either lands in this
design (with a coverage site) or is refused with a one-line
rationale. No silent drops.

## 3. Detailed model

### 3.1 Aggregate composition

```text
Clock  (aggregate root, process singleton)
├── mach_timebase_info_data_t   timebase_   (cached at first now() call; numer/denom)
└── (no other state — clock is otherwise stateless)
```

Companion value objects (header-side, no allocation):

```text
Instant  (value object)
└── std::int64_t   ns_   (nanoseconds since arbitrary monotonic epoch)

WallTime  (value object)
└── std::chrono::system_clock::time_point   point_

Duration = std::chrono::nanoseconds   (alias, §5.2 in SPEC)

FrameTick  (value object — caller-owned per-frame delta computer)
└── Instant   prev_   (last observed Instant; default-constructed == zero)

FixedStepAccumulator  (value object — caller-owned fixed-step bookkeeper)
├── Duration   step_         (immutable; the fixed step duration)
├── Duration   accumulator_  (residual time not yet consumed by ticks)
└── std::uint32_t  max_ticks_per_frame_  (spiral-of-death cap)
```

The `Clock` is a Meyer's singleton (§4.4 inv #5). Its only *mutable*
state is the `mach_timebase_info_data_t` cache — and that cache is
write-once: the first `now()` reads `mach_timebase_info`,
populates the cache, then the cache is read-only for the rest of
the process. Every subsequent `now()` is purely
read-cached-numer/denom + one `mach_absolute_time` syscall + one
128-bit multiply-divide. No locks, no atomics on the read path
(see §6.1 for the publication pattern).

`FrameTick` and `FixedStepAccumulator` are pure value objects:
copying them is well-defined, comparing them is well-defined, and
they own no resources that could conflict on copy. They do not
contain a `Clock&` reference; `tick(Clock&)` and `accumulate(...)`
take it as a method argument. This is deliberate — embedding a
reference would (a) prevent value-semantic copy, (b) force
lifetime coupling when none is needed, and (c) leak `Clock`'s
singleton-ness into every aggregate that holds a `FrameTick` field.

### 3.2 Monotonic source: `mach_absolute_time` (macOS / M1 baseline)

Locked from SPEC §6.6:

```cpp
namespace glibre::platform {

auto Clock::now() const noexcept -> Instant {
    static const auto info = []() noexcept -> mach_timebase_info_data_t {
        mach_timebase_info_data_t i{};
        ::mach_timebase_info(&i);
        return i;
    }();
    const auto t  = ::mach_absolute_time();
    const auto ns = static_cast<std::int64_t>(
        (__uint128_t(t) * info.numer) / info.denom);
#if !defined(NDEBUG)
    // §4.4 inv #1 defensive guard: detect monotonic regression in debug.
    // The atomic load below uses `relaxed` because the rule is
    // a *correctness* invariant, not a synchronization point —
    // any read that observes `last_seen` is correctness-preserving
    // regardless of inter-thread ordering, since the only valid
    // outcome is "no regression" or "abort".
    static std::atomic<std::int64_t> last_seen{0};
    auto prev = last_seen.load(std::memory_order_relaxed);
    while (ns < prev) {
        // Spec invariant violated by the OS. Abort, do not clamp.
        std::abort();
    }
    // Best-effort publish. Loss of update is fine; another thread
    // may have already advanced it past `ns`.
    last_seen.store(ns, std::memory_order_relaxed);
#endif
    return Instant{ns};
}

}  // namespace glibre::platform
```

Choices and their reasons:

- **`__uint128_t` widening multiply.** `mach_absolute_time` returns
  a `uint64_t` count of timebase ticks. On Apple Silicon the
  timebase numerator / denominator are typically `1` / `1` (the
  count is already nanoseconds), but the spec does not guarantee
  that — a future SoC could change the ratio. The 128-bit
  intermediate prevents overflow in the conversion at any plausible
  uptime (uptime in nanoseconds fits in `int64_t` for ~292 years).
  No platform we ship to lacks `__uint128_t`; clang on Apple Silicon
  always provides it.
- **`static const` Meyer-init for the timebase cache.** Thread-safe
  by C++17 magic-static rules, paid exactly once per process,
  non-blocking on every subsequent call. The closure form ensures
  `mach_timebase_info` is called inside the static initializer
  only.
- **Debug-only regression abort.** Release builds skip the load /
  store entirely (`!defined(NDEBUG)` guard). The OS guarantees
  monotonicity; the assertion is paranoia for catching a future
  OS bug or a broken simulator. On regression the platform aborts
  per §4.4 inv #1 — clamping would silently corrupt anything
  downstream that subtracts two `Instant` values.
- **No span-based RDTSC / `clock_gettime_nsec_np` alternative.** We
  pick exactly one source and never offer a knob; second-source
  selection would re-introduce the correlation gap that §4.4 closes.

The conversion from `mach_absolute_time` ticks to nanoseconds is
the only place the timebase ratio appears. Every consumer-side
arithmetic (`Instant - Instant`, comparisons, `FrameTick::tick`,
`FixedStepAccumulator::accumulate`) operates on the resulting
nanosecond representation. There is therefore exactly one site
where a future timebase-ratio change can break correctness, and
it is in this function.

### 3.3 `Instant` value semantics + non-serializability

Locked from SPEC §5.2:

```cpp
class Instant {
public:
    using rep = std::int64_t;            // ns since arbitrary epoch.
    constexpr Instant() noexcept = default;
    constexpr explicit Instant(rep ns) noexcept : ns_{ns} {}
    constexpr auto count() const noexcept -> rep { return ns_; }
    constexpr bool operator==(const Instant&) const noexcept = default;
    constexpr auto operator<=>(const Instant&) const noexcept = default;
private:
    rep ns_{0};
};

[[nodiscard]] constexpr auto operator-(Instant a, Instant b) noexcept -> Duration {
    return Duration{a.count() - b.count()};
}
```

Properties enforced by the type:

- **Trivially copyable, trivially destructible.** Sizeof = 8.
  Layout-stable: `int64_t` at offset 0, no padding. This is the
  layout the `glibre-types` ABI hash captures (per
  `reviews/decisions/fory-codegen.md`); peer plugins that keep an
  `Instant` field across a platform-self-reload are byte-identical
  before and after the swap (§8.2).
- **No `+` operator on `Instant`.** Only subtraction (returning
  `Duration`) and comparison are public. Adding two `Instant`s is
  meaningless (sum-of-times-since-epoch is not a time); the type
  forbids the bug. Adding a `Duration` to an `Instant` is provided
  via `operator+(Instant, Duration)` (header inline, additive — see
  §4 public surface).
- **No I/O surface.** `Instant` provides no `fmt` formatter, no
  `<<`, no Fory schema, no `to_string`. SPEC §4.4 inv #2: not
  serializable. The only way to convert is `count()` for telemetry
  payloads that explicitly wrap it; logs use `WallTime` for human
  reading and `Instant`'s `count()` only as an opaque correlation
  number.
- **`constexpr` everywhere except the `now()` source.** Tests and
  fixtures construct deterministic `Instant` values without
  reading the OS clock; this is the load-bearing primitive that
  makes the unit tests for `FixedStepAccumulator` (§11.1)
  reproducible.

### 3.4 `FrameTick` — per-frame delta computation

```cpp
namespace glibre::platform {

struct FrameDelta {
    Instant   now{};        // Captured at the call site.
    Duration  delta{};      // now - prev (zero for the very first tick).
    WallTime  wall{};       // Calendar correlation, captured atomically with now().
};

class FrameTick {
public:
    constexpr FrameTick() noexcept = default;

    // Stamps `now()` and returns the delta from the previous call
    // (or zero if this is the first call). Updates the internal
    // prev_ field. The Clock& is a method parameter, not a member.
    [[nodiscard]] auto tick(Clock& clock) noexcept -> FrameDelta {
        const auto t   = clock.now();
        const auto w   = clock.wall();
        const auto dt  = (prev_ == Instant{}) ? Duration::zero() : (t - prev_);
        prev_ = t;
        return FrameDelta{t, dt, w};
    }

    // Reset the tick history (e.g. after a long pause / suspension).
    // Next tick() returns Duration::zero() instead of the gap.
    constexpr auto reset() noexcept -> void { prev_ = Instant{}; }

    [[nodiscard]] constexpr auto last() const noexcept -> Instant { return prev_; }

private:
    Instant prev_{};
};

}  // namespace glibre::platform
```

Choices and their reasons:

- **`tick(Clock&)` returns `{ now, delta, wall }` together.**
  Capturing both `now` and `wall` in one call removes the
  correlation race between them: between two separate `Clock::now()`
  + `Clock::wall()` calls a context switch could interleave a
  wall-clock slew (NTP step) and produce a bogus delta. Reading
  them inside the same `FrameTick::tick` keeps the gap to a few
  hundred nanoseconds and documents that "the wall stamp belongs
  to *this* monotonic instant".
- **No `Clock&` member.** Reasons in §3.1. The caller owns the
  reference; `Clock::get()` is fine to pass on every call (the
  singleton accessor is itself O(1)).
- **`reset()` is explicit.** Long pauses (debugger break, OS
  suspend) produce huge deltas that would crater the
  fixed-step accumulator into ~minutes of catch-up. Callers that
  detect such a pause (e.g. on `FocusGained` after a long
  `FocusLost`) call `reset()` to clear the carry. The clock does
  not auto-detect; that policy belongs to the consumer (core's
  schedule-frame loop), not to platform.
- **`FrameDelta` is a value tuple.** Three POD fields, copyable.
  Returned by value because it is small (24 bytes) and the
  consumer almost always destructures.

### 3.5 `FixedStepAccumulator` — deterministic substep counting

```cpp
namespace glibre::platform {

class FixedStepAccumulator {
public:
    // `step` is the fixed simulation interval (e.g. 1/60 s for
    // physics, 1/30 s for AI). `max_ticks_per_frame` caps the
    // catch-up burst when a frame is unusually long, preventing
    // the spiral-of-death where each catch-up tick costs more
    // wall-time than it advances simulation. Default cap matches
    // harmonius core-runtime/game-loop.md guidance.
    [[nodiscard]] static constexpr auto with(Duration step,
                                             std::uint32_t max_ticks_per_frame = 8) noexcept
        -> FixedStepAccumulator;

    // Add a frame delta to the residual. Idempotent for zero deltas.
    constexpr auto accumulate(Duration delta) noexcept -> void;

    // Consume as many full steps as fit in `accumulator_`, capped
    // at `max_ticks_per_frame`. Returns the number of ticks the
    // consumer should run *this frame*; the residual stays in
    // `accumulator_` so alpha() is continuous.
    [[nodiscard]] constexpr auto consume() noexcept -> std::uint32_t;

    // Interpolation alpha in [0, 1]: the residual divided by step.
    // Used by the renderer to interpolate between the two most
    // recent simulation states (current + previous, which the sim
    // shadow-copies at each tick).
    [[nodiscard]] constexpr auto alpha() const noexcept -> float;

    // Clears accumulator_ to zero. Used by the consumer at long
    // pauses (mirrors FrameTick::reset). Step + cap unchanged.
    constexpr auto reset() noexcept -> void;

    [[nodiscard]] constexpr auto step()       const noexcept -> Duration { return step_; }
    [[nodiscard]] constexpr auto residual()   const noexcept -> Duration { return accumulator_; }
    [[nodiscard]] constexpr auto max_ticks()  const noexcept -> std::uint32_t { return max_ticks_per_frame_; }

private:
    constexpr FixedStepAccumulator(Duration s, std::uint32_t cap) noexcept
        : step_{s}, accumulator_{Duration::zero()}, max_ticks_per_frame_{cap} {}

    Duration       step_{};
    Duration       accumulator_{Duration::zero()};
    std::uint32_t  max_ticks_per_frame_{8};
};

}  // namespace glibre::platform
```

Semantics, locked precisely:

- **`accumulate(delta)` adds; `consume()` subtracts and returns the
  count.** The two-phase shape lets the consumer combine multiple
  delta sources (e.g. add a paused-time-debt from a future debug
  feature) before consuming. In MVP the only source is
  `FrameDelta::delta` from `FrameTick::tick`, so the typical
  consumer is `acc.accumulate(fd.delta); for (auto i = 0u; i <
  acc.consume(); ++i) physics.step();`.

- **Cap behaviour.** When `accumulator_ >= step_ *
  max_ticks_per_frame_`, `consume()` returns
  `max_ticks_per_frame_` and **subtracts only that many steps
  worth of duration** from `accumulator_`. Excess time is *not*
  retained — it is dropped. This is the deterministic
  spiral-of-death prevention: clamping the residual would corrupt
  alpha; dropping the excess accepts a one-frame "simulation
  slowdown" rather than letting the catch-up explode.

- **`alpha()` is residual / step, clamped to `[0, 1]`.** Always in
  bounds even if the consumer forgot to call `consume()` (in which
  case the accumulator may exceed `step_` and the unclamped ratio
  would be `>1`). The clamp is a safety net; the contract is "call
  `consume()` before reading `alpha()` in the same frame".

- **`max_ticks_per_frame_` minimum / maximum.** Construction with
  `cap == 0` is rejected at the call site (the constructor is
  private and `with` clamps to `1` minimum). No upper bound is
  imposed by the type; sensible values are `4`-`16`.

- **No `Clock&` member.** Reasons in §3.1. `accumulate(Duration)`
  takes the duration directly; the typical caller wires
  `acc.accumulate(frame_tick.tick(clock).delta)`.

- **All `constexpr`.** The type performs no I/O. Tests at
  `tests/platform/test_fixed_step_accumulator.cpp` construct
  arbitrary `Duration` inputs and assert exact tick counts /
  residuals (§11.1.3-§11.1.6). Golden-table verification.

The `FixedStepAccumulator` type is *not* a substitute for the
core-runtime `schedule-frame` module. Where it lives:

- The accumulator computes "how many substeps for this frame".
- The consumer (physics in MVP, animation / AI later) loops on
  the count, calling its own substep function.
- The schedule-frame loop owns the *placement* of those calls (in
  phase 3 for physics-fixed) and the *interpolation alpha
  publication* (so phase 6's cull-extract sees the up-to-date
  alpha for previous-vs-current transform interpolation).

This split honours the §1 refusal "platform refuses to schedule"
while still giving the consumer the deterministic primitive it
needs.

### 3.6 `Clock::get()` — the singleton accessor

Locked from SPEC §5.9 / §6.6:

```cpp
namespace glibre::platform {

Clock& Clock::get() noexcept {
    static Clock c;  // Meyers singleton — §4.4 inv #5.
    return c;
}

}  // namespace glibre::platform
```

Why a Meyer's singleton (and not a per-engine context handle, or
a thread-local instance, or a `Clock` field on a master `Engine`):

- **One process, one OS time source.** `mach_absolute_time` reports
  one number. Multiple `Clock` instances would be a redundant
  abstraction with identical behaviour, costing nothing but
  increasing the chance of a peer plugin keeping the wrong
  reference and getting stale state across hot-reload.
- **C++17 magic-static initialization is thread-safe.** No
  `std::once_flag`, no `pthread_once`, no construction race. The
  first thread to call `get()` initializes; everyone else gets
  the post-init reference.
- **Lifetime is the process.** No `Engine::shutdown` call to wire
  through; no risk of a later `Clock::now()` after the engine is
  torn down. The static destructor runs at program exit, which is
  after every plugin unload, which is after every consumer can
  possibly call `now()`.
- **Hot-reload-safe.** When the *platform* `.dylib` itself reloads
  (§8.3 below), the static singleton in the new image is a *new*
  `Clock` instance; the previous instance was destroyed by
  `dlclose`. The OS counter is the same OS counter, so
  `Instant::count()` values produced by the pre-swap instance and
  by the post-swap instance are still mutually comparable. §4.4
  inv #5 is preserved across the swap.

Aggregates that need time must take a `Clock&` parameter (§4.4
inv #5); they MUST NOT call `Clock::get()` themselves except in
top-level wiring code. This rule is enforced socially (review +
SPEC §4.4 inv #5) rather than mechanically; a future static-
analysis lint can promote it.

### 3.7 `WallTime` value semantics

Locked from SPEC §5.2:

```cpp
struct WallTime {
    std::chrono::system_clock::time_point point{};  // calendar-bearing; may slew.
    bool operator==(const WallTime&) const noexcept = default;
};
```

- **`std::chrono::system_clock` directly.** No custom calendar type;
  `spdlog`'s built-in formatter takes `time_point` natively, so
  log emission is one-line.
- **No comparison ordering.** `WallTime` provides `==` only,
  not `<=>`. This is deliberate: ordering wall times is
  meaningful only inside a non-slewed window; comparing across an
  NTP step produces nonsense. Consumers who *really* want to
  order wall times (e.g. log replay) explicitly access `.point`
  and accept the contract violation.
- **Slew is allowed.** `WallTime` is *not* monotonic. NTP / manual
  setting / daylight-savings transitions all change it freely.
  This is the entire reason `Instant` exists separately.
- **Single source.** §4.4 inv #3: every wall-stamped log /
  telemetry / crash record reads `Clock::wall()`. Direct
  `time(nullptr)` / `gettimeofday` / `[NSDate date]` calls are
  forbidden in engine code.

## 4. Public surface

The public surface is exactly what `specs/platform/SPEC.md` §5.2 +
§5.9 declare, plus the additive `FrameTick` /
`FixedStepAccumulator` / `FrameDelta` value objects defined in
§3.4 / §3.5 above. No new ABI symbols beyond those derivable from
the existing `Clock::now` / `wall` / `native_tick`. Every public
function is `noexcept`; none are fallible (per SPEC §10.3.4 the
clock surface returns no `Result<T>` — see §10).

```cpp
// specs/platform/clock-design.md — public seam, presented as one
// translation unit. Normative declarations live in
// `glibre/platform/platform.hpp` per SPEC §5.
#pragma once

#include <chrono>
#include <cstdint>

namespace glibre::platform {

// ---------------------------------------------------------------
// Time value objects (locked from SPEC §5.2)
// ---------------------------------------------------------------

class Instant {
public:
    using rep = std::int64_t;
    constexpr Instant() noexcept = default;
    constexpr explicit Instant(rep ns) noexcept : ns_{ns} {}
    constexpr auto count() const noexcept -> rep { return ns_; }
    constexpr bool operator==(const Instant&) const noexcept = default;
    constexpr auto operator<=>(const Instant&) const noexcept = default;
private:
    rep ns_{0};
};

using Duration = std::chrono::nanoseconds;

[[nodiscard]] constexpr auto operator-(Instant a, Instant b) noexcept -> Duration {
    return Duration{a.count() - b.count()};
}

[[nodiscard]] constexpr auto operator+(Instant a, Duration d) noexcept -> Instant {
    return Instant{a.count() + d.count()};
}

[[nodiscard]] constexpr auto operator+(Duration d, Instant a) noexcept -> Instant {
    return a + d;
}

struct WallTime {
    std::chrono::system_clock::time_point point{};
    bool operator==(const WallTime&) const noexcept = default;
};

// ---------------------------------------------------------------
// Clock (locked from SPEC §5.9)
// ---------------------------------------------------------------

class Clock {
public:
    [[nodiscard]] static auto get() noexcept -> Clock&;

    [[nodiscard]] auto now()         const noexcept -> Instant;
    [[nodiscard]] auto wall()        const noexcept -> WallTime;
    [[nodiscard]] auto native_tick() const noexcept -> Duration;

    Clock(const Clock&)            = delete;
    Clock& operator=(const Clock&) = delete;

private:
    Clock() noexcept = default;
};

// ---------------------------------------------------------------
// FrameTick + FrameDelta (this design, §3.4)
// ---------------------------------------------------------------

struct FrameDelta {
    Instant   now{};
    Duration  delta{};
    WallTime  wall{};
};

class FrameTick {
public:
    constexpr FrameTick() noexcept = default;

    [[nodiscard]] auto tick(Clock& clock) noexcept -> FrameDelta;
    constexpr auto reset() noexcept -> void { prev_ = Instant{}; }
    [[nodiscard]] constexpr auto last() const noexcept -> Instant { return prev_; }

private:
    Instant prev_{};
};

// ---------------------------------------------------------------
// FixedStepAccumulator (this design, §3.5)
// ---------------------------------------------------------------

class FixedStepAccumulator {
public:
    [[nodiscard]] static constexpr auto with(Duration step,
                                             std::uint32_t max_ticks_per_frame = 8) noexcept
        -> FixedStepAccumulator
    {
        if (max_ticks_per_frame == 0) max_ticks_per_frame = 1;
        return FixedStepAccumulator{step, max_ticks_per_frame};
    }

    constexpr auto accumulate(Duration delta) noexcept -> void;
    [[nodiscard]] constexpr auto consume() noexcept -> std::uint32_t;
    [[nodiscard]] constexpr auto alpha() const noexcept -> float;
    constexpr auto reset() noexcept -> void { accumulator_ = Duration::zero(); }

    [[nodiscard]] constexpr auto step()      const noexcept -> Duration { return step_; }
    [[nodiscard]] constexpr auto residual()  const noexcept -> Duration { return accumulator_; }
    [[nodiscard]] constexpr auto max_ticks() const noexcept -> std::uint32_t { return max_ticks_per_frame_; }

private:
    constexpr FixedStepAccumulator(Duration s, std::uint32_t cap) noexcept
        : step_{s}, accumulator_{Duration::zero()}, max_ticks_per_frame_{cap} {}

    Duration      step_{};
    Duration      accumulator_{Duration::zero()};
    std::uint32_t max_ticks_per_frame_{8};
};

}  // namespace glibre::platform
```

ABI footprint:

- `Clock::now`, `Clock::wall`, `Clock::native_tick`, `Clock::get`
  are exported symbols from the platform `.dylib`. Their
  signatures are stable across the platform-self-reload (§8.3);
  the `glibre-types-abi-hash` covers them via the standard
  middleman-types path (`reviews/decisions/fory-codegen.md`).
- `FrameTick::tick`, `FixedStepAccumulator::accumulate` /
  `consume` / `alpha` are header-inline; they cross the dylib
  boundary as inlined call sites in their consumers and
  contribute to the ABI hash via their *type layouts* only.
- `FrameDelta`, `Instant`, `WallTime`, `FixedStepAccumulator`,
  `FrameTick` layouts are recorded in the middleman type
  catalogue. A change to any of these layouts is an ABI break
  and forces a coordinated reload.

No `Result<T>` arms appear; per SPEC §10.3.4 the clock surface is
totally-defined.

## 5. Hot/cold path split

The clock has exactly two hot operations and a small cluster of
cold ones. The split is fundamental to the §9 budget.

### 5.1 Hot path — read every frame

| Operation                         | Phase     | Per-frame cost    | Inline-able?          |
|-----------------------------------|-----------|-------------------|------------------------|
| `Clock::now()`                    | 1, 9      | <0.001 ms         | Out-of-line (mach_absolute_time syscall is in libSystem; LTO can fold around the wrapper). |
| `Clock::wall()`                   | 1, 9      | <0.001 ms         | Out-of-line (`std::chrono::system_clock::now`). |
| `Instant::operator-`              | 1, 3, 6, 9| compile-time      | `constexpr`, fully inlined. |
| `FrameTick::tick(Clock&)`         | 1         | <0.001 ms         | Header-inline; calls Clock::now + Clock::wall. |
| `FixedStepAccumulator::accumulate`| 3         | nanoseconds       | `constexpr`, fully inlined. |
| `FixedStepAccumulator::consume`   | 3         | nanoseconds       | `constexpr`, fully inlined. |
| `FixedStepAccumulator::alpha`     | 6 (interp)| nanoseconds       | `constexpr`, fully inlined. |

Hot-path properties:

- **No allocation, ever.** The clock allocates zero bytes per
  frame. The 4 KiB sub-arena (§9.2 in SPEC) is a write-once cache
  for `mach_timebase_info_data_t` and rounds up to a sub-arena
  for accounting consistency only.
- **No locks.** `mach_absolute_time` is lock-free in the kernel /
  libSystem layer; the static-init of the timebase cache is the
  only synchronization, and it runs once.
- **Branch-free in release.** The debug-only regression check (§3.2)
  compiles to nothing under `NDEBUG`.

### 5.2 Cold path — calibration / one-shot

| Operation                                 | When                | Cost                         |
|-------------------------------------------|---------------------|------------------------------|
| `mach_timebase_info(&i)` cache fill       | First `now()` call  | A few hundred nanoseconds; pays itself back across the rest of the run. |
| `Clock::native_tick()`                    | Boot / introspection| One memory read of a `Duration` constant; not really cold, but called rarely (boot or telemetry only). |
| Static destructor (program exit)          | Process end         | Dominated by `dlclose` of the dylib, not the clock. |

The hot-cold split is captured in code by:

- The `now()` body has a branch on `static const auto info = ...`
  whose first execution does work and whose subsequent executions
  return the cached value without doing any work. After the first
  call, the branch is dead-eliminable by the compiler under PGO.
- `native_tick()` returns a `Duration{1}` literal on macOS / M1
  (§3.2 + SPEC §6.6); the constant is in the `.text` section of
  the dylib and compiles to a single load.
- `Clock::get()`'s body is a single `static Clock c; return c;`
  pair — the static-init runs once; subsequent calls are a
  constant memory read.

There is no warmup / pre-roll / calibration sequence other than
the implicit first-call cache fill above. We do not measure the
timebase at boot; we trust the OS's reported `numer/denom` and
abort if monotonicity is violated.

## 6. Concurrency

### 6.1 Read-side — lock-free, thread-safe, callable from any thread

`Clock::now()`, `Clock::wall()`, and `Clock::native_tick()` may
be called from any thread at any time. The MVP topology
(SPEC §6.8) creates at most three OS threads — main, FileWatcher
I/O, FileIo workers — and any of them may stamp time:

- **Main / driver thread** — calls `Clock::now()` in phase 1 and
  phase 9 (the §9.1 budgeted reads), and additionally inside any
  consumer that wants frame-bounded timing.
- **FileWatcher I/O thread** — may call `Clock::wall()` to stamp
  outgoing `FileEvent` records (informational only; the platform
  context does not currently stamp these, but the option is open).
- **FileIo worker threads** — may call `Clock::now()` to measure
  their own internal latency for tagged-allocator telemetry.
- **Any plugin's worker thread (none in MVP)** — same rules.

The thread-safety story:

1. **`mach_absolute_time` is lock-free in the kernel.** Apple
   documents it as safe to call from any thread; on Apple Silicon
   it reads the per-cluster timebase via `mrs` (a normal user-mode
   counter read). No system call in the steady state.
2. **`std::chrono::system_clock::now` is lock-free on macOS.** It
   calls `clock_gettime(CLOCK_REALTIME, ...)` which is implemented
   via the kernel commpage; no global mutex.
3. **The timebase cache is published via C++17 magic-static.** The
   first thread to enter `now()` runs the initializer; concurrent
   threads block on the C runtime's static-init guard until the
   value is ready, then never block again. Cost is paid exactly
   once per process.
4. **The debug-only regression guard uses relaxed atomics.** As
   discussed in §3.2, this is not a synchronization point — it is
   a paranoia check whose only valid outcome is "abort or move on".
   No fence is needed; concurrent threads racing on `last_seen`
   may each abort if they observe a regression, or each not-abort
   if they don't, which is the correct behaviour.

There is no "current frame's time" cached on the clock — caching
that would impose a thread-affinity (the cache must be updated by
exactly one thread, which forces the same thread to read it for
correctness). Instead, every consumer that wants the
"frame-start instant" stamps it at phase 1 and threads the
`FrameDelta` through the systems that need it (this is what
core's schedule-frame loop does, per `schedule-frame-design.md`).

### 6.2 `FrameTick` and `FixedStepAccumulator` — single-thread per instance

These are value objects; they are **not** thread-safe by design.
A `FrameTick` instance has a `prev_ : Instant` field that
`tick(...)` mutates; concurrent calls from two threads would race.
A `FixedStepAccumulator` has the same shape with `accumulator_`.

Discipline: each instance is owned by exactly one thread. In
practice:

- The schedule-frame loop owns one `FrameTick` (driver thread).
- The physics aggregate owns one `FixedStepAccumulator` for the
  fixed-step loop (driver thread, phase 3).
- Future animation / AI plugins each own their own
  `FixedStepAccumulator` (driver thread, their respective phases).

No coordination is needed because nothing in the engine demands
cross-thread sharing of these instances; the underlying clock is
shared, but the per-domain bookkeeping is not.

If a future consumer needs a thread-shared accumulator (e.g. an
audio mixer thread driving its own fixed step), the **right
answer is "give that thread its own instance"**, not "make
`FixedStepAccumulator` atomic". Atomicity would force every
consumer to pay for synchronization that only one consumer needs;
the value-object pattern keeps the cost where it belongs.

### 6.3 Hot-reload barrier

Phase 8 (hot-reload) runs on the driver thread between
render-submit and present (`frame-phases.md`). Worker threads
that may call `Clock::now()` are guaranteed to be in *one* of:

- Inside libSystem's `mach_absolute_time` call — atomic,
  pre-emptable, completes in nanoseconds and observes the
  pre-swap text address. This is fine.
- Suspended at the FileWatcher / FileIo drain barrier (§8.3 of
  SPEC). Drain waits for in-flight work to complete before swap;
  any worker thread that was about to call `now()` either
  completed its call before drain or has not yet started. No
  worker holds an internal pointer into platform's text segment
  across a swap point.
- Idle on a condvar / kqueue wait. These don't read time.

The clock therefore needs no special hot-reload synchronization —
the swap is safe by virtue of (a) `now()` being a leaf call and
(b) the pre-existing drain semantics from `FileWatcher` /
`FileIo`. See §8 for the post-swap re-entry.

## 7. Persistence + ABI

### 7.1 Persistence (Fory schemas)

Per SPEC §7 (Persistence & Schemas) and the table at line 1644:

| Type           | Serialized? | Rationale                                                                                                  |
|----------------|-------------|------------------------------------------------------------------------------------------------------------|
| `Clock`        | No          | Process singleton; no state to persist.                                                                    |
| `Instant`      | No          | §4.4 inv #2: no calendar meaning; cross-process / cross-run comparisons are forbidden.                     |
| `WallTime`     | No          | Wall stamps in logs use spdlog's formatter, not a Fory schema. `WallTime` is sourced fresh each call.       |
| `Duration`     | No (as a clock-aggregate type) | `Duration` *as a primitive* is widely serializable (`int64_t` ns); but persistence of `Duration` is the *consumer's* schema concern, not platform's. The platform contributes none. |
| `FrameDelta`   | No          | Transient per-frame value; not retained across frames or stored to disk.                                   |
| `FrameTick`    | No          | Transient per-system value; reconstructed on each plugin load.                                             |
| `FixedStepAccumulator` | **Maybe — see §7.2** | Could be retained across hot-reload to preserve in-flight residual; case discussed below. |

### 7.2 Telemetry payloads — a non-persistence carve-out

When a telemetry / log record carries an `Instant::count()` value
as an opaque correlation number (e.g. "system X took N
nanoseconds; here is the event's monotonic stamp so the analyst
can correlate it with another span"), that is **not persistence
in the Fory sense** — it is an opaque `int64_t` field in some
peer context's schema (most likely `obs` post-MVP). The platform
contributes zero schemas; the consumer that wants the field
declares it as `int64_t`, not as `Instant`.

This rule prevents accidental `Instant`-to-disk: if a peer
context tries to declare a `glibre::platform::Instant` field in
its Fory schema, the Fory codegen step (per `fory-codegen.md`)
fails because no `.fory.cpp` file is registered for `Instant`.

### 7.3 ABI (middleman types)

Per `reviews/decisions/fory-codegen.md`, the middleman dylib
(`glibre-types.dylib`) holds the canonical layout of every type
that crosses a plugin boundary. Layouts contributed by this
design:

| Type                       | Size  | Alignment | Contents                                  |
|----------------------------|-------|-----------|-------------------------------------------|
| `Instant`                  | 8     | 8         | `int64_t ns_`                             |
| `WallTime`                 | 8     | 8         | `std::chrono::system_clock::time_point`   |
| `Duration` (alias)         | 8     | 8         | `std::chrono::nanoseconds`                |
| `FrameDelta`               | 24    | 8         | `Instant` + `Duration` + `WallTime`       |
| `FrameTick`                | 8     | 8         | `Instant prev_`                           |
| `FixedStepAccumulator`     | 24    | 8         | `Duration step_` + `Duration accumulator_` + `uint32_t max_ticks_per_frame_` (+ 4 bytes implicit padding) |

`Clock` itself is not in the table — it is a singleton with
private construction; no peer plugin can hold a `Clock` value, only
a `Clock&` whose target is the static instance.

The `glibre-types-abi-hash` includes these six types. A change to
any of them — e.g. switching `Instant::ns_` to `int128_t`, or
adding a field to `FixedStepAccumulator` — bumps the hash and
forces a coordinated reload of every plugin that holds one of
these types. Per `reviews/decisions/plugin-abi.md` this is the
correct behaviour.

### 7.4 Persistence refusal

`Clock::wall()` produces values that *look* persistable (a
`time_point` is a number of ticks since the Unix epoch). The
platform refuses to provide a Fory schema anyway, because:

- Persisting wall stamps invites the consumer to assume monotonic
  wall time across save/load — false on slew.
- Persisting wall stamps in a save game is a gameplay concern
  (e.g. "last save time"), and gameplay code can declare its own
  `int64_t` field via Fory directly.
- The error-model rule "errors are constructed at the site they
  happen" has a parallel: schemas are owned by the context that
  serializes their values, not by the source that produced them.

Consequence: every plugin that wants to log / persist time stamps
declares the type in its own schema. Platform's contribution is
just the read.

## 8. Hot-reload

The clock is the simplest hot-reload case in the platform context.
SPEC §8.1 already records this: "Yes — pass-through" for
peer-plugin swaps, and "monotonic origin is OS-owned; the
in-process `Clock` accessor is rebuilt by Q::register but
`Instant::count()` values remain comparable across the swap".
This section refines the contract.

### 8.1 Peer plugin reloads

When a peer plugin (render, content, ecs, tools, …) reloads at
phase 8:

1. The peer plugin's `glibre_plugin_drain` runs while the platform
   `.dylib` is unchanged. Any time-related state the peer kept
   (e.g. an `Instant last_frame_start_` field) is its own
   responsibility; the standard drain → swap → migrate → resume
   protocol moves those bytes through the middleman type
   catalogue.
2. The peer plugin's `glibre_plugin_register` re-acquires its
   `Clock&` reference by calling `Clock::get()`. The reference
   refers to the *same* static instance as before (platform
   `.dylib` did not move).
3. Any `Instant` value the peer kept across the swap is still
   valid: `Instant::count()` is the absolute nanosecond delta
   from the same arbitrary-but-stable epoch
   (`mach_absolute_time`'s arbitrary start; not changed by a peer
   plugin's `dlclose`).
4. Any `FrameTick` instance the peer kept across the swap is
   valid: its `prev_` field is an `Instant`, which (per #3) is
   still meaningful. The next `tick(...)` call returns the delta
   from before the swap to right after — usually a small number,
   but legitimately the gap of the swap itself if the consumer
   stamps frame-start through this `FrameTick`.
5. Any `FixedStepAccumulator` instance the peer kept across the
   swap is valid: its `step_` and `accumulator_` are `Duration`
   nanoseconds, which carry their meaning byte-for-byte. The next
   `accumulate(delta)` continues from the pre-swap residual.

In short, **clock state migrates trivially because every value
type is layout-stable and the OS source is reload-stable**. No
`migrate(...)` body is needed. Per SPEC §8.1, `migrate(...) =
empty`.

### 8.2 Platform-self-reload (the platform `.dylib` itself swaps)

When platform is the outgoing plugin P at phase 8 (see SPEC §8.3
for the broader rules):

- **Drain step.** The drain function does **not** touch the
  clock — there is no in-flight clock work to wait for; every
  `Clock::now()` / `wall()` call is a leaf that completes within
  a few hundred nanoseconds. SPEC §8.3 states this explicitly:
  "Drain MUST NOT touch `Clock` — the OS monotonic source is
  reload-stable and re-reading `now()` after the swap returns a
  value that compares correctly against any pre-swap `Instant`."
- **Swap step.** Standard. The new platform `.dylib` is loaded;
  its static-init runs, including the lazy-init of the
  `Clock::get()` singleton's storage (the actual
  `mach_timebase_info` call has not happened yet — it deferred
  to first `now()` call).
- **Migrate step.** Empty. There are no bytes to reshape.
- **Resume step.** `Q::glibre_plugin_register` does **not** need
  to call `Clock::get()` itself; consumers will re-acquire on
  demand. The first `now()` call from any thread runs the
  static-init of the timebase cache — paid once, just as on
  first-ever boot.

The continuity guarantee — "an `Instant` produced by P's instance
compares correctly against an `Instant` produced by Q's instance"
— rests on:

- Both instances call `mach_absolute_time`, which is process-
  global (the timebase counter does not reset on `dlclose`).
- Both instances apply the same `numer/denom` conversion, because
  `mach_timebase_info` returns the same numerator / denominator
  for the entire process lifetime.
- The arbitrary epoch of `mach_absolute_time` is per-boot; it
  does not move within a process. So `count()` values are
  consistent across the swap.

This is why §4.4 inv #5 ("one `Clock` per process") is robust to
hot-reload: the *type* `Clock` exists in two `.dylib` images
(P's and Q's, briefly during the swap), but the *resource*
(the OS counter) is one and the same.

### 8.3 What hot-reload cannot break

The §11 test plan includes a "long-run hot-reload preservation"
check (§11.2.2) that verifies:

- `FixedStepAccumulator::residual()` immediately before swap and
  immediately after swap (with no `accumulate(...)` call in between)
  is byte-identical.
- `FrameTick::last()` immediately before and after swap is
  byte-identical.
- A re-issued `Clock::now()` returns a value `>=` any pre-swap
  `Instant` observed (monotonicity preserved).

These are unit-style assertions inside the hot-reload e2e
fixture; they fail loudly on any future regression that wires a
non-trivial clock state into the swap path.

### 8.4 Refusal cases (none owned by clock)

The clock contributes no refusal cases to phase 8. SPEC §8.4
enumerates platform-owned refusal cases and the clock is absent
from that list — by construction. If a future change introduces a
clock-owned in-memory cache with version skew, the new refusal
case is registered there and a new §8 entry is added. Today,
there are zero.

## 9. Performance

### 9.1 Per-frame budget cell

Locked from SPEC §9.1 (the row reproduced verbatim):

> | `Clock` (§4.4) | 1, 9 | 0.001 | 0.001 | `mach_absolute_time()` is O(1); read at frame start (sim) and at present (submit). Two reads / frame; <0.001 ms each. Listed as 0.001/0.001 to keep the cell sum honest at the precision the gate measures. |

Refinement from this design:

- The "two reads / frame" assumes the schedule-frame loop calls
  `Clock::now()` once at phase 1 and once at phase 9 (matching
  the `frame-phases.md` schedule). `Clock::wall()` is called the
  same way — once at phase 1 (for the `FrameDelta::wall` field)
  and once at phase 9 (for the present-stamp).
- The `FrameTick::tick` call inside phase 1 expands to one
  `Clock::now()` + one `Clock::wall()` + a few register ops. The
  budget already reflects this.
- Consumer calls inside other phases (e.g. a profiler stamping
  spans inside phase 7) are the consumer's budget, not platform's.
  Platform's row covers the *frame-loop's* fixed two-read pair.
- `FixedStepAccumulator::accumulate` / `consume` / `alpha` cost
  nanoseconds and are billed to phase 3 (physics) — not to
  platform. Platform's job is the `Duration`-arithmetic primitive;
  the consumer pays for the call.

Empirical expectation on macOS / M1 baseline:

- `mach_absolute_time` on Apple Silicon is a single `mrs`
  (move-from-system-register) instruction reading
  `CNTVCT_EL0`. The wall is `commpage_gettimeofday` — a couple
  of memory reads from the kernel commpage. Together, well under
  100 ns per call in the steady state.
- Two pairs of these per frame: ~400 ns of clock work per frame,
  i.e. **0.0004 ms** — comfortably under the 0.001 ms cell.
- The 0.001 ms cell is therefore an *upper bound*, not a target.
  The §9.6 SPEC note ("absorbed by the 0.099 / 0.049 reserved
  tail") confirms: any actual value below the gate's precision is
  fine.

### 9.2 Heap ceiling

Locked from SPEC §9.2:

> | `Clock` (§4.4) | 4 KiB | One `mach_timebase_info_data_t` cache. Effectively zero; bucketed into the reserved tail. |

The 4 KiB is rounded up to a sub-arena page for accounting
consistency. Actual usage:

- `mach_timebase_info_data_t` is `8` bytes
  (`uint32_t numer; uint32_t denom`).
- The static `Clock c;` singleton is 0 bytes (empty class; though
  C++ guarantees `sizeof(Clock) >= 1`, the storage is in the
  dylib's `.bss` and is not counted against the platform sub-arena
  per `reviews/decisions/perf-budget.md` — sub-arenas are heap,
  not BSS).
- Consumer-side `FrameTick` (8 B) and `FixedStepAccumulator`
  (24 B) instances live in the *consumer's* sub-arena. Platform
  contributes none.
- The debug-only `last_seen : std::atomic<int64_t>` is
  `defined(NDEBUG)`-gated out in shipping; debug builds carry an
  extra 8 bytes inside the dylib's `.bss` (still not heap).

Effective platform-clock heap: **0 bytes per frame, 0 bytes
total**. The 4 KiB bucket is a rounding artifact. SPEC §9.2's
"effectively zero" line is correct.

### 9.3 Allocation discipline

The clock has zero per-frame allocation by construction:

- `now()` allocates nothing.
- `wall()` allocates nothing.
- `native_tick()` allocates nothing.
- `FrameTick::tick` allocates nothing.
- `FixedStepAccumulator::accumulate` / `consume` / `alpha`
  allocate nothing.

Caller-side allocations (e.g. constructing a `FixedStepAccumulator`
on the heap) are caller-billed. The construction itself is O(1).

### 9.4 CI gate fixture

The platform-cell gate fixture (SPEC §9.4) already exercises the
clock as part of the "idle frame budget assert":

> The fixture runs a 600-frame loop calling `Clock::now()`,
> `Pump::drain()`, `Clock::wall()` (and nothing else from
> platform). Asserts:
> - p99 driver-thread platform CPU ≤ 0.005 ms per frame (the
>   idle floor: 2x `Clock` + empty `Pump::drain` short-circuit).

This design adds two micro-benchmarks under
`platform/test/perf/test_clock_bench.cpp` (Catch2 `BENCHMARK`
blocks):

1. **`now()` throughput.** Measures p50 / p99 latency of
   `Clock::now()` over 1e6 iterations on a hot loop. Asserts p99
   ≤ **100 ns**. Localizes a regression in the
   `mach_absolute_time` wrapper to this aggregate without needing
   the full S1 fixture to bisect.
2. **`FixedStepAccumulator::consume` correctness + perf.**
   Measures p99 latency over 1e6 iterations of
   `accumulate(step + step/2); consume()`. Asserts p99 ≤ **20 ns**
   (pure arithmetic; should be near-zero).

Both benchmarks are macOS / M1 only and run on `macos-26-m1` CI
runners alongside the existing platform fixtures.

### 9.5 Refusals (out of platform-clock §9 scope)

- **Phase-9 GPU present cost.** The 0.05 ms phase-9 budget covers
  the SDL3 → CAMetalLayer present, the `CAMetalDisplayLink`
  trampoline, and *the second `Clock::wall()` read*. The clock's
  per-call cost (<0.001 ms) is included; the SDL3 + Metal
  components are not platform-clock's responsibility. (See SPEC
  §9.1 paragraph after the table.)
- **Profiler span cost.** The profiler may call `Clock::now()`
  many times per frame to stamp span starts / ends. Each call is
  <0.001 ms; *aggregate* profiler overhead is `tools` budget, not
  platform's.
- **Long-run wall-clock drift.** Drift accumulates across hours;
  the platform cell is per-frame. Drift is the e2e long-run test
  (§11.3.4), not a cell entry.

## 10. Failure modes

The clock surface is **totally defined**: every public function
returns a plain value, never `Result<T>`. SPEC §10.3.4 records
this:

> `Clock::now`, `wall`, `native_tick` are total functions returning
> plain values; they cannot fail. The only failure mode in §4.4 is
> the monotonic-regression abort (`Clock` aborts the process on
> detected non-monotonic behavior), which never returns at all.

This design refines the failure model below.

### 10.1 Catastrophic failures (process abort)

| Failure                                              | Trigger                                                              | Severity | Recovery       |
|------------------------------------------------------|----------------------------------------------------------------------|----------|----------------|
| **`ClockMonotonicRegression`**                       | `now()` returns a value `<` a previously-observed value (debug-only check). | fatal    | abort process. |

The abort is intentional: a non-monotonic `Instant` corrupts every
downstream subtraction, including the `FixedStepAccumulator`
delta, the per-frame budget, and any profiler span. Clamping
would silently mask the bug; aborting localizes it to the OS
counter where it actually originates.

This failure mode does not appear in the §4.7 closed sum
(`platform::Error` variants) because no `Result<T>` is ever
returned — the process terminates before the call site can
inspect anything. SPEC §10.3.4 confirms.

### 10.2 Bounded-but-rare failures (none)

`mach_absolute_time` and `std::chrono::system_clock::now` cannot
fail in any documented way on macOS. They never return an error,
never block, never throw. The brief's mention of `ClockReadFailed`
+ `CalibrationFailed` is **refused for MVP** — there is no
observable trigger on the macOS / M1 baseline that would surface
either, and inventing the variant would be speculative.

If a future port (Linux's `clock_gettime`, Windows's
`QueryPerformanceCounter`) introduces a documented failure path,
the §4.7 closed sum gains an arm and `Clock::now()`'s signature
changes to `Result<Instant>` at that point — a coordinated SPEC
amendment, not a silent surface change. Today, the surface is
total.

### 10.3 Caller-side misuse (not failures, but worth listing)

| Misuse                                                                 | Effect                                                                       | Mitigation                                                            |
|------------------------------------------------------------------------|------------------------------------------------------------------------------|-----------------------------------------------------------------------|
| Constructing a peer `Clock` instance.                                  | Compile error — copy / move constructors deleted, default ctor private.       | Enforced by the type system.                                          |
| Calling `Clock::get()` from a non-top-level site.                       | Works, but violates §4.4 inv #5 social rule.                                  | Code review + future static-analysis lint.                            |
| Sharing a `FrameTick` across threads.                                   | Data race; UB.                                                                 | Documented in §6.2; not enforced at compile time. ThreadSanitizer catches it in test. |
| Calling `accumulate(negative_duration)`.                                | Reduces the accumulator; `consume()` may return 0 unexpectedly.                | Documented as the consumer's responsibility (deltas should be `>= 0`). The accumulator does not validate; spending compile-time enforcement on this is over-engineering. |
| Forgetting to call `consume()` for many frames.                          | `accumulator_` grows; subsequent `consume()` returns `max_ticks_per_frame_` and drops the rest. | Documented; this is the spiral-of-death cap behaviour, not a bug. |
| Constructing `FixedStepAccumulator::with(Duration::zero(), ...)`.       | `consume()` would loop forever (residual never reaches step). The accumulator does not validate. | Caller-side responsibility; passing `Duration::zero()` is a programming error. A debug assertion in `with(...)` may be added if it becomes a recurring bug; not in MVP. |
| Mixing `Instant`s from different processes.                              | `Instant::count()` values from different processes have different epochs; subtraction returns nonsense. | §4.4 inv #2 (non-serializable) means there's no API to do this. The only path is via `count()` + reconstruction; that is by definition out of contract. |

### 10.4 Refusals (out of clock §10 scope)

- **GPU device-lost.** Render's concern (`render::Error`).
- **Plugin reload refusal.** Core's concern (`core::Error`).
  Clock survives every reload (§8) so contributes no refusal arm.
- **File I/O failures.** `FileIo`'s concern.
- **Determinism / replay divergence.** Engine concern; clock is
  intrinsically non-deterministic across runs (each boot has a
  fresh `mach_absolute_time` epoch), and the engine's replay
  framework records `Duration` streams from the e2e harness, not
  `Instant` values.

## 11. Test plan

The clock aggregate is exercised by three test tiers: **unit**
tests for value-object correctness, **integration** tests for the
OS seam, and **e2e / long-run** for drift and hot-reload
preservation.

### 11.1 Unit tests (Catch2, `tests/platform/`)

All unit tests are runnable without OS time access — they
construct `Instant` values directly via the `Instant{int64_t}`
constructor, advance them in `constexpr` fashion, and assert the
exact arithmetic outcome.

**11.1.1 `test_instant_arithmetic`**

- `Instant{} == Instant{}`.
- `Instant{1000} - Instant{500} == Duration{500}` (ns).
- `Instant{500} + Duration{300} == Instant{800}`.
- `Duration{300} + Instant{500} == Instant{800}` (commutativity).
- `Instant{a}` `<=>` `Instant{b}` matches the ordering of `a` vs `b`
  for arbitrary `int64_t` pairs (parameterized over a small
  golden table).

**11.1.2 `test_walltime_value_semantics`**

- Default-constructed `WallTime` equals another default-
  constructed `WallTime` (both at epoch).
- `WallTime{tp1} == WallTime{tp1}` and `!=` `WallTime{tp2}`.
- `WallTime` is trivially copyable (`std::is_trivially_copyable_v`).

**11.1.3 `test_fixed_step_accumulator_zero_delta`**

- `acc.accumulate(Duration::zero()); acc.consume()` returns `0`.
- `acc.alpha()` returns `0.0f` after the no-op.

**11.1.4 `test_fixed_step_accumulator_partial_step`**

- `acc.accumulate(step / 2); acc.consume()` returns `0`.
- `acc.alpha()` returns `0.5f` (within ULP).
- `acc.residual() == step / 2`.

**11.1.5 `test_fixed_step_accumulator_full_step`**

- `acc.accumulate(step); acc.consume()` returns `1`.
- `acc.alpha()` returns `0.0f` (residual is exactly zero after
  consuming).
- `acc.residual() == Duration::zero()`.

**11.1.6 `test_fixed_step_accumulator_carry`**

- `acc.accumulate(step + step / 4); acc.consume()` returns `1`.
- `acc.alpha()` returns `0.25f`.
- A subsequent `acc.accumulate(step / 2); acc.consume()` returns
  `0` (residual is `0.75 step`, still under the threshold).
- A further `acc.accumulate(step / 2); acc.consume()` returns
  `1` (residual now `1.25 step`, consumes one, leaves `0.25
  step`).

**11.1.7 `test_fixed_step_accumulator_max_ticks_cap`**

- `acc = with(step, /*max_ticks=*/4)`.
- `acc.accumulate(step * 10); acc.consume()` returns `4`.
- `acc.residual() == Duration::zero()` — the *excess* `step * 6`
  is **dropped** (deterministic spiral-of-death prevention,
  §3.5 cap behaviour).
- `acc.alpha()` returns `0.0f`.

**11.1.8 `test_fixed_step_accumulator_min_max_ticks_clamp`**

- `with(step, /*max_ticks=*/0)` returns an accumulator whose
  `max_ticks() == 1` (constructor clamps the zero to one).

**11.1.9 `test_fixed_step_accumulator_alpha_clamp`**

- After unbalanced `accumulate` + no `consume`, `acc.alpha()`
  is clamped to `<= 1.0f` even if `residual_ > step_`.

**11.1.10 `test_frame_tick_first_call_zero_delta`**

- `FrameTick ft; auto fd = ft.tick(test_clock);` returns
  `fd.delta == Duration::zero()` on the very first call.
- `ft.last() == fd.now`.

**11.1.11 `test_frame_tick_reset`**

- After two `tick(...)` calls, `ft.reset()` makes the third call
  return `Duration::zero()` again.

**11.1.12 `test_frame_tick_value_semantics`**

- Copying a `FrameTick` produces an independent instance with the
  same `prev_`.
- The two copies, ticked separately, advance independently.

These unit tests use a `test_clock` fake (see §11.4) that
returns deterministic, advancing `Instant` values. The fake is
not part of the public surface; it is a test-only helper under
`tests/platform/support/`.

### 11.2 Integration tests (Catch2, `tests/platform/integration/`)

**11.2.1 `test_clock_real_now_monotonic_short`**

- Run a tight loop of 100k `Clock::now()` calls.
- Assert that each return is `>=` the prior return.
- Assert that the gross delta from first to last is `> 0` and
  `< 1 second` (sanity bound).

**11.2.2 `test_clock_hot_reload_preservation`**

- Open the e2e hot-reload harness with the platform `.dylib` as
  the swap target.
- Capture `Clock::now()` immediately before drain.
- Trigger a platform-self-reload swap.
- Capture `Clock::now()` immediately after resume.
- Assert post-swap `now()` is `>=` pre-swap `now()` and that the
  delta is `< 100 ms` (the swap should not take longer; if it
  does, the harness fails for an unrelated reason).
- Also: capture a `FixedStepAccumulator` instance's `residual()`
  before swap, hold the value across the swap (the consumer's
  responsibility per §8.1), and assert byte-equality on the
  other side.

**11.2.3 `test_clock_wall_correlation`**

- Stamp `(now1, wall1) = (Clock::now(), Clock::wall())`.
- Sleep 1 ms.
- Stamp `(now2, wall2)`.
- Assert `now2 - now1` is in `[0.5 ms, 5 ms]` (sleep tolerance).
- Assert `wall2 - wall1` is in `[0.5 ms, 5 ms]`.
- Assert the two deltas agree to within `± 100 µs` (sanity check
  on the OS reporting both clocks consistently in the absence of
  NTP slew during the test window).

**11.2.4 `test_native_tick_is_at_least_1_ns_resolution`**

- `Clock::native_tick() <= Duration{1}` — the macOS / M1
  baseline reports nanosecond resolution per §3.2.

### 11.3 E2E / long-run tests (`e2e/perf/`)

**11.3.1 `test_idle_frame_budget` (existing fixture, SPEC §9.4)**

- Already includes the clock as part of the 600-frame idle loop;
  this design adds no new e2e assertion to it.

**11.3.2 `test_clock_now_microbench` (new, see §9.4)**

- p99 `Clock::now()` ≤ 100 ns over 1e6 iterations on `macos-26-m1`.

**11.3.3 `test_fixed_step_accumulator_consume_microbench` (new)**

- p99 `accumulate(...) + consume()` ≤ 20 ns over 1e6 iterations.

**11.3.4 `test_clock_long_run_drift` (long-run, opt-in)**

- Run a 60-second wall-time loop on the test machine.
- Stamp `(monotonic_delta, wall_delta)` at t=0 and t=60.
- Assert `|monotonic_delta - wall_delta| < 1 ms` over the 60-
  second window. This catches gross OS-clock-skew bugs while
  tolerating the small drift that NTP slewing introduces. Excluded
  from PR-blocking CI; runs on the nightly bench.

### 11.4 Test support: `test_clock` fake

```cpp
// tests/platform/support/test_clock.hpp
//
// A test-only Clock substitute that advances by an explicit Duration
// per `tick()` call. Used by FrameTick / FixedStepAccumulator unit
// tests so they do not depend on `mach_absolute_time` for determinism.

namespace glibre::platform::test {

class TestClock {
public:
    constexpr explicit TestClock(Instant start = Instant{}) noexcept : at_{start} {}

    [[nodiscard]] auto now()  const noexcept -> Instant  { return at_; }
    [[nodiscard]] auto wall() const noexcept -> WallTime { return wall_; }
    [[nodiscard]] auto native_tick() const noexcept -> Duration { return Duration{1}; }

    auto advance(Duration d) noexcept -> void;

private:
    Instant   at_{};
    WallTime  wall_{};
};

}  // namespace glibre::platform::test
```

`TestClock` is structurally compatible with `Clock&` consumers via
templated functions (e.g. `template <class C> auto tick(C& clock)`)
where the test fixture passes `TestClock` and the production code
passes `Clock`. We deliberately do **not** introduce a virtual
`IClock` base class — virtual dispatch would impose a runtime cost
on every consumer for the sake of testability, and the templated-
parameter pattern works at zero cost. (This mirrors the
window-surface-design approach to hot-cold seam testability.)

In MVP, the only consumer that uses templated-clock indirection is
the `FrameTick::tick` test fixture. Production code calls
`tick(Clock::get())` directly.

### 11.5 Test summary

| Tier         | File                                           | Asserts                                  |
|--------------|------------------------------------------------|------------------------------------------|
| Unit         | `tests/platform/test_instant.cpp`              | 11.1.1 / 11.1.2                          |
| Unit         | `tests/platform/test_fixed_step_accumulator.cpp` | 11.1.3 – 11.1.9                        |
| Unit         | `tests/platform/test_frame_tick.cpp`           | 11.1.10 – 11.1.12                        |
| Integration  | `tests/platform/integration/test_clock_seam.cpp` | 11.2.1 – 11.2.4                        |
| E2E / perf   | `e2e/perf/test_idle_frame_budget.cpp`          | (existing) idle floor includes clock     |
| E2E / perf   | `platform/test/perf/test_clock_bench.cpp`      | 11.3.2 / 11.3.3                          |
| E2E / nightly| `e2e/nightly/test_clock_long_run_drift.cpp`    | 11.3.4                                   |

These tests close the SPEC §11 acceptance criterion #358
("platform/clock: monotonic non-decreasing + wall correlation
(§4.4 inv #1, #2, #3, #4) — pts:2") and are the basis for the
sibling `task-breakdown-platform-clock-detailed` issue's plan
output.

## 12. Open questions

- [OPEN] **Should `FrameTick` and `FixedStepAccumulator` live on
  `glibre::platform::` or under a dedicated `glibre::platform::time::`
  sub-namespace?** Both are *value-object utilities* layered over
  `Clock`, not aggregates of platform's OS-seam responsibility per
  se. The brief asked for them inside this aggregate, which is
  what this design delivers. Re-opens if the sibling
  task-breakdown carve-out (issue #TBD) discovers that the
  consumer-side ergonomics prefer a separate header
  (`<glibre/platform/time.hpp>`). Owner: sibling task-breakdown
  spike for clock.
- [OPEN] **Do we want a typed `Result<Instant>` arm for the
  Linux / Windows ports?** As §10.2 notes, today's macOS surface
  is total. The Linux `clock_gettime(CLOCK_MONOTONIC_RAW, ...)`
  call can return `EFAULT` / `EINVAL` (the former implausible,
  the latter only on a kernel that does not support
  `CLOCK_MONOTONIC_RAW`). Windows's
  `QueryPerformanceCounter` has historical reliability issues on
  pre-Win8 hardware; the macOS-26 / Apple Silicon baseline does
  not constrain MVP, but the ports will. Re-opens when the Linux
  port spike (post-MVP) lands. Owner: post-MVP cross-platform
  porting initiative.
- [OPEN] **Should `FixedStepAccumulator` validate
  `step != Duration::zero()` at construction?** Today it does
  not. A `Duration::zero()` step would cause `consume()` to never
  return non-zero (residual never reaches the threshold), which
  is a programming error but is silent. Adding a debug-only
  assertion is cheap; the value-object discipline makes a
  release-side check feel heavyweight. Re-opens if a CI failure
  ever traces back to this misconfiguration. Owner: clock
  task-breakdown reviewer.
- [OPEN] **VBlank correlation handle on `Clock`?** The brief
  mentioned an "optional VBlank sync handle" as a possible
  detail. The MVP refuses it (per §1) — `CAMetalDisplayLink`
  callbacks are render's seam, and exposing a VBlank instant
  through `Clock` would re-introduce render-platform coupling.
  Re-opens if a future requirement (e.g. variable-refresh-rate
  pacing in render's plan #97) demonstrates that the timestamp
  needs to flow through the clock seam rather than render's own
  interface. Owner: render present-pacing task-breakdown.
- [OPEN] **`Instant::count()` exposure as the persistence
  escape-hatch.** §7.2 documents that telemetry payloads carry
  `Instant::count()` as opaque `int64_t`. There is a structural
  question — should we type-wrap that as
  `OpaqueMonotonicNs : int64_t` to discourage cross-process
  comparison? Today, callers obtain the `int64_t` directly. Re-
  opens when the `obs` (observability) context lands and the
  field shape is normalized. Owner: future `obs` SPEC.
