# E2E Spec

## 1. Purpose

The `e2e` context owns one responsibility: proving — frame-deterministically
and in CI — that a `type:user-story` issue's acceptance criteria hold against
the running engine + editor binaries. That is the closure gate before any
human manual test runs (per `AGENTS.md` §User Story closure). Concretely,
e2e owns the `.glibre-trace` file format (recorded `InputEvent` stream
indexed by frame number, interleaved assertion ops, environment manifest),
the **replay driver** that satisfies `platform`'s `InputDriver` seam by
emitting recorded events at their exact recorded frame indices instead of
draining SDL3, the assertion vocabulary executed against the live process
(`assert_state`, `assert_screenshot` against a golden image, `assert_ecs_snapshot`
byte-equal against a Fory blob, `assert_log_contains`), the in-process
trace runner that drives the engine through one frame loop until trace
end, the per-PID / per-window injection bridge for editor traces that
need real OS event delivery without disrupting the interactive desktop
(macOS `CGEventPostToPid`, Windows `PostMessage`, Linux `xdotool --window`),
the optional full-OS-automation harness restricted to isolated CI runners,
the golden-image store + tolerance policy + diff publication for visual
assertions, and the trace-author tool the editor exposes for capturing
new traces. The collapse rule from `PHILOSOPHY.md` applies: harmonius
fractured "input replay", "automation scripts", "headless editor API",
"screenshot capture", "deterministic replay" across `tools`, `ai-assistant`,
`networking/replay-system`, and per-domain verification clauses; glibre
fuses them into one primitive whose only justification for existence is
"a user-story is not closeable until its acceptance criteria are
mechanically re-provable on every commit". E2E **refuses** to own: the
features under test (each domain owns its own logic; e2e only observes
inputs and outputs), Catch2 unit tests (those live with their plan and
are scoped below the public interface — e2e operates on whole binaries),
manual test scripts (those live in the user-story issue body and are
executed by humans during QA), the `InputDriver` seam itself (defined by
`platform`; e2e supplies the replay implementation but does not own the
abstraction), the recording UI and `.glibre-trace` writer (those live in
`tools` — record-mode is an editor capability; e2e is the consumer of
its output), perf benchmarking (a sibling `benchmarks` context owns micro
+ frame-time regressions; e2e asserts behavioural equivalence, not speed),
runtime asset cooking (cooked artefacts must already exist on disk before
a trace plays — e2e refuses to bake), gameplay-side networked replay
(`networking/replay-system` mirrors world state for spectating; e2e
replays *inputs* against a single deterministic process), and render
correctness verification beyond pixel-tolerance image diffing against a
golden (deeper render verification belongs to `render`'s own GPU
validation suite). Per SRP every reason e2e has to change must trace back
to one of those listed responsibilities; anything else routes to the
owning context.

## 2. Ubiquitous Language

Terms used unchanged in code (identifiers, file names, comments).

| Term | Meaning |
|------|---------|
| `Trace` | One `.glibre-trace` file: header + manifest + ordered `(frame_index, op)` stream + footer hash. The unit of replayable evidence for an acceptance criterion. |
| `TraceFile` | On-disk representation of a `Trace`; canonical extension `.glibre-trace`; binary Fory-encoded; checked into `tests/e2e/<ctx>/`. |
| `FrameIndex` | Monotonically increasing 64-bit frame counter started at 0 at trace begin; the only time axis e2e recognises. Wall-clock is never consulted. |
| `TraceOp` | Sum of every operation a trace can emit at a given `FrameIndex`: `Input`, `AssertState`, `AssertScreenshot`, `AssertEcsSnapshot`, `AssertLogContains`, `End`. |
| `InputOp` | A `TraceOp` carrying one `platform::InputEvent` payload to be re-emitted by the replay driver on the recorded frame. |
| `AssertOp` | Any non-`Input` `TraceOp`; failure aborts the run with a typed `E2eError` and exit code that fails CI. |
| `AssertState` | Predicate over a named ECS resource or component value at the recorded frame; encodes (path, expected-Fory-blob) pairs. |
| `AssertScreenshot` | Capture a swapchain readback at the recorded frame and compare to a `GoldenImage` using `PixelTolerance`. |
| `AssertEcsSnapshot` | Serialise the named `World` (or sub-aggregate) via Fory and require byte-equal match against a stored snapshot file. |
| `AssertLogContains` | Require a substring (or regex) to have appeared in the structured log channel between the previous assert and this frame. |
| `GoldenImage` | Reference PNG checked into `tests/e2e/<ctx>/golden/`; addressed by trace path + assert id; updated only via the explicit `golden-update` workflow. |
| `PixelTolerance` | Per-`AssertScreenshot` policy: max per-pixel ΔE, max % differing pixels, optional region mask. Default tier set engine-wide. |
| `TraceManifest` | Header section recording engine version, plugin set + ABI hash, asset-pack hash, locale, window size, DPI, RNG seed, and target driver tier. |
| `EnvHash` | Stable hash over `TraceManifest`; mismatch on replay aborts with `EnvDrift` rather than producing a misleading green/red. |
| `ReplayDriver` | The `platform::InputDriver` implementation that reads a `Trace` and yields each `InputOp` at its `FrameIndex`. The default trace runner injection layer. |
| `RealDriver` | The non-replay `InputDriver` implementation backed by SDL3; named here only to define what `ReplayDriver` substitutes for. Owned by `platform`. |
| `TraceRunner` | The host process driving one trace to completion: launches the binary under test, installs the `ReplayDriver`, advances frames, executes asserts, reports. |
| `InjectionLayer` | The mechanism the runner uses to deliver inputs. One of `InProcess`, `PerProcess`, `OsAutomation`. |
| `InProcess` | Default layer: `ReplayDriver` is linked into the engine binary; no OS event delivery occurs. Used for headless and CI. |
| `PerProcess` | Non-disruptive layer: events delivered to a single PID/window via `CGEventPostToPid` (macOS), `PostMessage` (Windows), or `xdotool --window` (Linux). For interactive editor traces. |
| `OsAutomation` | Full desktop control layer (synthesised global mouse/keyboard); restricted to isolated CI runners by policy; refused on developer hosts. |
| `RunnerHost` | Tag describing where a trace runs: `dev-headless`, `dev-interactive`, `ci-headless`, `ci-isolated`. Drives which `InjectionLayer`s are permitted. |
| `TraceWriter` | Editor-side tool that captures a live session into a `.glibre-trace` file; consumed here only by reference — its implementation lives in `tools`. |
| `GoldenStore` | The on-disk + git-tracked corpus of `GoldenImage` and `EcsSnapshot` reference blobs; addressed by trace-relative path. |
| `EcsSnapshot` | A Fory-encoded serialisation of a `World` (or sub-aggregate) used both as input baseline and as `AssertEcsSnapshot` reference. |
| `TraceReport` | Structured per-run output: pass/fail, failing assert id, captured artefacts (diff image, snapshot diff), wall-clock duration, frame count, `EnvHash`. |
| `DivergenceReport` | Specialised `TraceReport` produced when two replays of the same trace diverge — names the first differing `FrameIndex` and op. |
| `E2eError` | Closed sum of typed failures (`TraceParse`, `EnvDrift`, `AssertFailed`, `DriverInstall`, `InjectionRefused`, `GoldenMissing`, `Timeout`, `BinaryCrash`). No exceptions cross the boundary. |
| `ClosureGate` | The CI step that flips a user-story from "in progress" to "QA-ready" once its referenced traces report green. Required before any manual PASS comment. |

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
