# platform — Detailed Design: event-pump aggregate

> Detailed design for the `Pump` + `EventQueue<InputEvent>` /
> `EventQueue<WindowEvent>` aggregate declared in `specs/platform/SPEC.md`
> §4.2 / §5.3 / §5.4 / §5.7 / §6.3. Refines those sections in place;
> cites `reviews/decisions/error-model.md`,
> `reviews/decisions/frame-phases.md`,
> `reviews/decisions/perf-budget.md`,
> `reviews/decisions/plugin-abi.md`,
> `reviews/decisions/hot-reload-protocol.md`, and
> `reviews/decisions/fory-codegen.md`. Introduces no new public surface
> beyond the §5 stub already locked into `specs/platform/SPEC.md`;
> deviations from the cited records would require an amendment spike,
> not an in-place edit.
>
> Refs: spike #717 — `[SPIKE] design-platform-event-pump-detailed`.
> Parent #714. Sibling task-breakdown spike is blocked by this
> deliverable. All conclusions re-derived; harmonius prior art
> (`harmonius/docs/requirements/input/device-abstraction.md`,
> `harmonius/docs/requirements/platform/window-display.md`,
> `harmonius/docs/design/input/`) is research input only.

## 1. Purpose

The event-pump aggregate is the **single OS-event ingress** for the
engine. Per `platform` SPEC §4.2 it owns:

1. The **`Pump`** — sole owner of the `SDL_PollEvent` loop. Drains
   SDL3's single event stream once per frame on the main thread and
   normalises every accepted event into a typed `InputEvent` /
   `WindowEvent` sealed-variant payload.
