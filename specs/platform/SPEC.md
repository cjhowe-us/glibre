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

Each aggregate below is the smallest cohesive cluster of entities and
value objects whose state mutates together at the OS seam. The
**SRP justification** for every aggregate is "one OS facet — one
reason to change"; if two facets ever shared a reason to change we
would fuse them, but the OS already keeps them separate
(window-server vs filesystem vs scheduler vs process), so the seam
inherits that separation.

The aggregate boundary is what the engine enforces; OS handles
themselves are opaque implementation details and never escape
through public headers.

### 4.1 `Window` (aggregate root) — `Display` (entity) — `Surface` (entity) — `LogicalSize` / `PhysicalSize` / `DpiScale` (value objects)

A `Window` is the aggregate root for one OS window-server entry
plus everything attached to it that cannot outlive it: its
`Surface` (a single `metal_cpp::MTL::Layer*` reached through the
SDL3 → `CAMetalLayer` bridge) and the `Display` it currently
inhabits. `Display` re-enumeration is owned here because hot-plug
events are delivered to the same SDL3 pump that drives windows,
and the window's current `Display`, refresh rate, DPI, and HDR
capability are read together as one consistent snapshot.
`LogicalSize`, `PhysicalSize`, and `DpiScale` are immutable value
objects returned by query and updated by `WindowEvent::Resized` /
`DpiChanged`.

**SRP:** one reason to change — the OS window-server contract
shifted (SDL3 minor bump, AppKit fullscreen behavior, DPI rounding
rule). Splitting `Window` from `Surface` would lie about lifetime;
splitting `Display` into its own aggregate would force every
window-state read to cross two aggregate boundaries for what is
one consistent OS query.

Invariants enforced at every public boundary of this aggregate:

1. **Surface lifetime is strictly bound to its `Window`.** A
   `Surface` handle handed to render is valid only while the owning
   `Window` is alive; `Window::~Window()` releases the
   `CAMetalLayer*` and any `Surface` value previously vended is
   thereafter UB to dereference. The engine enforces this by
   issuing `Surface` only through `Window::surface()` and never
   through a free function.
2. **`Window` operations are main-thread-only.** Any call from a
   non-main thread is a contract violation; debug builds assert,
   release builds return `PlatformError::Unsupported`. SDL3 + AppKit
   require this and we do not paper over it.
3. **`LogicalSize` >= 1x1; `DpiScale` > 0.** Constructed values
   outside this range are rejected at the API boundary with
   `PlatformError::Unsupported` (the aggregate refuses to enter an
   invalid state rather than carry a `Validity` flag).
4. **`PhysicalSize = round(LogicalSize * DpiScale)` always.**
   Conversion lives in one place; callers cannot construct a
   `PhysicalSize` directly from a `LogicalSize` without going
   through the owning `Window`'s current `DpiScale`.
5. **`Display` snapshots are immutable.** A `Display` value is the
   state at the moment it was queried; subsequent hot-plug arrives
   as `WindowEvent::DisplayChanged` and a new query returns the new
   snapshot. Old values do not silently mutate.
6. **`Surface` is opaque to engine code.** Its only public shape is
   `metal_cpp::MTL::Layer*`; the bridging Objective-C translation
   unit is the sole site that touches AppKit. No header outside
   platform's bridge file may include `<AppKit/AppKit.h>` or any
   Objective-C runtime symbol.

### 4.2 `EventQueue<T>` (aggregate root) — `InputEvent` / `WindowEvent` (sealed sums) — `Pump` (entity)

A pair of typed bounded SPSC ring buffers — `EventQueue<InputEvent>`
and `EventQueue<WindowEvent>` — together with the `Pump` that
writes to them, forms one aggregate: the **OS-event ingress**.
The two queues live or die together because the `Pump` drains
SDL3's single event stream and produces both families in the order
SDL3 reported them; splitting the two queues into independent
aggregates would re-introduce the cross-family ordering bug we are
collapsing away.

`InputEvent` is a closed `eastl::variant` over: `KeyDown`, `KeyUp`,
`MouseMove`, `MouseButton`, `Wheel`, `TextInput`, `GamepadAxis`,
`GamepadButton`. `WindowEvent` is a closed `eastl::variant` over:
`Resized`, `DpiChanged`, `Minimized`, `Restored`, `FocusGained`,
`FocusLost`, `CloseRequested`, `DisplayChanged`. Both sums are
sealed at compile time; adding a variant is a deliberate central
edit, not an open extension point.

**SRP:** one reason to change — SDL3's event vocabulary or the
ring-buffer protocol shifted. Splitting per-event-family would
fragment the ordering invariant across aggregates.

Invariants:

1. **Pump is single-threaded, main-thread-only.** Exactly one
   `Pump::drain()` call per frame, from the main thread. SDL3 event
   pumping is not thread-safe and we do not pretend otherwise.
2. **Per-device monotonic event ordering.** For events emitted by
   the same physical device (e.g. one keyboard, one gamepad index),
   the order in the queue matches the order SDL3 reported them.
   Cross-device interleave is permitted but never reordered within
   a single device.
3. **Exactly-once delivery; no silent drops.** Every event accepted
   from SDL3 is enqueued exactly once. A queue that is full at pump
   time is a fatal pump misconfiguration: the platform returns
   `PlatformError::IoFailure { OsCode }` and the engine treats this
   as unrecoverable. We do not coalesce, drop, or reorder to make
   room.
4. **`CloseRequested` and `DpiChanged` are never coalesced.** Even
   if multiple arrive in one pump cycle, each is delivered as its
   own queue entry with its frame-local sequence preserved.
5. **Sealed variant: no unknown event escapes.** Any SDL3 event
   that does not map to a known variant is dropped at the pump
   boundary, never queued as "unknown". This refusal is logged at
   `debug` level and does not surface as an error.
6. **Queues are bounded; capacity is fixed at construction.** The
   `EventQueue<T>` does not grow at runtime; it is sized for the
   worst-case frame burst once and never resized.

### 4.3 `FileWatcher` (aggregate root) — `FileEvent` (sealed sum) — `CanonicalPath` (value object)

A `FileWatcher` owns a recursive directory subscription and the
deduplication state needed to emit only `(canonical_path, kind)`
tuples to the engine. The aggregate fuses three concerns harmonius
kept apart — path canonicalization, OS watch delivery, and
content-hash dedup — because they only have value when delivered
together; consumers only care about the deduped stream.

`FileEvent` is a closed `eastl::variant` over: `Created`, `Modified`,
`Deleted`, `Renamed { from, to }`. Every payload carries a
`CanonicalPath` value object (UTF-8 absolute path with symlinks,
junctions, and case folded per platform rule).

**SRP:** one reason to change — the OS file-watch backend shifted
(SDL3 filesystem events on macOS today; kqueue / inotify /
ReadDirectoryChangesW elsewhere later) or the canonicalization
rule changed. Splitting "watcher" from "canonicalizer" would force
every consumer to re-canonicalize and re-dedup, defeating the
collapse.

Invariants:

1. **Every emitted path is canonical.** No raw OS path, no
   relative path, no symlink-bearing path is ever emitted. The
   aggregate refuses to construct a `FileEvent` with a
   non-canonical path; canonicalization failure becomes
   `PlatformError::IoFailure` at the watch ingestion site, not a
   leaky event.
2. **Deduplication is by canonical path + content hash.** Two OS
   events that resolve to the same `(canonical_path, content_hash)`
   within the watcher's debounce window collapse to one
   `FileEvent`. Hash computation is internal; the algorithm is
   pluggable but always produces a stable digest.
3. **Renames are atomic.** A rename surfaces as one
   `FileEvent::Renamed { from, to }`, not as a `Deleted` plus a
   `Created`. If the OS reports the latter pair, the aggregate
   reassembles the rename from inode / file-id metadata.
4. **Watcher lifetime owns its subscriptions.** `~FileWatcher`
   tears down every native subscription it holds; no orphaned
   kqueue fd / inotify watch / FSEvents stream survives.
5. **Watcher does not block the main thread.** Watch delivery
   happens on an internal I/O thread and is funneled to the engine
   through an `EventQueue<FileEvent>` drained by `Pump::drain()`.
6. **`CanonicalPath` is a value object.** Two `CanonicalPath`s
   with byte-equal contents are equal. Construction validates UTF-8
   and absoluteness; failure returns `PlatformError::Unsupported`
   rather than a half-formed value.

### 4.4 `Clock` (aggregate root) — `Instant` / `WallTime` / `Duration` (value objects)

`Clock` is the aggregate root for time, owning both a monotonic
source and a wall source. They live together because the engine
periodically needs to correlate the two (e.g. wall-time stamp on a
crash dump emitted from a monotonic frame budget) and the
correlation rule must be one place.

`Instant` is opaque, monotonic, never-wrapping, never-decreasing,
immune to NTP slew. `WallTime` is the calendar source; it may slew.
`Duration = Instant - Instant` and is the only signed time
arithmetic allowed at the seam.

**SRP:** one reason to change — the OS time source's resolution,
backing API, or guarantees shifted. Splitting monotonic from wall
into separate aggregates would re-create the correlation gap.

Invariants:

1. **Monotonic non-decreasing.** For any two `Instant` values
   `a, b` produced by the same `Clock` with `a` returned before
   `b`, `b - a >= Duration::zero()`. The platform aborts on a
   detected regression rather than silently clamping.
2. **`Instant` is not serializable.** It carries no calendar
   meaning; cross-process or cross-run comparisons are forbidden
   and the type provides no I/O surface for them.
3. **`WallTime` is the only source for log timestamps.** Engine
   code never reads `time(nullptr)` or `gettimeofday` directly.
4. **Resolution is at least 1 ms; native resolution is exposed.**
   The `Clock` advertises its native tick so the deterministic
   simulation step can be sized to it without integer drift.
5. **One `Clock` per process.** It is a static / Meyer's
   singleton at the seam; aggregates that need time take a
   `Clock&` parameter rather than constructing their own.

### 4.5 `Process` (aggregate root) — `Argv` / `Env` / `ExitCode` / `SignalHandler` (value objects + entity)

`Process` is the aggregate root for the running process's
identity-and-control surface: argv, environment, working directory,
executable path, PID, exit-code setter, and signal install /
uninstall. These belong in one aggregate because they share the
same lifetime (the process) and changing one (e.g. installing a
fatal-signal handler) interacts with another (e.g. the exit-code
path used by a crash dump).

**SRP:** one reason to change — the OS process-control contract
shifted (a new POSIX revision, a sandboxing rule, a notarization
constraint that forbids SIGTERM trapping). Splitting argv from
signals would separate fields that crash-handling code reads
together.

Invariants:

1. **Argv / env / cwd are read-only snapshots.** They are captured
   once at startup and exposed as `eastl::span<const eastl::string_view>`
   / equivalent. Mutation through the aggregate is forbidden;
   out-of-band `setenv` use is unsupported and undefined for
   engine code.
2. **Exit code is set, never read back.** `Process::set_exit_code`
   stores the value to be reported when the engine returns from
   `main`. The aggregate exposes no getter; debugging exit codes
   reads them from logs.
3. **At most one handler per signal.** Installing a handler for a
   signal that already has one engine-side returns
   `PlatformError::AlreadyExists`. The aggregate does not chain.
4. **Signal handlers are async-signal-safe.** Engine code that
   passes a callable to `Process::install_signal` accepts the
   constraint and the aggregate documents it; calling unsafe
   functions from the handler is the caller's contract violation.
5. **Process aggregate is single-instance.** There is one
   `Process` per running engine; second-instance construction is a
   programming error and the aggregate refuses to build.

### 4.6 `FileIo` (aggregate root) — `IoToken` (entity) — `OpenMode` / `Stat` / `DirEntry` (value objects)

`FileIo` is the aggregate root for blocking and bounded-async file
primitives: open, read, write, stat, list, delete — all keyed by
`CanonicalPath`. The bounded-async surface returns an `IoToken`
entity that the caller polls or awaits through a `complete(token)`
API; the token, the in-flight buffer, and the completion event
form a single aggregate so that lifetime and cancellation cannot
get out of sync.

**SRP:** one reason to change — the OS file-I/O contract shifted
(io_uring on Linux; APFS atomic-rename rule on macOS; long-path
support on Windows) or the bounded-async budget shape changed.
Splitting blocking from async would duplicate the canonical-path
discipline.

Invariants:

1. **Every path is `CanonicalPath`.** No raw `const char*` /
   `std::filesystem::path` ever reaches the public surface; the
   type system prevents non-canonical paths from being passed in.
2. **`FileIo` never blocks the main thread.** The bounded-async
   path runs on an internal I/O thread pool sized at construction;
   the blocking surface is documented as off-main-thread only and
   asserts in debug builds when called from the main thread. This
   is the load-bearing rule that keeps frame timing intact.
3. **`IoToken` is a single-consumer handle.** Each token is owned
   by exactly one caller; copying is forbidden, moving transfers
   ownership, dropping cancels the operation. Cancellation is
   best-effort — the OS may have already completed.
4. **Atomic write protocol.** `write_atomic(path, bytes)` writes
   to a sibling temp file, fsyncs, and renames over the target.
   The aggregate is the sole site that knows this protocol;
   callers that want atomicity ask for it by name.
5. **`Stat` and `DirEntry` are value objects.** They are
   point-in-time snapshots; staleness is the caller's problem.
   The aggregate does not auto-refresh.
6. **Bounded-async budget is enforced.** The internal I/O pool
   is sized once; saturating it returns
   `PlatformError::IoFailure { code: OutOfBudget }` rather than
   queuing unboundedly.
7. **No public callbacks.** Completion is observed via
   `IoToken::poll()` or `IoToken::wait_for(Duration)`; no callable
   crosses the boundary, in either direction.

### 4.7 `PlatformError` (closed sum, cross-aggregate invariant)

`PlatformError` is the closed sum of typed failures every
aggregate above may surface at its public boundary, returned via
`std::expected<T, PlatformError>` per the engine-wide error model
(`reviews/decisions/error-model.md`). Variants:

- `NotFound` — path / window / display id not present.
- `PermissionDenied` — OS denied the operation.
- `AlreadyExists` — create-only operation collided.
- `Interrupted` — OS-level interrupt (EINTR class).
- `Unsupported` — operation valid in shape but not on this OS /
  this hardware (e.g. fullscreen-mode transition unavailable,
  non-canonical path supplied).
- `IoFailure { OsCode }` — wraps an opaque OS error code where no
  finer mapping exists.
- `OsCode` — raw fallback when the OS reports a code with no
  semantic mapping yet.

Cross-aggregate invariants:

1. **Closed sum.** Adding a variant is a deliberate central edit
   to platform's error enum; aggregates do not invent their own
   error types. The platform context's enum sits inside the
   engine-wide `glibre::Error` variant per the error-model
   decision record.
2. **No exceptions cross the boundary.** Every public function
   returns `std::expected<T, PlatformError>` (or `void` on
   guaranteed-success paths). The Objective-C bridge file is the
   only place that may catch an `NSException`-equivalent, and it
   converts before returning.
3. **Errors are constructed at the site they happen.** No
   aggregate translates another aggregate's error into its own
   automatically; the call site that crosses the boundary maps
   explicitly, per the error-model composition rule.

## 5. Public Interface

