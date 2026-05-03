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

Harmonius is unreliable prior art — every conclusion below was
independently re-derived per `PHILOSOPHY.md`. The following harmonius
files were consulted as research input only.

### Citations (research input)

Requirements:

- `docs/requirements/cross-cutting.md` — R-X.5.1 (cross-platform
  bit-identical physics simulation given identical initial state +
  input sequence; the precondition for `Trace` re-provability), R-X.5.2
  (per-system seeded RNG streams logged in the session record; the
  precondition for `EnvHash` over `RngSeed`), R-X.6.1 (asset hot-reload
  applied only at sync points between frames; the precondition for
  `FrameIndex` being the only time axis e2e recognises), R-X.7.1
  (explicit serialised-vs-reconstructed scope; the basis for resolving
  `AssetHandle`s underneath an `AssertEcsSnapshot`), R-X.8.1
  (cross-platform parity matrix; the basis for `RunnerHost` tagging).
- `docs/requirements/tools/ai-assistant.md` — R-15.9.6 (headless
  editor API with UI automation primitives, support for concurrent
  isolated agents, CI/CD integration; the basis for `InProcess`
  injection running in `dev-headless` and `ci-headless` `RunnerHost`s),
  R-15.9.7 (screenshot capture via `ScreenCaptureKit` /
  `DXGI` / `PipeWire`; cited only to confirm refusal — e2e takes a
  swapchain readback instead of capturing the OS screen).
- `docs/requirements/tools/editor-framework.md` — R-15.1.10
  (non-linear undo tree; cited only to confirm refusal — e2e is
  *outside* the editor and asserts against the running binary,
  not against undo history), R-15.1.11 (editor state in a separate
  ECS world, `EventBridge` to game world; the upstream guarantee
  that `EditorWorld` mutations do not perturb `GameWorld` snapshots
  a trace asserts against), R-15.1.12 (logic-graph-based editor
  extension for automation scripts; the `tools` author-time surface
  that **collapses into** `TraceWriter` + `.glibre-trace` consumed
  here).
- `docs/requirements/networking/replay-system.md` — R-8.6.1 (record
  full snapshots interleaved with per-tick deltas), R-8.6.2 (replay
  recorded state deterministically by feeding snapshots and deltas
  into the simulation, reproducing exact visual result with frame
  checksums at sample points), R-8.6.3 (seek to any point in a
  replay by loading the nearest snapshot keyframe and replaying
  deltas forward; cited only to confirm refusal — e2e replays
  *inputs*, not state-deltas, and the seek primitive is not in
  scope).

Designs:

- `docs/design/networking/network-services.md` — `harmonius_net::replay`
  module layout (snapshots/, deltas/, killcam.rs); cited only to
  confirm refusal — the gameplay-side networked replay subsystem is
  not e2e and does not own `.glibre-trace`.
- `docs/design/tools/editor-core.md` — MCP server `get_screenshot()`,
  `list_entities`, `get_component`, `set_component`, `spawn_entity`,
  `run_validation` tools (lines 1276–1295); cited as the upstream
  shape of headless inspection, **collapsed** here into the
  trace-runner's in-process inspection loop driven by `AssertOp`s
  rather than an MCP RPC surface.
- `docs/design/rendering/render-pipeline.md` — `test_cross_backend_image_diff`
  test name (line 990) and `docs/design/rendering/render-pipeline-test-cases.md`
  (line 273) "compare hash of final framebuffer; assert pixel-identical
  within tolerance"; the basis for `AssertScreenshot` + `PixelTolerance`
  + `GoldenImage`.
- `docs/design/rendering/render-effects-test-cases.md` (line 305) —
  "golden image; assert PSNR > 40 dB"; cited as the upstream
  precedent for golden-image storage and PSNR-based tolerance —
  **collapsed** into the engine-wide `PixelTolerance` policy
  (max ΔE + max % differing pixels + optional region mask) rather
  than a per-test PSNR threshold.

### Occam collapses (multiple harmonius concepts → one glibre primitive)

Harmonius fractured the responsibility "prove that a story's
acceptance criteria mechanically hold against a built binary"
across four unrelated surfaces. Glibre fuses them into one
`e2e` primitive — `.glibre-trace` driven by `ReplayDriver`
through one of three `InjectionLayer`s — and refuses every
adjacent concern that a sibling context already owns.

- **`tools/editor-framework` automation-script extension
  (R-15.1.12) + `tools/ai-assistant` headless editor API
  (R-15.9.6) + `tools/editor-core` MCP `get_screenshot` /
  `list_entities` / `get_component` tool surface +
  `networking/replay-system` snapshot-and-delta recorder (R-8.6.1,
  R-8.6.2) → one `InputDriver`-shaped `ReplayDriver` reading
  one `.glibre-trace` file.** Harmonius scattered four
  unconnected ways to drive the engine without a human — a
  visual-graph automation extension hosted *inside* the editor,
  a separate headless-only API host targeted at AI agents, an
  MCP server exposing fine-grained inspection RPCs to external
  clients, and a snapshot-stream replay subsystem in the
  networking module. None of them composed: the editor
  automation graph could not exercise a built game binary, the
  MCP host required an editor process, the headless API
  duplicated the editor's command vocabulary, and the
  networking replay system replayed *world state*, not
  *inputs*. Glibre collapses all four into a single seam: the
  E2E runner satisfies the existing `platform::InputDriver`
  abstraction with a `ReplayDriver` that reads one
  `.glibre-trace` file and emits its recorded `InputEvent`s at
  their recorded `FrameIndex`. Authoring is owned by `tools`
  (its `TraceRecorder`, the editor-side capture); consumption
  is owned here. There is exactly one record format, one
  driver shape, one process target — a built binary running
  one frame loop — and exactly one closure-gate semantics:
  the trace ran green. Refusing the four-way split is the
  load-bearing decision of this context.
