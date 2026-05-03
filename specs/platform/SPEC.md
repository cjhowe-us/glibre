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

`InputEvent` is a closed `std::variant` over: `KeyDown`, `KeyUp`,
`MouseMove`, `MouseButton`, `Wheel`, `TextInput`, `GamepadAxis`,
`GamepadButton`. `WindowEvent` is a closed `std::variant` over:
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

`FileEvent` is a closed `std::variant` over: `Created`, `Modified`,
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
   once at startup and exposed as `std::span<const std::string_view>`
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