The full surface is a single header `glibre/platform/platform.hpp`
(split across files at implementation time; the spec presents it as one
translation unit so reviewers can see the whole seam). Every public
function is `noexcept`; every fallible function returns
`glibre::Result<T>` (= `std::expected<T, glibre::Error>`) per
`reviews/decisions/error-model.md`. The platform context contributes
one new arm to the engine-wide `glibre::Error` variant: the closed sum
`platform::Error` (§4.7).

The header is verified compileable with
`clang++ -std=c++23 -fsyntax-only`.

```cpp
// specs/platform — public interface (header-only stub)
//
// One header per family in the real tree; presented here as a single
// unit. Engine code includes only this seam; libdispatch / AppKit /
// SDL3 / metal-cpp / POSIX never escape into a sibling context.
#pragma once

#include <EASTL/array.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <EASTL/optional.h>
#include <EASTL/span.h>
#include <EASTL/string_view.h>
#include <utility>
#include <EASTL/variant.h>

// Engine-wide error type, defined in core/include/glibre/error.hpp.
// Forward-declared here so this header is self-contained for syntax
// checking; the real header pulls in <glibre/error.hpp>.
namespace glibre {
struct ErrorContext;
class  Error;
template <class T> using Result = std::expected<T, Error>;
}  // namespace glibre

namespace glibre::platform {

// ---------------------------------------------------------------------------
// 5.1  Closed sum of typed failures (§4.7)
// ---------------------------------------------------------------------------
//
// Variant-of-tags: `Error` is an `eastl::variant` so the
// `IoFailure { OsCode }` payload survives without losing the closed-sum
// shape. Each non-payload arm is a zero-sized tag struct; the engine-
// wide `glibre::Error` rolls this whole sum into one of its arms.

struct OsCode {
    std::int32_t value{0};  // platform-native errno / NSError code / HRESULT.
    constexpr bool operator==(const OsCode&) const noexcept = default;
};

struct NotFound          { constexpr bool operator==(const NotFound&)         const noexcept = default; };
struct PermissionDenied  { constexpr bool operator==(const PermissionDenied&) const noexcept = default; };
struct AlreadyExists     { constexpr bool operator==(const AlreadyExists&)    const noexcept = default; };
struct Interrupted       { constexpr bool operator==(const Interrupted&)      const noexcept = default; };
struct Unsupported       { constexpr bool operator==(const Unsupported&)      const noexcept = default; };
struct IoFailure         { OsCode code{}; constexpr bool operator==(const IoFailure&) const noexcept = default; };

using Error = eastl::variant<
    NotFound,
    PermissionDenied,
    AlreadyExists,
    Interrupted,
    Unsupported,
    IoFailure,
    OsCode>;

// Convenience: every public fallible function returns Result<T>.
template <class T>
using Result = ::glibre::Result<T>;

// ---------------------------------------------------------------------------
// 5.2  Value objects: paths, sizes, time
// ---------------------------------------------------------------------------

class CanonicalPath {
public:
    // Construct after canonicalization. Rejects relative / non-UTF-8 /
    // non-absolute inputs. The only path shape accepted by FileIo /
    // FileWatcher (§4.3 inv #1, §4.6 inv #1).
    [[nodiscard]] static auto from_absolute(eastl::string_view utf8_abs) noexcept
        -> Result<CanonicalPath>;

    [[nodiscard]] auto view() const noexcept -> eastl::string_view { return view_; }

    constexpr bool operator==(const CanonicalPath&) const noexcept = default;

private:
    constexpr explicit CanonicalPath(eastl::string_view v) noexcept : view_{v} {}
    eastl::string_view view_{};  // backed by an internal arena owned by platform.
};

struct LogicalSize  { std::uint32_t width{1}; std::uint32_t height{1}; constexpr bool operator==(const LogicalSize&)  const noexcept = default; };
struct PhysicalSize { std::uint32_t width{1}; std::uint32_t height{1}; constexpr bool operator==(const PhysicalSize&) const noexcept = default; };

struct DpiScale {
    float value{1.0f};  // §4.1 inv #3: always > 0, validated at construction.
    [[nodiscard]] static auto make(float v) noexcept -> Result<DpiScale>;
    constexpr bool operator==(const DpiScale&) const noexcept = default;
};

// PhysicalSize = round(LogicalSize * DpiScale)  — single conversion site
// per §4.1 inv #4.
[[nodiscard]] auto to_physical(LogicalSize, DpiScale) noexcept -> PhysicalSize;

// Monotonic + wall time (§4.4). Instant is opaque, never serialized.
class Instant {
public:
    using rep    = std::int64_t;            // ns since arbitrary epoch.
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

struct WallTime {
    std::chrono::system_clock::time_point point{};  // calendar-bearing; may slew.
    bool operator==(const WallTime&) const noexcept = default;
};

// ---------------------------------------------------------------------------
// 5.3  EventQueue<T>: bounded SPSC ring (§4.2)
// ---------------------------------------------------------------------------
//
// One queue per event family. Sized at construction; never grows.
// Producer (Pump) is the platform; consumer is the engine. Drain on the
// main thread. Full-on-write is a fatal pump misconfiguration and the
// platform reports it via Error::IoFailure (§4.2 inv #3).

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
    [[nodiscard]] auto empty()    const noexcept -> bool { return size() == 0; }

    // Engine-side drain. Returns the number of events written into `out`.
    // Never blocks; never reorders within a single device.
    [[nodiscard]] auto drain(eastl::span<T> out) noexcept -> std::size_t;

private:
    EventQueue() noexcept = default;
    struct Impl;
    Impl* impl_{nullptr};
};

// ---------------------------------------------------------------------------
// 5.4  Input + Window event sums (§4.2)
// ---------------------------------------------------------------------------

enum class KeyCode      : std::uint16_t { Unknown = 0 /* SDL3 keycode mirror, sealed at compile time */ };
enum class ScanCode     : std::uint16_t { Unknown = 0 };
enum class MouseButton  : std::uint8_t  { Left, Right, Middle, X1, X2 };
enum class GamepadAxis  : std::uint8_t  { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger };
enum class GamepadBtn   : std::uint8_t  { A, B, X, Y, Back, Guide, Start, LStick, RStick, LShoulder, RShoulder, DUp, DDown, DLeft, DRight };

struct ModifierMask {
    std::uint16_t bits{0};  // shift, ctrl, alt, gui, num/caps lock.
    constexpr bool operator==(const ModifierMask&) const noexcept = default;
};

namespace input {

struct KeyDown      { KeyCode key; ScanCode scan; ModifierMask mods; bool repeat; };
struct KeyUp        { KeyCode key; ScanCode scan; ModifierMask mods; };
struct MouseMove    { float x; float y; float dx; float dy; };
struct MouseButtonEv{ MouseButton button; bool pressed; float x; float y; std::uint8_t click_count; };
struct Wheel        { float dx; float dy; bool flipped; };
struct TextInput    { eastl::array<char, 32> utf8; std::uint8_t length; };  // §3 collapse: IME commit lives here.
struct GamepadAxisEv{ std::uint8_t device; GamepadAxis axis; float value; };  // value in [-1, 1].
struct GamepadBtnEv { std::uint8_t device; GamepadBtn button; bool pressed; };

}  // namespace input

using InputEvent = eastl::variant<
    input::KeyDown,
    input::KeyUp,
    input::MouseMove,
    input::MouseButtonEv,
    input::Wheel,
    input::TextInput,
    input::GamepadAxisEv,
    input::GamepadBtnEv>;

struct WindowId  { std::uint32_t value{0}; constexpr bool operator==(const WindowId&)  const noexcept = default; };
struct DisplayId { std::uint32_t value{0}; constexpr bool operator==(const DisplayId&) const noexcept = default; };

namespace window_event {

struct Resized         { WindowId window; LogicalSize logical; PhysicalSize physical; };
struct DpiChanged      { WindowId window; DpiScale scale; };
struct Minimized       { WindowId window; };
struct Restored        { WindowId window; };
struct FocusGained     { WindowId window; };
struct FocusLost       { WindowId window; };
struct CloseRequested  { WindowId window; };
struct DisplayChanged  { WindowId window; DisplayId display; };

}  // namespace window_event

using WindowEvent = eastl::variant<
    window_event::Resized,
    window_event::DpiChanged,
    window_event::Minimized,
    window_event::Restored,
    window_event::FocusGained,
    window_event::FocusLost,
    window_event::CloseRequested,
    window_event::DisplayChanged>;

// ---------------------------------------------------------------------------
// 5.5  Surface: opaque metal-cpp layer wrapper (§4.1 inv #1, #6)
// ---------------------------------------------------------------------------
//
// `Surface` carries an opaque pointer to a `CAMetalLayer` typed as
// `void*` at the public surface so this header never depends on
// metal-cpp / Objective-C. The bridging translation unit (the only
// file allowed to touch AppKit) up-casts to `MTL::Layer*` for render
// consumption. The handle is non-owning; lifetime is bound to the
// `Window` that vended it (§4.1 inv #1).

class Surface {
public:
    Surface()                                 = default;
    Surface(const Surface&)                   = delete;
    Surface& operator=(const Surface&)        = delete;
    Surface(Surface&&) noexcept               = default;
    Surface& operator=(Surface&&) noexcept    = default;

    [[nodiscard]] auto raw_layer() const noexcept -> void* { return layer_; }  // CAMetalLayer*
    [[nodiscard]] auto window()    const noexcept -> WindowId { return owner_; }
    [[nodiscard]] auto valid()     const noexcept -> bool     { return layer_ != nullptr; }

private:
    friend class Window;
    explicit Surface(WindowId w, void* layer) noexcept : owner_{w}, layer_{layer} {}
    WindowId owner_{};
    void*    layer_{nullptr};
};

// ---------------------------------------------------------------------------
// 5.6  Window + Display (§4.1)
// ---------------------------------------------------------------------------

struct WindowDesc {
    eastl::string_view title{};
    LogicalSize      size{1280, 720};
    bool             resizable{true};
    bool             fullscreen{false};
};

struct Display {
    DisplayId     id{};
    PhysicalSize  bounds{};
    DpiScale      scale{};
    std::uint32_t refresh_hz{60};
    bool          hdr_capable{false};
    bool operator==(const Display&) const noexcept = default;
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

    // §4.1 inv #1: surface lifetime strictly bound to this Window.
    [[nodiscard]] auto surface() noexcept -> Result<Surface>;

    [[nodiscard]] auto request_resize(LogicalSize) noexcept -> Result<void>;
    [[nodiscard]] auto request_close()              noexcept -> Result<void>;

    [[nodiscard]] auto logical_size()  const noexcept -> LogicalSize;
    [[nodiscard]] auto physical_size() const noexcept -> PhysicalSize;
    [[nodiscard]] auto dpi_scale()     const noexcept -> DpiScale;
    [[nodiscard]] auto display()       const noexcept -> Display;

private:
    Window() noexcept = default;
    struct Impl;
    Impl* impl_{nullptr};
};

// ---------------------------------------------------------------------------
// 5.7  Pump: drain SDL3 events into typed queues (§4.2)
// ---------------------------------------------------------------------------

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

    // Single-threaded, main-thread-only (§4.2 inv #1). Returns the
    // total number of events enqueued across both queues this cycle.
    [[nodiscard]] auto drain() noexcept -> Result<std::size_t>;

private:
    Pump() noexcept = default;
    struct Impl;
    Impl* impl_{nullptr};
};

// ---------------------------------------------------------------------------
// 5.8  FileWatcher (§4.3)
// ---------------------------------------------------------------------------

struct WatchToken { std::uint64_t value{0}; constexpr bool operator==(const WatchToken&) const noexcept = default; };

namespace file_event {

struct Created  { CanonicalPath path; };
struct Modified { CanonicalPath path; };
struct Deleted  { CanonicalPath path; };
struct Renamed  { CanonicalPath from; CanonicalPath to; };

}  // namespace file_event

using FileEvent = eastl::variant<
    file_event::Created,
    file_event::Modified,
    file_event::Deleted,
    file_event::Renamed>;

class FileWatcher {
public:
    [[nodiscard]] static auto create() noexcept -> Result<FileWatcher>;

    FileWatcher(FileWatcher&&) noexcept;
    FileWatcher& operator=(FileWatcher&&) noexcept;
    FileWatcher(const FileWatcher&)            = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;
    ~FileWatcher();

    // Subscribe recursively. Returns a token used to drain events / cancel.
    [[nodiscard]] auto watch(CanonicalPath root) noexcept -> Result<WatchToken>;
    [[nodiscard]] auto unwatch(WatchToken)        noexcept -> Result<void>;

    // Drain events that arrived for this token since the last call.
    // Internal I/O thread feeds the buffer (§4.3 inv #5); take_events
    // never blocks the main thread.
    [[nodiscard]] auto take_events(WatchToken, eastl::span<FileEvent> out) noexcept
        -> Result<std::size_t>;

private:
    FileWatcher() noexcept = default;
    struct Impl;
    Impl* impl_{nullptr};
};

// ---------------------------------------------------------------------------
// 5.9  Clock (§4.4)
// ---------------------------------------------------------------------------

class Clock {
public:
    // §4.4 inv #5: one Clock per process; aggregates take it by reference.
    [[nodiscard]] static auto get() noexcept -> Clock&;

    [[nodiscard]] auto now()         const noexcept -> Instant;
    [[nodiscard]] auto wall()        const noexcept -> WallTime;
    [[nodiscard]] auto native_tick() const noexcept -> Duration;

    Clock(const Clock&)            = delete;
    Clock& operator=(const Clock&) = delete;

private:
    Clock() noexcept = default;
};

// ---------------------------------------------------------------------------
// 5.10 Process (§4.5)
// ---------------------------------------------------------------------------

using SignalHandlerFn = void (*)(int signal) noexcept;  // async-signal-safe.

enum class Signal : std::uint8_t {
    Interrupt,    // SIGINT
    Terminate,    // SIGTERM
    SegFault,     // SIGSEGV  - install only for crash dumps.
    BusError,     // SIGBUS
    IllegalInst,  // SIGILL
    FpError,      // SIGFPE
};

class Process {
public:
    // §4.5 inv #5: single-instance accessor.
    [[nodiscard]] static auto get() noexcept -> Process&;

    [[nodiscard]] auto argv()             const noexcept -> eastl::span<const eastl::string_view>;
    [[nodiscard]] auto env(eastl::string_view name) const noexcept -> eastl::optional<eastl::string_view>;
    [[nodiscard]] auto cwd()              const noexcept -> CanonicalPath;
    [[nodiscard]] auto executable_path()  const noexcept -> CanonicalPath;
    [[nodiscard]] auto pid()              const noexcept -> std::uint32_t;

    // §4.5 inv #2: setter only, no getter.
    auto set_exit_code(int code) noexcept -> void;

    [[nodiscard]] auto install_signal(Signal, SignalHandlerFn) noexcept -> Result<void>;
    [[nodiscard]] auto uninstall_signal(Signal)                 noexcept -> Result<void>;

    Process(const Process&)            = delete;
    Process& operator=(const Process&) = delete;

private:
    Process() noexcept = default;
};

// ---------------------------------------------------------------------------
// 5.11 FileIo + IoToken (§4.6)
// ---------------------------------------------------------------------------

enum class OpenMode : std::uint8_t { Read, Write, ReadWrite, Append };

struct Stat {
    std::uint64_t size{0};
    WallTime      modified{};
    bool          is_directory{false};
    bool          is_symlink{false};
};

struct DirEntry {
    CanonicalPath path;
    bool          is_directory{false};
};

// IoToken - single-consumer handle for an in-flight async op (§4.6 inv #3).
// Move-only. Drop = best-effort cancel. Completion is poll-only; no
// callbacks cross the boundary (§4.6 inv #7).
class IoToken {
public:
    enum class State : std::uint8_t { InFlight, Ready, Cancelled };

    IoToken()                                 = default;
    IoToken(const IoToken&)                   = delete;
    IoToken& operator=(const IoToken&)        = delete;
    IoToken(IoToken&&) noexcept;
    IoToken& operator=(IoToken&&) noexcept;
    ~IoToken();

    // Non-blocking; may be called repeatedly.
    [[nodiscard]] auto poll() noexcept -> State;

    // Bounded blocking wait. Returns Ready / Cancelled / InFlight (timeout).
    [[nodiscard]] auto wait_for(Duration) noexcept -> State;

    // Best-effort cancellation; OS may have already completed.
    auto cancel() noexcept -> void;

    // Once Ready, take the result. Calling before Ready returns
    // Error::Interrupted; calling twice returns Error::AlreadyExists.
    [[nodiscard]] auto take_result() noexcept -> Result<eastl::span<const std::byte>>;

private:
    friend class FileIo;
    struct Impl;
    Impl* impl_{nullptr};
};

struct FileIoConfig {
    std::uint8_t io_thread_budget{2};  // §4.6 inv #6, sized at construction.
};

class FileIo {
public:
    [[nodiscard]] static auto create(FileIoConfig = {}) noexcept -> Result<FileIo>;

    FileIo(FileIo&&) noexcept;
    FileIo& operator=(FileIo&&) noexcept;
    FileIo(const FileIo&)            = delete;
    FileIo& operator=(const FileIo&) = delete;
    ~FileIo();

    // Synchronous primitives. Asserted off-main-thread in debug builds
    // (§4.6 inv #2). Public surface accepts only CanonicalPath.
    [[nodiscard]] auto read_all(CanonicalPath) noexcept                                 -> Result<eastl::span<const std::byte>>;
    [[nodiscard]] auto write_atomic(CanonicalPath, eastl::span<const std::byte>) noexcept -> Result<void>;  // §4.6 inv #4.
    [[nodiscard]] auto stat_path(CanonicalPath) noexcept                                -> Result<Stat>;
    [[nodiscard]] auto list_dir(CanonicalPath, eastl::span<DirEntry> out) noexcept        -> Result<std::size_t>;
    [[nodiscard]] auto remove(CanonicalPath) noexcept                                   -> Result<void>;

    // Bounded-async primitives. Poll-only; never callbacks (§4.6 inv #7).
    [[nodiscard]] auto read_async(CanonicalPath) noexcept                                     -> Result<IoToken>;
    [[nodiscard]] auto write_atomic_async(CanonicalPath, eastl::span<const std::byte>) noexcept -> Result<IoToken>;

private:
    FileIo() noexcept = default;
    struct Impl;
    Impl* impl_{nullptr};
};

}  // namespace glibre::platform
```

