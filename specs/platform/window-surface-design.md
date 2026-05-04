# platform — Detailed Design: window-surface aggregate

> Detailed design for the `Window` / `Display` / `Surface` /
> `LogicalSize` / `PhysicalSize` / `DpiScale` aggregate declared in
> `specs/platform/SPEC.md` §4.1. Refines §4.1, §5.5, §5.6, §6.1, §6.2,
> §6.3 (window/event seams that pertain to surface lifetime), §8.3
> (platform self-reload window/surface clauses), §9.1 (window/surface
> per-frame budget), §9.2 (window/surface heap sub-arena), and §10
> (window/surface refusal cases).
>
> Cites `reviews/decisions/error-model.md`,
> `reviews/decisions/frame-phases.md`,
> `reviews/decisions/perf-budget.md`,
> `reviews/decisions/plugin-abi.md`,
> `reviews/decisions/hot-reload-protocol.md`,
> `reviews/decisions/fory-codegen.md`. Does not introduce any new public
> surface beyond the §5 stubs in `specs/platform/SPEC.md`; deviations
> from those records require an amendment spike, not an in-place edit.
>
> Harmonius prior art (`harmonius/docs/requirements/platform/
> window-display.md`, `harmonius/docs/design/platform/windowing.md`)
> cited as research input only — every conclusion below was
> independently re-derived per `PHILOSOPHY.md`.

Refs: spike #715 — `[SPIKE] design-platform-window-surface-detailed`.
Parent: #714. Sibling task-breakdown spike blocked-by this deliverable.

## 1. Purpose

The `window-surface` aggregate is the single component in the platform
context permitted to call SDL3's window-server APIs and to drive the
SDL3 → `CAMetalLayer` bridge. Its one responsibility is **owning the
lifetime of an OS window plus the GPU-presentable layer attached to
it**: create the `SDL_Window` with the metal/high-density flags, attach
a `CAMetalLayer` through the `SDL_Metal_CreateView` bridge, ingest
`Resized` / `DpiChanged` / `DisplayChanged` events from the platform
`Pump` to refresh its size + DPI snapshot, vend a non-owning `Surface`
handle to render, and tear everything down in destruction-safe order.

What this aggregate explicitly refuses to own:

