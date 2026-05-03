# Platform Spec

## 1. Purpose

The `platform` bounded context owns the **single OS-facing seam** of the
engine. Its sole reason to change is "the operating system's surface
shifted" — a new SDL3 release, a Metal SDK revision, an APFS event
behavior change, a kqueue quirk. Every other context (render, ecs,
codegen, plugins, editor, asset) consumes platform through a narrow
typed C++ API and never links libdispatch, AppKit, Cocoa, X11, Win32,
SDL, metal-cpp, or POSIX directly. Concretely, platform owns: the
**window** and **display** lifecycle (create / resize / destroy /
fullscreen / DPI) on top of SDL3; the **input** event pump (keyboard,
mouse, gamepad, text input) drained from SDL3 once per frame into a
typed engine queue; the **surface bridge** that turns an `SDL_Window`
into a `CAMetalLayer` exposed as an opaque `metal_cpp::MTL::Layer*`
handle (the bridge file is the only translation unit allowed to touch
Objective-C runtime; engine code stays pure C++23/26); the **file
watcher** abstraction backed by SDL3's filesystem events on macOS and
kqueue / inotify / ReadDirectoryChangesW elsewhere, emitting
deduplicated `(canonical_path, kind)` events; the **clock** primitive
(monotonic + wall, both expressible as fixed-tick game time); the
**process** primitive (argv, working directory, environment, exit
code, signal install / uninstall); and the **file IO** primitive
(blocking + bounded async open / read / write / stat / list / delete
against canonical absolute paths). Platform **refuses** to own GPU
rendering logic (no command encoding, no shader compilation, no
swapchain scheduling — render owns that, consuming the surface
handle), the ECS (no archetype storage, no component access, no system
scheduling — ecs owns that), domain logic of any kind (no input-to-
gameplay mapping, no asset semantics, no scene state), the job system
or fiber scheduler (a sibling context owns task graph + thread pool;
platform exposes only the OS thread / TLS / atomics primitives needed
to build it), platform-service SDK integration (Steam, EOS, Game
Center, GPGS, console SDKs all live in a separate `platform-services`
context behind their own seam), and any logging / telemetry policy
(crash dump *capture* primitives may live here; aggregation, sink
composition, and channel routing belong to a `diagnostics` context).
The collapse rule from `PHILOSOPHY.md` applies: where a harmonius
requirement split a concern across "OS integration", "filesystem",
"window/display", and "threading", glibre fuses them into one OS-
boundary primitive whose only justification for existence is "C++
cannot reach the OS without it".

## 2. Ubiquitous Language

Terms used unchanged in code.