Event types listed above are the `InputEvent` / `WindowEvent` /
`FileEvent` sealed sums (§4.2, §4.3). Schemas: the platform context
exposes no Fory-serialized schema - every event lives only in-memory
inside its `EventQueue<T>`; persistence is owned by other contexts
(`data`, `content`). Error type: `glibre::platform::Error`, the closed
sum from §4.7, contributed as one arm of the engine-wide
`glibre::Error` variant per `reviews/decisions/error-model.md`.

## 6. Internal Architecture

Non-binding sketch for implementers. Nothing in this section adds a
contract beyond §4–§5; if an implementation diverges in shape but
preserves the §4 invariants and the §5 surface, the divergence is
allowed. The intent is to give a reader who has just finished §5 a
mechanical picture of how the seven aggregates partition into source
files, where the lone Objective-C++ translation unit sits, and which
abstraction seams are *kept open* against the day a non-macOS host is
added.

### 6.1 Module layout

The platform context lives at `engine/platform/` and is split into
seven sibling modules — one per §4 aggregate — plus a tiny `detail/`
folder for shared internals. Each module is a directory under
`engine/platform/src/<name>/` with a parallel public header tree under
`engine/platform/include/glibre/platform/<name>/` (the §5 single-
header projection is the union of these). The seven modules and their
1:1 mapping to §4 aggregates:

| Module      | §4 aggregate(s)                                      | One reason to change                                          |
|-------------|------------------------------------------------------|---------------------------------------------------------------|
| `window/`   | §4.1 `Window` / `Display` / `LogicalSize` / DPI      | OS window-server contract shifted (SDL3 minor, AppKit, DPI).  |
| `surface/`  | §4.1 `Surface` (the GPU-presentable handle)          | The SDL3 → `CAMetalLayer` → metal-cpp bridge contract shifted.|
| `event/`    | §4.2 `EventQueue<T>` / `Pump` / `InputEvent` / `WindowEvent` | SDL3's event vocabulary or the SPSC ring protocol shifted. |
| `watcher/`  | §4.3 `FileWatcher` / `FileEvent` / dedup / canonical | The OS file-watch backend or canonicalization rule shifted.   |
| `clock/`    | §4.4 `Clock` / `Instant` / `WallTime`                | The OS time source's resolution / API / guarantees shifted.   |
| `process/`  | §4.5 `Process` / argv / env / signals                | The OS process-control contract shifted.                      |
| `fileio/`   | §4.6 `FileIo` / `IoToken` / `Stat` / `DirEntry`      | The OS file-IO contract or async budget shape shifted.        |

`Surface` is split out of `window/` into its own module deliberately:
the SDL3 → AppKit bridge is the single Objective-C++ translation unit
in the entire engine (§6.2), and giving it a sibling directory rather
than burying it inside `window/` makes that load-bearing fact visible
in the source tree. Lifetime is still the `Window`'s — the §4.1 inv #1
"Surface lifetime is strictly bound to its Window" rule is preserved
by `Window::surface()` being the sole `Surface` factory and by
`Window`'s destructor releasing the layer through the bridge.

`detail/` holds two shared internals only: a private arena that backs
`CanonicalPath::view_` (so equal canonical paths share a backing
buffer and `operator==` reduces to pointer compare in the common
case), and a tiny `error/` translator that maps `errno` /
`SDL_GetError()` / `NSError` → `platform::Error` at one site per
backend. No other cross-module reach-throughs exist; if a module
needs another module's output, it goes through the §5 public surface
just like the engine does.

The build system compiles `engine/platform/` as a single static
library `libglibre_platform.a`; modules are not separate compilation
units linked together because the §4.7 `PlatformError` closed sum and
the §6.5 `Pump` cross-aggregate fan-out require them to share inline
visibility. CMake (or the build tool of choice) globs `src/*/`*.cpp`
plus the lone `surface/bridge.mm` (§6.2) and produces one archive
that engine targets link against.

### 6.2 The single Objective-C++ bridge file

The entire engine has **exactly one** Objective-C++ translation unit:
`engine/platform/src/surface/bridge.mm`. It is the only file in the
repository that:

- compiles with `-x objective-c++`,
- includes `<AppKit/AppKit.h>`, `<QuartzCore/CAMetalLayer.h>`, or any
  Cocoa / Foundation header,
- may catch an `NSException`,
- links against the `AppKit`, `Foundation`, `QuartzCore`, and `Metal`
  frameworks.

Every other `.cpp` (and `.hpp`) in the engine is pure C++23/26. The
build system enforces this: a CMake check rejects any file with a
`.mm` extension outside `engine/platform/src/surface/`, and the
linker flag `-framework AppKit` is added only to
`bridge.mm.o`'s compile flags, not engine-wide. The bridge is what
the §1 collapse "C++ cannot reach the OS without it" stands on; if
two `.mm` files appear, we have failed the rule and one must be
re-collapsed.

The bridge's public C++ shape is a tiny header
`engine/platform/src/surface/bridge.hpp` (internal to the platform
library, not part of the §5 surface):

```cpp
// Internal — platform/surface only. No engine code outside the
// surface module includes this header.
namespace glibre::platform::surface::detail {

// Create a CAMetalLayer-backed view on the given SDL_Window. The
// returned void* is a non-owning CAMetalLayer*; the SDL_MetalView
// out-parameter holds the owning handle that must be released by
// destroy_metal_view() when the Window dies.
struct LayerHandle {
    void* layer{nullptr};       // CAMetalLayer*, opaque outside this TU.
    void* sdl_metal_view{nullptr};  // SDL_MetalView, opaque outside this TU.
};

[[nodiscard]] auto create_metal_view(void* sdl_window) noexcept
    -> Result<LayerHandle>;

auto destroy_metal_view(LayerHandle) noexcept -> void;

// Read-back the layer's drawable size and DPI in one call. Used by
// Window to refresh its DpiScale snapshot after DpiChanged events.
[[nodiscard]] auto query_layer_metrics(void* layer) noexcept
    -> Result<eastl::pair<PhysicalSize, DpiScale>>;

}  // namespace glibre::platform::surface::detail
```

Inside `bridge.mm` the implementation calls `SDL_Metal_CreateView` to
get the `SDL_MetalView`, retrieves the underlying `CAMetalLayer*` via
`SDL_Metal_GetLayer`, sets `layer.contentsScale` to match the
window's `backingScaleFactor`, and returns the layer as `void*`. Any
`@try / @catch` around AppKit calls converts the `NSException` to
`Error::IoFailure { OsCode { (std::int32_t) [exception code] } }`
before returning — per §4.7 inv #2, no exception ever crosses the
public surface. The pointer that crosses the bridge is `void*`; it is
up-cast to `MTL::Layer*` only on the **render** side via
`reinterpret_cast`, gated by the engine-wide rule that render is the
sole consumer of `Surface::raw_layer()`.

The bridge owns no observable state: it is a stateless mapping
function. `LayerHandle`s are stored inside `Window::Impl`, not inside
the bridge. This keeps the bridge's lifetime concerns trivial (none)
and lets `Window::~Window()` drive teardown via a single
`destroy_metal_view` call regardless of how the `Window` got
destroyed.

### 6.3 SDL3 facade — `window/` and `event/`

`window/` and `event/` together form the SDL3 facade for the
window-server and input subsystems. SDL3 is an implementation detail;
no SDL type, header, or macro escapes either module's public surface.

`window/` wraps `SDL_Window` and the SDL3 display query API:

- `Window::open(WindowDesc)` → `SDL_CreateWindow` with the
  `SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY` flags on macOS,
  followed by `surface::detail::create_metal_view` to attach the
  `CAMetalLayer`. The handle pair `(SDL_Window*, surface::detail::LayerHandle)`
  is stored in the pimpl `Window::Impl` along with a cached
  `LogicalSize` / `DpiScale` snapshot kept in sync by the §6.5
  pump.
- `Window::request_resize` → `SDL_SetWindowSize`; the actual resize
  arrives back as a `WindowEvent::Resized` from the pump.
- `Window::request_close` → `SDL_PushEvent` with a synthesized
  `SDL_EVENT_WINDOW_CLOSE_REQUESTED`, so the close path goes through
  the same §6.5 fan-out as user-initiated close.
- `Window::display()` → re-query SDL3's display list each call. The
  §4.1 inv #5 "Display snapshots are immutable" rule is preserved
  by the value semantics of the returned `Display` struct; the
  underlying SDL3 query is *not* cached across frames — caching is
  the consumer's choice if they want it.

The `Display` enumerator lives inside `window/` rather than its own
module because hot-plug events are delivered to the same SDL3 event
pump that drives windows; splitting them would re-introduce the
two-callback-site bug §4.1 collapses away.

`event/` wraps `SDL_PollEvent` and SDL3's gamepad / sensor APIs:

- The `Pump` is the sole owner of the `SDL_PollEvent` loop. On
  construction it captures references to one `EventQueue<InputEvent>`
  and one `EventQueue<WindowEvent>` (the two queues in §4.2).
- `Pump::drain()` runs `while (SDL_PollEvent(&ev))` to exhaustion,
  classifies each event into one of three buckets — input, window,
  or "consumed-internally" — and writes typed variants into the
  appropriate `EventQueue<T>`. No SDL constant, no `SDL_Event`, no
  `SDL_Keycode` value ever appears in either queue: every payload
  goes through a translation table that maps the SDL3 enum to the
  §5 `KeyCode` / `ScanCode` / `MouseButton` / `GamepadAxis` /
  `GamepadBtn` enums. Unknown SDL3 events are dropped per §4.2 inv #5.
- `EventQueue<T>` itself is a fixed-capacity SPSC ring buffer
  templated on the payload type, sized at construction. Its
  `Impl` uses two `std::atomic<std::size_t>` indices (head, tail)
  with `memory_order_release` on producer publish and
  `memory_order_acquire` on consumer drain — the canonical lock-
  free SPSC. Capacity is a power of two so wrap-around is a mask, not
  a modulo. The producer side is touched only by `Pump::drain()`
  and by `FileWatcher`'s I/O thread (which writes into a third
  `EventQueue<FileEvent>` — see §6.4); the consumer side is the
  engine's main thread.

A subtle point: `WindowEvent::DpiChanged` is the only event whose
processing crosses module boundaries inside the platform library.
The pump receives `SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED` from SDL3,
calls `surface::detail::query_layer_metrics` to get the new
`PhysicalSize`+`DpiScale` pair, updates the owning `Window::Impl`'s
cached snapshot, and *then* enqueues the typed `DpiChanged` event.
The §4.1 inv #4 "PhysicalSize = round(LogicalSize * DpiScale) always"
rule survives because the snapshot is updated before the event is
visible to the engine; a frame that reads `Window::physical_size()`
after draining `DpiChanged` reads the new value.

### 6.4 `FileWatcher` — SDL3 FSEvents on macOS, abstracted for the rest

The §1 collapse points the watcher at "SDL3's filesystem events on
macOS today; kqueue / inotify / ReadDirectoryChangesW elsewhere
later". `watcher/` realizes that with a one-method internal interface
plus one backend implementation:

```cpp
// engine/platform/src/watcher/backend.hpp - internal.
namespace glibre::platform::watcher::detail {

// One concrete backend per OS family. Selection is compile-time.
class IBackend {
public:
    virtual ~IBackend() = default;

    [[nodiscard]] virtual auto subscribe(CanonicalPath root) noexcept
        -> Result<WatchToken>                     = 0;
    [[nodiscard]] virtual auto unsubscribe(WatchToken)   noexcept
        -> Result<void>                           = 0;