2. The **paired `EventQueue<InputEvent>` and `EventQueue<WindowEvent>`**
   — bounded, fixed-capacity SPSC ring buffers sized at construction.
   They live or die together because SDL3 reports both families
   through the same stream and the cross-family ordering invariant
   (§4.2 inv #2) collapses into a single per-device monotone the
   moment the two queues are split apart.
3. The **classify-and-translate table** — the sealed mapping from
   SDL3 event tags + payload fields to glibre's `KeyCode` / `ScanCode`
   / `MouseButton` / `GamepadAxis` / `GamepadBtn` / `WindowId` /
   `DisplayId` / `DpiScale` value objects, declared in §5.4 and
   realised in `event/translate.cpp` (this design's name; SPEC §6.3
   names it the "translation table").
4. The **drain timing contract** — the `Pump` runs in **phase 1
   (input)** of `frame-phases.md`, exactly once per frame, and the
   queues are quiescent at the phase 8 (hot-reload) barrier
   (§4.2 inv #1, §8.2 platform-pump-quiescence guarantee).

The aggregate **refuses to own**:

- **Input mapping / actions / bindings.** Translating
  `InputEvent::KeyDown { key=Space }` into `Action::Jump` is the
  `gameplay` / `tools` plugins' job (collapsed into harmonius's R-6.2
  refusal, §3 Refusals). The pump emits raw normalised events; the
  consumer maps.
- **Window state / lifetime.** `WindowId` is opaque; the pump never
  dereferences it. Ownership of `Window` / `Display` / `Surface`
  belongs to the sibling design `specs/platform/window-display-design.md`
  (spike #715).
- **File watching.** The pump does not own or write to
  `FileWatcher::EventQueue<FileEvent>`. `SDL_EVENT_DROP_FILE` values
  are forwarded to `FileWatcher::ingest_drop` (see §3.6 + responsibility
  6 in §1), which is the file-watcher's own enqueue path — the pump
  produces a single function call, not a ring write. Routed to the
  sibling design (spike #719).
- **Per-device polling.** Gamepad / sensor state polling
  (`SDL_GetGamepadAxis`) is *not* the pump's responsibility; SDL3
  delivers gamepad axis / button changes as events through
  `SDL_PollEvent`, and the pump simply normalises those events. A
  hypothetical "snapshot the current gamepad state at frame start"
  primitive would belong to the input-mapping plugin, not here.
- **IME composition state.** The pump receives a single
  `SDL_EVENT_TEXT_INPUT` carrying the committed UTF-8 from SDL3's
  internal IME and emits one `input::TextInput` event; the
  composition / candidate-window UI is owned by the OS and surfaces
  through SDL3's IME hooks (out of MVP scope per harmonius R-14.2.6
  refusal).
- **Frame pacing / display-link callbacks.** The pump does not block
  on a vsync source. `frame-phases.md` phase 9 (present) drives
  pacing through `CAMetalDisplayLink`; the pump is a phase-1 drain,
  not a phase-9 wait.
- **Cross-process replay.** Recorded event streams (e2e tests,
  determinism replay) are produced by the e2e harness's
  `InputDriver` injecting directly into `EventQueue<InputEvent>`
  (`specs/e2e/SPEC.md`, `specs/platform/SPEC.md` §8.7). The pump
  does not participate in capture or replay; it is the producer SDL3
  feeds, and it is bypassed under replay.

The SRP boundary is sharp: if the **SDL3 event vocabulary**, the
**SDL3 → typed-variant translation table**, the **per-device
ordering rule**, the **bounded SPSC ring protocol**, the
**per-frame drain trigger**, or the **drop-event routing policy**
change, this design changes. Anything else — input action mapping,
window aggregate state, display pacing — is out of scope.

Responsibility 6 (owned, in-scope): the *policy* of drop-event
routing — that the pump forwards `SDL_EVENT_DROP_FILE` to the
file-watcher aggregate. This policy stays owned regardless of which
*mechanism* implements it (current: `FileWatcher::ingest_drop` direct
call; future option: `WindowEvent::FileDropped` via the queue, gated
on §12 [NON-BLOCKING] one-caller audit). The SRP boundary tracks the
policy, not the mechanism — switching mechanisms does NOT trigger an
SRP-list edit, but adding or removing the policy itself would.

## 2. Requirements coverage

This section verifies that every harmonius MVP-scope requirement /
design clause concerning the event-pump is either covered by the
design below or explicitly refused with rationale. Inputs:

- `harmonius/docs/requirements/input/device-abstraction.md`
  (R-6.1.1 .. R-6.1.15) — the input event vocabulary.
- `harmonius/docs/requirements/platform/window-display.md`
  (R-14.1.9, R-14.1.11) — single-poller and bounded-channel rules.
- `harmonius/docs/requirements/platform/os-integration.md`
  (R-14.2.4 — drag/drop) — assessed for inclusion.
- `harmonius/docs/design/input/` — implementation notes (treated as
  research only; conclusions re-derived).

| Harmonius clause                                                                                                                     | Glibre disposition (MVP)                                                                                                                                                                                                                                                                                                                                  |
|--------------------------------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **R-6.1.1** key press/release/repeat with scancode + keycode + modifiers                                                             | **Covered.** §5.4 emits `input::KeyDown { KeyCode, ScanCode, ModifierMask, repeat }` and `input::KeyUp { KeyCode, ScanCode, ModifierMask }`. The translation table maps `SDL_EVENT_KEY_DOWN`/`UP` and `SDL_KeyboardEvent::repeat` directly into the typed payload (§3.3 below).                                                                            |
| **R-6.1.2** scancode normalisation to USB HID                                                                                        | **Covered (delegated to SDL3).** SDL3's `SDL_Scancode` *is* the USB-HID-aligned namespace; the translation step is identity-by-cast into `glibre::platform::ScanCode`, sealed at compile time so a future SDL3 enum extension is a deliberate central edit, not a silent expansion (§3.3).                                                                |
| **R-6.1.3** mouse button events (L / R / M / X1 / X2)                                                                                | **Covered.** `input::MouseButtonEv { MouseButton, pressed, x, y, click_count }`. SDL3's button index 1..5 maps to the closed enum `MouseButton::Left/Right/Middle/X1/X2`. Scroll lives in `Wheel` (§5.4 / §3.3).                                                                                                                                            |
| **R-6.1.4** mouse delta / position in high-DPI                                                                                       | **Covered.** `MouseMove { x, y, dx, dy }` reports all four fields in logical points. Absolute `x/y` from `SDL_MouseMotionEvent` are already in logical points (matching `LogicalSize` when `SDL_WINDOW_HIGH_PIXEL_DENSITY` is set). Relative `dx/dy` from `SDL_SetRelativeMouseMode` are in physical pixels and must be divided by `Pump::Impl`-cached `DpiScale` (initialised `1.0f`, refreshed in §3.5 on `SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED`) before being placed in the payload. The cached copy is passed by value to `translate_mouse` — no cross-aggregate call on the hot translation path. See §3.3 mouse-coord rule. Invariant: every `MouseMove` payload reports x, y, dx, dy in logical points (see §3.3 for the rescale and unit test).                                          |
| **R-6.1.5** trackpad continuous scroll vs discrete wheel                                                                             | **Covered.** `Wheel { dx, dy, flipped }`. SDL3's `SDL_MouseWheelEvent::direction` produces `flipped`; floating-point `dx/dy` carries the precise trackpad delta. Discrete vs continuous distinction is not exposed as a separate field — consumers that need it inspect the magnitude (≥ 1.0 ⇒ likely discrete) per the input-plugin's interpretation rule. |
| **R-6.1.6** unified gamepad abstraction (buttons, axes, triggers)                                                                    | **Covered.** `input::GamepadButtonEv { device, GamepadBtn, pressed }`, `input::GamepadAxisEv { device, GamepadAxis, value }`. Trigger is an axis (`LeftTrigger`, `RightTrigger`) per the `GamepadAxis` enum. Sticks rescale `[-32768, 32767]` to `[-1, 1]`; triggers rescale `[0, 32767]` to `[-1, 1]` using formula `axis = (raw / 32767.0f) * 2.0f - 1.0f`. Consequently a fully-released trigger (raw=0) reports `axis=-1.0f`; gameplay code wanting released-as-zero must remap: `(axis + 1.0f) / 2.0f`. See §3.3 for the formula and unit tests.        |
| **R-6.1.7** gyroscope / accelerometer as ECS events                                                                                  | **Refused for MVP, deferred.** Sensor events would require new variants in the closed `InputEvent` sum (§4.2 inv #5: sealed at compile time). VR / motion-controller scope is out of MVP per `specs/platform/SPEC.md` §3 refusals; reopening the sum is a deliberate amendment spike when a controller-with-IMU lands as a target device.                  |
| **R-6.1.8** per-device capability flags                                                                                              | **Refused at the pump.** Capability queries are state on the device handle, not events on the stream. Routed to a future `InputDevice` aggregate (sibling, post-MVP). The pump does not synthesise capability events.                                                                                                                                       |
| **R-6.1.9** 10-finger touch tracking                                                                                                 | **Refused for MVP.** No touch hardware on the macOS-first baseline (`PHILOSOPHY` §macOS-first). Reopening the `InputEvent` sum for touch is gated on the second-target-platform spike.                                                                                                                                                                       |
| **R-6.1.10** pen / stylus events                                                                                                     | **Refused for MVP.** Same gate as touch — pen events are platform-specific (Wacom, Apple Pencil) and require a sealed extension to `InputEvent`. Deferred.                                                                                                                                                                                                  |
| **R-6.1.11** device hot-plug detection                                                                                               | **Partially covered.** SDL3 emits `SDL_EVENT_GAMEPAD_ADDED` / `_REMOVED`; the pump consumes these *internally* and emits no public glibre event today (§3.5 below — the input-plugin queries the active gamepad list at use time). Promoting hot-plug to a public `InputEvent` variant is gated by the input-plugin spike.                                  |
| **R-6.1.12** hot-plug enumeration on a background thread                                                                             | **Refused at the pump.** SDL3 enumerates synchronously inside `SDL_PollEvent` on the main thread; budget-wise this is well under §9's `Pump::drain` cell. A background thread would re-introduce SDL3's main-thread-only constraint as a cross-thread invariant we'd then have to defend.                                                                   |
| **R-6.1.13** sub-5 ms enumeration                                                                                                    | **Refused at the pump.** Boot-time concern; the input-plugin handles initial enumeration. The pump's per-frame budget is governed by §9, not by an enumeration deadline.                                                                                                                                                                                    |
| **R-6.1.14** input device state in ECS                                                                                               | **Refused.** Routed to the input-plugin: phase 1 systems read the queue, write `Input` / `KeyboardState` / `GamepadState` components. The pump is a producer of events, never of components.                                                                                                                                                                |
| **R-6.1.15** most-recently-used device tracking                                                                                      | **Refused.** Stateful; belongs to the input-plugin. The pump emits each event with its source device id (`device: u8` for gamepads); MRU tracking is a one-line phase-1 system.                                                                                                                                                                              |
| **R-14.1.9** bounded async channel, exactly-once FIFO, never-drop                                                                    | **Covered.** `EventQueue<T>` is bounded (fixed capacity, set at construction; §5.3); SPSC head/tail is exactly-once FIFO (`memory_order_release` / `acquire`); never-drop is `§4.2 inv #3` — full-on-write is fatal, not coalesced, surfaced as `Error::IoFailure { OsCode { 0 } }` with prefix `"queue-full"`.                                              |
| **R-14.1.11** single OS-event poller, centralised translation                                                                        | **Covered.** Exactly one `SDL_PollEvent` loop in the process — owned by `Pump::drain()`; the §6.3 SDL3 facade documents this as a load-bearing rule. Other contexts MUST NOT call `SDL_PollEvent` even transitively (verified by a build-time linker probe — see §11.1 test #4).                                                                            |
| **R-14.1.10** DPI policy (`SystemScaled` vs `ApplicationScaled`)                                                                     | **Refused at the pump.** The pump emits `WindowEvent::DpiChanged { scale }` regardless of policy; the *response* (resize or stay) is the window aggregate's job (§6.3 cross-module note). The pump's role is the event, not the response.                                                                                                                  |
| **R-14.2.4** drag-drop file events                                                                                                   | **Partially covered, partially deferred.** SDL3 emits `SDL_EVENT_DROP_FILE`. MVP routes drop into the `FileEvent` family carried by the `FileWatcher`-fed queue (sibling spike #719) rather than `WindowEvent`, because consumers of drop are content-import code that already drains the file queue. The pump consumes drop events internally and re-emits them through the file-event ring; see §3.6. |
| **R-14.2.5** keyboard layout query                                                                                                   | **Refused.** Stateful; belongs to a future `InputDevice` aggregate. The pump emits `input::KeyDown { ScanCode, KeyCode }` already carrying both layouts of identity (scancode = layout-independent, keycode = layout-dependent), which covers the consumer use case without the layout-query API.                                                            |
| **R-14.2.6** IME composition                                                                                                         | **Partially covered.** `input::TextInput { utf8, length }` carries the committed UTF-8 (max 32 B per event, multi-event for longer runs). Composition / candidate UI is owned by SDL3 / the OS; no glibre event is emitted for in-progress composition (§1 refusal).                                                                                          |
| Harmonius design — `InputEvent` enum with per-device payloads (`InputEvent::KeyboardKey`, `InputEvent::GamepadAxis`, …)             | **Covered + collapsed.** Glibre's `InputEvent` is a sealed `eastl::variant` with the eight payloads listed in SPEC §5.4. Harmonius's per-device segmentation re-emerges as the `device: u8` field on the gamepad payloads (§5.4); keyboard / mouse are singleton in MVP (one keyboard, one mouse).                                                            |
| Harmonius design — separate `WindowEvent`, `MonitorEvent`, `LifecycleEvent` channels                                                 | **Covered + collapsed.** One `EventQueue<WindowEvent>` carries `Resized`, `DpiChanged`, `Minimized`, `Restored`, `FocusGained`, `FocusLost`, `CloseRequested`, `DisplayChanged` (the latter folds harmonius's `MonitorEvent` into the same queue, preserving the cross-family ordering rule). Lifecycle (app-foregrounded etc.) collapses into focus events. |
| Harmonius design — `EventIterator` consumer abstraction                                                                              | **Refused.** Glibre exposes `EventQueue<T>::drain(eastl::span<T> out) -> std::size_t` (SPEC §5.3) which fills a caller-supplied buffer and returns the count. An iterator object would hide the bounded-buffer contract; the span-out shape forces the consumer to size their drain explicitly.                                                              |
| Harmonius design — coalescing of redundant resize events under flood                                                                 | **Refused.** §4.2 inv #4 forbids coalescing of `CloseRequested` and `DpiChanged`; for symmetry and simplicity, **no event family is coalesced at the pump**. Drag-resize floods (~120 events / s on macOS) fit comfortably inside the §9.2 ring-buffer sizing (~16k slots for `WindowEvent`), so coalescing buys nothing. Consumers that want a "latest-wins" resize read the queue tail and discard earlier events themselves. |
| Harmonius design — async OS event delivery via OS-thread → game-loop SPSC                                                            | **Covered.** SPSC rings between the SDL3-fed pump and the engine's main-thread consumer (§5.3). The pump itself runs on the main thread (SDL3 constraint), but the SPSC discipline is preserved because `Pump::drain()` and `EventQueue<T>::drain()` are decoupled in time even when both run on the same thread — the drain consumer reads what the producer pumped earlier in phase 1. |

Net result: every R-6.1.* and R-14.1.* / R-14.2.* requirement that
falls into the event-pump's responsibility is either implemented as
designed below or explicitly refused with rationale. The harmonius
multi-channel event-bus collapses cleanly into glibre's two queues
(input + window) plus the file-event queue owned by the sibling
`FileWatcher` aggregate; sensor / touch / pen / capability surfaces
are deferred behind a single sealed-sum-amendment gate.

## 3. Detailed model

The pump body is small (single-file, ~400 LOC at implementation
time) but carries five load-bearing structural rules. Each is given
a sub-section.

### 3.1 The two-queue topology

```text
  +-------+    SDL_PollEvent     +------+   classify     +-----------------+
  | SDL3  | -------------------> | Pump | -------------> | EventQueue<     |
  | core  |   (one event/iter)   |      |    & translate | InputEvent>     |
  +-------+                      +------+                +-----------------+
                                    |  \                          ^
                                    |   \                         | drain()
                                    |    \---> EventQueue<        | (engine
                                    |          WindowEvent>       |  main thread,
                                    |    /---> ^                  |  phase 1
                                    |   /                         |  consumer)
                                    |  /  classify & translate    |
                                    +-/                           |
                                    | \                           |
                                    |  \  drop (debug log)        |
                                    +---> /dev/null (unknown SDL  |
                                          event tag — §4.2 inv #5)|
                                                                  |
   FileWatcher I/O thread ----> EventQueue<FileEvent> ------------+
   (separate ring, owned by FileWatcher aggregate, sibling design)
```

Per-frame in phase 1:

1. The pump's main-thread `Pump::drain()` runs `SDL_PumpEvents()`
   once (so SDL3 transfers the OS queue into its internal ring),
   then `while (SDL_PollEvent(&ev))` to exhaustion.
2. Each `SDL_Event` is classified into one of four buckets:
   `input` / `window` / `file-drop` / `unknown` / `consumed-internally`.
3. `input` and `window` events go through the translation table to
   produce typed `InputEvent` / `WindowEvent` payloads, then
   `try_emplace` into the corresponding ring.
4. `file-drop` events (`SDL_EVENT_DROP_FILE`, `SDL_EVENT_DROP_TEXT`)
   are forwarded to the `FileWatcher` aggregate's drop-ingestion API
   so they ride the `EventQueue<FileEvent>` ring with the rest of
   the file events; the pump never owns drop on its own ring (§3.6).
5. `consumed-internally` events (gamepad add/remove, display
   add/remove → re-query SDL3 display list, DPI change → update
   `Window::Impl` cached metrics before emitting `DpiChanged`) are
   acted on then either re-emitted as a typed variant or dropped per
   §4.2 inv #5.
6. `unknown` events are dropped at debug-log severity and never
   queued (§4.2 inv #5 — sealed variant, no `Unknown` arm).

The producer is `Pump::drain()`. The consumer of each ring is the
engine's phase-1 input system, calling `EventQueue<T>::drain(span)`
once per frame after the pump returns. The main-thread-on-both-sides
shape is honest — both producer and consumer happen to be the same
thread in MVP — but the SPSC discipline is preserved because the
two operations are never interleaved within a single frame: the
schedule guarantees `Pump::drain()` completes before any
`EventQueue<T>::drain()` call begins (phase 1 entry contract).

### 3.2 Per-device monotone ordering invariant (§4.2 inv #2)

The cross-family ordering bug §4.2 invariant collapses around: in
SDL3 a single `SDL_PollEvent` call may return a window-focus-lost
event followed by a key-up event followed by a mouse-button-up event
all caused by the same Cmd-Tab. If those three events were split
across independent queues without a sequencing rule, downstream code
that reads "I last had focus when key was down" would see the
post-focus-loss key-up as the *current* state. We refuse that bug at
the source: the pump emits events into its target queue in the
exact order SDL3 reported them. Per-device monotone is a
*consequence* of preserving global SDL3 order, not a separate rule.

The implementation follows from this:

- **No reordering inside a single drain.** The classify-and-translate
  step is straight-line; we never sort or rebatch. If event K
  precedes event K+1 from `SDL_PollEvent`, then either (a) both go
  to the same queue and K's tail-index is strictly less than K+1's,
  or (b) K and K+1 go to different queues and the engine reads them
  in two separate `drain` calls but the schedule guarantees both
  drain calls happen in phase 1 before any consumer reads either.
- **No batching across drain calls.** `Pump::drain()` is exactly one
  SDL3 drain per frame. Multiple drains per frame would re-introduce
  the cross-family interleave question between drains.
- **Per-device id is intrinsic to the payload, not the sequencing.**
  `GamepadAxisEv::device` carries the SDL3 player index; consumers
  that want per-device monotone filter by device id and the
  ring-FIFO discipline does the rest.

### 3.3 Translation table (`event/translate.cpp`)

The table is straight-line C++ — no runtime registration, no plugin
extension point. Six functions, one per input family + one for
window events:

```cpp
// engine/platform/src/event/translate.hpp — internal.
namespace glibre::platform::event::detail {

// Returns nullopt for unknown / consumed-internally tags.
[[nodiscard]] auto translate_keyboard(const SDL_Event&) noexcept
    -> eastl::optional<InputEvent>;
[[nodiscard]] auto translate_mouse(const SDL_Event&, DpiScale) noexcept
    -> eastl::optional<InputEvent>;
[[nodiscard]] auto translate_wheel(const SDL_Event&) noexcept
    -> eastl::optional<InputEvent>;
[[nodiscard]] auto translate_text(const SDL_Event&) noexcept
    -> eastl::optional<InputEvent>;
[[nodiscard]] auto translate_gamepad(const SDL_Event&) noexcept
    -> eastl::optional<InputEvent>;
[[nodiscard]] auto translate_window(const SDL_Event&) noexcept
    -> eastl::optional<WindowEvent>;

}  // namespace glibre::platform::event::detail
```

Key normalisation rules realised in the table:

- **`KeyCode` is the SDL3 keycode by value.** SDL3's `SDL_Keycode`
  matches the `glibre::platform::KeyCode` enum's underlying integers
  one-to-one for the printable + named-key range (`SDL_K_SPACE` →
  `KeyCode::Space` etc.). This is a deliberate seal: the enum is
  declared in `platform.hpp` § 5.4 with `Unknown = 0` and is
  *closed* at compile time. A future SDL3 keycode addition that we
  haven't added an enum value for is dropped at the pump (debug
  log) — never queued as `Unknown`.
- **`ScanCode` is the USB-HID-aligned SDL3 scancode by value.** Same
  cast-and-seal rule as `KeyCode`.
- **`ModifierMask` packs SDL3's `SDL_Keymod` bits into the
  `ModifierMask::bits` field.** The bit layout is documented in
  `platform.hpp` § 5.4 and frozen across MVP; consumers (input
  plugin) decode by AND-mask against named bit constants exposed
  alongside `ModifierMask`.
- **Mouse coordinates: all four fields in logical points.**
  Absolute `x/y` from `SDL_MouseMotionEvent` are already logical
  points (matching `LogicalSize`) when `SDL_WINDOW_HIGH_PIXEL_DENSITY`
  is set — window-relative so coordinate (0, 0) is top-left of the
  client area regardless of compositor placement. Relative `dx/dy`
  from `SDL_SetRelativeMouseMode` are physical pixel deltas on macOS
  and must be divided by the current `DpiScale` before being placed
  in `MouseMove::dx/dy`. `Pump::Impl` caches the current `DpiScale`
  (initialised `1.0f` at construction; updated in §3.5 on
  `SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED`). Each `translate_mouse`
  call passes the cached `DpiScale` by value as its second argument.
  Documented invariant: every `MouseMove` payload reports x, y, dx, dy
  in logical points. A unit test at `DpiScale != 1.0` asserts the
  divide-by-scale path (§11.1 test #3b).
- **Wheel deltas use `SDL_MouseWheelEvent::x/y` (floating-point on
  modern SDL3).** `flipped` reflects the user's "natural scrolling"
  preference per `SDL_MouseWheelEvent::direction`.
- **Text input UTF-8.** SDL3 delivers UTF-8 already via
  `SDL_TextInputEvent::text[32]` — a fixed 32-byte null-terminated
  buffer. SDL3 truncates at that boundary before the event reaches
  the pump, so no single `SDL_EVENT_TEXT_INPUT` will ever carry more
  than 32 bytes. The pump copies up to 32 bytes into
  `input::TextInput::utf8` and sets `length`. The four-byte
  look-ahead codepoint scanner described in earlier drafts is
  therefore dead code for the SDL3 path and is **not implemented**
  in the production translator. If the test harness's `InputDriver`
  needs to inject an overlong synthetic string (more than 32 bytes)
  in test-only paths, it is responsible for splitting at codepoint
  boundaries before enqueueing multiple `TextInput` events directly.
- **Gamepad axis range rescale.** SDL3's `Gamepad` axis range is
  `[-32768, 32767]` for sticks and `[0, 32767]` for triggers.
  Sticks: `axis = raw / 32767.0f` (clamped to `[-1, 1]`).
  Triggers: `axis = (raw / 32767.0f) * 2.0f - 1.0f` (range `[-1, 1]`).
  Consequently a fully-released trigger (SDL3 raw = 0) maps to
  `axis = -1.0f`. Gameplay code that wants released-as-zero must
  remap at the consumer: `(axis + 1.0f) / 2.0f`. This is documented
  as an invariant; §11.1 test #6b asserts `translate_gamepad(raw=0,
  axis=LeftTrigger).value == -1.0f`. This is the only arithmetic in
  the translate path; everything else is cast-and-pack.

### 3.4 Bounded SPSC ring (`event/queue_impl.hpp`)

The `EventQueue<T>::Impl` is the canonical lock-free SPSC ring:

```cpp
// engine/platform/src/event/queue_impl.hpp — internal.
namespace glibre::platform::event::detail {

template <class T>
struct RingImpl {
    std::size_t        capacity{0};       // power of two (mask-and-wrap).
    std::size_t        mask{0};           // capacity - 1.
    // alignas(128) padding mandatory for SPSC discipline on Apple Silicon
    // (128B cache line). Without padding, every producer push invalidates
    // the consumer's cache line on the other core via false sharing,
    // defeating the lock-free benefit. The 256-byte struct overhead is
    // negligible against the 1 MiB slot array.
    alignas(128) std::atomic<std::size_t> head{0};     // producer-only (release on publish).
    alignas(128) std::atomic<std::size_t> tail{0};     // consumer-only (release on consume).
    T*                 slots{nullptr};    // capacity slots, allocated from
                                          // platform sub-arena (§9.2 SPEC).

    [[nodiscard]] auto try_push(T value) noexcept -> bool;
    [[nodiscard]] auto pop_into(eastl::span<T> out) noexcept -> std::size_t;
};

}  // namespace glibre::platform::event::detail
```

Ordering pairs:

- **Producer publish (`Pump::drain` → `try_push`).** Write the slot
  using a relaxed store, then `head.store(new_head, release)`. The
  release-store pairs with the consumer's acquire-load on `head`.
- **Consumer drain (`EventQueue::drain`).** `head_snap = head.load(acquire)`,
  then read up to `head_snap - tail_snap` slots, then
  `tail.store(new_tail, release)`. The release-store on `tail`
  pairs with the producer's acquire-load on `tail` to detect the
  full-ring condition.

Capacity is fixed at construction (§4.2 inv #6) and a power of two
(mask-and-wrap is one `&` instead of one `%`). `with_capacity(N)`
silently rounds `N` up to the next power of two so callers need not
know about the power-of-two sizing constraint (§11.1 test #8b:
`with_capacity(100).capacity() == 128`). Sizes are §9.2:
~32k slots for `EventQueue<InputEvent>`, ~16k slots for
`EventQueue<WindowEvent>`. Slot storage is a single contiguous
allocation from the platform sub-arena tagged
`platform::ContextTag` (§9.2 SPEC).

Full-on-`try_push` returns `false`; the pump:
1. Stamps the TLS prefix slot via
   `detail::error::set_prefix(prefix::queue_full)`.
2. Emits an `error`-severity log with the prefix `"queue-full"` and
   the queue family name. The stamp must precede this log call so
   `log_error` reads the stamped prefix (not `prefix::none`).
3. Returns `unexpected(IoFailure { OsCode{0} })`.

The TLS prefix stamp is mandatory before any `IoFailure{OsCode{0}}`
construction where the prefix is the only discriminator; without the
stamp a caller testing `current_prefix() == prefix::queue_full` will
see `prefix::none` and silently misroute the fatal error. The stamp
is cold-path-only — never executed on the success path.

**No coalescing, no drop, no grow** — §4.2 inv #3 / inv #6 is structural.

### 3.5 Internally-consumed SDL3 events

Some SDL3 events are not surfaced to consumers as glibre events
because the *state* they update is owned by a sibling aggregate, but
the event itself triggers a side effect inside the pump:

| SDL3 tag                                                                | Side effect                                                                                                                                                                                                                          | Public emission                              |
|-------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|----------------------------------------------|
| `SDL_EVENT_GAMEPAD_ADDED` / `_REMOVED`                                  | Update SDL3's internal gamepad table by calling `SDL_OpenGamepad` / `SDL_CloseGamepad` so subsequent axis/button events resolve a valid `device` index. No glibre `InputEvent` emitted; consumers query gamepad presence at use time. | None (R-6.1.11 partial).                     |
| `SDL_EVENT_DISPLAY_ADDED` / `_REMOVED` / `_ORIENTATION_CHANGED`         | Cause the next `Window::display()` call to re-query SDL3's display list (`Display` is a snapshot per SPEC §4.1 inv #5, not cached). Emitted as `WindowEvent::DisplayChanged { window, display }` for the focused window.              | `WindowEvent::DisplayChanged`.                |
| `SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED`                                | Update the owning `Window::Impl`'s cached `(LogicalSize, PhysicalSize, DpiScale)` snapshot via `surface::detail::query_layer_metrics` *before* the event becomes visible to the engine (§6.3 cross-module note in SPEC). Also updates the Pump::Impl-cached DpiScale value from the new `Window::Impl` value; the updated scale is passed to `translate_mouse` on the next `SDL_EVENT_MOUSE_MOTION` in this frame (consumed by the divide-by-scale path per §3.3). | `WindowEvent::DpiChanged`.                    |
| `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` / `_RESIZED`                      | Update the owning `Window::Impl`'s cached `LogicalSize` / `PhysicalSize`.                                                                                                                                                            | `WindowEvent::Resized`.                       |
| `SDL_EVENT_WINDOW_FOCUS_GAINED` / `_LOST`                               | None at the pump (the window aggregate's focus state is the queue contents themselves).                                                                                                                                              | `WindowEvent::FocusGained` / `FocusLost`.     |
| `SDL_EVENT_WINDOW_MINIMIZED` / `_RESTORED`                              | None.                                                                                                                                                                                                                                | `WindowEvent::Minimized` / `Restored`.        |
| `SDL_EVENT_WINDOW_CLOSE_REQUESTED`                                      | None — the engine decides whether to honour the request.                                                                                                                                                                             | `WindowEvent::CloseRequested`.                |
| `SDL_EVENT_QUIT`                                                        | Re-emitted as `WindowEvent::CloseRequested` against the focused window (or the primary window if no focus). Synthesises a single CloseRequested rather than introducing a separate `Quit` variant; the engine policy is the same.    | `WindowEvent::CloseRequested` (synthesised).  |
| `SDL_EVENT_DROP_FILE` / `_DROP_TEXT`                                    | Forwarded to the `FileWatcher` aggregate's drop-ingestion API; rides the `EventQueue<FileEvent>` ring (§3.6).                                                                                                                        | `FileEvent::Created` (on the file ring).       |
| `SDL_EVENT_TEXT_EDITING` (IME composition in-flight, not committed)     | Dropped silently. Composition state is OS-owned; only `SDL_EVENT_TEXT_INPUT` (committed) becomes a glibre event.                                                                                                                     | None.                                         |
| `SDL_EVENT_KEYMAP_CHANGED`                                              | Dropped at the pump. Keyboard layout queries are the input-plugin's job (R-14.2.5 refusal).                                                                                                                                          | None.                                         |
| `SDL_EVENT_USER` (custom synthetic events)                              | Dropped (§4.2 inv #5: sealed variant, no extension point).                                                                                                                                                                           | None.                                         |
| Anything else (sensor, touch, pen, locale-changed, terminating, …)      | Dropped at debug-log severity (§4.2 inv #5).                                                                                                                                                                                         | None.                                         |

This table is the closed disposition of every SDL3 event SDL3 emits
on macOS 26 / Apple Silicon. A future SDL3 release that adds new tags
either lands an explicit row here (translation) or falls through to
the "anything else" drop. Adding a new variant to `InputEvent` /
`WindowEvent` is a deliberate central edit per §4.2 inv #5; the
unknown-event drop refusal is what makes that edit safe.

### 3.6 Drag-drop ingestion (R-14.2.4 partial coverage)

`SDL_EVENT_DROP_FILE` carries a UTF-8 path string. The pump:

1. Calls `CanonicalPath::from_absolute(...)` on the SDL3-supplied
   path. A failure (non-UTF-8, non-absolute) is logged at `info`
   and dropped.
2. Calls `FileWatcher::ingest_drop(canonical_path)` — an
   internal-only entry point not on the public surface — which
   pushes a `FileEvent::Created { path }` onto the per-watcher
   ring corresponding to the drop target (typically the editor's
   "scratch" watch root).
3. Returns to the SDL3 drain loop without emitting any
   `InputEvent` / `WindowEvent`.

This is a controlled cross-aggregate call: the pump and watcher
both live in the platform plugin, share the platform sub-arena, and
the drop ingestion is small and well-typed. The reason it is here
rather than promoted to a `WindowEvent::FileDropped` variant: the
*consumer* of drop is content-import code, which already drains the
file queue for editor-managed assets; routing drop through a
separate `WindowEvent` would force every importer to drain two
queues. Single-consumer ⇒ single channel.

`ingest_drop` has one concrete caller at MVP (this branch — the
`SDL_EVENT_DROP_FILE` handler above). Per PHILOSOPHY §Anti-patterns,
promotion to a stable cross-aggregate internal contract is gated on
a second caller materialising. Until then, `ingest_drop` is
event-pump's private extension to file-watcher and may be removed if
the second caller never appears. See §12 [NON-BLOCKING] open question.

`SDL_EVENT_DROP_TEXT` (clipboard-style drop of plain text) is
dropped at `info` level in MVP; a `WindowEvent::TextDropped` variant
is gated behind the second-consumer trigger (editor's text-input
fields handle their own paste already).

### 3.7 Aggregate composition

```text
                                       Pump
                                        |
            +---------------------------+---------------------------+
            |                           |                           |
            v                           v                           v
   EventQueue<InputEvent>      EventQueue<WindowEvent>      (forwarded to
   (32k slots, ~1 MiB)         (16k slots, ~1 MiB)           FileWatcher's
                                                             EventQueue<FileEvent>)
```

- The `Pump`, the two `EventQueue<T>`s, and the translation table
  form one aggregate (§4.2 SPEC). Lifetimes are coupled: the
  queues are passed by reference into `Pump::create(input_q, window_q)`
  (§5.7 SPEC); the pump holds non-owning references; destroying
  either queue before the pump is undefined behaviour.
- The aggregate is constructed once at platform-plugin
  registration (`glibre_plugin_register`) and torn down once at
  unregistration. There is no per-frame allocation, no per-frame
  reseat.
- The translation table has no state — it is pure functions
  (`translate_keyboard`, etc.) over the `SDL_Event` union. Threading
  topology aside, the table is reentrant and thread-safe by
  construction; we don't rely on that today (single-threaded), but
  it costs nothing to keep.

## 4. Public surface

The public surface is **already locked** in `specs/platform/SPEC.md`
§5.3, §5.4, and §5.7. This section reproduces it verbatim for
review convenience and adds no new entry points. Any deviation
between the snippet here and the SPEC is a SPEC bug; the SPEC wins.

```cpp
namespace glibre::platform {

// §5.3 — bounded SPSC ring.
template <class T>
class EventQueue {
public:
    [[nodiscard]] static auto with_capacity(std::size_t capacity) noexcept
        -> Result<EventQueue>;

    EventQueue(EventQueue&&) noexcept;
    EventQueue& operator=(EventQueue&&) noexcept;
    EventQueue(const EventQueue&)            = delete;
    EventQueue& operator=(const EventQueue&) = delete;
    ~EventQueue();

    [[nodiscard]] auto capacity() const noexcept -> std::size_t;
    [[nodiscard]] auto size()     const noexcept -> std::size_t;
    [[nodiscard]] auto empty()    const noexcept -> bool;

    // Engine-side drain; never blocks; never reorders within a single
    // device. Returns the number of events written into `out`.
    [[nodiscard]] auto drain(eastl::span<T> out) noexcept -> std::size_t;
};

// §5.4 — closed sums.
using InputEvent  = eastl::variant<
    input::KeyDown, input::KeyUp,
    input::MouseMove, input::MouseButtonEv, input::Wheel,
    input::TextInput,
    input::GamepadAxisEv, input::GamepadBtnEv>;

using WindowEvent = eastl::variant<
    window_event::Resized, window_event::DpiChanged,
    window_event::Minimized, window_event::Restored,
    window_event::FocusGained, window_event::FocusLost,
    window_event::CloseRequested, window_event::DisplayChanged>;

// §5.7 — pump.
class Pump {
public:
    [[nodiscard]] static auto create(EventQueue<InputEvent>&  input_queue,
                                     EventQueue<WindowEvent>& window_queue) noexcept
        -> Result<Pump>;

    Pump(Pump&&) noexcept;
    Pump& operator=(Pump&&) noexcept;
    Pump(const Pump&)            = delete;
    Pump& operator=(const Pump&) = delete;
    ~Pump();

    // §4.2 inv #1: main-thread-only, exactly one call per frame.
    // Returns total events enqueued across both queues this cycle.
    [[nodiscard]] auto drain() noexcept -> Result<std::size_t>;
};

}  // namespace glibre::platform
```

ABI-relevant properties (cross-references `plugin-abi.md`):

- Every public function is `noexcept`. No exception ever crosses
  the plugin boundary (`plugin-abi.md` §"Boundary Rules").
- Every fallible function returns `Result<T> = std::expected<T,
  glibre::Error>`. Platform contributes one arm —
  `platform::Error` — to the engine-wide `glibre::Error` variant
  (`error-model.md` §"Composition Rules").
- The header `glibre/platform/platform.hpp` does not include any
  SDL3, AppKit, metal-cpp, or POSIX header. No `#ifdef __APPLE__`
  in the public surface. The `Pump`'s implementation is pimpl
  (`Impl* impl_`) so the SDL3 dependency stays in the .cpp.
- No Obj-C++. The translation table is pure C++; SDL3's macOS
  back-end already bridges to AppKit internally.

The internal-only entry point used by §3.6 — `FileWatcher::ingest_drop`
— is **not** on the public surface; it lives in
`engine/platform/src/watcher/internal.hpp` and is callable only from
within the platform plugin's own translation units.

## 5. Hot/cold path split

The aggregate has a sharp hot/cold boundary that the SRP rests on.
Mixing the two would force the cold path to either allocate per-frame
(violating §6.9 SPEC) or to take a lock the hot path could observe.

### 5.1 Hot path (every frame, phase 1)

Three operations, each O(events-this-frame):

1. **`Pump::drain()`** — `SDL_PumpEvents` + `while (SDL_PollEvent)` +
   classify-and-translate + `try_push` per emitted event. Zero
   allocation; zero locking; pure stack + atomics. Steady-state cost
   is §9 below.
2. **`EventQueue<InputEvent>::drain(span)`** — load `head` (acquire),
   memcpy up to `min(span.size(), head - tail)` slots, store `tail`
   (release). Zero allocation.
3. **`EventQueue<WindowEvent>::drain(span)`** — same shape.

These three operations comprise the entire phase 1 contribution of
the platform context (the `FileWatcher` event drain happens on its
own thread, off-frame; see §6 SPEC). Nothing else in the platform
context runs on the driver thread between phase 1 and phase 9.

The only branch in the hot path is the classify dispatch in
`Pump::drain` (a switch on `SDL_Event::type`), which the compiler
turns into a jump table of ≤ 64 entries. There is no virtual
dispatch, no `std::function`, no `eastl::function`.

### 5.2 Cold path (rare events)

Three operations, each O(1) amortised but non-trivial in absolute cost:

1. **`Pump::create(input_q, window_q)`** — initial SDL3 subsystem
   bring-up. Calls `SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)`,
   captures the queue references into `Pump::Impl`, and primes the
   gamepad table by enumerating any already-connected gamepads. May
   allocate at boot from the platform sub-arena. Cost: ~1 ms on macOS
   M1; runs once per platform-plugin lifetime.
2. **`EventQueue<T>::with_capacity(N)`** — round `N` up to a power
   of two, allocate the slot array from the platform sub-arena, zero
   the `head` / `tail` atomics. Cost: ~10 µs; runs twice per
   platform-plugin lifetime.
3. **`~Pump()` / `~EventQueue()`** — tear-down on plugin unload or
   process exit. Releases the slot array back to the sub-arena, calls
   `SDL_QuitSubSystem`. Cost: ~1 ms; runs once per lifetime.

Subscription management (R-14.1.9 talks about a "channel" — implicit
in glibre's `EventQueue<T>` reference handed to `Pump::create`) does
not have a runtime add/remove API in MVP. The set of subscribers is
fixed at platform-plugin registration: one input queue + one window
queue, both vended to the engine through the
`platform::register_plugin_state(...)` middleman path
(§8.5 SPEC + `hot-reload-protocol.md` §"Middleman Types").

### 5.3 Why the split matters

- The hot path is allocation-free and lock-free — a fundamental
  property required by §9 (zero bytes / frame from platform on the
  driver thread) and §6.9 SPEC.
- The cold path is allowed to allocate from the sub-arena because
  it runs at well-known points (boot, plugin reload) outside the
  per-frame budget. §9 carries no cell for cold-path cost.
- Subscriber-list management *not* being a runtime API is the
  smallest possible extension surface. R-14.1.9's "bounded async
  channel" is realised by the queue's bounded capacity, not by a
  pub/sub bus that would force lock-free subscriber walks per push.
  Adding runtime subscription is gated behind the second-consumer
  trigger (today there is exactly one consumer per family: the
  engine's phase-1 input system).

## 6. Concurrency

The pump's threading topology is forced by SDL3's contract: SDL3
event pumping is **main-thread-only** on macOS (the AppKit run-loop
must own the OS event source). We do not pretend otherwise.

### 6.1 Threads involved

| Thread          | Role                                                            | Lifetime                  |
|-----------------|-----------------------------------------------------------------|---------------------------|
| Main            | Producer (`Pump::drain`) and consumer (`EventQueue<T>::drain`)  | Process                   |
| (none)          | The pump owns no helper thread.                                 | n/a                       |

The pump itself owns zero threads. The `FileWatcher` aggregate
(sibling design) owns one I/O thread that produces `FileEvent` into
its own ring; the `FileIo` aggregate (sibling) owns up to two
worker threads for bounded-async file I/O (`FileIoConfig::io_thread_budget`).
Those threads do not touch the pump's queues.

### 6.2 Synchronisation primitives

- `std::atomic<std::size_t>` head and tail per `EventQueue<T>::Impl`
  (§3.4). Release/acquire pairing as described.
- No mutex inside the pump. No condition variable. No semaphore.
- The pump never calls `SDL_WaitEvent` (which would block the main
  thread). Phase pacing is owned by phase 9 / `CAMetalDisplayLink`,
  not by the pump.

### 6.3 Hand-off to consumers

The contract between the pump (producer) and the engine's input
system (consumer) is **per-frame snapshot semantics**:

- Phase 1 entry: `Pump::drain()` runs first. Returns
  `Result<std::size_t>` with the total events enqueued this cycle.
- Phase 1 mid: the engine's input system calls
  `EventQueue<InputEvent>::drain(span)` and
  `EventQueue<WindowEvent>::drain(span)` once each, into
  caller-supplied (typically per-frame transient) buffers.
- Phase 1 exit: both queues are empty (under steady-state load;
  see §9 for over-flow ceiling).

Multiple consumers are not supported in MVP. If a future plugin wants
to peek at the input stream (e.g. an editor-side recorder), the
contract is **fan-out at the consumer**, not at the pump — the
input system writes the drained events into ECS components, and any
peer plugin reads those components in phase 2. R-14.1.9's
"bounded channel" rule does not require multi-consumer; it requires
exactly-once-FIFO-never-drop, which the single-consumer SPSC delivers.

### 6.4 Off-thread call refusal

Calling `Pump::drain()` from any thread other than the main thread
is a contract violation. Detection in MVP:

- **Debug builds.** `Pump::drain` opens with
  `assert(std::this_thread::get_id() == platform::main_thread_id)`.
  The aggregate captures `main_thread_id` at construction.
- **Release builds.** Same check, downgraded to a one-shot warn-then-
  return-`Error::Unsupported` (per §10.3.2 SPEC, the pump's drain is
  permitted to return `Unsupported` on off-main-thread call). No
  abort in shipping builds because a misbehaving plugin should
  surface as an error, not crash the engine.

This is the canonical detection pattern consistent with SPEC §4.1
inv #2 and §4.2 inv #1 — assert in debug, demote in shipping.

### 6.5 Hot-reload barrier interaction

Per §8.2 SPEC ("Pump quiescence at the barrier"): phase 8 runs
after `render-submit` and before `present`. The per-frame
`Pump::drain` already executed in phase 1 and will not run again
until phase 1 of frame N+1. Therefore the queues are consistent
(neither full-mid-write nor partially drained) when the loader
takes phase 8. No coordination between platform and the loader is
needed; it falls out of the frame-phase ordering. This design
preserves that property by **forbidding any call to `Pump::drain`
or `EventQueue::drain` outside phase 1**.

## 7. Persistence + ABI

Per §7.1 SPEC: the pump and its queues are **not persistent**. Their
state is in-flight ring contents, recreated from scratch on each
process bring-up, drained every frame, never written to disk by
platform code, never crossing a save boundary.

Consequence:

- **Zero `.fory` schemas** for `Pump` / `EventQueue<InputEvent>` /
  `EventQueue<WindowEvent>` / `InputEvent` / `WindowEvent`
  (`fory-codegen.md` enumerates `data/schemas/platform/**/*.fory`
  and finds none for these types).
- **No migration ledger entry** — there is nothing to migrate
  (SPEC §7.2).
- **No version field** on any payload. Adding a new variant to
  `InputEvent` / `WindowEvent` is an ABI-hash bump, not a schema
  version increment.

ABI surface (`plugin-abi.md` §"Type ABI"):

- The `EventQueue<InputEvent>` and `EventQueue<WindowEvent>`
  *references* are passed across the platform-plugin → engine seam
  via the standard middleman path. Per §8.5 SPEC, no
  `glibre::types::platform::EventQueueSurvival` middleman type is
  declared today because the queues themselves are torn down and
  rebuilt on platform self-reload (their contents are by-construction
  empty at the barrier — §8.1 SPEC). What survives the reload is
  the *engine-side reference holder*, which Q::register repopulates
  with the freshly-vended queue handles (§8.6 SPEC observer reseat).
- The `InputEvent` / `WindowEvent` variants and their payload
  structs are part of the platform's contribution to the
  engine-wide `host_glibre_types_abi_hash`. Adding a variant bumps
  the hash. Removing a variant bumps the hash. Reordering variants
  bumps the hash. The hash is computed by `Foryc` over the
  declarations under `glibre/types/platform/event/*.hpp`
  (`fory-codegen.md` §"ABI Hash").
- No raw pointer to an SDL3 type, AppKit type, or `CAMetalLayer*`
  appears in any of the variant payloads. `WindowId` and `DisplayId`
  are the only handles, both `std::uint32_t`.

## 8. Hot-reload

Per §8.1 / §8.2 SPEC the pump aggregate's hot-reload disposition
collapses to one rule: **the pump survives every kind of swap, but
its queues are quiescent (empty) at the barrier and the references
to the queue objects are re-bound by Q::register**.

### 8.1 Survival across peer-plugin swap

When a peer plugin (render, content, gameplay, tools) swaps:

- The pump keeps running. Its `Pump::Impl` is unaffected — it lives
  in the platform `.dylib`, not in the swapping plugin.
- The two queues keep running. Their slot storage lives in the
  platform sub-arena, not in the swapping plugin.
- The engine-side queue reference may have been held by a peer
  plugin's system. That system is rebuilt by Q::register and
  reseats by re-acquiring the queue handle through the platform
  API — same numeric `WindowId`, same queue object identity.

`Pump::drain()` runs unchanged in phase 1 of frame N (the swap
frame); the events drained go into the same queue; the new peer-
plugin systems read those events in their phase-1 consumer body.

### 8.2 Survival across platform-plugin self-reload

Per §8.3 SPEC, when the platform `.dylib` itself is the outgoing
plugin P:

- **Drain (step 1 of the protocol).** The barrier already
  guarantees the queues are empty (§8.2 SPEC pump-quiescence). The
  pump's `drain()` does not need to be invoked specially during
  step 1; the natural phase-1 drain of frame N already happened
  and frame N+1 has not started.
- **Swap (step 2).** Standard `dlclose(P) ; dlopen(Q)`.
  `host_glibre_types_abi_hash` covers the `InputEvent` /
  `WindowEvent` variants; a layout-differing rebuild forces a
  hash mismatch and the loader refuses (§8.4 universal refusal).
- **Migrate (step 3).** Empty. The pump has no plugin-private
  bytes to reshape (§8.1 SPEC "migrate body: empty").
- **Resume (step 4).** Q::register:
  1. Re-creates `EventQueue<InputEvent>` and
     `EventQueue<WindowEvent>` at the same capacities.
  2. Re-creates `Pump` against the new queue references.
  3. Re-publishes the queue handles through the engine-side
     reference holder (§8.6 SPEC observer reseat — input/window
     event consumers re-acquire the queues).
- **Mid-replay determinism (§8.7 SPEC).** The e2e harness's
  `InputDriver` is frame-locked, not handle-locked: it writes
  synthesised `InputEvent`s into whatever queue the platform plugin
  currently exposes, before phase 1 of each replayed frame. A
  platform self-reload between frames is therefore invisible to
  replay; the determinism gate in CI exercises this with a
  "reload mid-replay" e2e variant.

### 8.3 Refusal cases

Per §8.4 SPEC, the only pump-specific refusal is **P1 — mid-frame
window drop** — which is not strictly a pump failure but a
`Window` aggregate failure that happens to be caught at the same
barrier. The pump itself contributes no additional refusal beyond
the universal three (ABI hash mismatch, schema migration failure
[vacuous], plugin init failure).

Specifically, the `EventQueue<T>` does **not** refuse the swap if
non-empty: the §8.2 SPEC quiescence rule guarantees the queues are
empty by frame ordering, and any non-empty observation at step 1
would be a `frame-phases.md` violation surfacing as
`core::Error::FramePhaseMisordered` rather than a pump-level
refusal.

### 8.4 Subscriber re-validation at the barrier

R-14.1.9's bounded-channel contract permits one consumer per queue
in MVP. At the barrier:

- The engine-side reference holder's slot for `EventQueue<InputEvent>`
  is null between step 2 and step 4.1 (the queue object was torn
  down with the outgoing platform plugin).
- Q::register at step 4.2 re-vends the new queue object and the
  reference holder is repopulated.
- The peer-plugin input-system's queue handle is invalidated for
  the duration of the swap; the system body would not run during
  the barrier (phase 8 ≠ phase 1) so this is observable only by
  test fixtures that probe the reference between phases. The
  observer-reseat protocol (§8.6 SPEC) covers this case: the
  `HotReloadCompleted` event is published synchronously on the
  loader thread before phase 9 begins, and any subscriber that
  cached a queue handle re-acquires it inside the observer
  callback.

There is no per-subscriber callback ABI inside the pump itself.
The "subscriber list" is implicit in the engine's reference holder;
re-validation is the holder's job, not the pump's.

## 9. Performance

This section refines §9.1 / §9.2 / §9.4 SPEC for the pump aggregate.
The cell numbers are quoted from the SPEC, not invented here.

### 9.1 CPU budget (cited from `perf-budget.md` and SPEC §9.1)

| Aggregate                   | Phase | CPU ms (sim) | CPU ms (submit) | Notes                                                           |
|-----------------------------|-------|--------------|-----------------|-----------------------------------------------------------------|
| `EventQueue<T>` / `Pump`    | 1     | 0.100        | 0.000           | One `SDL_PumpEvents` + drain loop; per-event classify+translate; SPSC `try_push` per emitted event. |

The `Pump::drain` budget is **0.100 ms p50, 0.180 ms p99** on the
S1 sample scene under the §9.4 SPEC fixture (≤ 128 events / frame).
Decomposition:

- `SDL_PumpEvents` call: ~5 µs (kernel boundary + AppKit run-loop
  dispatch). One per drain.
- Per-event `SDL_PollEvent`: ~0.3 µs each (one cmpxchg on SDL3's
  internal ring + one memcpy of `SDL_Event` 56 bytes).
- Per-event classify (switch dispatch on `SDL_Event::type`):
  ~0.05 µs. The compiler turns this into a jump table.
- Per-event translate (e.g. keyboard payload pack): ~0.1 µs. No
  branches inside the translation function for the common case.
- Per-event `EventQueue::try_push`: ~0.05 µs (one relaxed store +
  one release store on `head`).
- Total per event: ~0.5 µs.
- 128 events: ~64 µs ≈ 0.064 ms. Plus SDL3 overhead ≈ 0.07 ms.
  Comfortably inside the 0.10 ms p50 cell.

The 0.180 ms p99 covers SDL3's occasional internal jitter (gamepad
table mutations, display reconfig delivered in-band, run-loop
priority inversions on macOS thermal events).

The cell **does not include**:

- Time spent inside the engine's phase-1 input system (which calls
  `EventQueue::drain` and writes ECS components). That is the
  input-plugin's budget cell.
- Time spent in `FileWatcher`'s I/O thread (off-thread; not on the
  driver-thread cell per §9.1 SPEC).

### 9.2 Heap residency (cited from SPEC §9.2)

| Aggregate                       | Sub-arena | Contents                                                                                  |
|---------------------------------|-----------|-------------------------------------------------------------------------------------------|
| `EventQueue<T>` / `Pump` (§4.2) | 2 MiB     | `EventQueue<InputEvent>` slot array (1 MiB ≈ 32k × 32 B) + `EventQueue<WindowEvent>` slot array (1 MiB ≈ 16k × 64 B). Sized for the largest observed burst (drag-resize spam, focus-change storms) without overflow. |

Sizing rationale:

- **`InputEvent` payload size.** `eastl::variant<8 alternatives>`
  with the largest alternative (`TextInput` with 32-byte UTF-8
  buffer) sets the slot at ~32 B + variant tag. Power-of-two round
  up gives 32 B / slot. 32k slots × 32 B = 1 MiB.
- **`WindowEvent` payload size.** Slightly larger because
  `Resized` carries `LogicalSize + PhysicalSize` (~16 B); slot
  rounds to 64 B. 16k slots × 64 B = 1 MiB.
- **Burst headroom.** Drag-resize on macOS emits one
  `WindowEvent::Resized` per pixel-change (~120 / s on a 1080p
  drag). Even sustained drag never approaches 16k events / frame
  (would require 240 fps × 16k / 240 = 16k events / frame — orders
  of magnitude above observed). Headroom is generous on purpose;
  §4.2 inv #6 forbids resizing.

### 9.3 Allocation budget (cited from SPEC §6.9 and §9.3)

After construction:

- `Pump::drain()` allocates **zero bytes**.
- `EventQueue<T>::drain(span)` allocates **zero bytes**.
- `EventQueue<T>::with_capacity` allocates **once** at boot (slot
  array, from the platform sub-arena).
- `Pump::create` allocates **once** at boot (pimpl, from the
  platform sub-arena).

The steady-state per-frame allocation rate from the pump aggregate
on the driver thread is **zero bytes**. This is the load-bearing
guarantee of §9.4 SPEC fixture #1 (CI gate: zero allocations on the
driver thread between `Pump::init` and `Pump::shutdown`, verified
by tagged-allocator counter).

### 9.4 Drain wall-time gate (CI fixture #1, cited from SPEC §9.4)

The Catch2 `BENCHMARK` block under `tests/platform/perf/` synthesises
the S1 event stream (keyboard / mouse / window-focus / DPI-change /
display-hot-plug at the densities observed in the sample-scene
replay; ≤ 128 events / frame), runs `Pump::drain()` for 600 frames,
and asserts:

- **p50 driver-thread `Pump::drain()` time ≤ 0.10 ms.**
- **p99 ≤ 0.18 ms.**
- **Zero dropped events** (the SPSC ring never overflows under
  fixture load; verified by counting events injected vs events
  drained).
- **Zero allocations** on the driver thread between `Pump::init`
  and `Pump::shutdown` (verified by the
  `core::PerContextAllocator::resident_bytes(ContextTag::Platform)`
  delta being ≤ the boot-time allocation).

CI runs this on `macos-26-m1` runners only; the SDL3 event source
and `mach_absolute_time` are host-platform-specific.

### 9.5 Idle-frame floor (CI fixture #2, cited from SPEC §9.4)

With no events injected, `Pump::drain()` returns 0 events and runs
in ≤ 5 µs on M1 (the `SDL_PumpEvents` + empty `SDL_PollEvent` loop).
Asserted at p99 ≤ 5 µs across 600 frames.

### 9.6 What the cell deliberately does not cover

- Sustained event floods above 128 / frame. Outside the S1
  scenario; if a future scenario needs it, the §9.2 sub-arena
  scales linearly, and the §9.1 budget grows linearly until it
  consumes the 0.099 ms reserved tail. Beyond that, an amendment
  spike against `perf-budget.md` opens.
- Phase-9 cost (the cell is 0.05 ms in submit). Phase 9 reads
  `Clock::wall()` and posts the SDL3 → CAMetalLayer present; no
  pump activity participates.

## 10. Failure modes

This section refines §10.3.2 SPEC for the pump aggregate. Every
failure is a `glibre::Error` arm (with the platform-private
`platform::Error` rolled into one variant) per
`reviews/decisions/error-model.md`.

### 10.1 Per-entry-point failure surface (cited from SPEC §10.3.2)

| Entry point                     | Returnable arms                                  | Trigger                                         |
|---------------------------------|--------------------------------------------------|-------------------------------------------------|
| `EventQueue<T>::with_capacity`  | `Unsupported`                                    | zero capacity / capacity overflow beyond u32::max (the sub-arena ceiling); non-power-of-two is silently rounded up (not an error) |
| `EventQueue<T>::drain`          | total — never returns `unexpected`               | drain is infallible by design (SPSC pop)        |
| `Pump::create`                  | `IoFailure { OsCode }`                           | `SDL_InitSubSystem(SDL_INIT_VIDEO \| SDL_INIT_GAMEPAD)` failure |
| `Pump::drain`                   | `IoFailure { OsCode { 0 } }` with prefix `"queue-full"` (fatal); `Unsupported` (off-main-thread) | Queue full at `try_push` site (§4.2 inv #3 fatal); drain called from non-main thread (§6.4) |

Failure-translation seam: per §6.10 SPEC, `Pump::create` uses
`detail::error::sdl_to_error(SDL_GetError())`; `Pump::drain` directly
constructs `Error::IoFailure { OsCode { 0 } }` on queue-full because
the failure is *not* an SDL3 error — it is a glibre invariant
violation. The `"queue-full"` prefix is stamped into the thread-local
diagnostic buffer at the same site.

Note: `specs/platform/SPEC.md` §10.3.2 predates this design refinement
and still reads "zero / wildly oversized capacity" as the sole trigger
for `Unsupported` from `EventQueue<T>::with_capacity`. This design
canonicalises silent round-up to the next power-of-two (§3.4) — a
non-power-of-two capacity is not an error. SPEC §10.3.2 must be amended
to replace the trigger description with "capacity overflow beyond
`u32::max`" before the first plan PR consuming this design lands.
Tracked in §12 [BLOCKING IMPLEMENTATION].

### 10.2 `SDLPollFailed` recovery shape

`Pump::drain()` interprets `SDL_PollEvent` returning a negative value
(very rare; SDL3 documents this as "not expected to occur in normal
operation") as a translated `IoFailure { OsCode { errno } }`. The
recovery contract:

1. Caller (engine frame loop) catches `IoFailure` from
   `Pump::drain`.
2. Caller logs at `error` severity with the `OsCode` value.
3. Caller does **not** retry; the pump's drain is non-idempotent
   per §4.2 inv #1 (exactly one drain per frame). A retry would
   re-execute `SDL_PumpEvents` and silently double-pump.
4. Caller treats the failure as fatal and sets the engine's exit
   code via `Process::set_exit_code` (a clean shutdown path).

This is the same recovery contract as SPEC §10.3.2's
`"queue-full"` arm: log-and-fatal, no retry.

### 10.3 `SubscriberOverflow` (queue-full) recovery shape

When `EventQueue<T>::try_push` returns false during `Pump::drain`:

1. The pump calls `detail::error::set_prefix(prefix::queue_full)`.
   The stamp must precede the log so `log_error` reads the stamped
   prefix. Reorder is cold-path-only — no impact on hot-path performance.
2. The pump emits an `error`-severity log with the prefix
   `"queue-full"` and the queue family name (reads the stamped
   prefix set in step 1).
3. The pump returns `unexpected(IoFailure { OsCode { 0 } })`
   from the current `Pump::drain` call. The TLS prefix stamp is the
   load-bearing discriminator: callers that receive `IoFailure` from
   `Pump::drain` read `current_prefix()` to confirm the queue-full
   case. Without the stamp a caller testing
   `current_prefix() == prefix::queue_full` would see `prefix::none`
   and silently misroute the error.
4. The engine frame loop catches the failure, logs at `error`,
   sets the exit code, and tears down. There is no graceful
   recovery — a queue-full at sustained load is a misconfiguration
   (sub-arena sized too small for the workload); the only real
   fix is rebuild + restart.

§4.2 inv #3 is uncompromising on this point: drop-or-coalesce would
break the exactly-once R-14.1.9 contract; resize-on-overflow would
break §4.2 inv #6. The platform refuses both. If the operator's
workload genuinely exceeds the §9.2 sub-arena ceiling, they need an
amendment spike against `perf-budget.md`, not a runtime escape.

### 10.4 Off-main-thread refusal recovery

`Pump::drain()` called from any thread other than the main thread
returns `unexpected(Unsupported)` and emits a once-per-process
`error`-severity log. There is no retry path; calling drain from a
worker thread is a programming error. Debug builds assert hard at
the offending site so the bug surfaces in tests.

### 10.5 What the pump deliberately does NOT raise

- **`NotFound`.** The pump never resolves names; it has nothing to
  not-find.
- **`PermissionDenied`.** No path / permission boundary inside the
  pump body. (SDL3's gamepad-add hot-plug failures surface as
  `IoFailure` from the gamepad subsystem, not as
  `PermissionDenied`.)
- **`AlreadyExists`.** The pump is single-instance per platform
  plugin (one `Pump::create` per plugin lifetime); duplicate
  construction is a programming error caught at construction
  (`unexpected(Unsupported)` per SPEC §10.3.2 if SDL3 is already
  initialized — this is rare and falls under `Unsupported`).
- **`Interrupted`.** The pump's drain loop is non-blocking; there
  is nothing to interrupt.

### 10.6 Severity table (cited from SPEC §10.6)

| Trigger                                  | Severity | Once-per |
|------------------------------------------|----------|----------|
| `Pump::create` SDL3 init failed          | `error`  | call     |
| `Pump::drain` queue full                 | `error`  | call (process fatal anyway) |
| `Pump::drain` off-main-thread            | `error`  | process  |
| `Pump::drain` `SDL_PollEvent` returned negative | `error`  | call (process fatal) |
| Unknown SDL3 event tag (sealed-variant drop) | `debug`  | call     |
| `SDL_EVENT_TEXT_EDITING` (composition in-flight) drop | `trace`  | call     |
| `SDL_EVENT_USER` drop                    | `debug`  | call     |
| `SDL_EVENT_DROP_FILE` non-canonical path | `info`   | call     |

## 11. Test plan

Catch2 tests live under `tests/platform/event_pump/` (unit) and
`tests/platform/integration/` (integration). The user-story-level
acceptance criterion is `specs/platform/SPEC.md` §11 (cross-references
the platform user-story issues).

### 11.1 Unit tests

1. **`event_pump_translates_keyboard`** — feed a synthetic
   `SDL_Event` of type `SDL_EVENT_KEY_DOWN` with known scancode,
   keycode, modifiers, repeat flag; assert
   `EventQueue<InputEvent>::drain` returns one element matching
   `input::KeyDown { ... }` field-for-field.
2. **`event_pump_translates_mouse_button_and_wheel`** — same
   shape for `SDL_MouseButtonEvent` and `SDL_MouseWheelEvent`,
   covering left / right / middle / X1 / X2 buttons and natural-
   scroll flipped-direction.
3. **`event_pump_translates_window_resize_and_dpi`** — assert
   `WindowEvent::Resized { logical, physical }` and
   `WindowEvent::DpiChanged { scale }` payloads round-trip from
   SDL3 events; specifically verify that the cached
   `(LogicalSize, PhysicalSize, DpiScale)` snapshot on
   `Window::Impl` is updated *before* the typed event is enqueued
   (§3.5 / §6.3 SPEC cross-module note).
3b. **`event_pump_mouse_relative_motion_divided_by_dpi_scale`** — call
    `translate_mouse(ev, DpiScale{2.0f})` with a `SDL_MouseMotionEvent`
    whose relative `dx/dy` is 200 physical pixels; assert the emitted
    `MouseMove::dx/dy` is `100.0f` (logical points). Confirms the
    divide-by-dpi_scale invariant documented in §3.3 and that the
    `DpiScale` parameter is the operative divisor. (R-6.1.4)
4. **`event_pump_drops_unknown_sdl_tag`** — feed an
   `SDL_EVENT_USER` and an out-of-MVP-range tag; assert the
   queues remain empty and a `debug`-level log line was emitted.
5. **`event_pump_seals_unknown_keycode`** — feed a `SDL_EVENT_KEY_DOWN`
   with a scancode outside the closed `ScanCode` enum; assert it
   is dropped (debug log) and never queued as `Unknown`. (§3.3
   sealed-enum rule.)
6. **`event_pump_normalizes_gamepad_axis_range`** — feed a
   `SDL_GamepadAxisEvent` with raw value 32767 (left stick)
   and 32767 (left trigger); assert the emitted
   `input::GamepadAxisEv::value` is `1.0f` for both.
6b. **`event_pump_trigger_released_state_is_negative_one`** — feed a
    `SDL_GamepadAxisEvent` with raw value 0 for `LeftTrigger`; assert
    the emitted `input::GamepadAxisEv::value` is `-1.0f`. Documents
    the invariant: released trigger = -1.0; consumer remap is
    `(axis + 1.0f) / 2.0f` for released-as-zero semantics. (§3.3.)
7. **`event_pump_text_input_copies_sdl_buffer`** —
   feed a single `SDL_EVENT_TEXT_INPUT` with a 28-byte UTF-8
   string (within SDL3's 32-byte `text[]` buffer); assert one emitted
   `input::TextInput` event carrying the exact bytes. Also assert that
   the production translator does NOT contain a multi-event splitting
   path (SDL3 truncates at 32 bytes; splitting is test-harness-only).
8. **`event_queue_overflow_returns_io_failure`** — fill the
   `EventQueue<InputEvent>` to capacity, then attempt one more
   push via the pump; assert `Pump::drain()` returns
   `unexpected(IoFailure { OsCode { 0 } })` whose stamped
   diagnostic prefix is `"queue-full"`. (§10.3 SPEC.)
8b. **`event_pump_with_capacity_rounds_up_to_power_of_two`** — assert
    `EventQueue<InputEvent>::with_capacity(100).capacity() == 128`.
    Confirms the silent round-up contract; non-power-of-two is never
    an error. (§3.4, §10.1.)
9. **`event_queue_overflow_refuses_coalesce`** — same setup as
   #8 but with all events being `WindowEvent::DpiChanged` (one
   of the §4.2 inv #4 never-coalesce events); assert the failure
   surfaces rather than the events being coalesced.
10. **`event_queue_drain_preserves_per_device_order`** — push
    events from two synthetic gamepad devices interleaved; assert
    that for each device the per-device sub-sequence is monotone in
    insertion order. (§3.2.)
11. **`pump_drain_off_main_thread_returns_unsupported`** — call
    `Pump::drain` from a `std::thread`; assert
    `unexpected(Unsupported)` and a process-once `error` log. (§6.4
    + §10.4.)
12. **`pump_drain_zero_events_idle_floor`** — call `Pump::drain`
    with no SDL3 events queued; assert it returns
    `expected<std::size_t>{0}` and runs in ≤ 5 µs on M1 (Catch2
    `BENCHMARK` block, soft assertion in dev, hard in CI). (§9.5.)
13. **`pump_drain_zero_alloc`** — wrap the pump aggregate in the
    tagged allocator probe; call `Pump::drain` 600 times under the
    S1 fixture; assert the platform `ContextTag` resident bytes do
    not change between calls. (§9.4 fixture #1 contribution.)
14. **`pump_single_poller_no_other_caller`** — build-time linker
    probe: assert that no translation unit outside
    `engine/platform/src/event/` calls `SDL_PollEvent`, `SDL_WaitEvent`,
    or `SDL_PumpEvents`. Realised as a test-only `nm` /
    `llvm-objdump --syms` scan in CI. (R-14.1.11 single-poller.)

### 11.2 Integration tests

1. **`event_pump_full_lifecycle`** — open a `Window`, create a
   `Pump`, run a 60-frame loop where the OS queue is fed via
   `SDL_PushEvent` from a fixture thread before each phase 1;
   assert each frame's drain returns the expected event count and
   the queue is empty at phase exit.
2. **`event_pump_subscriber_churn_across_swap`** — load the
   platform plugin, drain a few frames, trigger a platform
   self-reload via `glibre::platform::test::enqueue_platform_reload`
   (§8.7 SPEC), continue draining; assert (a) no event is lost
   across the swap, (b) the new queue handles are correctly
   re-bound by the engine-side reference holder, (c) replay
   determinism is preserved (the e2e harness's `InputDriver`
   continues injecting against the new queues without test code
   changes).
3. **`event_pump_drag_resize_burst`** — synthesise 256 rapid
   `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` events in one frame
   (drag-resize burst); assert (a) all 256 are delivered as
   `WindowEvent::Resized` (no coalescing), (b) the §9.4 p99
   budget is met, (c) no overflow.
4. **`event_pump_focus_loss_during_keypress`** — synthesise
   `KEY_DOWN(Cmd) + KEY_DOWN(Tab) + WINDOW_FOCUS_LOST + KEY_UP(Tab) + KEY_UP(Cmd)`;
   assert that the events arrive in their queues with the
   global ordering preserved (the FocusLost event is written
   between the two KeyDowns and the two KeyUps, observed by the
   consumer reading both queues into a merged timeline by
   per-event timestamp).
5. **`event_pump_drop_file_routes_to_filewatcher`** — emit
   `SDL_EVENT_DROP_FILE` with a valid absolute path; assert
   (a) no event lands on `EventQueue<InputEvent>` /
   `EventQueue<WindowEvent>`, (b) one `FileEvent::Created`
   appears on the `FileWatcher`'s scratch ring with a canonical
   path. (§3.6.)
6. **`event_pump_quit_synthesises_close_requested`** — emit
   `SDL_EVENT_QUIT`; assert one `WindowEvent::CloseRequested`
   appears on the focused window's queue, no `Quit` variant is
   exposed. (§3.5.)

### 11.3 E2E coverage

The platform user-story for "engine consumes OS events" lives in
`specs/e2e/SPEC.md`'s S1 trace: a 600-frame replay drives synthetic
input events through the pump and asserts the resulting ECS
component snapshots are byte-equal across runs.

**Drag-drop and quit-synthesis E2E coverage status:**
- Drag-drop (`SDL_EVENT_DROP_FILE`) is asserted in the S1 trace at
  step 7 (file-drop-into-window-surface), which exercises the
  `§3.6` routing path through `FileWatcher::ingest_drop`. Integration
  test #5 (`event_pump_drop_file_routes_to_filewatcher`) covers the
  direct routing assertion.
- Quit-synthesis (`SDL_EVENT_QUIT` → `WindowEvent::CloseRequested`)
  is integration-test-only at MVP per §12 [NON-BLOCKING]: no E2E
  trace covers menu-driven quit. The S1 trace exits via window close,
  not Cmd-Q. Integration test #6
  (`event_pump_quit_synthesises_close_requested`) is the acceptance
  gate. The gap is tracked in §12 [NON-BLOCKING].

The "reload mid-replay" e2e variant (§8.7 SPEC) covers the
hot-reload survival path; runs against the same S1 trace with a
`platform::test::enqueue_platform_reload` injected at frame 300.

### 11.4 Coverage matrix

| Concern                                    | Test                                                                                  |
|--------------------------------------------|---------------------------------------------------------------------------------------|
| SDL3 → typed translation correctness       | unit #1, #2, #3, #3b, #6, #7                                                          |
| Mouse dx/dy DPI divide (§3.3, R-6.1.4)    | unit #3b                                                                              |
| Trigger released-state formula (§3.3)      | unit #6b                                                                              |
| with_capacity round-up (§3.4, §10.1)      | unit #8b                                                                              |
| Sealed-variant boundary (unknown drop)     | unit #4, #5                                                                           |
| Per-device monotone ordering (§4.2 inv #2) | unit #10; integration #4                                                              |
| Bounded-channel never-drop (§4.2 inv #3)   | unit #8, #9                                                                           |
| Single-poller (R-14.1.11)                  | unit #14                                                                              |
| Main-thread-only (§4.2 inv #1)             | unit #11                                                                              |
| Zero-alloc hot path (§9.3)                 | unit #13                                                                              |
| p50 / p99 wall-time (§9.4)                 | unit #12; integration #3                                                              |
| Hot-reload survival (§8.1, §8.2 SPEC)      | integration #2; e2e reload-mid-replay variant                                         |
| Drop-file routing (§3.6, R-14.2.4)         | integration #5; S1 trace step 7                                                       |
| Quit synthesis (§3.5)                      | integration #6 (no E2E; gap tracked §12 [NON-BLOCKING])                               |
| Phase-1 quiescence at phase 8 (§8.2 SPEC)  | integration #2 covers it incidentally; explicit assertion in `tests/core/integration/hot_reload/` |

## 12. Open questions

- [OPEN] **Multi-window queue routing.** Today
  `EventQueue<WindowEvent>` carries events for every window,
  discriminated by `WindowId`. If the engine ever opens > 4
  windows simultaneously (editor + game preview + perf overlay +
  asset inspector + …), per-window queues may be more cache-
  friendly than one merged queue. Trigger to reopen: a measured
  cache-miss spike on the consumer drain loop with > 4 windows
  active. Owner: window-display-design (sibling spike #715).
- [OPEN] **`WindowEvent::TextDropped` variant.** R-14.2.4 plain-
  text drop is currently dropped at `info` level. Promotion to a
  first-class `WindowEvent` arm is gated by a second consumer
  beyond the editor's own paste handler. Owner: tools-plugin spike
  (deferred, post-MVP).
- [OPEN] **Sensor / touch / pen event variants.** R-6.1.7,
  R-6.1.9, R-6.1.10. Reopening the sealed `InputEvent` sum is a
  central edit; gated by a target-device trigger (motion-controller,
  touchscreen laptop, stylus tablet). Owner: input-plugin spike
  (post-MVP).
- [OPEN] **Custom-key-repeat behaviour.** SDL3 honours the OS key-
  repeat policy and emits `KeyDown { repeat = true }` accordingly.
  Some applications (game-feel-sensitive) want explicit control
  over repeat rate. Trigger: the input-mapping plugin spike. Owner:
  input-plugin spike (post-MVP).
- [OPEN] **High-frequency mouse / gamepad (>1 kHz polling).**
  SDL3's event delivery is bounded by the OS's USB polling
  interval. A future high-precision mouse path may need
  `SDL_GetMouseState` polled at sub-frame granularity; that path
  bypasses the event queue and is therefore out of scope for this
  aggregate, but the boundary should be explicit. Trigger:
  competitive-gaming user-story landing in scope. Owner:
  input-plugin spike (post-MVP).
- [OPEN] **Off-main-thread drain demotion strategy.** §6.4 / §10.4
  return `Unsupported` on shipping builds; whether to instead crash
  hard (matching debug behaviour) is a question that should land
  with the first real bug report. Trigger: a plugin author files a
  ticket about silent failure. Owner: error-model amendment spike.
- [NON-BLOCKING] **`ingest_drop` two-concrete-users gate.** Currently
  has one caller: the event-pump §3.6 `SDL_EVENT_DROP_FILE` branch.
  Promotion to a stable cross-aggregate internal contract is gated on
  a second caller materialising. Until then, `ingest_drop` is
  event-pump's private extension to file-watcher and may be removed if
  the second caller never appears. Per PHILOSOPHY §Anti-patterns.
- [NON-BLOCKING] **Quit-synthesis E2E coverage gap.** `SDL_EVENT_QUIT`
  → `WindowEvent::CloseRequested` (§3.5) is not exercised by any E2E
  trace at MVP. The S1 trace exits via window close, not Cmd-Q.
  Integration test #6 (`event_pump_quit_synthesises_close_requested`)
  is the acceptance gate; no E2E trace covers menu-driven quit.
  Trigger to close: the user-story for Cmd-Q app quit lands in scope.
- [BLOCKING IMPLEMENTATION] **SPEC §10.3.2 `EventQueue::with_capacity`
  trigger column amendment.** The current SPEC §10.3.2 entry describes
  the `Unsupported` trigger as "zero / wildly oversized capacity". This
  design (§10.1 + §3.4) establishes silent round-up to the next
  power-of-two — a non-power-of-two capacity is not an error, and
  `Unsupported` is only emitted when the requested capacity overflows
  `u32::max` (the sub-arena ceiling). SPEC §10.3.2 must be amended to
  replace the trigger description with "capacity overflow beyond
  `u32::max`" before the first plan PR consuming this design lands.
  Two concrete consumers: shipping runtime input pump, editor input pump.
  Owner: platform-SPEC amendment — must land before plan PRs open.
  STATUS: The amendment is a one-line edit to replace "zero / wildly
  oversized capacity" with "capacity overflow beyond `u32::max`" in the
  SPEC §10.3.2 trigger column. The orchestrator (or first plan PR
  author) must either (a) file
  `[SPIKE] amend-platform-spec-event-pump-with-capacity-trigger`
  parented to #714 and backfill the issue number into this entry, OR
  (b) land the one-line SPEC edit directly in the first plan PR
  consuming this design. Plan PRs cannot land until SPEC §10.3.2 is
  corrected via either route. Issue: #TBD (to be filed at plan-PR
  authoring time).