| Term            | Meaning                                                                                                                                                       |
|-----------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Window          | Engine handle wrapping a single `SDL_Window`. Owns its lifecycle, dimensions, DPI scale, and surface binding. One process may own many.                       |
| Display         | A connected output device (monitor). Carries logical id, physical bounds, refresh rate, DPI, HDR capability. Re-enumerated on hot-plug.                       |
| Surface         | Opaque GPU-presentable target obtained from a Window. On macOS this is a `CAMetalLayer*` reached via `SDL_Metal_CreateView`, exposed as `metal_cpp::MTL::Layer*`. |
| LogicalSize     | DPI-independent extent in points. Public window APIs accept and return this type.                                                                              |
| PhysicalSize    | DPI-scaled extent in pixels. Used at the GPU boundary; converted from `LogicalSize` via the Window's current DPI scale.                                       |
| InputEvent      | Typed sum: KeyDown / KeyUp / MouseMove / MouseButton / Wheel / TextInput / GamepadAxis / GamepadButton. Drained FIFO from the SDL3 pump once per frame.        |
| WindowEvent     | Typed sum: Resized / DpiChanged / Minimized / Restored / FocusGained / FocusLost / CloseRequested / DisplayChanged. FIFO, exactly-once, never dropped.        |
| EventQueue      | Bounded SPSC ring buffer the platform pump writes and the engine drains. One queue per event family. Full queue is a fatal pump misconfiguration.             |
| FileWatcher     | Recursive directory subscription. Emits `FileEvent { canonical_path, kind }` after path canonicalization and content-hash deduplication.                      |
| FileEvent       | Sum: Created / Modified / Deleted / Renamed. Always carries a canonical absolute path.                                                                        |
| CanonicalPath   | UTF-8 absolute path with symlinks / junctions / case resolved per platform rules. The only key shape accepted by file IO and the watcher.                     |
| Clock           | Monotonic + wall time source. Returns `Instant` and `WallTime`; never wraps; never goes backward; immune to NTP slew on the monotonic side.                   |
| Instant         | Opaque monotonic timestamp. Subtracts to `Duration`. Not serializable.                                                                                        |
| Process         | View over the running process: argv, env, cwd, executable path, PID, exit-code setter, signal install / uninstall.                                            |
| FileIo          | Async-capable open / read / write / stat / list / delete primitive over `CanonicalPath`. Bounded I/O thread budget; never blocks a worker.                    |
| Pump            | The platform's once-per-frame call that drains OS events into the typed `EventQueue`s. Single-threaded; called from the main thread only.                     |
| DpiScale        | `float` ratio mapping `LogicalSize` to `PhysicalSize`. Updated atomically on `DpiChanged`. Always > 0.                                                        |
| PlatformError   | Closed sum of typed failures (NotFound, PermissionDenied, AlreadyExists, Interrupted, Unsupported, IoFailure, OsCode). No exceptions cross the boundary.      |

## 3. Derived From

Harmonius is unreliable prior art — every conclusion below was
independently re-derived per `PHILOSOPHY.md`. The following harmonius
files were consulted as research input only.

### Citations (research input)

Requirements:

- `docs/requirements/platform/window-display.md` — R-14.1.1 …
  R-14.1.12 (window lifecycle, fullscreen, multi-monitor, DPI,
  presentation, HDR, raw handle, event delivery, logical/physical
  size types).
- `docs/requirements/platform/os-integration.md` — R-14.2.1 …
  R-14.2.9 (clipboard, file dialogs, notifications, drag/drop, IME,
  layout query, structured errors, console fallback).
- `docs/requirements/platform/threading-async.md` — R-14.3.1 …
  R-14.3.17 (thread pool, fibers, async runtime, scoped tasks,
  game-loop graph, GCD bridging on macOS).
- `docs/requirements/platform/crash-reporting.md` — R-14.4.1 …
  R-14.4.11 (crash dumps, symbol upload, structured logs, perf
  counters, GPU breadcrumbs, OOP capture, platform-native log /
  profiler sinks).
- `docs/requirements/platform/platform-services.md` — R-14.5.1 …
  R-14.5.12 (achievements, leaderboards, rich presence, voice, cloud
  saves, entitlements, preferences, asset cache, PSO cache, temp
  files, console certification).
- `docs/requirements/platform/filesystem.md` — R-14.6.1 … R-14.6.11
  (async I/O, lifecycle ops, stat, list, file watching, BLAKE3
  dedup, canonical paths, hash cache, throughput target).
- `docs/requirements/platform/sdk-integration.md` — R-14.7.* (Steam,
  Apple, console SDK shapes; cited only to confirm scope refusals
  below).

Designs:

- `docs/design/platform/windowing.md` (+ `windowing-test-cases.md`)
  — main-thread sole-owner of OS APIs; bounded-channel event
  delivery; surface handle abstraction.
- `docs/design/platform/threading.md` (+ `threading-test-cases.md`)
  — worker pool topology; main-thread I/O polling; worker I/O
  request/handle pattern.
- `docs/design/platform/crash-reporting.md` (+
  `crash-reporting-test-cases.md`) — out-of-process monitor stub;
  signal-safe fault path; minidump format.
- `docs/design/platform/console-integration.md` (+
  `console-integration-test-cases.md`) — abstract trait + no-op
  stub + private-fork pattern for proprietary SDKs.