    // Drains pending raw events from the OS into the supplied span;
    // returns the number written. Called only by the watcher I/O
    // thread, never by the engine.
    [[nodiscard]] virtual auto poll_raw(eastl::span<RawEvent> out) noexcept
        -> std::size_t                            = 0;
};

}  // namespace glibre::platform::watcher::detail
```

The MVP ships exactly one implementation,
`watcher/backends/sdl3_fsevents.cpp`, which uses
`SDL_GetPathInfo` for canonicalization and SDL3's filesystem-event
API (which on macOS sits on top of `FSEventStreamCreate` /
`FSEventStreamScheduleWithRunLoop` from Core Services). The runloop
is hosted inside the watcher's own I/O thread (one thread per
`FileWatcher` instance, started by `FileWatcher::create()` and
joined by the destructor — §4.3 inv #4 "Watcher lifetime owns its
subscriptions"). The thread's loop, sketched:

```text
while (!stop_requested) {
    backend->poll_raw(scratch);          // pulls raw OS events
    canonicalize(scratch, canon_buf);    // §4.3 inv #1
    dedup(canon_buf, content_hash_lru);  // §4.3 inv #2
    reassemble_renames(canon_buf);       // §4.3 inv #3
    for (auto& e : canon_buf) {
        per_token_queue[e.token].push(e);  // SPSC ring per WatchToken
    }
}
```

`FileWatcher::take_events(token, out)` is a non-blocking drain of
the SPSC ring keyed on the `WatchToken`; the I/O thread is producer,
the calling (main) thread is consumer. This is the §4.3 inv #5
"Watcher does not block the main thread" rule, structurally enforced.

The future kqueue / inotify / RDC ports are dropped in by adding
`watcher/backends/kqueue.cpp`, `.../inotify.cpp`,
`.../rdc.cpp` and selecting one at build time via a CMake variable.
The selection is compile-time, not runtime — there is no virtual
dispatch in the hot path past construction; the `IBackend` interface
exists only so the watcher's frontend code (canonicalize / dedup /
rename-reassembly / SPSC fan-out) is shared verbatim across hosts.
The macOS FSEvents-via-SDL3 implementation specifically uses SDL3's
event-pump bridging where it can; if SDL3's filesystem-event surface
proves insufficient for recursive watches across mount points, the
backend falls back to a direct `FSEventStreamCreate` call inside the
same `.cpp` (still no `.mm` — Core Services is C, not Objective-C),
and that decision is local to one file.

Content-hash dedup uses BLAKE3 (a small in-tree dependency, no
networking or platform calls) over the file contents at the moment
of the OS event; an LRU keyed on `(canonical_path, hash)` collapses
debounce-window duplicates per §4.3 inv #2. The LRU is a fixed-
capacity ring (no allocation in the hot path) sized at construction
via a `WatcherConfig` parameter not shown in the §5 stub but added
when the implementation lands; the spike notes this so a future PR
that surfaces the config can do so without re-deriving the
invariant.

### 6.5 `FileIo` — bounded SPSC queue + worker thread; poll-only `IoToken`

The §4.6 `FileIo` aggregate takes the same shape as `watcher/` but
with the directionality reversed: the engine writes requests into a
queue, a worker thread reads them and performs blocking POSIX I/O,
and per-request `IoToken`s carry the completion result back through
their own per-token slot. There is **no** completion queue and **no**
callback dispatch — §4.6 inv #7 "No public callbacks" is the rule
that makes this whole design simple.

Layout inside `fileio/`:

- `fileio/sync.cpp` — the synchronous primitives
  (`read_all`, `write_atomic`, `stat_path`, `list_dir`, `remove`).
  Each is a thin wrapper over POSIX (`open` / `read` / `pwrite` /
  `stat` / `readdir` / `unlink` / `rename`). `write_atomic` follows
  §4.6 inv #4: write to `<target>.tmp.<pid>.<rand>`, `fsync`,
  `rename` over the target, `fsync` the parent dir. None of these
  allocate; they take spans and write into caller-supplied buffers
  whenever the API allows it. Debug builds assert the calling thread
  is *not* the main thread (§4.6 inv #2) by comparing
  `pthread_self()` to a TLS sentinel set by the engine's main loop.
- `fileio/async.cpp` — the bounded-async primitives
  (`read_async`, `write_atomic_async`). Each enqueues a `Request`
  struct into a single bounded SPSC ring, returns an `IoToken` whose
  `Impl` holds a pointer to a slot in a `Slot[]` array indexed by
  the request id, and lets the caller poll. The `Slot` carries:
  - `std::atomic<IoToken::State>` state,
  - `eastl::span<const std::byte>` result_bytes (filled before state
    transitions to `Ready`, with `memory_order_release`),
  - a `Result<void>` error code for failure cases.
- `fileio/worker.cpp` — exactly one worker thread per `FileIo`
  instance (the §4.6 inv #6 budget knob — `FileIoConfig::io_thread_budget`
  — sizes a *pool* when > 1, but the MVP ships with the default of
  2 and the design holds for both 1 and N).
- `fileio/queue.hpp` — the bounded SPSC ring. Same lock-free
  primitive as `event/`'s `EventQueue<T>` (a tiny shared template
  in `detail/spsc_ring.hpp` consumed by both modules). Bounded
  capacity from the start; saturating it returns
  `Error::IoFailure { OsCode{ENOBUFS-equivalent} }` per §4.6 inv #6,
  not enqueue-and-grow.

The hot path:

```text
engine main thread                 worker thread
------------------                 -------------
read_async(path) →
   alloc slot, push Request →     pop Request,
   return IoToken{slot}           open + read(path),
                                  copy bytes into result_buf,
                                  store state = Ready (release)
poll() →
   load state (acquire) ─────────┘
   if Ready → take_result()
