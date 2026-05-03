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

Harmonius requirement IDs / file paths cited as research input. Note any
collapse decisions (multiple harmonius concepts → one glibre primitive).

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