- **Four harmonius input-injection surfaces (in-editor logic-graph
  automation, headless API, full OS automation via screen-capture
  APIs, MCP RPC) → three explicit `InjectionLayer`s
  (`InProcess`, `PerProcess`, `OsAutomation`) gated by
  `RunnerHost`.** The injection mechanism is not free: each
  layer trades off coverage against host disruption. Glibre
  enumerates the three meaningful tiers explicitly and ties
  each to the `RunnerHost`s where it is permitted. `InProcess`
  links the `ReplayDriver` directly into the binary and is the
  default for `dev-headless` and `ci-headless`. `PerProcess`
  routes events to one PID/window via `CGEventPostToPid`
  (macOS), `PostMessage` (Windows), or `xdotool --window`
  (Linux) — the only layer that can drive the *interactive*
  editor without taking over the developer's desktop, and the
  only viable layer for `dev-interactive`. `OsAutomation`
  (synthesised global mouse/keyboard) is restricted to
  `ci-isolated` runners by policy and refused on developer
  hosts with `InjectionRefused`. The harmonius
  `screen-capture-driven AI control` clause (R-15.9.7) is
  refused outright — full-desktop control on developer
  workstations is a category error for E2E.
- **Cross-platform physics determinism (R-X.5.1) + per-system
  seeded RNG streams (R-X.5.2) + sync-point hot reload
  (R-X.6.1) + serialised-vs-reconstructed scope (R-X.7.1) +
  parity matrix (R-X.8.1) → one `EnvHash` over one
  `TraceManifest` and one fail-fast `EnvDrift`.** Harmonius
  documented these as five disjoint cross-cutting clauses,
  each verified by its own integration test. They share one
  invariant: a deterministic replay is meaningful only when
  every input that can perturb the simulation matches between
  record and replay. Glibre folds engine version, plugin set
  + ABI hash, asset-pack hash, locale, window size, DPI, RNG
  seed, and target driver tier into a single `TraceManifest`,
  hashes it to one `EnvHash`, and refuses to run a trace whose
  recorded `EnvHash` disagrees with the live environment —
  abort with `EnvDrift` rather than producing a misleading
  green or red. One hash, one refusal site, one diagnosis.
- **Reference-image testing scattered across `render-pipeline`
  (R-2.1.1 framebuffer hash within tolerance), `render-effects`
  (PSNR > 40 dB), `2d` (1% pixel diff), `world-geometry` (1px
  tolerance), `ui-framework` (1 ULP tolerance) → one
  `PixelTolerance` policy + one `GoldenStore` + one
  `golden-update` workflow.** Five harmonius render-suites
  invented five different tolerance metrics and five different
  storage conventions for reference images. Glibre fuses them
  into one engine-wide policy tier set: per-`AssertScreenshot`
  `PixelTolerance` carrying max per-pixel ΔE, max % differing
  pixels, optional region mask. References live in one
  `GoldenStore` keyed by trace path + assert id. Updates land
  through one explicit `golden-update` workflow — never
  silently. The deeper render-correctness suites
  (PSNR-on-effects, ULP-on-tessellation, cross-backend
  framebuffer-hash) remain owned by `render`'s own GPU
  validation; e2e's image-diff is the *behavioural* gate, not
  the rendering-correctness gate.

### Refusals (routed to peer contexts, not e2e)

- **The features under test.** Each domain owns its own logic;
  e2e only observes inputs and outputs of a built binary.
- **Catch2 unit tests.** Owned by each plan; scoped below the
  public interface. E2e operates on whole binaries, not on
  module-internal seams.
- **Manual test scripts.** Owned by the `type:user-story` issue
  body; executed by humans during the QA stage per `AGENTS.md`.
  The trace's `ClosureGate` only flips a story into
  `QA-ready`; closure still waits for the manual PASS.
- **The `InputDriver` abstraction itself.** Defined by
  `platform`. E2e supplies the `ReplayDriver` implementation
  but does not own the seam.
- **Trace-recording UI and `.glibre-trace` *writer*.** Owned by
  `tools` — `TraceRecorder` lives in the editor as a record-mode
  capability. E2e is the consumer of its output, not the producer.
- **Perf benchmarking.** Owned by a sibling `benchmarks` context.
  E2e asserts behavioural equivalence (same inputs → same
  outputs); micro-benchmarks and frame-time regressions are not
  in scope.
- **Runtime asset cooking.** Cooked artefacts must already exist
  on disk before a trace plays; e2e refuses to bake. Asset-pack
  hash is part of the `EnvHash` precondition.
- **Render-correctness verification beyond pixel-tolerance image
  diffing.** Cross-backend framebuffer hashing (R-2.1.1), PSNR
  thresholds on effects, ULP-tight tessellation, and other deep
  GPU-validation suites stay with `render`. E2e's
  `AssertScreenshot` is a behavioural gate against a golden, not
  a rendering-correctness gate.
- **Gameplay-side networked replay.** `networking/replay-system`
  (R-8.6.1 … R-8.6.5) mirrors world *state* for spectating /
  kill-cam / esports broadcast. E2e replays *inputs* against a
  single deterministic process. The two share a noun and nothing
  else.
- **Symbolicated crash dumps and aggregated diagnostics.** When a
  trace's process crashes, the `BinaryCrash` `E2eError` carries
  the dump path; aggregation, symbolication, and clustering are
  owned by `diagnostics` (cf. `platform`'s symmetric refusal).

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