```

`IoToken::poll()` is a single relaxed `atomic::load` followed by an
acquire fence on the state transition — no syscall, no allocation.
`IoToken::wait_for(Duration)` uses
`std::this_thread::sleep_for` with exponential backoff capped at the
supplied duration; we deliberately avoid futex / mutex /
condition-variable, both because this keeps the per-token slot the
size of one cache line and because pulling in a kernel wait would
re-introduce a callback-shaped surface.

`IoToken::cancel()` flips the slot to `Cancelled` (release); the
worker thread checks the state on dequeue and short-circuits the
operation if cancellation arrives before the I/O begins. If the
operation has already started, cancel is best-effort and the worker
runs it to completion before noticing — this matches §4.6 inv #3
"Cancellation is best-effort — the OS may have already completed".

`~IoToken()` is the load-bearing piece: it stamps `Cancelled` and
returns the slot to a free-list, but only **after** the worker has
released the slot back. The implementation uses a two-phase scheme:
the destructor sets a "abandoned" bit, the worker on completion
checks the bit and either delivers the result or marks the slot
free. The slot pool is sized by `FileIoConfig::io_thread_budget *
queue_depth`; abandoned-but-not-yet-released slots count against
the budget, so a caller that drops `IoToken`s without polling can
exhaust the budget — that is by design, mirroring the kernel's
"don't drop file descriptors" rule.

### 6.6 `Clock` — `mach_absolute_time()` on macOS; abstracted seam

`clock/` is the simplest module. The MVP ships
`clock/clock_macos.cpp` with this shape:

```cpp
namespace glibre::platform {

auto Clock::now() const noexcept -> Instant {
    static const auto info = []() noexcept -> mach_timebase_info_data_t {
        mach_timebase_info_data_t i{};
        ::mach_timebase_info(&i);
        return i;
    }();
    const auto t = ::mach_absolute_time();
    // Convert mach ticks -> nanoseconds via numer/denom.
    const auto ns = static_cast<std::int64_t>(
        (__uint128_t(t) * info.numer) / info.denom);
    return Instant{ns};
}

auto Clock::wall() const noexcept -> WallTime {
    return WallTime{std::chrono::system_clock::now()};
}

auto Clock::native_tick() const noexcept -> Duration {
    // Minimum representable Duration on this clock — sized so the
    // deterministic simulation step (specs/ecs §...) lands on an
    // integer multiple of it without drift.
    return Duration{1};  // 1 ns; macOS's mach_absolute_time is sub-µs.
}

Clock& Clock::get() noexcept {
    static Clock c;  // Meyers singleton — §4.4 inv #5.
    return c;
}

}  // namespace glibre::platform
```

The §4.4 inv #1 "monotonic non-decreasing" rule is structurally true
of `mach_absolute_time` (it does not slew, does not wrap on the
timescales the engine cares about). A defensive `assert(ns >= last)`
guards regression in debug builds; on a detected regression we abort
rather than clamp, per the invariant.

The future ports follow the same one-file-per-host pattern:
`clock/clock_windows.cpp` calls `QueryPerformanceCounter` /
`QueryPerformanceFrequency`; `clock/clock_linux.cpp` calls
`clock_gettime(CLOCK_MONOTONIC_RAW, &ts)`. CMake selects exactly one
`clock_<host>.cpp` for the build; there is no virtual dispatch — the
function bodies vary, the API does not.

### 6.7 `Process` — POSIX argv / env / signals

`process/process_macos.cpp` is a thin POSIX wrapper:

- argv / env captured via `_NSGetArgv()` / `_NSGetArgc()` /
  `_NSGetEnviron()` on macOS, copied once at startup into UTF-8
  string-view spans backed by an arena owned by `Process::Impl`.
  The capture function is the engine's `main()` shim; `Process::get()`
  returns the populated singleton thereafter.
- `cwd()` calls `getcwd` once at startup (its result is the only
  source of the engine's notion of working directory; chdir
  out-of-band is unsupported per §4.5 inv #1).
- `executable_path()` calls `_NSGetExecutablePath` and canonicalizes
  the result via `CanonicalPath::from_absolute`.
- `pid()` returns `getpid()`.
- `set_exit_code(int)` writes to a `std::atomic<int>` read by the
  engine's `main` shim on return.
- `install_signal(Signal, Fn)` calls `sigaction` with `SA_SIGINFO`
  and an internal trampoline that calls the user-supplied
  async-signal-safe function. The trampoline lives in a `.text`
  segment marked `__attribute__((no_sanitize_address))` so ASan
  builds do not corrupt the signal-safety guarantee. §4.5 inv #3
  "At most one handler per signal" is enforced by checking the
  internal handler-table slot before calling `sigaction`; an
  already-installed slot returns `Error::AlreadyExists`.

`process/process_macos.cpp` is the only host-specific file in this
module; the future Linux port shares it nearly verbatim minus the
`_NSGet*` symbols (replaced by `__libc_argv` / `environ`), and the
Windows port replaces signals with `SetConsoleCtrlHandler` /
`AddVectoredExceptionHandler`. The aggregate's surface (§5.10) is
host-agnostic; only the implementation file changes.

### 6.8 Threading topology

The platform context creates and owns at most three OS threads at
any time; everything else runs on the engine's main thread:

| Thread          | Owner            | Producer for                     | Lifetime                  |
|-----------------|------------------|----------------------------------|---------------------------|
| Main            | engine           | (consumer of every queue)        | process                   |
| Watcher I/O     | `FileWatcher`    | `EventQueue<FileEvent>` per token| `FileWatcher` instance    |
| FileIo worker(s)| `FileIo`         | per-`IoToken` slots              | `FileIo` instance         |

There is no platform-owned thread pool, no fiber scheduler, no
job-graph dispatcher — those are sibling-context concerns per §1's
refusal list. SDL3 is configured with
`SDL_HINT_MAIN_CALLBACK_RATE` set to single-threaded mode; we never
let SDL3 spawn its own helper threads.

Cross-thread communication is exclusively through the SPSC rings
described above. There are no mutexes on the platform → engine
producer side; there is exactly one mutex inside `process/`, used
during signal-handler installation to guard the handler table
against concurrent installs from non-main threads (which are
themselves a contract violation, but the mutex catches the race
deterministically rather than producing a torn write).

### 6.9 Allocation discipline

Every heap allocation in the platform context happens inside a
constructor or static factory. After construction:

- `Pump::drain()` does not allocate.
- `EventQueue<T>::drain()` does not allocate.
- `FileWatcher::take_events()` does not allocate.
- `IoToken::poll()` / `take_result()` does not allocate.
- `Clock::now()` / `wall()` does not allocate.
- `Process::env()` / `argv()` does not allocate.

The synchronous `FileIo::read_all` is the lone exception: it must
allocate to return the file contents. The aggregate returns the
allocated buffer as `eastl::span<const std::byte>` and the platform
library owns the backing storage in a per-call arena released on
the next `read_all` from the same `FileIo` instance. Callers that
need to keep the bytes copy them out before the next call; this is
documented at the API site when the implementation lands.

### 6.10 Failure-translation seam

Every backend call funnels its error through one of three
translators in `detail/error/`:

- `errno_to_error(int)` — POSIX errno → `platform::Error`.
- `sdl_to_error(const char* sdl_msg)` — `SDL_GetError()` →
  `platform::Error`. The message is parsed for known prefixes
  (`"Permission denied"`, `"No such file"`) and falls through to
  `Error::IoFailure { OsCode{0} }` with the message stashed in a
  thread-local diagnostic buffer for logging.
- `ns_to_error(NSException*)` — only callable from `bridge.mm`;
  reads `[exception code]` and produces
  `Error::IoFailure { OsCode{code} }`.

This is the §4.7 inv #3 "Errors are constructed at the site they
happen" rule made physical: no aggregate ever calls another
aggregate's translator; if a `FileIo` call surfaces an SDL3 error
(it should not — `FileIo` is POSIX-direct), that is a bug, not a
fall-through path.

### 6.11 What §6 does *not* do

- §6 does not introduce any new public type, function, or invariant.
  Everything visible to the engine is in §5; everything enforced is
  in §4. This section's job is to make a reader's mental model of
  the implementation faithful, not to extend the contract.
- §6 does not pin any specific SDL3 version, BLAKE3 implementation,
  or POSIX revision. Those are vendor decisions recorded under
  `reviews/decisions/` when the implementation PR lands.
- §6 does not specify the build system. CMake is the working
  assumption everywhere else in the repository, and §6 reads
  naturally on top of CMake, but a future move to Bazel / Buck /
  Meson would not change any of the §4 invariants or §5 surface;
  it would change a few sentences here and nothing else.

## 7. Persistence & Schemas

Platform owns very little persistent state. The collapse rule in §1 +
PHILOSOPHY §10 plus the "Persistence ⇔ Schema" biconditional in
`specs/data/SPEC.md` §4.10 inv. 1 force the question down to a single
predicate: *does this aggregate's value cross a save, hot-reload, or
plugin-dylib boundary?* Apply that predicate to each §4 aggregate and
the answer is **no for almost all of them**. The aggregates are
ephemeral views of OS-owned state — recreated from scratch on each
process bring-up, never carried across a hot-reload swap, never written
to disk by platform code.

### 7.1 Aggregate-by-aggregate disposition

| §4 aggregate                        | Persistent? | Rationale                                                                                                                                                                |
|-------------------------------------|-------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `Window` / `Display` / `Surface`    | No          | OS handles. `Display` is a re-queried snapshot (§4.1 inv #5); `Surface`'s `CAMetalLayer*` is process-local UB to serialize; `Window` reopens from `WindowDesc` defaults. |
| `EventQueue<T>` / `Pump`            | No          | In-flight ring buffers. Drained every frame; cross-process or cross-run replay is an `e2e` trace concern (`specs/e2e/SPEC.md`), not a platform schema.                   |
| `FileWatcher`                       | No          | Subscriptions are reseated by the consuming context after a hot-reload; the watcher itself owns no value the engine needs to round-trip.                                 |
| `Clock` / `Instant` / `WallTime`    | No          | `Instant` is explicitly non-serializable (§4.4 inv #2). `WallTime` is sourced fresh; `Clock` is a process-singleton accessor.                                            |
| `Process`                           | No          | argv / env / cwd / exit code / signal table are OS-owned process state, captured fresh at startup (§4.5 inv #1). The aggregate has no value the engine writes back.      |
| `FileIo` / `IoToken`                | No          | Bounded-async tokens are single-consumer in-flight handles (§4.6 inv #3); cancelling on drop is the load-bearing rule. Persisting an `IoToken` would violate it.         |
| `PlatformError` (closed sum, §4.7)  | No          | A by-value error returned through `std::expected`; never crosses a save boundary. Loggers stringify; they do not Fory-encode.                                            |

The platform context therefore ships **zero** `data/schemas/platform/`
files for the MVP. The `data` context's "Persistence ⇔ Schema"
biconditional holds vacuously: there are no persistent aggregates here,
so there are no schemas, and `Foryc` enumerating
`data/schemas/platform/**/*.fory` finds nothing. Schema file paths
follow the `data/schemas/platform/<Type>.fory` convention if and when a
future addition crosses a persistence boundary; until then the
directory is intentionally absent (an empty directory would lie about
the spine's shape).

### 7.2 Migration rules

There is nothing to migrate. The single-file-per-aggregate migration
discipline from `specs/data/SPEC.md` §4.6 / §4.7 applies if and when a
schema is added; today the migration ledger for the platform context
is empty.

If §7.3's deferred candidates (window placement, input bindings) are
later promoted into platform, they will follow the standard rules
without exception:

- Each schema gets a monotonically-increasing `SchemaVersion`
  (`specs/data/SPEC.md` §4.1 inv #2).
- Tag numbers are immutable once shipped; removed fields move to the
  reserved set (§4.1 inv #3).
- Per `(N → N+1)` migration is a pure free function authored by
  platform and registered via the codegen-emitted macro
  (`reviews/decisions/fory-codegen.md` §"Migration Mechanic"),
  allocating only inside the supplied arena and reading no I/O.
- The migration chain is total — every step from 1 to current must
  exist, enforced at codegen time.

### 7.3 Deferred candidates (refusals routed elsewhere)

Three pieces of state superficially look like platform persistence but
are explicitly **refused** here. Each is recorded so a future re-read
of this spec does not re-relitigate the boundary.

- **Window placement / size memory across launches.** A "remember last
  position and size" feature is user-preferences state, not OS-seam
  state. Routed to a future user-preferences context (sketched as a
  `tools` / editor responsibility in §3 refusals). The platform
  context's `WindowDesc` already accepts a `LogicalSize`; the
  preferences context is responsible for reading the persisted
  placement and constructing the `WindowDesc` at bring-up.
  Schema lives at `data/schemas/<that-context>/<…>.fory`, not under
  `data/schemas/platform/`.
- **Input bindings (key → action mapping).** A binding map is a
  domain concept (a particular key chord means *jump*), not an OS
  fact. Platform's input vocabulary is the closed `InputEvent` sum
  (§4.2); turning an event into an action is owned by a future
  input-binding context (or the editor's settings UI), and the schema
  for the binding table lives there. Platform refuses to host the
  schema because the SRP test (one OS facet → one reason to change)
  rejects it: the binding table changes when the *game* changes, not
  when *SDL3* changes.
- **File-watcher canonical-path subscription list (cache).** A naive
  reading of §4.3 would persist "the set of paths I am watching" to
  warm-start the watcher across runs. Refused: subscriptions are
  reseated by the consumer (asset / content / editor) on every
  startup; persisting them in platform would (a) duplicate what
  `content` already authoritatively owns and (b) break the
  determinism property — a re-run with a stale persisted list emits
  a different event stream than a fresh run. The
  `CanonicalPath`-keyed BLAKE3 hash cache mentioned by harmonius
  R-14.6.6 / R-14.6.9 (collapsed in §3) is, where it survives at all,
  a `content` build-cache concern, not a platform schema.

### 7.4 Adjacent non-Fory artefacts (refusals)

The following platform-touching artefacts are **never** Fory-serialized
and are called out so reviewers do not accidentally route them through
this section:

- **Log files** are line-oriented text written by the future
  `diagnostics` context. The platform contributes only the file IO
  primitive and the wall-time source; line schemas, rotation policy,
  and log levels are owned downstream.
- **Crash dumps / minidumps** are a fixed OS-defined binary format
  (the Mach minidump produced from the signal-safe capture path,
  §3 collapse "Multi-OS log sinks → crash-dump *capture* primitive
  only"). They are not Fory payloads and have no `.fory` schema; the
  `diagnostics` context owns post-capture symbolication and upload.
- **PSO caches, asset bundles, plugin manifests.** Owned by `render`,
  `content`, and `core` respectively. Platform's only contribution is
  the `FileIo` byte stream they sit on top of (`specs/data/SPEC.md`
  §1 — the data spine routes these elsewhere).

The net effect of §7: the platform context contributes **nothing** to
`AbiHash` (`specs/data/SPEC.md` §4.4) — its schema-source-hash list is
empty — so a platform-only edit can never be the cause of a plugin
ABI-hash mismatch. That is the load-bearing reason this section is
short.

## 8. Hot-Reload Contract

The platform context is loaded into the engine in two distinct shapes,
and the hot-reload contract separates them mechanically:

1. **Platform aggregates as ambient OS state.** `Window`, `Surface`,
   `EventQueue<T>`, `Pump`, `FileWatcher`, `Clock`, `Process`,
   `FileIo`, and their value objects are *consumed* by every other
   plugin (render, content, ecs, tools). When any of those *peer*
   plugins reload at the phase 8 barrier, the platform aggregates
   **do not** participate — they keep their state because their
   state lives in the OS (window-server entries, kqueue / FSEvents
   subscriptions, monotonic counters, kernel file descriptors), not
   in the swapping plugin's heap. The four-step protocol from
   `reviews/decisions/hot-reload-protocol.md` runs unchanged for
   the peer plugin; platform-owned handles are passed through by
   reference and remain valid across the swap.
2. **Platform itself as a plugin.** Per PHILOSOPHY §3, every domain
   ships as a `.dylib`, and platform is no exception. When the
   *platform* `.dylib` itself is the outgoing plugin P at phase 8,
   the same drain → swap → migrate → resume sequence runs, but the
   state-survival rules below apply specifically to OS-backed
   aggregates: most state survives by re-acquisition from the OS,
   not by carrying bytes across the swap.

This section answers, for each §4 aggregate, the three questions the
hot-reload protocol asks (`reviews/decisions/hot-reload-protocol.md`
§State Survival Rules, §Migrate Function Contract, §Refusal Cases):
*what bytes does the loader preserve, what does `migrate(...)` do,
what triggers refusal*.

### 8.1 Per-aggregate disposition

| §4 aggregate                       | Survives peer-plugin swap? | Survives platform-plugin swap?                                                          | `migrate(...)` body                                                                  |
|------------------------------------|----------------------------|-----------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------|
| `Window` / `Display` / `Surface`   | Yes — opaque pass-through  | Re-acquired from OS by Q::register; `WindowId` numeric value is preserved across swap. | Empty (no in-memory state to reshape; OS owns the bytes).                            |
| `EventQueue<T>` / `Pump`           | Yes — opaque pass-through  | Buffered events drain to the engine before phase 8 (already true per §4.2 inv #1).      | Empty (queue contents are by construction empty at the barrier).                     |
| `FileWatcher`                      | Yes — opaque pass-through  | Subscription list (`eastl::span<CanonicalPath>`) preserved; native fds re-acquired and replayed as fresh `Created` events. | Empty in plugin memory; fresh-event replay is a Q::register responsibility, not a typed migration. |
| `Clock` / `Instant` / `WallTime`   | Yes — pass-through          | Monotonic origin is OS-owned; the in-process `Clock` accessor is rebuilt by Q::register but `Instant::count()` values remain comparable across the swap (same OS source). | Empty.                                                                               |
| `Process`                          | Yes — pass-through          | Argv / env / cwd / pid recaptured by Q::register from `getpid` / `argv` / `environ`; installed signal handlers are re-installed by Q against the same `SignalHandlerFn` symbols (which now live in Q's text segment). | Empty.                                                                               |
| `FileIo` / `IoToken`               | Yes — pass-through          | In-flight `IoToken`s are **drained to terminal state** by P::drain before swap (§8.4 refusal #1). The bounded I/O thread pool is torn down by P::drain and re-spun by Q::register. | Empty (no surviving plugin-side bytes; OS file handles are scoped to single tokens). |
| `PlatformError` (closed sum, §4.7) | Yes — pass-through          | By-value error type returned through `std::expected`; never crosses the boundary as live state. | N/A.                                                                                  |

The pattern above is a direct instance of `hot-reload-protocol.md`'s
mechanical rule "*if it has a `.fory` schema, it survives; otherwise,
it does not*" applied in reverse: §7 already established that platform
ships **zero** `.fory` schemas, so by the survival rule there are no
plugin-private bytes to migrate. Every aggregate's surviving state is
either OS-owned (preserved by the OS across the loader's `dlclose` /
`dlopen`) or vanished by drain (in-flight queues / tokens).

The consequence — **`migrate(...)` is empty for every platform
aggregate** — is the load-bearing simplification of this section.
There is nothing to reshape because there are no bytes that simultaneously
(a) live in plugin memory, (b) outlive the swap, and (c) change layout
across versions. If a future addition violates this — e.g. a platform
aggregate gains a persistent in-memory cache with a versioned layout —
§7.3 already routes that to a peer context (preferences / content),
so the precondition holds by construction.

### 8.2 What the platform exports for peer-plugin reloads

When a peer plugin reloads at phase 8, the platform contributes
exactly two guarantees that the loader and the incoming plugin Q rely
on:

1. **Handle stability.** Every handle vended by platform —
   `WindowId`, `DisplayId`, `WatchToken`, `IoToken` — is a numeric
   identifier owned by the platform aggregate that vended it, not by
   any peer plugin. Phase-8 swap of the *peer* plugin does not
   invalidate the handle. Peer plugin Q's `glibre_plugin_register`
   may receive the same `WindowId` it observed before, and
   dereferencing it through `glibre::platform::Window` returns the
   same OS window. The platform aggregate's lifetime is the
   process's lifetime (or the platform-plugin's own swap, §8.3),
   not the peer plugin's.
2. **Pump quiescence at the barrier.** Per §4.2 inv #1, exactly one
   `Pump::drain()` call per frame on the main thread. Phase 8 runs
   *after* `render-submit` and *before* `present`
   (`reviews/decisions/hot-reload-protocol.md` §Context); the per-
   frame `drain()` already ran in phase 1 and will not run again
   until phase 1 of frame N+1. So the `EventQueue<InputEvent>` and
   `EventQueue<WindowEvent>` are consistent — neither full-mid-write
   nor partially drained — when the loader takes over phase 8. No
   coordination between platform and the loader is needed for this;
   it falls out of the frame-phase ordering.

These guarantees are why peer plugins can hold platform handles
across their own reload without the loader needing per-handle
migration support.

### 8.3 Platform-plugin self-reload

When the platform `.dylib` itself is the outgoing plugin P:

- **Drain (step 1).** `glibre::platform::glibre_plugin_drain` MUST:
  1. Refuse the swap if any `Window` is mid-frame — defined as
     `Pump::drain()` having been called in phase 1 of the current
     frame but the per-frame `Window::surface()` handle still being
     held by render. The loader detects mid-frame state by the
     phase 8 invariant from `frame-phases.md` (phase 8 begins after
     `render-submit`), so the drain function only needs to assert
     that no `Surface` value vended this frame has an outstanding
     reference. Failure → §8.4 refusal case P1.
  2. Drain every in-flight `IoToken` to its terminal state
     (`Ready` or `Cancelled`); pending operations are forced to
     completion by joining the bounded I/O pool. The pool is then
     torn down. This is the only meaningful "wait for in-flight
     work" responsibility platform has.
  3. Capture the `FileWatcher`'s subscription list as
     `eastl::span<CanonicalPath>` into a middleman-typed singleton
     (re-derived by Q in step 4 — see *fresh-event replay* below).
     Native fd / FSEvents stream handles are released; the OS
     subscription list is *not* preserved at the OS layer because
     SDL3 / kqueue / inotify do not survive `dlclose` on the
     subscribing image. The canonical-path list does survive, in
     middleman memory.
  4. Capture each open `Window`'s `WindowDesc` (title, size,
     resizable, fullscreen) and the per-window `WindowId` into a
     middleman singleton; close the SDL3 windows. (Closing here
     rather than re-using is the pragmatic choice: SDL3 windows
     are bound to the SDL3 subsystem instance, which is itself
     reset across plugin swap. Re-opening is sub-millisecond on
     macOS.)
  5. Uninstall every signal handler installed via
     `Process::install_signal`; capture the `(Signal, fn)` set into
     a middleman singleton for re-installation by Q.

  Drain MUST NOT touch `Clock` — the OS monotonic source is
  reload-stable and re-reading `now()` after the swap returns a
  value that compares correctly against any pre-swap `Instant`.

- **Swap (step 2).** Standard. ABI hash check is binding;
  `host_glibre_types_abi_hash` includes the middleman types
  declared above for survival (see §8.5).

- **Migrate (step 3).** Empty. No platform-owned `.fory` schema
  exists today (§7.1). If §7.3's deferred candidates are ever
  promoted into platform, they get standard versioned migrate
  functions per `hot-reload-protocol.md` §Migrate Function
  Contract; until then the migrate phase is a no-op pass.

- **Resume (step 4).** `Q::glibre_plugin_register` MUST:
  1. Re-initialize the SDL3 subsystem and re-open every window
     from the captured `WindowDesc` set. The new `WindowId` for
     each reopened window is mapped to the prior `WindowId` via
     the middleman singleton, so peer plugins continue to
     dereference the same numeric value. (This is the single
     non-trivial piece of platform-plugin migration, and it lives
     entirely in Q's register, not in a typed migrate function —
     because the reshape is "OS handle re-acquisition", not "byte
     layout conversion".)
  2. Re-spin the `FileIo` bounded I/O thread pool with the
     configured `io_thread_budget`.
  3. Re-subscribe the `FileWatcher` to every captured
     `CanonicalPath`. **Each re-subscribed root re-emits a fresh
     `FileEvent::Created` for every existing file under it.** This
     is by design: consumers of the watcher (`content`, `tools`)
     already idempotently handle `Created` events as "load or
     refresh"; replaying them on platform-reload converges the
     consumer's view to the current filesystem state without
     platform needing to persist content hashes. The dedup window
     (§4.3 inv #2) collapses the burst correctly.
  4. Re-install every captured `(Signal, SignalHandlerFn)` pair
     against the *same function pointers in Q's text segment*.
     The middleman captures `Signal` enum values, not raw function
     pointers, and Q is responsible for re-resolving the symbol;
     this is what makes signal handlers survive a code swap.
  5. Re-publish a `HotReloadCompleted` event per
     `hot-reload-protocol.md` §Observer Notification.

### 8.4 Refusal cases

Platform inherits the three universal refusal cases from
`hot-reload-protocol.md` §Refusal Cases (ABI hash mismatch, schema
migration failure — vacuous here, plugin init failure) and adds one
context-specific refusal:

**P1. Mid-frame window drop.** If `glibre_plugin_drain` is called
while any `Surface` vended this frame is still referenced by render
(detected by a per-window outstanding-surface counter the platform
maintains), the drain returns `unexpected(Unsupported)` and the
loader maps it to `core::Error::HotReloadRefused` per
`hot-reload-protocol.md` §Refusal Cases. The previous-good platform
plugin remains live; the operator's recourse is to retry on the next
frame boundary (the editor's reload UI typically does this
automatically). This refusal exists because dropping a
`CAMetalLayer*` mid-frame is undefined behavior in render's command
encoding — the strictest possible failure mode and the one the
protocol is built to prevent.

The other three universal cases land naturally:

- **ABI hash mismatch** — Q built against a different
  `glibre-types.dylib` than the host. Detected at step 2.1. The
  middleman types added by §8.5 are part of the hash, so any
  platform-private survival type that changes layout forces a
  rebuild rather than a silent migration.
- **Schema migration failure** — vacuous in MVP; no platform
  `.fory` schema exists today.
- **Plugin init failure** — Q's `register` returns `unexpected`,
  e.g. SDL3 fails to re-initialize because a window-server
  dependency went away. Detected at step 4.1; rollback per
  `hot-reload-protocol.md` §Failure & Rollback re-runs P's
  register against the captured middleman state, which restores
  windows and watchers to their pre-drain configuration.

### 8.5 Middleman types contributed by platform

Platform self-reload requires four pieces of plugin-side state to
survive the swap. Per the survival rule, each is declared as a
middleman type and gets a `.fory` schema living under
`data/schemas/platform/` if and when this becomes implemented (the
schemas are not part of the MVP §7 ledger because no peer context
reads them — they are loader-internal). The types:

1. `glibre::types::platform::WindowSurvival` —
   `(WindowId, WindowDesc)` per open window.
2. `glibre::types::platform::WatcherSurvival` —
   `eastl::span<CanonicalPath>` of subscription roots.
3. `glibre::types::platform::SignalSurvival` —
   `eastl::span<Signal>` of installed handlers (the function-pointer
   identity is recovered from the symbol name in Q, not stored).
4. `glibre::types::platform::FileIoSurvival` — `FileIoConfig`
   (the bounded-async budget the rebuilt pool must match).

These types are loader-visible only during phase 8; they have no
public surface in `glibre/platform/platform.hpp`. Their addition to
`glibre-types.dylib` would bump `host_glibre_types_abi_hash` once
when platform self-reload is first implemented; until then this
section is a forward-declaration and platform's contribution to the
hash remains zero (§7's net effect line preserved).

### 8.6 Observer reseat

Per `hot-reload-protocol.md` §Observer Notification, subscribers of
the `HotReloadCompleted` event are called synchronously on the
loader thread before phase 9 begins. For platform self-reload, the
specific observers that MUST reseat are:

- **Input event consumers** (any peer plugin draining
  `EventQueue<InputEvent>`): the queue object is re-vended by Q;
  consumers re-acquire the queue handle via the platform API
  rather than caching the prior pointer.
- **Window event consumers**: same pattern, against
  `EventQueue<WindowEvent>`.
- **File event consumers** (typically `content`): re-acquire the
  `FileWatcher` handle and re-issue `take_events(WatchToken, …)`
  against the new tokens vended by Q. Watch tokens **do not**
  carry numeric identity across platform self-reload (unlike
  `WindowId`); consumers receive new tokens as part of the
  reseat. This trade-off is intentional: preserving watcher token
  identity would force the loader to know the watcher's internal
  token-to-subscription map, which violates
  `hot-reload-protocol.md`'s rule that the loader needs no
  plugin-private knowledge.

The observer-reseat protocol is itself middleman-typed via the
existing `HotReloadEvent` (`hot-reload-protocol.md` §Observer
Notification, last paragraph), so no platform-private observer bus
is needed.

### 8.7 Test hooks

The replay-driven `InputDriver` used by the e2e harness
(`specs/e2e/SPEC.md`) is **frame-locked**, not handle-locked: it
synthesizes `InputEvent` values into the input queue before phase 1
of each replayed frame and never holds a long-lived reference to
the queue object. This means a platform self-reload between
recorded frames is invisible to replay — the next-frame inject runs
against whatever queue the platform plugin currently exposes. The
replay golden snapshots (`specs/e2e/SPEC.md` §6) therefore remain
deterministic across an injected platform reload, and the e2e CI
matrix can include a "reload mid-replay" scenario without changing
the input vocabulary.

The platform-specific addition to the loader's
`#if defined(GLIBRE_E2E)` test surface is one fixture function:

```cpp
namespace glibre::platform::test {

// E2E-only. Forces a platform self-reload to be requested at the
// next phase 8 by enqueuing a reload of the platform plugin against
// `replacement_dylib_path`. Returns the same ReloadRequestId the
// generic loader hook returns; the harness blocks on
// glibre::core::test::await_reload to observe completion.
ReloadRequestId enqueue_platform_reload(
    std::filesystem::path replacement_dylib_path) noexcept;

}  // namespace glibre::platform::test
```

This is a thin wrapper over `glibre::core::test::enqueue_hot_reload`
with the platform plugin's fqn baked in; it exists so the e2e
fixture set under `tests/e2e/plugins/platform/` can ship a
`platform-self-reload` variant without leaking the platform fqn
literal across test files.

## 9. Performance Budget

Frozen per-context cell (from `reviews/decisions/perf-budget.md`):

| Context  | CPU ms (sim) | CPU ms (submit) | GPU ms | Heap ceiling |
|----------|--------------|-----------------|--------|--------------|
| platform | 0.20         | 0.05            | n/a    | 16 MiB       |

Phase ownership (from `reviews/research/frame-phases.md`): **owns
phase 1 (input)** and **phase 9 (present)**. No participation in
phases 2-8 on the game-loop driver thread; off-thread work
(`FileWatcher`, `FileIo` worker) is not on the hot path and does
not draw against the cell.

This section quotes the row verbatim, decomposes the 0.20 ms sim +
0.05 ms submit + 16 MiB heap across the seven §4 aggregates, and
specifies the CI gate fixture and the per-aggregate sub-arena
discipline that core's `PerContextAllocator` enforces. The spike
that produced this section did not amend the locked cell; it
allocated within it.

### 9.1 Per-aggregate cycle budget

Steady-state cost on the game-loop driver thread under the S1
sample scene (1 character + 200 props + 8 dynamic lights at
1920x1080; see `reviews/decisions/perf-budget.md`). All times are
M1 firestorm at 3.2 GHz; cells sum to the row's totals with no
hidden slack.

| Aggregate      | Phase | CPU ms (sim) | CPU ms (submit) | Notes                                                         |
|----------------|-------|--------------|-----------------|---------------------------------------------------------------|
| `Window` / `Surface` (§4.1) | idle  | 0.000        | 0.000           | No per-frame work in steady state. Resize / DPI events are bounded by human input rate and arrive through the `Pump`; the aggregate's per-frame cost is **0**. |
| `EventQueue<T>` / `Pump` (§4.2) | 1     | 0.100        | 0.000           | One `SDL_PumpEvents` + drain loop dispatching to the two SPSC ring buffers. Dozens of events / frame max under S1, S2, S3; SIMD-bounded copy + tag dispatch. |
| `FileWatcher` (§4.3)        | -     | 0.000        | 0.000           | Runs off-main-thread (FSEvents callback). Hot-path cost on the driver thread is **0**; the engine drains via `take_events()` from the `core` hot-reload phase 8 (which is core's budget, not platform's). |
| `Clock` (§4.4)              | 1, 9  | 0.001        | 0.001           | `mach_absolute_time()` is O(1); read at frame start (sim) and at present (submit). Two reads / frame; <0.001 ms each. Listed as 0.001/0.001 to keep the cell sum honest at the precision the gate measures. |
| `Process` (§4.5)            | idle  | 0.000        | 0.000           | argv / env are read once at boot; signal handlers are async-only. **0** per-frame cost. |
| `FileIo` (§4.6)             | -     | 0.000        | 0.000           | Async path runs on a dedicated worker thread; the driver-thread cost of `IoToken::poll()` is a single non-blocking SPSC dequeue (~0.001 ms) **only on frames that have a pending I/O completion**, and is amortized into the unallocated remainder of the cell. The synchronous `read_all` (§6.9) is reserved for boot / tools and is not budgeted on the hot path; calling it from a steady-state frame is a SPEC violation. |
| Reserved       | 1, 9  | 0.099        | 0.049           | Unallocated remainder inside the cell. Absorbs `FileIo` poll spikes, SDL3 internal jitter (event-loop wakeups, drawable-acquire callbacks), and growth (e.g. additional `EventQueue<T>` families). **Not** the 1.5 ms global headroom in `perf-budget.md`; this is platform's local margin within its 0.20 + 0.05 cell. |
| **Total**      |       | **0.200**    | **0.050**       | Matches the row exactly.                                      |

The asymmetry is intentional: under S1 / S2 / S3, the only
aggregates that do non-trivial per-frame work on the driver thread
are the `Pump` (phase 1) and `Clock` (phases 1 + 9). Everything
else is either idle, off-thread, or one-shot at boot. Budgeting
zeros for them is honest; a future SPEC change that adds per-frame
driver-thread work to e.g. `Process` or `FileIo` is a perf-budget
amendment, not a silent reallocation.

Phase 9 (present) on the platform cell is **0.05 ms** wall-clock —
the drawable-acquire wait does not count against CPU because it
overlaps GPU execution of the prior frame (per `perf-budget.md` and
`frame-phases.md`); the 0.05 ms covers the SDL3 → CAMetalLayer
present call, the `CAMetalDisplayLink` callback trampoline, and
the second `Clock::wall()` read.

The hot-reload frame (S2) is allowed up to 0.40 ms in phase 8 (core's
migration budget). Platform contributes **0** to that overage:
phase 8 reads `FileWatcher::take_events()` if any reload was
triggered, which is already accounted for above as off-thread.

### 9.2 Per-aggregate heap ceiling