- `docs/design/platform/platform-services.md` (+
  `platform-services-test-cases.md`) — vendor-agnostic services
  facade over Steamworks / Game Center / GDK / PSN.
- `docs/design/platform/telemetry.md` (+
  `telemetry-test-cases.md`) — opt-in scope, offline buffer,
  batched HTTP/3 upload.

### Occam collapses (multiple harmonius concepts → one glibre primitive)

- **Windowing back-ends → SDL3-only.** Harmonius split per-OS code
  across Win32 (`windows-rs`), `objc2-app-kit` on macOS, and
  `x11rb` / `wayland-client` / `wp_fractional_scale_v1` on Linux,
  with a `cfg`-gated module per platform (`windowing.md` § Module
  Layout). Glibre collapses all of that into a single SDL3 backend.
  SDL3 already encapsulates fullscreen mode transitions, multi-
  monitor enumeration, hot-plug, DPI events, IME, drag-drop, and
  clipboard across Windows / macOS / Linux. The only Objective-C
  translation unit we keep is the `SDL_Metal_CreateView` →
  `CAMetalLayer*` bridge file, exposed as
  `metal_cpp::MTL::Layer*`. Engine code stays pure C++23/26.
- **`raw-window-handle` trait → opaque `Surface` value.** The
  multi-platform-handle abstraction (R-14.1.8) collapses to a
  single typed `Surface` returned by `Window::surface()`. macOS is
  the only target this MVP supports; render consumes the layer
  pointer directly.
- **Window-event channel + input-event channel + surface-event
  channel → typed `EventQueue<T>` per family.** Harmonius
  `windowing.md` defined three separate bounded channels with
  different types. Glibre keeps the family separation but unifies
  them under one bounded SPSC ring primitive (`EventQueue`)
  templated on the event sum type, drained once per frame by the
  `Pump`.
- **Filesystem watcher ports (kqueue / inotify /
  ReadDirectoryChangesW / FSEvents) + BLAKE3 hash cache +
  canonicalization → `FileWatcher` emitting deduplicated
  `(canonical_path, kind)`.** Harmonius split path canonicalization
  (R-14.6.7), watch delivery (R-14.6.5, R-14.6.8), and content-hash
  dedup (R-14.6.6, R-14.6.9) into separate primitives. Glibre folds
  them into one watcher whose only public output is already
  deduped, hashed, canonicalized events. Backend choice (SDL3
  filesystem events on macOS; native APIs on other targets) is an
  implementation detail behind that seam.
- **`LogicalSize` + `PhysicalSize` + `Point` + `Rect` (R-14.1.12)
  → `LogicalSize` / `PhysicalSize` only.** Glibre keeps the two
  load-bearing types and refuses the geometry pair. `Point` and
  `Rect` belong in the `geometry` context, not at the OS seam.
- **Tokio `current_thread` runtime + GCD bridge + IOCP / io_uring
  job-system bridge (R-14.3.5 / .6 / .12) → not in this context.**
  Harmonius placed the I/O completion bridge in
  `harmonius_platform::threading`. Glibre routes that to a sibling
  job-system context; platform exposes only the underlying OS
  primitives (`Process`, `Clock`, blocking + bounded async
  `FileIo`, threads / TLS / atomics).
- **`async fn` everywhere (R-14.2.7, R-14.6.1, R-14.6.8) →
  bounded async via small typed handles.** Glibre's C++23/26 stack
  has no Rust-style `Future` / `.await`. The platform exposes
  blocking primitives plus a small bounded I/O thread pool used
  internally; public API returns `std::expected<T,
  PlatformError>` synchronously or `IoToken` for the bounded
  async path. No public callbacks, no cross-boundary coroutines.
- **OS-toast / system-tray / drag-drop / IME / clipboard
  (R-14.2.x) → input + window event sums, no first-class APIs.**
  IME composition + commit, drag-drop, layout change, and DPI
  change all surface as `InputEvent` / `WindowEvent` variants from
  the SDL3 pump. Clipboard read/write is a thin synchronous
  helper; tray icons and OS toasts are deferred (refusal — see
  below).
