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

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

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
// Variant-of-tags: `Error` is a `std::variant` so the
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

using Error = std::variant<
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
    [[nodiscard]] static auto from_absolute(std::string_view utf8_abs) noexcept
        -> Result<CanonicalPath>;

    [[nodiscard]] auto view() const noexcept -> std::string_view { return view_; }

    constexpr bool operator==(const CanonicalPath&) const noexcept = default;

private:
    constexpr explicit CanonicalPath(std::string_view v) noexcept : view_{v} {}
    std::string_view view_{};  // backed by an internal arena owned by platform.
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
    [[nodiscard]] auto drain(std::span<T> out) noexcept -> std::size_t;

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
struct TextInput    { std::array<char, 32> utf8; std::uint8_t length; };  // §3 collapse: IME commit lives here.
struct GamepadAxisEv{ std::uint8_t device; GamepadAxis axis; float value; };  // value in [-1, 1].
struct GamepadBtnEv { std::uint8_t device; GamepadBtn button; bool pressed; };

}  // namespace input

using InputEvent = std::variant<
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

using WindowEvent = std::variant<
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
    std::string_view title{};
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

using FileEvent = std::variant<
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
    [[nodiscard]] auto take_events(WatchToken, std::span<FileEvent> out) noexcept
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

    [[nodiscard]] auto argv()             const noexcept -> std::span<const std::string_view>;
    [[nodiscard]] auto env(std::string_view name) const noexcept -> std::optional<std::string_view>;
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
    [[nodiscard]] auto take_result() noexcept -> Result<std::span<const std::byte>>;

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
    [[nodiscard]] auto read_all(CanonicalPath) noexcept                                 -> Result<std::span<const std::byte>>;
    [[nodiscard]] auto write_atomic(CanonicalPath, std::span<const std::byte>) noexcept -> Result<void>;  // §4.6 inv #4.
    [[nodiscard]] auto stat_path(CanonicalPath) noexcept                                -> Result<Stat>;
    [[nodiscard]] auto list_dir(CanonicalPath, std::span<DirEntry> out) noexcept        -> Result<std::size_t>;
    [[nodiscard]] auto remove(CanonicalPath) noexcept                                   -> Result<void>;

    // Bounded-async primitives. Poll-only; never callbacks (§4.6 inv #7).
    [[nodiscard]] auto read_async(CanonicalPath) noexcept                                     -> Result<IoToken>;
    [[nodiscard]] auto write_atomic_async(CanonicalPath, std::span<const std::byte>) noexcept -> Result<IoToken>;

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
    -> Result<std::pair<PhysicalSize, DpiScale>>;

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
    [[nodiscard]] virtual auto poll_raw(std::span<RawEvent> out) noexcept
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
  - `std::span<const std::byte>` result_bytes (filled before state
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
allocated buffer as `std::span<const std::byte>` and the platform
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
| `FileWatcher`                      | Yes — opaque pass-through  | Subscription list (`std::span<CanonicalPath>`) preserved; native fds re-acquired and replayed as fresh `Created` events. | Empty in plugin memory; fresh-event replay is a Q::register responsibility, not a typed migration. |
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
     `std::span<CanonicalPath>` into a middleman-typed singleton
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
   `std::span<CanonicalPath>` of subscription roots.
3. `glibre::types::platform::SignalSurvival` —
   `std::span<Signal>` of installed handlers (the function-pointer
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

Cycles / frame, memory ceiling, allocation rules.

## 10. Failure Modes & Error Model

Typed errors. Recovery.

## 11. Acceptance Criteria

GitHub `type:user-story` issues this spec closes:

- #TBD — `<title>`

Each must have a Catch2 test by name.

## 12. Open Questions

- Owner / resolution gate.