The 16 MiB cell is partitioned into **per-aggregate sub-arenas**
under the `platform` `ContextTag` (per `perf-budget.md` Allocator
Rule #1). Sub-arenas are constructed at boot, sized at the values
below, and never grow at runtime: an allocation that would push a
sub-arena past its size returns
`std::unexpected{platform::Error::OutOfBudget}` in strict-mode
builds (Allocator Rule #2) and logs a `warn` once-per-tag-per-frame
in shipping builds (Rule #3).

| Aggregate      | Sub-arena | Contents                                                                                  |
|----------------|-----------|-------------------------------------------------------------------------------------------|
| `Window` / `Surface` (§4.1) | 1 MiB     | `Window`, `Display` table, `Surface` handle, SDL3 window-state singleton. No per-frame growth. |
| `EventQueue<T>` / `Pump` (§4.2) | 2 MiB     | Two SPSC ring buffers — `EventQueue<InputEvent>` (1 MiB, ~32k slots @ 32 B) + `EventQueue<WindowEvent>` (1 MiB, ~16k slots @ 64 B). Sized for the largest observed burst (drag-resize spam, focus-change storms) without dropping events. |
| `FileWatcher` (§4.3)        | 4 MiB     | FSEvents callback ring + `CanonicalPath` interner + pending-event SPSC. Off-main-thread allocator hangs off the same sub-arena; the writer is the FSEvents thread, the reader is the engine's hot-reload phase 8 consumer. |
| `Clock` (§4.4)              | 4 KiB     | One `mach_timebase_info_data_t` cache. Effectively zero; bucketed into the reserved tail.  |
| `Process` (§4.5)            | 256 KiB   | argv copy + env snapshot taken at boot. Frozen after `Process::init()` returns.            |
| `FileIo` (§4.6)             | 8 MiB     | SPSC request/response rings (1 MiB each) + worker-thread scratch + `read_all` per-call arena (5 MiB ceiling, recycled per call per §6.9). The largest sub-arena because async I/O carries the most state. |
| Reserved       | ~750 KiB  | Slack inside the 16 MiB cell. Absorbs `Clock`'s rounding plus growth headroom for sub-arenas that approach their cap. |
| **Total**      | **16 MiB**| Matches the row exactly.                                                                   |

All sub-arenas are tagged `platform::ContextTag` and registered
with `core::PerContextAllocator` at boot. Cross-aggregate borrowing
is forbidden: `FileIo`'s sub-arena cannot service a `Window`
allocation, even transiently. Violations are caught in
strict-mode builds by a tag mismatch in the allocator handle.

`FileWatcher` and `FileIo`'s worker-thread allocators draw from
their own sub-arenas (above) — **not** from a separate worker-thread
budget — because a single 16 MiB ceiling per `ContextTag` is the
discipline `perf-budget.md` Rule #1 imposes, and threading topology
does not split the tag.

### 9.3 Allocation discipline (cross-reference)

Per §6.9, after construction the platform aggregates do not
allocate. §9 makes that rule a budget contract: the steady-state
allocation rate from the platform context on the driver thread is
**zero bytes per frame**. The only per-frame allocations come from
the `FileWatcher` FSEvents thread and the `FileIo` worker thread,
both of which write into their sub-arenas' free-list and are
SPSC-bounded by the corresponding ring buffer. `read_all`'s
synchronous arena is recycled per call (§6.9) and is reserved for
boot / tools, not the hot path.

The transient-arena exemption (`perf-budget.md` Allocator Rule #4)
is **not used** by platform: every per-aggregate sub-arena above is
counted against the 16 MiB ceiling. Drain-by-phase-9 is enforced
trivially by "no transient arena exists" — there is nothing to drain.

### 9.4 CI gate fixture

The platform cell is exercised by a Catch2 `BENCHMARK` block under
`platform/test/perf/` (the implementation plan files this as part
of the `task-breakdown-error-perf` follow-on per `perf-budget.md`
Consequences). Two assertions:

1. **SDL3 event-pump fixture.** A test driver synthesizes the S1
   event stream — keyboard / mouse / window-focus / DPI-change /
   display-hot-plug — at the densities observed in the sample scene
   replay (≤128 events / frame; matches the SPSC ring's worst-case
   sustained throughput). The fixture runs `Pump::drain()` for 600
   frames and asserts:
   - p50 driver-thread `Pump::drain()` time ≤ **0.10 ms** (matches
     the §9.1 cell for the `Pump`),
   - p99 ≤ **0.18 ms** (within the 0.099 ms reserved tail),
   - **zero** dropped events (the SPSC ring never overflows under
     fixture load),
   - **zero** allocations on the driver thread between
     `Pump::init()` and `Pump::shutdown()` (verified by tagged-
     allocator counter in strict mode).
2. **Idle frame budget assert.** With no events injected, the
   fixture runs a 600-frame loop calling `Clock::now()`,
   `Pump::drain()`, `Clock::wall()` (and nothing else from
   platform). Asserts:
   - p99 driver-thread platform CPU ≤ **0.005 ms** per frame (the
     idle floor: 2x `Clock` + empty `Pump::drain` short-circuit).
   - p99 platform-tagged resident heap ≤ **16 MiB**, with each
     sub-arena under its §9.2 size; verified by querying
     `core::PerContextAllocator::resident_bytes(ContextTag::Platform)`
     and the per-sub-arena introspection hook.

Both fixtures are runnable on the macOS / M1 baseline only (the
SDL3 event source and the `mach_absolute_time` clock are
host-platform-specific); the gate runs them on `macos-26-m1` CI
runners. A future Linux / Windows port reseats the fixture, not the
budget — the cell numbers are platform-agnostic.

The S1 / S2 / S3 fixtures live under `e2e/perf/` per the global
gate spec; the platform-local micro-benchmark fixture lives under
`platform/test/perf/` and stubs out everything outside the
platform aggregates so a regression can be localized to the
platform context without bisecting the full engine.

### 9.5 Refusals (out of platform's §9 scope)

- **Render / GPU costs.** GPU memory and GPU time on the platform
  surface are **render's** budget (`perf-budget.md` Allocator Rule
  #5; the row's `GPU ms = n/a`). Platform owns the
  `CAMetalLayer*` lifetime, not its content.
- **Asset import / streaming costs.** Drag-drop import (S3) is
  content's budget. Platform's role is the file-system event that
  notifies the import worker; the worker's CPU time is `content`'s
  cell, not platform's.
- **Editor / tools costs.** ImGui draws, gizmo updates, inspector
  refresh are `tools`'s 0.80 / 0.20 / 0.5 cell. Platform does not
  carry editor cost even when the editor is the only consumer of
  an event.
- **Hot-reload migration cost.** Phase 8's drain → swap → migrate
  arena is core's 16 MiB sub-budget (`perf-budget.md` Allocator
  Rule #6). Platform contributes the `FileWatcher` event that
  triggers it; the migration itself is core's responsibility.

### 9.6 Open questions — resolved

Both questions originally carried into §12 resolved in place against
already-opened external gates; §12 holds no platform-owned residue.

- **`Clock` 0.001 / 0.001 vs noise floor** (M1 / macOS 26). Resolved
  by reading the §9.1 cell as an *upper bound*: `mach_absolute_time`
  is O(1), and any actual value below the gate's measurable precision
  is absorbed by the 0.099 / 0.049 reserved tail in the same row, so
  the cell stays honest under either reading. The empirical
  distinction is pulled into the macOS 26 / M1 thermal-throttling
  measurement spike that `reviews/decisions/perf-budget.md` Open Q
  #1 already opens against the first runnable harness; no platform
  follow-up is owed independently.
- **`FileWatcher` 4 MiB sub-arena sizing.** Resolved provisionally
  at 4 MiB: holds ~32k watched canonical paths at the MVP content
  tree's average path length, which exceeds the MVP ceiling captured
  in §4.3 / §6.4. The editor / content seam spike re-derives this
  number when the editor's "watch the entire content tree" use case
  lands; until that spike opens, the provisional ceiling is frozen
  and any growth attempt is a §4.3 invariant amendment, not a silent
  reallocation.

## 10. Failure Modes & Error Model

Platform's failure surface is the closed sum `platform::Error` declared
in §4.7 and laid out in §5.1. Every public function in §5 returns
`Result<T> = std::expected<T, glibre::Error>`, and platform contributes
exactly one arm to the engine-wide variant per
`reviews/decisions/error-model.md`. §10 fills three slots that §4.7 left
implicit:

1. **Per-arm semantics** — for each variant: trigger, recovery contract,
   and log severity.
2. **OS-side translation** — how POSIX `errno`, SDL3 `SDL_GetError()`,
   and AppKit `NSException` codes map onto typed arms so no raw `int`
   leaks past the bridge.
3. **Aggregate-specific recovery shapes** — `Surface` recreation on
   `SurfaceLost`, `FileWatcher` re-arm / polling fallback on
   `WatcherUnavailable`, and the loud-but-bounded paths every other
   aggregate exposes.

§10 introduces no new public types. `SurfaceLost` and
`WatcherUnavailable` are documented *recovery situations*, not new
variants — both surface as `Error::IoFailure { OsCode }` with a known
diagnostic prefix that the §6.10 translation seam stamps. Adding either
as a first-class arm is gated by the second-consumer trigger recorded
in §10.8; doing so before two callers need to discriminate would
violate the "closed sum, deliberate central edit" rule of §4.7 inv #1.

### 10.1 Per-arm contract

Trigger / Recovery / Severity for each `platform::Error` arm. "Recovery"
names what the *caller* may do; the platform itself never auto-retries
across the boundary (§4.7 inv #2).

#### `NotFound`

- **Trigger.** A path, `WindowId`, `DisplayId`, or `WatchToken` named in
  the call does not resolve to a live OS or aggregate-internal entity.
  Examples: `FileIo::read_all` on a missing path, `Window::display()`
  after the display was unplugged but before `DisplayChanged` was
  drained, `FileWatcher::unwatch` on a stale token.
- **Recovery.** Caller-domain decision. Content / asset code surfaces
  it as a missing-asset error and falls back to the engine's default
  asset; window code re-queries `Display` snapshots after the next
  pump cycle.
- **Severity.** `info` when the platform logs it at the boundary
  (most callers handle silently); the *caller's* domain may escalate
  (e.g. render's missing shader is `error`).

#### `PermissionDenied`

- **Trigger.** OS denied an otherwise well-formed operation: sandbox
  refusal on read, lack of accessibility entitlement for a global key
  hook, code-signing rule rejecting a watched directory, App Sandbox
  refusing `~/Library` access.
- **Recovery.** Not retryable inside the same process. Caller logs at
  `error`, reports through the editor / first-launch UX, and either
  prompts the user to grant permission (editor) or aborts the
  operation (engine). Platform does not synthesize a permission
  dialog.
- **Severity.** `error` at the handling boundary. This is loud because
  it is almost always actionable by the operator (grant the
  entitlement, sign the bundle, move the file out of a protected
  directory).

#### `AlreadyExists`

- **Trigger.** A create-only operation collided with existing state:
  `Process::install_signal` for a signal that already has an engine-
  side handler (§4.5 inv #3), `IoToken::take_result` called twice
  (§5.11 contract on the token), `FileIo::write_atomic` on a path
  reserved by another aggregate.
- **Recovery.** Programming error in nearly every case — caller fixes
  the call site. The platform refuses to chain or replace silently
  because doing so would mask a duplicate-installation bug.
- **Severity.** `warn` at the handling boundary (not `error` — the
  previous installation continues to function), with the duplicated
  handle / signal name attached. CI promotes this to a build failure
  in test runs.

#### `Interrupted`

- **Trigger.** OS-level interrupt (EINTR class) reached a syscall the
  platform has not classified as auto-restartable. Most often hit by
  `IoToken::take_result` when the wait-for-completion path is
  cancelled mid-syscall, or by `FileIo::read_all` when a fatal signal
  handler fires during a long read.
- **Recovery.** Caller reissues the operation. The platform itself
  retries POSIX `EINTR` for syscalls inside the I/O thread (a single
  bounded retry — see §10.3); only interrupts that survive the
  retry surface as `Interrupted`.
- **Severity.** `debug`. Routine for any process that traps SIGINT;
  noisy at higher levels would drown legitimate failures.

#### `Unsupported`

- **Trigger.** An operation valid in shape but not on this OS / this
  hardware / with these inputs. Examples: `Window` operations from a
  non-main thread (§4.1 inv #2 in release builds), `LogicalSize` with
  a zero dimension or `DpiScale <= 0` (§4.1 inv #3), non-canonical
  path passed to `CanonicalPath::from_absolute`, fullscreen transition
  on a display that does not advertise it, gamepad rumble on a device
  without haptics.
- **Recovery.** Caller validates inputs earlier or routes around the
  capability gap (e.g. render falls back to a non-fullscreen path).
  Almost never retryable on the same inputs.
- **Severity.** `warn` at the handling boundary. The "shape valid,
  capability missing" framing means this is a configuration /
  hardware-detection signal, not a bug.

#### `IoFailure { OsCode }`

- **Trigger.** A backend call failed with an OS code that the
  translation seam (§6.10) recognized as I/O-class but did not promote
  to a finer arm. Carries `OsCode { value }` where `value` is the
  underlying `errno` / `NSError.code` / SDL3 numeric tag, *opaque to
  callers*. Also the carrier for "rare-but-real" recovery situations:
  `SurfaceLost` (§10.4) and `WatcherUnavailable` (§10.5) both surface
  as `IoFailure` with a stamped `OsCode` and a thread-local
  diagnostic prefix.
- **Recovery.** Driven by the diagnostic prefix the translator stamps
  (§6.10): "surface-lost" → recreate via `Window::surface()`;
  "watcher-unavailable" → unwatch + re-watch (or fall back to the
  polling pseudo-watcher). All other prefixes are caller-domain
  decisions. The numeric `OsCode` value is for telemetry only; engine
  code never branches on its integer.
- **Severity.** `warn` for known recoverable prefixes (surface-lost,
  watcher-unavailable, EINTR-survivor); `error` for everything else.

#### `OsCode`

- **Trigger.** Backend reported a code with no semantic mapping yet —
  the catch-all carrier when no translator entry matched and we still
  want the raw value to survive into telemetry. Used only by the
  Objective-C bridge fallback path (§10.2.3) and the SDL3 unrecognized-
  message path (§10.2.2). Engine code never constructs this directly.
- **Recovery.** Treated as terminal at the call site. Telemetry sinks
  may decode the value post-hoc; engine code does not branch on it.
  Recurring `OsCode` appearances in telemetry are a signal that a new
  semantic arm should be added in the next §4.7 edit; this is the
  same promotion gate as the `SurfaceLost` / `WatcherUnavailable`
  entries in §10.8 — triggered by a second consumer or repeated
  unmapped telemetry, not by speculation.
- **Severity.** `error`. Always. We refuse to silence what we have not
  classified.

### 10.2 OS → typed arm translation

The §6.10 seam is the only place raw OS error codes exist. Every public
boundary surfaces the typed arm, never the integer. Three translators,
one per backend family.

#### 10.2.1 POSIX `errno` → `platform::Error`

`detail/error/errno_to_error.cpp` maps `errno` families. The mapping is
table-driven, exhaustive over the families platform actually invokes,
and explicitly enumerated so audits can verify "no integer leaks":

| `errno` family                               | Typed arm                                                       |
|----------------------------------------------|-----------------------------------------------------------------|
| `ENOENT`, `ENOTDIR`, `ESRCH`                 | `NotFound`                                                      |
| `EACCES`, `EPERM`, `EROFS`                   | `PermissionDenied`                                              |
| `EEXIST`, `ENOTEMPTY` (on create-only paths) | `AlreadyExists`                                                 |
| `EINTR` (after one bounded retry)            | `Interrupted`                                                   |
| `ENOSYS`, `ENOTSUP`, `EOPNOTSUPP`, `EINVAL`  | `Unsupported`                                                   |
| `EAGAIN`, `EWOULDBLOCK` (on blocking I/O)    | `IoFailure { OsCode { errno } }` with prefix `"again"`          |
| `EIO`, `ENXIO`, `EBADF`, `ENOSPC`, `EFBIG`   | `IoFailure { OsCode { errno } }` with prefix `"io"`             |
| `EDQUOT`, `EUSERS`, `ELOOP`, `ENAMETOOLONG`  | `IoFailure { OsCode { errno } }` with prefix `"resource"`       |
| anything else                                | `OsCode { errno }` with the raw value preserved                 |

Bounded-retry rule: `EINTR` is retried *exactly once* inside the
syscall wrapper before being surfaced as `Interrupted`. We do not
loop; doing so would invite tight unbounded retry on a process under
heavy signal load. One retry catches the common "SIGCHLD landed
during read" case without hiding a legitimate fatal-signal cancel.

#### 10.2.2 SDL3 → `platform::Error`

`SDL_GetError()` returns a human prose string with no stable code. The
translator parses the string for known prefixes and falls through:

| `SDL_GetError()` prefix substring   | Typed arm                                                  |
|-------------------------------------|------------------------------------------------------------|
| `"Permission denied"`               | `PermissionDenied`                                          |
| `"No such file"` / `"not found"`    | `NotFound`                                                  |
| `"already"` / `"exists"`            | `AlreadyExists`                                             |
| `"not supported"` / `"unsupported"` | `Unsupported`                                               |
| `"interrupted"`                     | `Interrupted`                                               |
| anything else, message non-empty    | `IoFailure { OsCode { 0 } }`, message in TLS diagnostic    |
| message empty / null                | `OsCode { 0 }`, with prefix `"sdl-empty"` in TLS diagnostic |

The full prose lands in a thread-local diagnostic buffer that
`glibre::log_error` reads and includes in the structured `error.detail`
field. Engine code never inspects the prose; only the typed arm and
the optional `OsCode.value` are load-bearing.

SDL3 specific: `"Surface lost"` / `"Could not create CAMetalLayer"` /
`"Drawable is nil"` map to `IoFailure { OsCode { 0 } }` with prefix
`"surface-lost"` (see §10.4).

#### 10.2.3 AppKit `NSException` → `platform::Error`

Only the bridge translation unit (`bridge.mm`, §6.2) is allowed to
catch `NSException`. The bridge converts before returning, never
re-throws across the C++ boundary:

| `[exception name]` / class       | Typed arm                                                |
|----------------------------------|----------------------------------------------------------|
| `NSFileNoSuchFileException`      | `NotFound`                                               |
| `NSFileLockingException`         | `PermissionDenied`                                       |
| `NSInvalidArgumentException`     | `Unsupported`                                            |
| `NSRangeException`               | `Unsupported`                                            |
| any FSEvents-stream-failed code  | `IoFailure { OsCode { code } }`, prefix `"watcher-unavailable"` |
| any CALayer/Metal acquire-drawable failure | `IoFailure { OsCode { code } }`, prefix `"surface-lost"` |
| anything else                    | `OsCode { code }` (raw fallback)                         |

The bridge stamps `error.detail` with the `[exception name]` string
before returning. As with SDL3, the prose is for telemetry; engine
code branches on the typed arm only.

### 10.3 Aggregate-by-aggregate failure surface

Each §4 aggregate enumerates: which arms it can return at which entry
points, and the recovery contract specific to that aggregate. Cross-
aggregate recovery is forbidden (§4.7 inv #3) — `FileWatcher` does not
re-translate a `FileIo` error.

#### 10.3.1 `Window` / `Display` (§4.1)

| Entry point             | Returnable arms                                      | Trigger summary                                 |
|-------------------------|------------------------------------------------------|-------------------------------------------------|
| `Window::open`          | `Unsupported`, `PermissionDenied`, `IoFailure`       | bad `WindowDesc`, sandbox denial, AppKit refusal |
| `Window::surface`       | `IoFailure` (prefix `"surface-lost"`)                | drawable / layer creation failed (§10.4)        |
| `Window::request_resize`| `Unsupported`                                        | non-main-thread call (§4.1 inv #2 release path) |
| `Window::request_close` | `Unsupported`                                        | non-main-thread call                            |
| `Window::display`       | `NotFound`                                           | display unplugged, hot-plug not yet drained     |

`PermissionDenied` from `Window::open` is the macOS "Screen Recording"
or "Accessibility" entitlement absence on a window that asked for
global capture; recovery is operator-level (grant entitlement and
relaunch). The §10.4 `SurfaceLost` recovery loop is described below.

#### 10.3.2 `EventQueue<T>` / `Pump` (§4.2)

| Entry point                     | Returnable arms                                  | Trigger summary                                 |
|---------------------------------|--------------------------------------------------|-------------------------------------------------|
| `EventQueue::with_capacity`     | `Unsupported`                                    | zero / wildly oversized capacity                |
| `Pump::create`                  | `IoFailure`                                      | SDL3 init failed                                |
| `Pump::drain`                   | `IoFailure` (prefix `"queue-full"`), `Unsupported` | queue full at write site (§4.2 inv #3, fatal); off-main-thread call |

Queue-full is fatal by §4.2 inv #3: the platform refuses to drop or
coalesce. Recovery is *process-level* — log at `error`, set the exit
code, surface the fatal to the engine's frame loop. The typed arm is
`IoFailure { OsCode { 0 } }` with prefix `"queue-full"` so telemetry
keeps the misconfiguration visible.

#### 10.3.3 `FileWatcher` (§4.3)

| Entry point             | Returnable arms                                      | Trigger summary                                 |
|-------------------------|------------------------------------------------------|-------------------------------------------------|
| `FileWatcher::create`   | `IoFailure`, `PermissionDenied`                      | FSEvents init failed, sandbox denial            |
| `FileWatcher::watch`    | `Unsupported`, `NotFound`, `PermissionDenied`, `IoFailure` (prefix `"watcher-unavailable"`) | non-canonical path, missing root, sandbox denial, FSEvents refused stream |
| `FileWatcher::unwatch`  | `NotFound`                                           | stale `WatchToken`                              |
| `FileWatcher::take_events` | `NotFound`, `Unsupported`                         | stale token, off-main-thread call               |

`WatcherUnavailable` recovery is described in §10.5.

#### 10.3.4 `Clock` (§4.4)

`Clock::now`, `wall`, `native_tick` are total functions returning plain
values; they cannot fail. The only failure mode in §4.4 is the
monotonic-regression abort (`Clock` aborts the process on detected
non-monotonic behavior), which never returns at all. No `Result<T>`
crosses this boundary. Listed here for completeness so the audit
matches §5.

#### 10.3.5 `Process` (§4.5)

| Entry point                       | Returnable arms                              | Trigger summary                                 |
|-----------------------------------|----------------------------------------------|-------------------------------------------------|
| `Process::install_signal`         | `AlreadyExists`, `Unsupported`               | duplicate handler (§4.5 inv #3); signal not installable on this OS |
| `Process::uninstall_signal`       | `NotFound`                                   | no handler currently installed for that signal  |

`argv`, `env`, `cwd`, `executable_path`, `pid`, `set_exit_code` are
total per §4.5 inv #1 (read-only snapshots) and inv #2 (set-only).
Second-instance construction of `Process` is a programming error and
the aggregate refuses to build (§4.5 inv #5) — this is a build-time /
init-time refusal, not a runtime arm.

#### 10.3.6 `FileIo` / `IoToken` (§4.6)

| Entry point                          | Returnable arms                                                                | Trigger summary                                 |
|--------------------------------------|--------------------------------------------------------------------------------|-------------------------------------------------|
| `FileIo::create`                     | `Unsupported`, `IoFailure`                                                     | invalid `FileIoConfig`, thread spawn failure    |
| `FileIo::read_all`                   | `NotFound`, `PermissionDenied`, `Interrupted`, `IoFailure`, `Unsupported`      | per §10.2.1; off-main-thread assert in debug   |
| `FileIo::write_atomic`               | `NotFound`, `PermissionDenied`, `AlreadyExists`, `IoFailure`, `Unsupported`    | atomic-rename failed, target reserved           |
| `FileIo::stat_path`                  | `NotFound`, `PermissionDenied`, `IoFailure`                                    | per §10.2.1                                     |
| `FileIo::list_dir`                   | `NotFound`, `PermissionDenied`, `Unsupported`, `IoFailure`                     | output span too small → `Unsupported`           |
| `FileIo::remove`                     | `NotFound`, `PermissionDenied`, `IoFailure`                                    | per §10.2.1                                     |
| `FileIo::read_async`                 | `NotFound`, `PermissionDenied`, `IoFailure` (prefix `"out-of-budget"`)         | I/O thread pool saturated (§4.6 inv #6)         |
| `FileIo::write_atomic_async`         | as `read_async`                                                                | as `read_async`                                 |
| `IoToken::poll`, `wait_for`, `cancel`| total / void; no `Result<T>`                                                   | —                                               |
| `IoToken::take_result`               | `Interrupted` (called before Ready), `AlreadyExists` (called twice), `IoFailure` | per §5.11 contract                              |

The `out-of-budget` prefix is the dedicated marker for §4.6 inv #6;
recovery is caller-domain (back off, retry next frame, or batch).

### 10.4 Aggregate-specific recovery: `SurfaceLost` (Window / Surface)

`Window::surface()` may return `IoFailure` with the diagnostic prefix
`"surface-lost"` even on macOS, where it is rare-but-real. Triggers we
have observed or expect:

- The owning `CAMetalLayer` was invalidated by a display reconfiguration
  (external monitor unplug → main, GPU switch on dual-GPU laptops, OS
  display sleep → wake under aggressive power management).
- A drawable acquire returned `nil` because the layer was unhooked from
  its `NSWindow` in the same frame as a fullscreen transition.
- An OS-level GPU reset (`MTLCommandBuffer` status `Error` with
  `MTLCommandBufferErrorDeviceRemoved`) bubbled up through the bridge
  before render had a chance to handle it via its own
  `render::Error::DeviceLost` arm.

Recovery contract:

1. Caller (render, typically) catches `IoFailure` with prefix
   `"surface-lost"`.
2. Caller releases its render-side references to the `Surface` value.
3. Caller calls `Window::surface()` again on the same `Window`. The
   aggregate re-acquires the `CAMetalLayer` (the bridge re-binds via
   the SDL3 metal-view API).
4. If the second call also returns `surface-lost`, the failure is
   escalated: caller logs `error`, asks `Window` to recreate via the
   render context's window-recreation path (re-`Window::open` with the
   same `WindowDesc`, transferring focus / position from the old
   handle).
5. The platform never auto-recreates the `Window`. Doing so would
   violate "errors are constructed at the site they happen" (§4.7
   inv #3) and would race with any peer-context state still keyed to
   the old `WindowId`.

Severity: `warn` for the first occurrence in a session; `error` if it
recurs within 1 s of a successful recreation (suggests the system is
in a thrash state and the editor / engine should surface a user-
visible message).

`SurfaceLost` is not a §4.7 arm today by deliberate Occam's-razor
choice: only one consumer (render) currently discriminates, and it
does so on the diagnostic prefix. Promotion to a typed arm is on the
§12 watch-list and trips when a second consumer needs the
discrimination.

### 10.5 Aggregate-specific recovery: `WatcherUnavailable` (FileWatcher)

`FileWatcher::watch` may return `IoFailure` with the diagnostic prefix
`"watcher-unavailable"`. Macros / SDL3 / FSEvents specific triggers:

- The watched root is on a volume that was unmounted or remounted
  (FSEvents streams die on volume change; the kernel drops the watch
  silently and the next event delivery surfaces the failure).
- FSEvents service exhaustion: too many concurrent streams system-
  wide, or the per-process resource budget hit.
- The watched root crossed a network-mount / sparse-bundle boundary
  that FSEvents cannot observe (some SMB / WebDAV / disk-image mounts
  refuse stream registration).
- Sandbox / TCC denial mid-stream (rare; usually surfaces as
  `PermissionDenied` at `watch()` time, but a re-grant flip can drop
  the stream after the fact).

Recovery contract:

1. Caller catches `IoFailure` with prefix `"watcher-unavailable"`.
2. Caller calls `FileWatcher::unwatch(token)` to release any partial
   subscription. (`unwatch` on an already-dead token returns
   `NotFound` — this is fine; ignore it.)
3. Caller waits one full pump cycle (lets any in-flight `FileEvent`
   drain through `EventQueue<FileEvent>`) and calls
   `FileWatcher::watch(root)` again. This re-arms FSEvents from a
   fresh state and resolves the volume-change case in the common path.
4. If re-arm fails twice in a row, the caller falls back to the
   polling pseudo-watcher: a periodic `FileIo::stat_path` /
   `FileIo::list_dir` sweep over the previously watched root, with a
   coarse interval (default 1 s, callable knob; re-opened by the
   editor hot-reload coordinator spike per §10.8). Polling
   surfaces the same `FileEvent` sum (`Created` / `Modified` /
   `Deleted` / `Renamed`) so consumers do not branch.
5. The polling fallback is owned by the *caller*, not the platform:
   `FileWatcher` is the FSEvents seam; the polling shim lives in the
   editor / hot-reload coordinator that already holds the long-lived
   subscription. Building polling into the aggregate would re-merge
   the responsibilities §4.3 SRP keeps apart.

Severity: `warn` on the first re-arm attempt; `error` when polling
fallback engages (operator-visible: "live reload is degraded").

`WatcherUnavailable`, like `SurfaceLost`, is not a §4.7 arm today by
the same Occam's-razor argument: one consumer (hot-reload coordinator)
discriminates today on the diagnostic prefix. §12 watch-list item.

### 10.6 Logging severity table (consolidated)

The §10.1 per-arm severities, restated as the table the
`glibre::log_error` helper uses when it formats a `platform::Error`
into `spdlog`:

| Arm                                    | Default severity | Notes                                            |
|----------------------------------------|------------------|--------------------------------------------------|
| `NotFound`                             | `info`           | Caller's domain may escalate                     |
| `PermissionDenied`                     | `error`          | Almost always operator-actionable                |
| `AlreadyExists`                        | `warn`           | CI promotes to build failure                     |
| `Interrupted`                          | `debug`          | Routine on SIGINT                                |
| `Unsupported`                          | `warn`           | Capability gap signal                            |
| `IoFailure` prefix `"surface-lost"`    | `warn`           | Escalates to `error` on rapid recurrence (§10.4) |
| `IoFailure` prefix `"watcher-unavailable"` | `warn`       | Escalates to `error` when polling engages (§10.5) |
| `IoFailure` prefix `"queue-full"`      | `error`          | §4.2 inv #3 fatal misconfiguration               |
| `IoFailure` prefix `"out-of-budget"`   | `warn`           | I/O pool saturation; backpressure signal         |
| `IoFailure` other                      | `error`          | Default for unclassified I/O failure             |
| `OsCode`                               | `error`          | Always — we refuse to silence the unclassified   |

The platform never logs at the *raise* site; logging is the *handler's*
responsibility per `reviews/decisions/error-model.md` §"Logging /
Telemetry" rule 1. The platform's contribution is the typed arm + the
TLS-buffered diagnostic prefix; the engine-wide log helper does the
formatting and dispatches to spdlog.

### 10.7 Refusals (out of §10 scope)

- **Render / GPU error model.** `MTL::Device` lost / pipeline compile
  failure / residency exceeded live in `specs/render/SPEC.md` §10 as
  `render::Error` arms. Platform's role stops at surfacing the host
  Window / Surface state through `IoFailure` prefix `"surface-lost"`
  (§10.4); the render context maps that to its own `DeviceLost` arm
  at the call site (per error-model composition rule 2).
- **Hot-reload refusal.** `core::Error::PluginAbiHashMismatch`,
  `PluginInitFailed`, etc., are owned by `specs/core/SPEC.md`.
  Platform contributes nothing new here.
- **Schema / persistence failure.** `data::Error` (Fory schema
  migration, etc.) is owned by `specs/data/SPEC.md`. Platform paths
  do not carry persisted state (§7) and therefore do not surface
  schema errors.
- **Determinism / replay divergence.** Engine concern, not platform.
- **Crash dump format.** Owned by the future `obs` (observability)
  context; platform's contribution stops at `Process::install_signal`
  and the `WallTime` correlation rule (§4.4 inv #3).

### 10.8 Open questions — resolved

The four questions originally carried into §12 each resolve to an
existing external gate or to an answer frozen until a known trigger;
§12 holds no platform-owned residue. Each entry below names the gate
that re-opens it, so a future change does not need to re-derive the
deferral.

- **Promote `"surface-lost"` to a first-class `SurfaceLost` arm.**
  Frozen as the diagnostic prefix until a *second* consumer beyond
  render needs to discriminate; per principle 10 (Occam's razor), one
  consumer does not justify a sealed-sum slot. Trigger to re-open:
  any new domain that wants to branch on surface loss without parsing
  the prefix string. Until then the §4.7 closed sum stays small.
- **Promote `"watcher-unavailable"` to a first-class
  `WatcherUnavailable` arm.** Same shape as the `SurfaceLost`
  question: frozen as the diagnostic prefix until polling-fallback
  ownership moves to a consumer that benefits from typed dispatch.
  Expected trigger is the editor's content-tree hot-reload coordinator
  landing, at which point the editor / content seam spike that re-
  derives §10.5's polling discipline also evaluates the promotion.
- **`magic_enum` vs hand-written `to_string` for arm names.** Owned
  by `core/error.hpp` per `reviews/decisions/error-model.md` Open Q
  #1; platform follows whatever core picks. No platform-side residue.
- **Polling-fallback interval default for §10.5.** Frozen at the
  provisional 1 s. Re-opened by the editor hot-reload coordinator
  spike (same gate as the `WatcherUnavailable` promotion above), so
  both questions resolve together when that work lands rather than
  drifting independently.

## 11. Acceptance Criteria

GitHub `type:user-story` issues this spec closes (drafted under spike
#46; each carries a Catch2 test name plus the story-required E2E
`.glibre-trace`):

- #349 — platform/window: open, resize, close lifecycle on macOS (§4.1, §9.1) — pts:3
- #350 — platform/window: LogicalSize / PhysicalSize / DpiScale invariant (§4.1 inv #3, #4) — pts:3
- #351 — platform/surface: SDL3 → CAMetalLayer bridge with strict Window lifetime (§4.1 inv #1, #6) — pts:5
- #352 — platform/pump: drain once per frame, main-thread-only, exactly-once delivery (§4.2 inv #1, #2, #3, #5) — pts:3
- #353 — platform/eventqueue: SPSC ring sized at construction, no runtime growth (§4.2 inv #6, §9.2) — pts:2
- #354 — platform/input: closed InputEvent variant for keyboard/mouse/gamepad/text (§4.2 sealed sum) — pts:3
- #355 — platform/window-events: CloseRequested + DpiChanged never coalesced (§4.2 inv #4) — pts:2
- #356 — platform/file-watcher: canonical paths + content-hash dedup + atomic renames (§4.3 inv #1, #2, #3, #6) — pts:5
- #357 — platform/file-watcher: off-main-thread, recursive subscription, RAII teardown (§4.3 inv #4, #5; §8.1) — pts:3
- #358 — platform/clock: monotonic non-decreasing + wall correlation (§4.4 inv #1, #2, #3, #4) — pts:2
- #359 — platform/process: argv/env/cwd snapshots + signal install + exit code (§4.5 inv #1, #2, #3, #5) — pts:3
- #360 — platform/fileio: IoToken bounded-async with poll/cancel + atomic write (§4.6 inv #3, #4, #6, #7) — pts:5
- #361 — platform/fileio: CanonicalPath-only public surface, never blocks main thread (§4.6 inv #1, #2) — pts:2

Total leaf rollup: **41 pts** across 13 stories. Each issue body
mirrors `.github/ISSUE_TEMPLATE/user-story.yml` (Domain, Phase,
Persona, User Story, Acceptance Criteria in Gherkin, Manual Test
Script, E2E Test Plan, Closure Checklist, Harmonius Source,
Notes / Open Questions). Per AGENTS.md the closure rule for every
story above is: E2E green in CI **before** manual testing begins;
PASS recorded as a comment; both gates required for close.

## 12. Open Questions

None. The two carry-ins in §9.6 and the four carry-ins in §10.8 each
resolved in place against existing external gates — see those sub-
sections for the trigger that re-opens each one. Per spike #47.