- **Multi-OS log sinks (`OutputDebugString` / `os_log` /
  `sd_journal_sendv`) → crash-dump *capture* primitive only.**
  Harmonius bound these into `harmonius_platform::diagnostics`
  (R-14.4.10). Glibre exposes only the macOS crash-dump capture
  hook here (signal-safe minidump write); aggregation, filtering,
  channel routing, sink composition (R-14.4.4, .9, .10, .11) all
  belong to a future `diagnostics` context, not platform.

### Refusals (routed to peer contexts, not platform)

- **GPU rendering, command encoding, swapchain scheduling, shader
  compilation, HDR color-space conversion, presentation modes
  (R-14.1.5, R-14.1.6).** → `render`. Platform owns only the
  surface handle.
- **ECS / system scheduling / archetype storage.** → `core` (the
  archetype ECS) and `data` (Fory schemas).
- **Job system, fiber scheduler, task graph, scoped tasks,
  GameLoopGraph, async I/O completion routing
  (R-14.3.3 … R-14.3.17).** → sibling job-system context. Platform
  exposes only OS thread / TLS / atomics + the `Process` and
  `Clock` primitives needed to build it.
- **Console certification (R-14.5.7, R-14.5.12), Steamworks /
  StoreKit / Game Center / GDK / PSN integration
  (R-14.5.1 … R-14.5.6, R-14.7.*).** → a separate
  `platform-services` context behind its own seam, with abstract
  trait + private-fork pattern as harmonius
  `console-integration.md` already sketched. The MVP does not
  open this context.
- **Logging, structured records, channel filtering, telemetry
  upload, GDPR export / delete, perf counters, GPU breadcrumbs
  (R-14.4.4 … R-14.4.6, R-14.4.9 … R-14.4.11, R-14.5.1 …
  R-14.5.6 in `telemetry.md`).** → `diagnostics` context (not yet
  opened). Platform owns crash-dump *capture* primitives only.
- **Asset bundle cache, PSO cache, temp directory manager, mod
  download cache (R-14.5.9 … R-14.5.12).** → `content` and
  `render` (PSO) and a future build-cache layer. Platform owns
  only the `FileIo` primitive they sit on top of.
- **Player preferences (TOML, atomic write, conflict dialog —
  R-14.5.8).** → `tools` / editor or a dedicated user-prefs
  context. Platform owns only the canonical-path + atomic file IO
  primitives.
- **File dialogs, drag-drop validation, notifications, system
  tray (R-14.2.2, R-14.2.3, R-14.2.4).** → `tools` (editor UI).
  Platform exposes the underlying SDL3 events; user-facing dialog
  policy lives in the editor.
- **Symbol upload, server-side symbolication, crash clustering,
  out-of-process monitor binary lifecycle (R-14.4.2, R-14.4.3,
  R-14.4.7).** → `diagnostics` and the build / CI pipeline. The
  in-process signal-safe stub is the only platform surface.
- **Anti-cheat (R-14.7.6 VAC integration).** → out of MVP scope
  entirely.

## 4. Aggregates & Invariants

- Aggregate / entity / value object.
- Invariants that must hold at every public API boundary.

## 5. Public Interface

```cpp
// header-only stub goes here
```

Event types, serialized schemas (Fory), error types.

## 6. Internal Architecture

Non-binding sketch for implementers.

## 7. Persistence & Schemas

Fory schemas. Migration rules.

## 8. Hot-Reload Contract

What survives swap, what `migrate(...)` must do, what triggers refusal.

## 9. Performance Budget

Cycles / frame, memory ceiling, allocation rules.

## 10. Failure Modes & Error Model

Typed errors. Recovery.

## 11. Acceptance Criteria

GitHub `type:user-story` issues this spec closes:

- #TBD — `<title>`

Each must have a Catch2 test by name.

## 12. Open Questions

- Owner / resolution gate.