- **Event pumping** — `Pump::drain` (SPEC §4.2) is a sibling aggregate
  (#717). The window-surface aggregate is a *consumer* of typed
  `WindowEvent`s, never an SDL3 poll site.
- **File watching** — `FileWatcher` (SPEC §4.3) is sibling aggregate
  #719.
- **File I/O** — `FileIo` / `IoToken` (SPEC §4.6) is sibling
  aggregate #721.
- **Clock** — monotonic / wall time (SPEC §4.4) is sibling
  aggregate #723.
- **Process** — argv / env / signals (SPEC §4.5) is sibling
  aggregate #725.
- **Platform error policy** — closed sum `platform::Error` and the
  POSIX / SDL3 / NSException → arm translation (SPEC §4.7, §10) is
  sibling aggregate #727. This design surfaces failures into
  pre-existing arms; it does not invent or rename them.
- **GPU rendering** — command encoding, shader compilation, swapchain
  scheduling, presentation-mode policy, HDR conversion, `CAMetalLayer`
  pixel format selection beyond defaults. The aggregate owns the
  `CAMetalLayer*` lifetime; render owns its content (per
  `specs/platform/SPEC.md` §1, §10 refusals).
- **Mid-frame swap** — `Surface` may not be invalidated mid-frame.
  This is enforced by `frame-phases.md` (phase 8 runs after
  render-submit, before present) and `specs/platform/SPEC.md` §8.4
  refusal P1.

The aggregate's SRP boundary is sharp: if SDL3's window-creation flags
shift, the AppKit `CAMetalLayer` attachment rule changes, the DPI
rounding policy moves, the Apple display hot-plug delivery contract
mutates, or the `Surface` opacity rule (`void*` outside platform,
`MTL::Layer*` only on the render side) needs to bend, this design
changes. Anything else is out of scope.

## 2. Requirements coverage

Mapping of harmonius window-display requirements
(`harmonius/docs/requirements/platform/window-display.md`,
`R-14.1.*`) and the windowing-design clauses
(`harmonius/docs/design/platform/windowing.md`) onto MVP coverage in
this aggregate. Every entry is independently re-derived; coverage
sites refer to sections of `specs/platform/SPEC.md` and to the design
sections below.

| Harmonius clause                                                            | Glibre disposition (MVP)                                                                                                                                                                                                                                                                  |
|-----------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **R-14.1.1** create / resize / minimize / maximize / restore / destroy      | **Covered.** `Window::open` (SPEC §5.6) creates; `Window::request_resize` requests; minimize / restore / focus arrive as `WindowEvent` variants drained from the `Pump`. `~Window` destroys. §3.4–§3.5.                                                                                  |
| **R-14.1.2** exclusive fullscreen / borderless fullscreen / windowed switch | **Partial — windowed + borderless-fullscreen only in MVP.** `WindowDesc::fullscreen` boolean maps to SDL3 borderless-fullscreen (`SDL_WINDOW_FULLSCREEN`). Exclusive-mode video-mode change deferred (post-MVP plan). Refusal documented in §3.3 as `Unsupported` for exclusive-only paths. |
| **R-14.1.3** enumerate displays + refresh / DPI / HDR                       | **Covered.** `Display` value object with `id` / `bounds` / `scale` / `refresh_hz` / `hdr_capable`; queried per call via `Window::display()` (SPEC §5.6). Hot-plug arrives as `WindowEvent::DisplayChanged`. §3.6.                                                                          |
| **R-14.1.4** per-monitor DPI; respond to DPI change                         | **Covered.** `WindowEvent::DpiChanged { window, scale }` (SPEC §5.4); the aggregate's DPI snapshot is refreshed before the event is enqueued (SPEC §6.3 last paragraph). §3.7.                                                                                                            |
| **R-14.1.5** presentation modes (immediate / FIFO / mailbox)                | **Refused (routed to `render`).** Per SPEC §3 refusal "GPU rendering, presentation modes (R-14.1.5, R-14.1.6) → render". Platform sets only the `CAMetalLayer.contentsScale`; render selects display sync.                                                                                |
| **R-14.1.6** HDR output enable + tonemap                                    | **Refused (routed to `render`).** Platform reports `Display::hdr_capable` for plumbing; HDR enable + colour-space selection live in render's pipeline-state code.                                                                                                                         |
| **R-14.1.8** RawWindowHandle exposing native handles                        | **Covered, collapsed.** Per SPEC §3 collapse "`raw-window-handle` trait → opaque `Surface` value": single `Surface` handle vending a `void*` `CAMetalLayer*` consumed by render. macOS-only; no multi-platform variant. §3.8.                                                             |
| **R-14.1.9** bounded-async window-event channel; never drop                 | **Covered, ownership delegated.** The `EventQueue<WindowEvent>` is a sibling aggregate (`Pump`/`EventQueue`, #717); window-surface is a *producer side* through the bridge, but the queue + drain semantics belong to that sibling. SPEC §4.2 inv #3 "exactly once, never dropped" applies. |
| **R-14.1.10** per-window DpiPolicy at creation                              | **Refused (collapsed).** MVP exposes one DPI policy: `SDL_WINDOW_HIGH_PIXEL_DENSITY` always on, `LogicalSize` semantics by default. SPEC §3 collapse: `LogicalSize` + `PhysicalSize` are the load-bearing types; per-window opt-out is post-MVP.                                          |
| **R-14.1.11** single event poller drives both window and input              | **Owned by sibling `Pump` aggregate (#717).** This design only specifies that the window-surface aggregate is a *consumer* of `WindowEvent`s, not the poller. The single-poller invariant lives in SPEC §4.2 inv #1.                                                                      |
| **R-14.1.12** `LogicalSize` / `PhysicalSize` / `Point` / `Rect`             | **Covered + collapsed.** `LogicalSize` + `PhysicalSize` only. `Point` / `Rect` routed to `geometry` per SPEC §3 collapse. `to_physical(LogicalSize, DpiScale) -> PhysicalSize` is the single conversion site (SPEC §5.2).                                                                  |

Harmonius design clauses (`windowing.md`):

| Design clause                                                            | Glibre disposition                                                                                                                                                                                                                                            |
|--------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Main-thread sole-owner of OS APIs                                        | **Covered.** SPEC §4.1 inv #2 "Window operations are main-thread-only"; this design enforces via debug assertion + release `Unsupported` (§3.10).                                                                                                              |
| Bounded-channel event delivery                                           | **Owned by sibling Pump aggregate.** This design names the produce-side coupling but never touches the queue.                                                                                                                                                 |
| `SurfaceHandle` separate from `Window`                                   | **Collapsed.** SPEC §4.1: `Window` is the aggregate root, `Surface` is a non-owning *value* vended only via `Window::surface()`. The lifetime is one. Splitting would lie about ownership; the bridge file's separation (`surface/bridge.mm`) is enough for SRP. |
| `cfg`-gated per-OS modules (Win32 / objc2-app-kit / x11rb / wayland)     | **Refused (collapsed).** SPEC §3 collapse "Windowing back-ends → SDL3-only". macOS-only MVP through one `bridge.mm`.                                                                                                                                          |
| Three thread roles (main / workers / render)                             | **Refused for MVP.** Single-threaded driver; render thread reads `Surface::raw_layer()` from the same main thread in MVP. Concurrency rules in §6 cover the post-MVP render-thread case.                                                                       |

Glibre-native requirements added beyond harmonius:

- **No Obj-C++ outside `bridge.mm`.** Engine code stays pure
  C++23/26 (CLAUDE.md "No Obj-C++ in engine code"); this aggregate is
  the sole anchor for that rule.
- **`Surface` is a pure value type.** No virtual methods, no PIMPL.
  The `void*` pointer carries identity; up-cast to `MTL::Layer*` is
  render-side only. This keeps the aggregate ABI-safe across plugin
  reload (§7).
- **Per-window outstanding-surface counter** for the §8 mid-frame
  refusal case P1 (SPEC §8.4): the `Window::Impl` tracks how many
  `Surface` values are live this frame, decremented when render
  signals "done with this surface". This mechanism is the
  load-bearing reason the platform self-reload protocol can refuse
  cleanly instead of corrupting in-flight command buffers.

Coverage rule: every harmonius clause above either lands in this
design (with a coverage site) or is refused with a one-line rationale.
No silent drops.

## 3. Detailed model

### 3.1 Aggregate composition

```text
Window  (aggregate root, owned by platform)
├── WindowId                  id_                 (numeric, monotonic per process)
├── SDL_Window*               sdl_window_         (opaque to engine; bridge-internal)
├── surface::detail::LayerHandle layer_handle_    (CAMetalLayer* + SDL_MetalView*)
├── LogicalSize               cached_logical_     (snapshot, refreshed by Pump events)
├── PhysicalSize              cached_physical_    (snapshot, == round(logical * dpi))
├── DpiScale                  cached_dpi_         (snapshot, refreshed by Pump events)
├── DisplayId                 cached_display_id_  (snapshot, refreshed on DisplayChanged)
├── std::atomic<std::uint32_t> outstanding_surfaces_  (in-flight reference count)
└── bool                      closed_pending_     (set by CloseRequested ingest)
```

The `Window` is the aggregate root; the `Surface` is a value vended
through `Window::surface()` (SPEC §5.6). The four cached fields
together form the `Window`'s public-query state and are always
mutually consistent (SPEC §4.1 inv #4: `PhysicalSize = round(LogicalSize
* DpiScale)`).

`Display` is an *entity* in the SPEC's terms but its on-disk shape is
a value object (`Display` struct in SPEC §5.6) — the aggregate does
not store a long-lived `Display` instance, it re-queries SDL3 each
call to `Window::display()`. This honours SPEC §4.1 inv #5: "`Display`
snapshots are immutable" — every consumer gets the state at the
moment of query.

`Surface` is the §4.1 entity that crosses into render. It has no PIMPL
and no allocation: its body is `(WindowId owner, void* layer)` only.
Construction is `friend`-restricted to `Window`; consumers receive
move-only `Surface` values (SPEC §5.5).

### 3.2 `WindowDesc` (creation parameters, locked from SPEC §5.6)

```cpp
struct WindowDesc {
    eastl::string_view title{};
    LogicalSize       size{1280, 720};
    bool              resizable{true};
    bool              fullscreen{false};   // borderless-fullscreen only in MVP.
};
```

Field semantics:

- **`title`** — borrowed string view. The aggregate copies into an
  internal arena (window-surface sub-arena, §9 / SPEC §9.2 1 MiB
  cell) before the SDL3 call so the original storage may go out of
  scope.
- **`size`** — initial `LogicalSize`. Validated at boundary: zero
  width or height → `Unsupported`. SDL3 receives `width * dpi` /
  `height * dpi` after the initial DPI is queried (§3.4 step 4).
- **`resizable`** — maps to `SDL_WINDOW_RESIZABLE`.
- **`fullscreen`** — maps to `SDL_WINDOW_FULLSCREEN` (borderless on
  macOS). Exclusive-mode is not exposed in MVP (R-14.1.2 partial).

The struct is a POD aggregate and passed by `const&` into
`Window::open` (SPEC §5.6). It does not survive past `open` — the
`Window` only retains the snapshot fields it needs.

### 3.3 Internal Objective-C++ bridge handle

`engine/platform/src/surface/bridge.hpp` (locked from SPEC §6.2):

```cpp
namespace glibre::platform::surface::detail {

struct LayerHandle {
    void* layer{nullptr};            // CAMetalLayer*, opaque outside this TU.
    void* sdl_metal_view{nullptr};   // SDL_MetalView, opaque outside this TU.
};

[[nodiscard]] auto create_metal_view(void* sdl_window) noexcept
    -> Result<LayerHandle>;

auto destroy_metal_view(LayerHandle) noexcept -> void;

[[nodiscard]] auto query_layer_metrics(void* layer) noexcept
    -> Result<eastl::pair<PhysicalSize, DpiScale>>;

}  // namespace glibre::platform::surface::detail
```

Notes:

- **`bridge.mm` is the sole `.mm` file in the engine** (SPEC §6.2,
  CLAUDE.md "No Obj-C++ in engine code"). The `void*` pointers
  outside `bridge.mm` are opaque; only `bridge.mm` casts to
  `id<MTLDevice>` / `CAMetalLayer*` / `SDL_MetalView`.
- **`create_metal_view`** internally calls `SDL_Metal_CreateView`
  on the supplied `SDL_Window*`, then `SDL_Metal_GetLayer` to get
  the `CAMetalLayer*`. It sets `layer.contentsScale` to match the
  window's `backingScaleFactor` (the macOS-side DPI convention).
- **`destroy_metal_view`** releases the `SDL_MetalView` (which
  releases its retained `CAMetalLayer`); idempotent on a default
  `LayerHandle{}`.
- **`query_layer_metrics`** reads `layer.drawableSize` and
  `layer.contentsScale` after a DPI / display change so the
  aggregate's cached snapshot stays SPEC §4.1 inv #4 consistent.
- All three functions return `Result<T>` — errors are
  `Error::IoFailure { OsCode }` for `NSException`-converted failures
  (SPEC §10 OS-side translation), `Error::Unsupported` for
  pre-condition violations (null window).

The bridge owns no observable state. `LayerHandle` is held inside
`Window::Impl`; the bridge is a stateless mapping (SPEC §6.2 last
paragraph).

### 3.4 Creation sequence — `Window::open(WindowDesc)`

Executed on the main thread (SPEC §4.1 inv #2). Steps:

1. **Validate `WindowDesc`.** `desc.size.width >= 1 &&
   desc.size.height >= 1`. Failure → `Unsupported`. Return.
2. **Allocate `WindowId`.** Monotonic `std::atomic<std::uint32_t>`
   counter inside the aggregate; never reused within one process. The
   counter survives platform self-reload via the
   `WindowSurvival` middleman type (SPEC §8.5.1) so peer plugins can
   continue to dereference the same id.
3. **Copy `title` into the window-surface sub-arena.** The arena
   (§9, SPEC §9.2 1 MiB cell) provides a stable backing buffer; the
   `eastl::string_view` stored on the SDL3 window-state singleton
   points into it. Failure (sub-arena exhausted) →
   `IoFailure { OsCode { ENOBUFS } }` mapped to
   `core::Error::OutOfBudget` per perf-budget.md (§9 below).
4. **Query the primary `Display`** via SDL3 to obtain initial
   `DpiScale` (`SDL_GetDisplayContentScale`). This precedes
   `SDL_CreateWindow` so the initial physical size is correct on the
   first frame; without this step a HiDPI display would create the
   window at half the requested logical size and immediately resize.
5. **`SDL_CreateWindow`** with flags
   `SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY |
   (desc.resizable ? SDL_WINDOW_RESIZABLE : 0) |
   (desc.fullscreen ? SDL_WINDOW_FULLSCREEN : 0)`. Width / height
   are passed as `LogicalSize` values; SDL3 uses logical units when
   the high-pixel-density flag is set. Failure → translate
   `SDL_GetError()` through the §10 mapping seam (sibling #727); most
   common arms are `Unsupported` (no display) or `IoFailure`.
   `dlclose`-equivalent: `nullptr` requires no cleanup.
6. **Attach `CAMetalLayer`** via
   `surface::detail::create_metal_view(sdl_window)`. Failure →
   `IoFailure`; clean up the SDL window with `SDL_DestroyWindow`
   before returning the error. (This is the only step that
   needs explicit rollback before the `Window` exists; subsequent
   steps are infallible.)
7. **Refresh cached snapshot.** Call
   `surface::detail::query_layer_metrics(layer)` to get the actual
   `PhysicalSize` + `DpiScale` (SDL3 may round differently than the
   query in step 4). Compute `cached_logical_` from
   `cached_physical_ / cached_dpi_`; if they disagree with the
   request by more than one logical pixel (rounding tolerance), use
   the OS-reported value — the OS wins.
8. **Store outstanding-surface counter at zero**, `closed_pending_
   = false`.
9. **Return `Window`** by move into the caller. The aggregate's
   pimpl `Impl` is allocated from the window-surface sub-arena; the
   pimpl pointer is the only heap reference the public `Window`
   holds.

Step 1–4 are pre-OS-touch and refusal there is cheap. Step 5 is the
single SDL3 boundary; step 6 is the single bridge boundary; step 7
makes the cached snapshot consistent with reality before the public
`Window` becomes observable. The flag combination in step 5 is the
load-bearing collapse: requesting both `METAL` and
`HIGH_PIXEL_DENSITY` together is what makes the
`SDL_Metal_CreateView` call in step 6 produce a `CAMetalLayer` that
already has the right `contentsScale`.

### 3.5 Destruction sequence — `~Window`

Executed on the main thread (SPEC §4.1 inv #2; a destructor on the
worker thread is a contract violation — debug builds assert,
release builds skip the SDL3 / bridge calls and leak the OS handles
loudly via `log_error(Unsupported, error)`).

Steps:

1. **Wait for `outstanding_surfaces_ == 0`.** This is a contract
   precondition, not a runtime block: render is required to release
   every `Surface` value before the `Window` goes out of scope.
   Debug builds assert; release builds proceed anyway and rely on
   the bridge's reference counting to keep the layer alive until
   render drops its last reference. The mid-frame refusal case (SPEC
   §8.4 P1) is the prevention rule for the only case where this
   matters in practice.
2. **`surface::detail::destroy_metal_view(layer_handle_)`.**
   Idempotent if `layer_handle_` is default-constructed. Releases
   the `SDL_MetalView`, which releases the `CAMetalLayer*`.
3. **`SDL_DestroyWindow(sdl_window_)`.** Idempotent if `nullptr`.
4. **Return the pimpl `Impl` to the window-surface sub-arena.**
   The arena's free-list reclaims the slot.

Move-from `Window` leaves the source in a "destructible-only"
state: `sdl_window_ = nullptr`, `layer_handle_ = {}`,
`outstanding_surfaces_ = 0`. Calling any non-destructor method on a
moved-from `Window` is UB; debug builds assert.

### 3.6 `Display` query — `Window::display()`

Returns a `Display` snapshot (SPEC §5.6 struct). Each call:

1. Read `cached_display_id_` (atomic; updated by Pump on
   `WindowEvent::DisplayChanged` per §3.7 step 4 below).
2. Call `SDL_GetDisplayBounds` /
   `SDL_GetDisplayContentScale` /
   `SDL_GetDisplayPrimaryRefreshRate` /
   `SDL_GetWindowFullscreenMode` against the cached id. SDL3
   queries are O(1) on macOS (cached internally by SDL3 since the
   display-server callback that updated them).
3. Read `hdr_capable` from
   `SDL_GetDisplayProperty(display, SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN)`.
4. Construct a `Display` value and return.

SDL3 query failure (rare; happens between display unplug and
`DisplayChanged` drain) → `NotFound`. The aggregate does not cache
the resulting `Display` value — caching is the consumer's choice
(SPEC §6.3 last paragraph).

### 3.7 Resize / DPI / Display ingestion

The aggregate is a *consumer* of `WindowEvent`s drained by the
sibling `Pump` aggregate (#717). The `Pump` invokes
`Window::Impl::ingest(WindowEvent)` synchronously on the main
thread between `SDL_PollEvent` returning the event and the typed
event being enqueued in the `EventQueue<WindowEvent>` (SPEC §6.3
last paragraph: "the snapshot is updated before the event is
visible to the engine").

Per-variant ingest:

- **`Resized { window, logical, physical }`** — write
  `cached_logical_ = logical`, `cached_physical_ = physical`. Verify
  `physical == to_physical(logical, cached_dpi_)`; on mismatch (SDL3
  reported inconsistent values, rare), take SDL3's `physical` as
  authoritative and recompute `cached_logical_ = round(physical /
  cached_dpi_)`. Bridge layer's `drawableSize` updates
  asynchronously inside Core Animation; the next
  `Window::physical_size()` reader sees the post-update value.
- **`DpiChanged { window, scale }`** — update `cached_dpi_ = scale`.
  Call `surface::detail::query_layer_metrics(layer_handle_.layer)`
  to refresh `cached_physical_` (the `CAMetalLayer.drawableSize`
  has been updated by AppKit by this point in the SDL3 dispatch).
  Recompute `cached_logical_ = round(cached_physical_ /
  cached_dpi_)` if AppKit's reported scale differs. The order
  "update DPI snapshot, then enqueue the typed event" is the SPEC
  §6.3 last-paragraph rule that makes
  `Window::physical_size()` post-`DpiChanged` consistent.
- **`DisplayChanged { window, display }`** — update
  `cached_display_id_`. The next `Window::display()` call
  re-queries SDL3 against the new id (§3.6). The DPI / size cached
  values are *not* updated here; SDL3 follows up with a separate
  `DpiChanged` event when the new display has a different scale.
- **`Minimized` / `Restored` / `FocusGained` / `FocusLost`** — no
  cached state changes; these affect rendering policy in render's
  budget, not platform's.
- **`CloseRequested`** — set `closed_pending_ = true`. The flag is
  read by the engine's main loop to decide when to start shutdown;
  this aggregate does not auto-close. (The engine may also choose
  to ignore the request and clear the flag; SDL3 has no equivalent
  of "ignore close" beyond not destroying the window.)

The ingest path is single-threaded (Pump owns it), so no atomics
are required on the cached snapshot fields except where readers
on other threads exist. In MVP only the main thread reads them; in
post-MVP render-thread builds the snapshot fields become
`std::atomic<...>` with release on ingest / acquire on read (§6).

### 3.8 `Surface` vending — `Window::surface()`

Returns `Result<Surface>`. Implementation:

1. **Verify `layer_handle_.layer != nullptr`.** Should always hold
   between `open` and `~Window`; null indicates a moved-from
   `Window` or post-`destroy` access. Failure → `Unsupported`
   (programming error; debug builds assert).
2. **Increment `outstanding_surfaces_`** (relaxed
   `fetch_add`). This counter is the load-bearing piece of the
   §8.4 P1 refusal: a non-zero value means render is mid-frame
   with this layer, and platform self-reload must refuse.
3. **Construct `Surface{ id_, layer_handle_.layer }`** via the
   private ctor (friend access from `Window`). The `void*` carried
   in the `Surface` is the same pointer the bridge produced in
   step 6 of §3.4; render's `reinterpret_cast<MTL::Layer*>` is
   the only place the type is recovered.
4. **Return the `Surface` by move.** Move-only; copy is deleted
   (SPEC §5.5).

`Surface` destruction (move-from or end-of-scope) decrements the
`Window`'s `outstanding_surfaces_` counter (relaxed
`fetch_sub`). The decrement is wired through the `Surface`
destructor; the `Surface` carries a back-pointer to its owning
`Window::Impl` (one extra `void*` in the value type — still no
heap allocation). The back-pointer is *non-owning* and is invalid
after `~Window`; calling `~Surface` after `~Window` is UB and
debug builds assert through a poisoned-pointer pattern.

The `Surface` is not a singleton: a `Window` may vend more than
one `Surface` value across its lifetime (e.g. one per frame, or
one held across multiple frames by the render thread). The
counter accommodates this.

Render's contract: hold `Surface::raw_layer()` only for the
duration of one frame's command-buffer recording (frame-phases
phase 7). Drop the `Surface` value before `Phase::HotReload`
(phase 8) begins, so the counter goes to zero and platform
self-reload is admissible. This contract is asserted by the e2e
fixture in §11.

### 3.9 Resize math — `to_physical(LogicalSize, DpiScale)`

Single conversion site (SPEC §4.1 inv #4, SPEC §5.2). Definition:

```cpp
[[nodiscard]] inline auto to_physical(LogicalSize l, DpiScale d) noexcept
    -> PhysicalSize {
    // d.value > 0 invariant from DpiScale::make (SPEC §5.2).
    const float fw = static_cast<float>(l.width)  * d.value;
    const float fh = static_cast<float>(l.height) * d.value;
    return PhysicalSize{
        .width  = static_cast<std::uint32_t>(fw + 0.5f),
        .height = static_cast<std::uint32_t>(fh + 0.5f),
    };
}
```

Rounding rule: half-to-even is *not* used; banker's rounding would
introduce a one-pixel oscillation at exactly `0.5f` boundaries
which surfaces as a flicker on resize. Plain round-half-up
(`+0.5f` then truncate) is deterministic and matches AppKit's
`backingScaleFactor` math.

Inverse direction (`PhysicalSize` → `LogicalSize`) is **not**
exposed publicly. It is only used inside the aggregate for resize
ingestion (§3.7), where SDL3 supplies both values and the
aggregate trusts SDL3's value over an inverse computation.

### 3.10 Main-thread enforcement

SPEC §4.1 inv #2 makes every public `Window` operation main-thread-
only. Implementation:

1. The aggregate captures the main thread's `pthread_self()` at
   `Window::Impl` construction time (or, in static-library form,
   at platform `init`). It is stored in a TLS sentinel.
2. Every public method body asserts `pthread_self() ==
   main_thread_` in debug builds (`GLIBRE_ASSERT`).
3. Release builds promote the assertion to a refusal: the method
   returns `Unsupported` instead of touching SDL3 / the bridge.
   Mutating methods like `request_resize` short-circuit before
   any SDL3 call; `~Window` cannot return — release builds fall
   through and leak the OS handles loudly.
4. `Surface::raw_layer()` and `Surface::window()` are *not*
   restricted: render reads them from the render thread by
   design. The pointer is read-only and the layer's content is
   render's responsibility.

The main-thread enforcement is what makes `OS thread != main
thread` a refusable condition rather than a UB.

## 4. Public surface

The `specs/platform/SPEC.md` §5.5 (`Surface`) and §5.6 (`Window`,
`Display`, `WindowDesc`) stubs are authoritative. This section
restates them with per-method behaviour annotations; nothing here
adds a public type.

### 4.1 Types (locked from SPEC §5)

```cpp
namespace glibre::platform {

struct WindowId  { std::uint32_t value{0}; };
struct DisplayId { std::uint32_t value{0}; };

struct LogicalSize  { std::uint32_t width{1};  std::uint32_t height{1}; };
struct PhysicalSize { std::uint32_t width{1};  std::uint32_t height{1}; };

struct DpiScale { float value{1.0f}; static auto make(float) noexcept -> Result<DpiScale>; };

[[nodiscard]] auto to_physical(LogicalSize, DpiScale) noexcept -> PhysicalSize;

struct WindowDesc {
    eastl::string_view title{};
    LogicalSize       size{1280, 720};
    bool              resizable{true};
    bool              fullscreen{false};
};

struct Display {
    DisplayId     id{};
    PhysicalSize  bounds{};
    DpiScale      scale{};
    std::uint32_t refresh_hz{60};
    bool          hdr_capable{false};
};

class Surface {
public:
    Surface()                                = default;
    Surface(const Surface&)                  = delete;
    Surface& operator=(const Surface&)       = delete;
    Surface(Surface&&) noexcept              = default;
    Surface& operator=(Surface&&) noexcept   = default;
    ~Surface();  // decrements Window's outstanding_surfaces_.

    [[nodiscard]] auto raw_layer() const noexcept -> void*;     // CAMetalLayer*
    [[nodiscard]] auto window()    const noexcept -> WindowId;
    [[nodiscard]] auto valid()     const noexcept -> bool;
};

class Window {
public:
    [[nodiscard]] static auto open(const WindowDesc&) noexcept -> Result<Window>;

    Window(Window&&) noexcept;
    Window& operator=(Window&&) noexcept;
    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;
    ~Window();

    [[nodiscard]] auto id() const noexcept -> WindowId;

    [[nodiscard]] auto surface() noexcept -> Result<Surface>;

    [[nodiscard]] auto request_resize(LogicalSize) noexcept -> Result<void>;
    [[nodiscard]] auto request_close()              noexcept -> Result<void>;

    [[nodiscard]] auto logical_size()  const noexcept -> LogicalSize;
    [[nodiscard]] auto physical_size() const noexcept -> PhysicalSize;
    [[nodiscard]] auto dpi_scale()     const noexcept -> DpiScale;
    [[nodiscard]] auto display()       const noexcept -> Display;
};

}  // namespace glibre::platform
```

### 4.2 Per-method behaviour (this design's annotations)

- **`Window::open(WindowDesc)`** — runs §3.4's nine-step creation
  sequence. Synchronous, main-thread-only, no SDL3 init responsibility
  (the Pump aggregate or an init plugin owns
  `SDL_Init(SDL_INIT_VIDEO)`; this aggregate assumes the subsystem is
  already initialized and refuses with `Unsupported` if it is not).
- **`Window::id()`** — read of `id_`. Trivial; no allocation, no
  syscall. Safe from the render thread (the value is immutable after
  construction).
- **`Window::surface()`** — runs §3.8's four steps. Cost is one
  atomic `fetch_add` plus a value construction. Main-thread-only
  in debug; release allows render-thread access *only* if render
  has explicitly opted in via a future post-MVP API (§12 [OPEN]).
  In MVP: main thread only.
- **`Window::request_resize(LogicalSize)`** — calls
  `SDL_SetWindowSize(sdl_window_, size.width, size.height)`. The
  actual resize arrives as `WindowEvent::Resized` from the Pump
  (SPEC §6.3 second bullet). SDL3 may refuse the resize on a
  fullscreen window; that surfaces as `SDL_GetError() != ""` →
  `IoFailure { OsCode }`. Validates `size.width >= 1 &&
  size.height >= 1` first (`Unsupported` on zero).
- **`Window::request_close()`** — synthesises an
  `SDL_EVENT_WINDOW_CLOSE_REQUESTED` via `SDL_PushEvent` so the
  close path goes through the same Pump fan-out as user-initiated
  close (SPEC §6.3 third bullet). The aggregate does not destroy
  the window; the engine's main loop handles `closed_pending_`.
- **`Window::logical_size()` / `physical_size()` / `dpi_scale()`** —
  reads of the cached snapshot fields. Atomic-load in the
  post-MVP multi-thread scenario; relaxed read in MVP. Always
  consistent per SPEC §4.1 inv #4.
- **`Window::display()`** — runs §3.6's four-step query. SDL3
  query cost is O(1) on macOS (cached in SDL3); ~µs.
- **`Surface::raw_layer()`** — returns the `void*` directly.
  Safe from any thread. Render up-casts to `MTL::Layer*` exactly
  once per frame.
- **`Surface::window()`** — returns the owning `WindowId`. Safe
  from any thread.
- **`Surface::valid()`** — `layer != nullptr`. Safe from any thread.
- **`Surface::~Surface`** — decrements `outstanding_surfaces_` on
  the owning `Window::Impl` (relaxed `fetch_sub`). Safe from the
  render thread *only because* the back-pointer is non-owning and
  the SPEC contract says the `Surface` may not outlive its
  `Window`.

### 4.3 Public ABI surface (cross-plugin)

Per `PHILOSOPHY.md` §11 ("Public plugin ABI surfaces never expose
`std::` containers or `eastl::` containers — they cross the
boundary as POD spans / handles only") and `plugin-abi.md`:

- `WindowId`, `DisplayId`, `LogicalSize`, `PhysicalSize`,
  `DpiScale`, `Display` — all POD-like aggregates of scalars.
  Stable layout; can cross plugin ABIs as values.
- `Surface` — a non-owning pair of `WindowId` + `void*` plus a
  back-pointer for the destructor. The back-pointer is platform-
  internal; render reads `raw_layer()` and `window()` only. The
  type is move-only and is *not* serialized — it never crosses
  Fory (§7).
- `Window` is a pimpl class; its `Impl*` is platform-internal and
  never crosses any boundary. Cross-plugin code holds a
  `WindowId` and dereferences through the platform API.

The platform aggregate exports nothing through `extern "C"` of its
own — the platform context is itself a plugin (SPEC §8.1.2) and
its registration goes through `glibre_plugin_register` (plugin-
abi.md §"Registration Entry-Point Signature"). The window-surface
aggregate is one of seven the plugin's `register` brings online.

## 5. Hot/cold path split

Window-surface operations partition cleanly:

| Path  | Operation                                             | Frequency                | Budget                                         |
|-------|-------------------------------------------------------|--------------------------|------------------------------------------------|
| Cold  | `Window::open` (§3.4)                                 | Once per window          | ~5–20 ms (SDL3 + AppKit + bridge); not budgeted on hot path. |
| Cold  | `~Window` (§3.5)                                      | Once per window          | <5 ms; main-thread.                             |
| Cold  | `Window::request_resize` / `request_close`            | Per user input           | <0.1 ms (single SDL3 call); main-thread.        |
| Cold  | Pump-side ingest (§3.7) — Resized/DpiChanged/etc       | On user gesture          | Bounded by §9 cell; main-thread, in phase 1.    |
| Hot   | `Window::id` / `logical_size` / `physical_size` / `dpi_scale` | Per frame, per consumer | <100 ns (cached snapshot read).                 |
| Hot   | `Window::surface` (§3.8)                              | Per frame                | <100 ns (atomic increment + value ctor).        |
| Hot   | `Surface::raw_layer` / `window` / `valid` / `~Surface` | Per frame, render-thread | <50 ns each; no syscall.                        |
| Hot   | `Window::display` (§3.6)                              | Tools / inspector refresh | <0.05 ms (SDL3 cached query).                   |

Hot-path invariants:

- **No SDL3 calls** on per-frame paths except `Window::display()`.
  Rendering reads cached snapshot only. SDL3 query latency is
  reserved for the cold path (resize / DPI / display events).
- **No bridge calls** on per-frame paths. `surface::detail::*` is
  invoked only on creation (§3.4), DPI ingest (§3.7), and
  destruction (§3.5). The hot path holds the `void* layer`
  directly.
- **No allocation** on per-frame paths. `Window::Impl` is allocated
  once at `open`; `Surface` is a value type. The window-surface
  sub-arena only grows on `Window::open`.

Cold-path invariants:

- **`SDL_CreateWindow` is the only window-server call in `open`.**
  The bridge's `create_metal_view` is one AppKit call
  (`SDL_Metal_CreateView`); the rest is SDL3-internal.
- **`bridge.mm` is touched on cold paths only** (open, destroy,
  DPI ingest). Other paths read the layer pointer without crossing
  the Obj-C++ boundary.

The split is what makes the platform's per-frame budget honest at
zero ms for the window/surface aggregate (SPEC §9.1 row "`Window` /
`Surface` (§4.1) | idle | 0.000 | 0.000"): there is no per-frame
work in steady state.

## 6. Concurrency

Window-surface operations are **main-thread-only with one exception:
the render thread reads `Surface::raw_layer()` and the cached
snapshot fields**. This section makes the rules precise.

### 6.1 Threading rules

1. **Main-thread-only operations.** `Window::open`,
   `Window::request_resize`, `Window::request_close`,
   `Window::display`, `~Window`, `Window::Impl::ingest` (Pump-driven),
   and `Window::surface` (vending). SDL3 + AppKit require this; the
   aggregate enforces via §3.10 sentinel.
2. **Any-thread reads.** `Window::id`, `Window::logical_size`,
   `Window::physical_size`, `Window::dpi_scale`,
   `Surface::raw_layer`, `Surface::window`, `Surface::valid`. These
   are all reads of fields that are either immutable
   (`id`, `Surface::layer`) or atomic (the cached snapshot, in the
   post-MVP multi-thread build).
3. **Render-thread access to `Surface`.** The render thread holds a
   `Surface` value across phase 7 (record command buffers) and
   drops it before phase 8 (hot-reload barrier). `Surface::~Surface`
   running on the render thread is safe because the destructor
   only touches the atomic counter on `Window::Impl` (§3.8); the
   counter is `std::atomic<std::uint32_t>` with relaxed ordering.

### 6.2 Pull rules from the render thread

The MVP frame loop is single-threaded; the render thread is the
*same* thread as the main thread, walking phases sequentially. The
"render thread" terminology in `frame-phases.md` is logical, not
physical, in MVP.

Post-MVP per-system parallelism (#496 deferred plan) introduces a
real render thread for phases 6 / 7. The pull rules:

- **`Surface` is acquired in phase 6** (`render::cull-extract`) on
  the main thread, *before* phase 7 begins. The phase-6 system
  calls `Window::surface()`; this is the only entry point that
  produces a `Surface` value, and it is main-thread-only.
- **The `Surface` value is moved into the `RenderFrame` extract**
  (frame-phases.md phase 6 column "An owned `RenderFrame` snapshot").
  The extract is then visible to phase 7's render-thread systems.
- **Phase 7 (render-thread)** reads `Surface::raw_layer()`, casts to
  `MTL::Layer*`, calls `nextDrawable` for the present, records
  command buffers. It never calls `Window::open` /
  `request_resize` / etc.
- **`Surface` is dropped at end of phase 7**, before phase 8. The
  destructor runs on the render thread and decrements
  `outstanding_surfaces_` atomically. By the time phase 8 begins,
  the counter is back to zero and platform self-reload is
  admissible.
- **Cached snapshot reads** from the render thread (phase 7 may
  read `Window::physical_size()` to set viewport) use the atomic
  load with `memory_order_acquire`; the matching release happens
  in `Window::Impl::ingest` (main thread, phase 1). This is
  acquire/release, not seq-cst — frame-phase ordering already
  serialises the writer (phase 1) before the reader (phase 7).

In MVP single-thread mode, the atomic ordering is no-op
(equivalent to `relaxed`); the post-MVP multi-thread mode is the
forward-compatible default.

### 6.3 No locks

The aggregate holds zero mutexes. All synchronisation is:

- Main-thread-only invariants (§3.10).
- Atomic counter for `outstanding_surfaces_`.
- Atomic snapshot fields for the post-MVP read-from-render path.

Mutex absence is a deliberate choice: SDL3's window-server APIs
already require main-thread ownership, so a mutex would only
serialise calls that were already serial. The atomic counter is
needed because `Surface` may move between threads via the
`RenderFrame` extract (post-MVP); the destructor's decrement
crosses the move.

### 6.4 Frame-phase mapping

| Phase            | Window-surface activity                                           |
|------------------|-------------------------------------------------------------------|
| 1 (input)        | Pump ingests `WindowEvent`s; aggregate's snapshot updates (§3.7). |
| 2–5              | None.                                                              |
| 6 (cull-extract) | Render calls `Window::surface()` on main thread; moves into `RenderFrame`. |
| 7 (render-submit) | Render reads `Surface::raw_layer()`, calls `nextDrawable`, records.       |
| 8 (hot-reload)   | None on the per-frame path; platform-self-reload uses the §3.4 / §3.5 sequences. |
| 9 (present)      | Render's present consumes the drawable; `Surface` is dropped *after* present in MVP, *before* phase 8 in post-MVP. |

The phase mapping is consistent with `frame-phases.md`'s row for
`platform`: this aggregate participates in phase 1 (Pump-driven
ingest, indirect) and phase 9 (present, indirect — the `Surface`
held by render is the platform-side input to phase 9).

## 7. Persistence + ABI

**Nothing in this aggregate is serialized.** The window-surface
aggregate ships **zero `.fory` schemas**. All state is process-local
and OS-backed:

- `WindowId` / `DisplayId` are runtime numeric handles; they are
  not stable across processes.
- `Window::Impl` is platform-internal; no public access path.
- `Surface` carries a `void*` and a `WindowId`; the layer pointer
  is only meaningful inside the running process and only valid
  while the owning `Window` lives.
- `Display` is a snapshot value with no on-disk shape.
- `LogicalSize` / `PhysicalSize` / `DpiScale` are POD scalars
  with no Fory schema; they are passed by value across function
  boundaries inside one process.

Consequence for `glibre-types.dylib` / `glibre_types_abi_hash`
(`fory-codegen.md` §"middleman dylib exposes"): **window-surface
contributes nothing to the ABI hash**. The aggregate's public types
(POD aggregates, opaque handles) are part of the
`glibre/platform/platform.hpp` header that every plugin includes,
but their stability is enforced by the layout-stability rules in
SPEC §6.10 and `fory-codegen.md` §"ABI Stability Rules", not by Fory.

The four middleman types contributed by *platform self-reload*
(SPEC §8.5) — `WindowSurvival`, `WatcherSurvival`, `SignalSurvival`,
`FileIoSurvival` — are **not part of this aggregate's surface**.
`WindowSurvival` is touched by the platform plugin's `drain` /
`register` entry points (#728 / future plan), not by `Window::open`
or any §4.1 method. SPEC §8.5 explicitly notes "these types are
loader-visible only during phase 8; they have no public surface in
`glibre/platform/platform.hpp`". This design upholds that rule.

If a future requirement forces a window-surface field into Fory
(e.g. cross-process editor inspector that needs `Display` over an
RPC), the schema would land under `data/schemas/platform/` and
this aggregate would gain its first migration. SPEC §8.5 gives the
skeleton for that future.

## 8. Hot-reload

The aggregate participates in hot-reload in two distinct shapes,
mirroring SPEC §8.1's two-shape framing:

### 8.1 Peer-plugin reload (window-surface as ambient state)

When a peer plugin (render, content, tools) reloads at phase 8:

- **`Window` survives.** The `Window::Impl` and its `WindowId` /
  `sdl_window_` / `layer_handle_` are platform-owned, not peer-plugin-
  owned. The peer's drain releases its `Surface` value (decrements
  `outstanding_surfaces_`); the swap happens; the peer's
  `glibre_plugin_register` re-acquires a fresh `Surface` via
  `Window::surface()`. Same `WindowId`, same `CAMetalLayer*`, same
  `SDL_Window*`. Render-side GPU resources (`MTL::Texture` views into
  the layer's drawable, `MTL::PipelineState` keyed off the layer
  format) are **re-derived** by the peer's register per
  `hot-reload-protocol.md` §"State Survival Rules" ("GPU resource
  handles internal to the plugin … must be re-derived").
- **`Display` snapshots are query-on-demand** so reload does not
  cache them; consumers re-call `Window::display()` after reseat.
- **Cached snapshot fields (`LogicalSize`, `PhysicalSize`,
  `DpiScale`)** survive — they are platform-state, not plugin-state.

Per SPEC §8.2 guarantee 1 ("Handle stability"): the `WindowId` is
preserved across peer reload. Render's reseat code does not need
per-window migration support.

### 8.2 Platform-plugin self-reload

When the platform `.dylib` itself is the outgoing P at phase 8,
`window-surface` participates in the four-step protocol per SPEC §8.3:

**Drain (step 1 of `hot-reload-protocol.md`):**

1. Verify `outstanding_surfaces_ == 0` for every open `Window`. A
   non-zero counter for any window → §8.4 refusal P1
   (`HotReloadRefused` wrapping `Unsupported`). Previous-good
   platform stays live.
2. For each open `Window`, capture `(WindowId, WindowDesc)` into
   the `WindowSurvival` middleman singleton (SPEC §8.5.1). The
   captured `WindowDesc` is reconstructed from the cached
   snapshot: `title` (from the internal arena copy), `size`
   (current `cached_logical_`, *not* the original
   `WindowDesc::size`), `resizable` (read back from
   `SDL_GetWindowFlags(sdl_window_)`), `fullscreen` (same).
3. Call `surface::detail::destroy_metal_view(layer_handle_)` for
   every open window. The `CAMetalLayer*` is released here, not at
   register-time — Q's register re-creates it.
4. Call `SDL_DestroyWindow(sdl_window_)` for every open window.
   The OS window-server entry vanishes for ~1 ms; SDL3 does not
   support handing a `SDL_Window*` across `dlopen`/`dlclose` of
   the SDL3-linking image.
5. The aggregate's `WindowId` counter survives in the
   `WindowSurvival` singleton — the next-allocated id starts at
   `max(captured_ids) + 1`, so re-opened windows take their old
   ids and peer plugins' cached `WindowId` references remain
   valid.

**Swap (step 2):** standard. ABI hash check is binding;
`WindowSurvival` is part of the hash via its (deferred, post-MVP)
`.fory` schema.

**Migrate (step 3):** empty. No window-surface bytes have a
versioned layout; the only "byte transfer" is the
`(WindowId, WindowDesc)` tuple which is a flat POD copy with no
schema evolution. (When `WindowDesc` first ships a versioned
schema, this clause amends in place.)

**Resume (step 4):** Q's `glibre_plugin_register` walks
`WindowSurvival` and:

1. Calls `Window::open(captured_desc)` for each entry. SDL3's
   subsystem has been re-initialized by the plugin's earlier
   register-time SDL_Init, so the calls succeed. The new
   `WindowId` produced by §3.4 step 2 is **discarded**; the
   captured id is re-bound to the new `Window::Impl` so peer
   plugins continue to dereference the same numeric value.
2. Re-emits the §3.4 sequence's bridge attachment, snapshot
   refresh, etc. — the new `CAMetalLayer*` is a different pointer
   than the pre-swap one, but render's reseat code re-acquires it
   through `Window::surface()` and treats it as a fresh resource.
3. After all windows are re-opened, the aggregate is
   indistinguishable from its pre-drain state from the public API's
   perspective — sizes, DPI, display id all match.

### 8.3 Re-binding of GPU resources at the barrier

The `CAMetalLayer*` *changes pointer identity* across platform
self-reload (the bridge releases the old one in drain step 3 and
creates a new one in resume step 1). Render's reseat must re-derive
every GPU resource that was keyed off the layer:

- `MTL::PipelineState` objects keyed on layer pixel format —
  re-create.
- Cached `MTL::Drawable` views — drop; the next frame's
  `nextDrawable` produces a fresh one against the new layer.
- Frame-buffer attachment descriptors that captured the layer
  pointer — rebuild from the new `Surface::raw_layer()`.

This is the responsibility of the *render plugin's* register at
its own reload (or at a peer's reload following platform's
reload), per `hot-reload-protocol.md` §"Re-derived by the
incoming plugin". The window-surface aggregate's contribution is
the guarantee that **every `Surface` produced after Q's register
is backed by the new layer**, so render's reseat can be
mechanical: drop everything keyed on layer pointer, re-acquire on
the next frame.

### 8.4 Refusal cases (cross-reference)

The aggregate contributes one refusal beyond the universal three:

- **Mid-frame surface drop (P1)** — SPEC §8.4 first paragraph.
  Detected by `outstanding_surfaces_ != 0` at drain time. The
  loud refusal is what makes the rule "no half-recorded GPU
  command buffer survives a swap" (`frame-phases.md` rationale)
  enforceable rather than aspirational.

The universal three (ABI hash mismatch, schema migration failure,
plugin init failure) are owned by the loader / barrier and surface
through this aggregate's drain / register only as the *carriers*
that report them, not as the *raisers*.

## 9. Performance

The aggregate's contribution to the platform context's per-frame
and heap budgets, locked against `perf-budget.md` and SPEC §9.

### 9.1 Per-frame (steady-state)

| Cell                                              | Budget       | Source                          |
|---------------------------------------------------|--------------|---------------------------------|
| platform CPU sim (full row)                       | 0.20 ms      | perf-budget.md row `platform`   |
| of which `Window` / `Surface` (§4.1)              | **0.000 ms** | SPEC §9.1 row "Window/Surface"  |
| platform CPU submit (full row)                    | 0.05 ms      | perf-budget.md                  |
| of which `Window` / `Surface`                     | **0.000 ms** | SPEC §9.1                       |

The aggregate's per-frame cost is **0** in steady state. Cached
snapshot reads (`logical_size`, `physical_size`, `dpi_scale`) are
~10 ns each (one atomic load); they are bucketed into the cell's
"Reserved" tail (SPEC §9.1 reserved row 0.099 / 0.049 ms), not
called out separately.

`Window::surface()` is called ~once per frame by render's phase 6
extract: ~50 ns (one atomic increment, one value construction).
Bucketed into the same reserved tail.

### 9.2 Per-event / cold-path

Driven by user input or display hot-plug; cost is bounded by SDL3
+ the bridge.

| Operation                                  | Budget       | Notes                                                                |
|--------------------------------------------|--------------|----------------------------------------------------------------------|
| `Window::open`                             | 5–20 ms      | dyld + SDL3 + AppKit + bridge attach. Cold-load only.                |
| `~Window`                                  | 1–5 ms       | bridge detach + SDL3 destroy.                                        |
| `Window::request_resize`                   | <0.1 ms      | One `SDL_SetWindowSize` call; resize event arrives next frame.       |
| `Window::request_close`                    | <0.05 ms     | `SDL_PushEvent`.                                                     |
| Pump ingest of `Resized`                   | <0.01 ms     | Three field writes.                                                  |
| Pump ingest of `DpiChanged`                | <0.05 ms     | Three writes + one `query_layer_metrics` (one bridge call).          |
| Pump ingest of `DisplayChanged`            | <0.005 ms    | One field write.                                                     |
| `Window::display()`                        | <0.05 ms     | SDL3 cached query × 4 properties.                                    |

These are off-the-hot-path costs; SPEC §9.1 reserves 0.099 ms / 0.049
ms in the platform cell for the union of these spikes plus
`Pump::drain()`'s own ~0.10 ms steady-state cost.

### 9.3 Heap

Platform's 16 MiB ceiling is partitioned into per-aggregate sub-
arenas (SPEC §9.2). The window-surface sub-arena is **1 MiB**,
holding:

| Object                                 | Size                  | Notes                                                  |
|----------------------------------------|-----------------------|--------------------------------------------------------|
| `Window::Impl` × N (open windows)      | ~256 B × N            | N ≤ 8 in MVP (editor + game-window).                    |
| `Display` snapshot scratch             | <1 KiB                | Stack-allocated per `display()` call; bucketed.         |
| Title strings (interned in the arena)  | sum of titles, ~256 B per window | UTF-8 bytes; lifetime = `Window`'s.            |
| `LayerHandle` per window               | 16 B × N              | `void* layer + void* sdl_metal_view`.                   |
| Reserved                               | ~1 MiB - sum          | Headroom for additional windows; never grows.           |

The aggregate **never grows the sub-arena at runtime**; allocation
is exclusively at `Window::open` time. Exhaustion → `IoFailure`
mapped through `core::Error::OutOfBudget` (perf-budget.md
"Allocator Rules" #2). The 1 MiB ceiling is comfortable for MVP's
≤8 windows; the editor's "many docked panels" scenario is a tools
concern, not a platform-window concern.

### 9.4 Wall-time budget for `surface()` query

SPEC §9.1 gives the cell-level number; this design fixes a
specific upper bound for `Window::surface()`:

- **p50: 50 ns** (atomic `fetch_add`, value ctor, return).
- **p99: 200 ns** (cache miss on `Window::Impl`).
- **Failure (debug-only, asserts on null `Impl`): immediate
  return, ~10 ns.**

This budget is asserted by a Catch2 `BENCHMARK` in §11 (microbench
`bench.window_surface_acquire_release_pair`).

### 9.5 GPU memory

`CAMetalLayer` GPU memory (the drawable + textures attached to it)
is **render's** budget per perf-budget.md "Allocator Rules" #5
("GPU memory is render-owned"). The window-surface aggregate
allocates zero GPU memory; it owns only the layer pointer's
lifetime.

## 10. Failure modes

Window-surface failures all surface through the closed sum
`platform::Error` declared in SPEC §4.7 / §5.1 / §10. This design
introduces no new error variants — every failure routes to a
pre-existing arm. The mapping below names which arm each refusal
case lands in and why.

### 10.1 Refusal table

| Symbolic case                  | Trigger                                                                                  | Maps to `platform::Error` arm                | Recovery                                                              |
|--------------------------------|------------------------------------------------------------------------------------------|----------------------------------------------|-----------------------------------------------------------------------|
| **SDLInitFailed**              | SDL3 video subsystem not initialized when `Window::open` runs                            | `Unsupported`                                | Caller (init plugin) initializes SDL3 before opening windows; refused before any OS handle is allocated. |
| **WindowCreateFailed**         | `SDL_CreateWindow` returned null (out of resources, display server crash, refused flags) | `IoFailure { OsCode = SDL_GetError() }`      | Caller may retry on a different display config; resources released in step 5 before return.             |
| **MetalLayerAttachFailed**     | `surface::detail::create_metal_view` failed (NSException-converted or null layer)        | `IoFailure { OsCode }`                       | Aggregate calls `SDL_DestroyWindow` to clean up; caller may retry. Most commonly missing Metal entitlement on non-Metal-capable hardware. |
| **SurfaceLost**                | `CAMetalLayer*` reports layer-lost from the OS (post-sleep recovery, GPU reset)          | `IoFailure { OsCode }` with `surface-lost` detail | Caller drops the `Window` and re-opens; render reseat re-derives GPU resources. (See §10.3.) |
| **ResizeRefused**              | `Window::request_resize` rejected by SDL3 (fullscreen window, zero dim, system policy)   | Either `Unsupported` (zero dim) or `IoFailure { OsCode }` (SDL3 refusal) | Caller does not retry blindly; either honours fullscreen state or fixes the dim.                |
| **MainThreadViolation**        | Any window-surface method called from a non-main thread (debug: assert; release: refuse) | `Unsupported`                                | Programming error; debug builds catch immediately.                                                       |
| **DpiInvalid**                 | `DpiScale::make(v <= 0)`                                                                | `Unsupported`                                | Programming error.                                                                                       |
| **LogicalSizeInvalid**         | `LogicalSize { 0, * }` or `{ *, 0 }` to `request_resize` or `WindowDesc`                 | `Unsupported`                                | Programming error.                                                                                       |
| **WindowOutstandingAtDestroy** | `~Window` while `outstanding_surfaces_ > 0` (debug-only; release falls through and leaks)| Debug `assert`; release: logs `error`, no return value. | Programming error in render; `Surface` outlived its `Window`.                                            |
| **HotReloadMidFrameDrop (P1)** | Platform self-reload drain finds `outstanding_surfaces_ > 0`                            | `Unsupported`, wrapped by core into `core::Error::HotReloadRefused` | Editor's reload UI retries on next frame boundary; SPEC §8.4 P1.                                          |
| **DisplayQueryAfterUnplug**    | `Window::display()` between display unplug and `DisplayChanged` drain                    | `NotFound`                                   | Caller re-queries after the next pump cycle.                                                              |
| **ArenaExhausted**             | Window-surface 1 MiB sub-arena cannot fit a new `Window::Impl` + title                  | `IoFailure { OsCode = ENOBUFS }` (mapped to `core::Error::OutOfBudget` in strict-mode builds) | Operator opens fewer windows or extends the sub-arena via a perf-budget amendment.                       |

The variant assignments above are **the ones in §4.7 / §5.1 / §10**
— no new variants are introduced. `SurfaceLost` is intentionally a
*recovery situation* surfaced via `IoFailure` with a known detail
prefix per SPEC §10's last paragraph, not a first-class arm. SPEC
§10.8 documents the second-consumer trigger that would promote it.

### 10.2 OS-side translation (cross-reference to sibling #727)

The mapping from POSIX `errno`, SDL3 `SDL_GetError()`, and
`NSException` codes to typed arms is **owned by the platform
error-policy aggregate (sibling #727)**. This aggregate calls into
that translator at exactly two ingress points:

- After `SDL_CreateWindow` returns null in §3.4 step 5.
- After any `surface::detail::*` returns `Result<...>` in §3.4 / §3.5
  / §3.7.

The translator returns a `platform::Error` arm; this aggregate
wraps it in `std::unexpected` and propagates. No translation
happens *inside* this aggregate's source files — that is what
makes #727's SRP sharp and lets this design assert "no new arm
introduced".

### 10.3 SurfaceLost recovery

Although `SurfaceLost` is not a first-class arm, the recovery path
matters because it is the only window-surface failure that can
occur *during* normal operation (every other arm is one-shot at
open / resize / drain).

Detection:

- macOS reports surface loss via the layer's
  `presentedTime` going to zero indefinitely or via a delegate
  callback when the GPU resets. SDL3 surfaces this through a
  `SDL_EVENT_DISPLAY_REMOVED` followed by `SDL_EVENT_DISPLAY_ADDED`,
  but the layer pointer is invalid in between.
- The bridge file's `query_layer_metrics` returns `IoFailure` with
  the `surface-lost` detail prefix in this case.

Recovery contract (caller-side):

1. Render observes the failure on the next-frame
   `Window::physical_size()` or `Surface` use.
2. Render drops the `Surface` value.
3. The engine main loop closes the `Window` (calls `~Window`) and
   re-opens it via `Window::open(WindowDesc{...})`. The new
   `WindowId` is different from the old one — peer plugins
   re-bind to the new id via the event flow.
4. Render reseat re-acquires the new `Surface` and rebuilds GPU
   resources keyed on the new layer.

Recovery is *not* automatic in MVP — there is no surface-loss-
retry inside this aggregate. The decision to re-open a window
belongs to the engine main loop, which has the policy context
(should we re-open vs. shut down vs. show an error dialog) that
this aggregate does not.

### 10.4 Logging discipline

Per `error-model.md` §"Logging / Telemetry" #1, every `Error`
constructed inside this aggregate is logged exactly once at the
boundary where it is *handled*, not where it is raised. The
boundaries:

- `Window::open` failures — handled by the caller of `open`,
  typically the engine init plugin. Severity: `error`.
- `Window::request_resize` / `request_close` failures — handled
  by the caller, typically tools. Severity: `warn` (recoverable).
- Pump-driven ingest failures (only `query_layer_metrics`
  failure during `DpiChanged` ingest) — handled by the Pump
  aggregate, surfaced via the `Pump::drain` `Result<size_t>`.
  Severity: `warn`.
- Hot-reload refusals — handled by `HotReloadBarrier` per
  hot-reload-protocol.md. Severity: `warn` (previous-good plugin
  stays live).

This aggregate writes nothing to `spdlog` directly; logging is the
caller's choice at the handling boundary.

## 11. Test plan

### 11.1 Unit tests (Catch2, `tests/platform/window/`)

Each row of §10.1 maps to one or more unit tests. The unit harness
uses a **headless SDL3 fixture** (a process-local SDL3 video init
gated by `SDL_HINT_VIDEO_DRIVER=offscreen` on Linux CI; on macOS CI
runners SDL3 always uses the real Cocoa driver because there is no
offscreen Metal). Tests that strictly require a real window-server
are gated by `[platform-fixture]` and run on `macos-26-m1` only.

Pure unit tests (no SDL3, no bridge):

| Test name                                          | Drives arm                          | Fixture                                      |
|----------------------------------------------------|-------------------------------------|----------------------------------------------|
| `window.desc_construction_zero_dim_refused`        | `Unsupported` (LogicalSizeInvalid)  | `WindowDesc{ .size = {0,1} }`.               |
| `window.desc_construction_minimal_accepted`        | (positive)                          | `WindowDesc{ .size = {1,1} }`.               |
| `dpi_scale.make_zero_refused`                      | `Unsupported` (DpiInvalid)          | `DpiScale::make(0.0f)`.                      |
| `dpi_scale.make_negative_refused`                  | `Unsupported` (DpiInvalid)          | `DpiScale::make(-1.0f)`.                     |
| `dpi_scale.make_positive_accepted`                 | (positive)                          | `DpiScale::make(2.0f)`.                      |
| `to_physical.identity_dpi`                         | (positive, math)                    | `to_physical({100,200}, DpiScale{1.0f})` == `{100,200}`. |
| `to_physical.retina_dpi`                           | (positive, math)                    | `to_physical({100,200}, DpiScale{2.0f})` == `{200,400}`. |
| `to_physical.fractional_dpi_round_half_up`         | (positive, math)                    | `to_physical({3,3}, DpiScale{1.5f})` == `{5,5}` (4.5 → 5, 4.5 → 5). |
| `to_physical.large_dpi`                            | (positive, math)                    | `to_physical({1920,1080}, DpiScale{3.0f})` == `{5760,3240}`. |
| `surface.default_invalid`                          | (positive)                          | Default-constructed `Surface` reports `valid() == false`. |
| `surface.move_only`                                | (positive, type-system)             | `static_assert(!std::is_copy_constructible_v<Surface>)`. |
| `window.move_only`                                 | (positive, type-system)             | `static_assert(!std::is_copy_constructible_v<Window>)`. |

Bridge-driven unit tests (require the headless or real driver):

| Test name                                          | Drives arm                          | Fixture                                      |
|----------------------------------------------------|-------------------------------------|----------------------------------------------|
| `window.open_minimal_succeeds`                     | (positive)                          | `Window::open(WindowDesc{})` returns `Result<Window>` with valid id. |
| `window.open_returns_unique_ids`                   | (positive)                          | Two `open` calls → two distinct `WindowId`s, second > first. |
| `window.open_destroy_round_trip`                   | (positive)                          | Open then drop; assert no leaks via tagged-allocator counter. |
| `window.surface_acquires_layer`                    | (positive)                          | `w.surface()` returns valid `Surface` with `raw_layer() != nullptr`. |
| `window.surface_increments_outstanding`            | (positive, atomic)                  | Get two `Surface`s; outstanding counter is 2 (probed via test-only accessor). |
| `window.surface_decrements_on_drop`                | (positive, atomic)                  | Drop two `Surface`s; counter back to 0.       |
| `window.physical_eq_logical_x_dpi`                 | (positive, invariant)               | Verify SPEC §4.1 inv #4 holds at every snapshot. |
| `window.dpi_changed_updates_snapshot_before_event` | (positive, ordering)                | Synthetic Pump-driven `DpiChanged`; assert `Window::dpi_scale()` reflects new value before `take_events` returns. |
| `window.resize_request_emits_resized_event`        | (positive, plumbing)                | `request_resize({640,480})` then drain Pump; assert `WindowEvent::Resized` arrives with the new size. |
| `window.close_request_emits_close_requested_event` | (positive, plumbing)                | `request_close()` then drain; assert `CloseRequested` arrives. |
| `window.fullscreen_flag_propagates`                | (positive, flags)                   | `WindowDesc{ .fullscreen = true }`; `Window::display()` reports fullscreen mode. |
| `window.display_query_returns_id`                  | (positive)                          | `display().id != DisplayId{0}` on a connected display. |
| `window.outstanding_at_destroy_asserts_debug`      | `WindowOutstandingAtDestroy`        | Debug-only: open + acquire surface + drop window; assert fires. |
| `window.zero_dim_resize_refused`                   | `Unsupported`                       | `request_resize({0,1})` → `unexpected(Unsupported)`. |

### 11.2 Integration tests (Catch2, `tests/platform/integration/`)

Full creation + DPI change + resize + destroy lifecycle:

- **`integration.window_lifecycle`** — Open a window with
  `WindowDesc{ .size = {800, 600} }`. Assert
  `physical_size() == to_physical(logical_size(), dpi_scale())`.
  Drain the pump; assert no spurious events. Drop the window.
  Assert `outstanding_surfaces_` is zero before destroy.
- **`integration.window_dpi_change`** — Open a window. Synthesise
  a `DpiChanged` event into the pump (the test harness has a
  fixture function `inject_dpi_changed(window_id, new_scale)` that
  pushes a synthetic SDL3 `SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED`
  via `SDL_PushEvent`). Drain. Assert `dpi_scale()` matches the
  new value, `physical_size()` updated, and the
  `WindowEvent::DpiChanged` was enqueued *after* the snapshot was
  refreshed. The strict ordering test reads the snapshot from a
  callback installed inside `Pump::drain`; the snapshot is the
  new value at the time the event becomes visible.
- **`integration.window_resize`** — Open at 1280x720. Call
  `request_resize({640, 480})`. Drain pump; `Resized` event
  arrives with the new logical size. `Window::logical_size()` and
  `physical_size()` reflect the new values. The cached snapshot
  was updated atomically in ingest.
- **`integration.window_display_changed`** — Requires multi-monitor
  fixture (`[platform-multimon]` tag, gated on CI runners with
  attached external display). Move the window to a second display.
  Drain; `DisplayChanged` event arrives. `display().id` reflects
  the new display.
- **`integration.window_destroy_releases_layer`** — Open + drop;
  bridge's `destroy_metal_view` is observed via a test-only hook
  in `bridge.mm` that increments a counter. Assert counter
  equals the number of windows created.
- **`integration.window_open_close_no_leak`** — Open + drop 100
  windows in a loop; assert window-surface sub-arena resident
  bytes return to baseline (per `core::PerContextAllocator::
  resident_bytes(ContextTag::Platform)` introspection).
- **`integration.headless_unsupported_refusal`** — On non-macOS
  CI runners (Linux offscreen driver), `Window::open` may refuse
  if Metal is unavailable. The test asserts the failure is
  reported as `IoFailure` with a known detail prefix, not a
  crash. Tagged `[non-macos]`.

### 11.3 Platform-fixture gated tests

A subset of tests strictly require a Cocoa window-server +
Metal-capable GPU. They are tagged `[platform-fixture]` in Catch2
and skipped on runners that do not advertise the capability via
`SDL_GetVideoDriver() == "cocoa"`. CI runs them on
`macos-26-m1` only; PRs from non-macOS contributors see them as
"skipped, not failed".

The fixture-gated subset:
`window.open_minimal_succeeds`, `window.surface_acquires_layer`,
`window.dpi_changed_updates_snapshot_before_event`,
`window.fullscreen_flag_propagates`,
`integration.window_lifecycle`, `integration.window_dpi_change`,
`integration.window_resize`, `integration.window_display_changed`,
`integration.window_destroy_releases_layer`,
`integration.window_open_close_no_leak`.

### 11.4 E2E coverage

E2E traces under `tests/e2e/platform/window-surface/` ship two
fixtures:

- **`window-surface-lifecycle`** — full open / resize / DPI-change /
  display-change / destroy cycle, recorded as a `.glibre-trace`
  golden. CI replays asserts byte-equal trace output.
- **`window-surface-mid-frame-reload-refusal`** — drives a
  platform self-reload with a `Surface` value still held by a
  test-only "render-stand-in" plugin; asserts
  `HotReloadRefused` fires with `Unsupported` cause arm and the
  prior-good platform plugin still ticks afterward (SPEC §8.4
  P1).

Both run on `macos-26-m1` CI only.

### 11.5 Performance microbenchmarks

Catch2 `BENCHMARK` blocks under
`tests/platform/perf/window_surface_bench.cpp`:

- **`bench.window_open_close`** — asserts <30 ms wall-clock for an
  open + immediate close. Covers cold-load.
- **`bench.window_surface_acquire_release_pair`** — asserts
  <500 ns for `surface()` + `~Surface` round-trip. Covers the §9.4
  hot-path budget.
- **`bench.window_size_query`** — asserts <100 ns for
  `logical_size()` + `physical_size()` + `dpi_scale()` triple
  read. Covers the cached-snapshot read path.
- **`bench.window_display_query`** — asserts <100 µs for a
  `display()` query. Covers the SDL3 cached-query path.

CI gates per perf-budget.md §"CI Gate Spec" #1: any benchmark
exceeding its budget fails the PR.

### 11.6 Resize math fuzz

A focused property test
(`fuzz.to_physical_round_trip`, `tests/platform/window/fuzz/`):

- For 10000 random `(LogicalSize, DpiScale)` inputs with
  `width, height ∈ [1, 16384]`, `dpi ∈ [0.25, 4.0]`, assert
  `to_physical(l, d).width == round(l.width * d.value)` exactly.
- Crucially: assert no overflow at boundary conditions
  (`width = 16384`, `dpi = 4.0` → `physical.width = 65536` fits in
  `std::uint32_t`).

The fuzz target catches any regression of the rounding rule (§3.9).

## 12. Open questions

- **[OPEN] Render-thread `Window::surface()` access in post-MVP.**
  MVP is single-threaded and `surface()` is main-thread-only. When
  per-system parallelism (#496 deferred) lands, render's phase 7
  runs on a dedicated thread; the design currently demands phase 6
  (main thread) acquire the `Surface` and move it. An alternative
  is to relax `surface()` to "any thread" with an explicit
  `MainThreadOrRender` whitelist. Resolve when #496 opens.

- **[OPEN] `WindowDesc` schema versioning for self-reload.** SPEC
  §8.5 declares `glibre::types::platform::WindowSurvival` as a
  middleman type but the `.fory` schema is not part of MVP. When
  it lands (first platform self-reload plan), a
  `migrate_WindowSurvival_v1_to_v2` may be needed for any
  `WindowDesc` field added (e.g. exclusive-fullscreen mode). The
  schema-evolution mechanism is normal `fory-codegen.md`; the
  question is when to ship v1.

- **[OPEN] Exclusive-fullscreen mode.** R-14.1.2 partial coverage:
  MVP supports only borderless-fullscreen via
  `SDL_WINDOW_FULLSCREEN`. Exclusive-mode video-mode change
  (resolution switch) is harder on macOS post-Catalina; deferred
  to a post-MVP plan. The `WindowDesc` field would extend with a
  `FullscreenMode` enum (`Borderless` / `Exclusive`) — additive,
  no schema bump.

- **[OPEN] `SurfaceLost` as a first-class arm.** SPEC §10.8 names
  the second-consumer trigger that would promote `SurfaceLost`
  from `IoFailure { detail: "surface-lost" }` to its own variant.
  The first consumer is render's reseat path; the second would
  be the editor's "surface-lost diagnostic" UI. Until then,
  `IoFailure` with the detail prefix is sufficient.

- **[OPEN] Multi-window event-fan-out load.** SPEC §4.1 invariant
  #6 ("`Surface` is opaque to engine code") is preserved by
  vending one `Surface` per `Window::surface()` call. If the
  editor opens N=8 windows, the Pump produces 8 streams of
  `WindowEvent`s that the aggregate ingests; the per-event ingest
  is O(1) per window but the cumulative cost on a hot-plug burst
  could exceed the 0.000 ms cell allocation. Provisional answer:
  the SPEC §9.1 reserved tail (0.099 ms) absorbs this; verify
  with a multi-window benchmark when the editor lands.

- **[OPEN] `Window::Impl` allocator strategy when ≥9 windows are
  open.** The 1 MiB sub-arena (SPEC §9.2, this design §9.3) holds
  ~256 B × N for `N` windows; the budget is comfortable for MVP's
  ≤8 ceiling but starts to bite at editor's "every panel is a
  window" extreme (which is not in MVP). Either grow the sub-arena
  via a perf-budget amendment or move to a fixed-N ring with
  explicit overflow refusal. Defer until the editor's window
  count exceeds 4.

- **[OPEN] Bridge file linker scope.** SPEC §6.2 says
  `bridge.mm` is the only `.mm` file in the engine and links
  `AppKit` / `Foundation` / `QuartzCore` / `Metal`. The
  `bridge.mm` build flag list grows when render adds Metal API
  needs that the bridge has to surface (e.g. the layer's
  `colorspace` for HDR). Provisional answer: keep the bridge
  file's scope minimal — exactly the three functions in §3.3 —
  and let render include `<Metal/MTLDevice.hpp>` via metal-cpp
  for everything else. Verify the rule holds when render's HDR
  story opens.
