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

This section enumerates e2e's aggregates, value objects, and the
invariants every public boundary must hold. Aggregates are listed in
data-flow order (record artefact on disk → manifest gating → replay
in process → assert against live state → report). Each aggregate owns
one dimension of "prove a `type:user-story`'s acceptance criteria
mechanically against a built binary, frame-deterministically, in CI";
per PHILOSOPHY §1 (SRP), an aggregate is admitted to this list only
when its single reason-to-change does not collapse into another's.
Where two harmonius primitives reduce to one glibre primitive, the
collapse is cited from §3.2. Cross-context concerns (the
`InputDriver` seam itself, the `.glibre-trace` writer, render-side
correctness, perf benchmarking, asset cooking, gameplay-side
networked replay, crash-dump aggregation) are explicitly delegated
and never re-asserted here (§3.3).

### 4.1 Aggregate roster

#### 4.1.1 `Trace` — frame-indexed sequence of `TraceOp` (aggregate root)

**Reason to change:** what a single playable trace is — header,
manifest, ordered `(FrameIndex, TraceOp)` stream, footer hash.
Distinct from "how a trace is stored on disk" (§4.1.2) and from
"what an op does" (§4.1.4 / §4.1.5).

**Composition.** Holds a `TraceManifest` (§4.1.3) by value, an
ordered `(FrameIndex, TraceOp)` stream — one frame may carry zero,
one, or many ops; ops on the same frame retain their recorded
intra-frame order — and a footer `Blake3Hash` over the canonical
encoding of manifest + stream. The terminating `TraceOp::End` marks
trace completion; everything beyond it is ignored on replay.
`FrameIndex` starts at 0 and is monotonically non-decreasing across
the stream. The entry-point identity for replay is `(EnvHash,
trace_path, footer_hash)`; the `Trace` aggregate is the unit of
replayable evidence cited by a `type:user-story`'s acceptance
criteria.

**Identity & lifetime.** Constructed by parsing one `TraceFile`
(§4.1.2); lives for the duration of one replay run. Never mutated
post-construction — the consumer side of `.glibre-trace` is
strictly read-only (the writer is owned by `tools` per §3.3).

**Public-boundary invariants.**

1. **Frame-locked.** Every `TraceOp` in the stream carries its
   recorded `FrameIndex` and is delivered or evaluated at exactly
   that frame index on replay; wall-clock is never consulted.
   `ReplayDriver` (§4.1.6) yields `InputOp`s at their recorded
   `FrameIndex`; `TraceRunner` (§4.1.7) executes `AssertOp`s at
   their recorded `FrameIndex`. The collapse rule in §3.2 #1 is
   load-bearing: there is exactly one time axis e2e recognises
   and it is the engine's frame counter.
2. **Monotonic frame indices.** Across the ordered stream,
   `FrameIndex_i <= FrameIndex_{i+1}`. Equal indices are
   permitted (multiple ops per frame, intra-frame order
   preserved); strictly decreasing indices are rejected at parse
   time with `E2eError::TraceParse`.
3. **Sealed `TraceOp` variant.** The op sum is closed at compile
   time (`Input | AssertState | AssertScreenshot |
   AssertEcsSnapshot | AssertLogContains | End`); adding a
   variant is a deliberate central edit, not an open extension
   point. Any op tag the parser does not recognise is rejected
   with `E2eError::TraceParse` rather than treated as
   forward-compatible.
4. **Footer hash covers everything.** The `Blake3Hash` footer is
   computed over the canonical Fory encoding of manifest + stream;
   any byte mutation between disk and parsed `Trace` is rejected
   with `E2eError::TraceParse`. There is no partial-trace
   recovery.
5. **Unique terminating `End`.** Exactly one `TraceOp::End`
   appears, as the last op in the stream. Streams without
   `End`, or with `End` followed by further ops, are rejected at
   parse with `E2eError::TraceParse`.

#### 4.1.2 `TraceFile` — on-disk representation of a `Trace` (entity)

**Reason to change:** the on-disk encoding shape — Fory schema
version of the manifest, framing of the op stream, footer-hash
algorithm, file extension policy, on-disk path conventions.

**Composition.** A `CanonicalPath` (per `platform`'s
`FileWatcher` discipline — paths are always UTF-8 absolute,
symlink-resolved) under `tests/e2e/<ctx>/`, an extension of
`.glibre-trace`, and a binary Fory-encoded payload of:
versioned magic + Fory schema version, the `TraceManifest`
(§4.1.3), the framed ordered op stream, the terminating
`TraceOp::End`, and the `Blake3Hash` footer. `TraceFile`s are
checked into the repo alongside the user-story whose acceptance
they prove; they are write-once on the consumption side.

**Identity & lifetime.** Identified by its `CanonicalPath`; the
file persists across runs and across machines. The aggregate
loads it into memory once via `platform::FileIo` and produces an
in-memory `Trace`; the file itself is never mutated by the
runner.

**Public-boundary invariants.**

1. **Read-only on the e2e side.** The `TraceWriter` lives in
   `tools` (§3.3); e2e never opens a `TraceFile` for write.
   Attempting to do so is a programming error refused at the
   API boundary.
2. **Canonical path discipline.** Every `TraceFile` reference
   crosses `platform`'s `CanonicalPath` value object; raw
   `const char*` / `std::filesystem::path` never enters the
   public surface (this is the same canonical-path discipline
   `platform` §4.6 declares).
3. **Versioned schema.** The magic + Fory schema version are the
   first bytes; mismatches fail-fast with
   `E2eError::TraceParse`. The Fory codegen pipeline
   (`reviews/decisions/fory-codegen.md`) owns the decoded schema;
   migration of trace files across schema bumps is by explicit
   re-record, never by silent up-conversion.
4. **One trace per file.** A `TraceFile` is exactly one `Trace`;
   we do not batch traces into archives. The bundling concern
   belongs to `content`, not to e2e.

#### 4.1.3 `TraceManifest` — environment header + golden refs (value object)

**Reason to change:** the set of inputs that can perturb a
deterministic replay. Adding or removing a manifest field
changes the `EnvHash` and therefore the set of replayable
traces; this is deliberate.

**Composition.** A versioned record of:

- Engine version (semver + git SHA of the engine build).
- Plugin set + ABI hash (the middleman dylib hash from
  PHILOSOPHY §9; missing or mismatched plugin = drift).
- Asset-pack hash (BLAKE3 over the cooked asset bundle the
  trace was recorded against; raw asset cooking belongs to
  `content` per §3.3, but the produced hash is part of e2e's
  drift fence).
- Locale (BCP-47 tag).
- Window size (`platform::LogicalSize` at record time).
- DPI (`platform::DpiScale` at record time).
- RNG seed (the engine-wide deterministic-seed that gates
  per-system seeded streams per harmonius R-X.5.2 — collapsed
  into one value here per §3.2 #3).
- Target driver tier (the `RunnerHost` permission set the
  trace was recorded under; see §4.1.9).
- `GoldenStore` references — for every `AssertScreenshot` and
  `AssertEcsSnapshot` op in the stream, the
  `GoldenStore`-relative path identifying the golden blob.

`EnvHash` is the canonical Blake3 of the manifest's Fory
encoding; one hash, one refusal site, one diagnosis per the
collapse rule in §3.2 #3.

**Identity & lifetime.** Lives by-value inside its owning
`Trace`; never mutated post-parse. The manifest is the *gate*
for replay — the runner consults it before any `TraceOp` is
dispatched.

**Public-boundary invariants.**

1. **Gate before any op runs.** The `TraceRunner` (§4.1.7)
   computes the live `EnvHash` from the running process's
   environment and compares it against
   `manifest.env_hash` *before* installing the
   `ReplayDriver` (§4.1.6). Mismatch returns
   `E2eError::EnvDrift` and the runner refuses to proceed —
   never produces a misleading green or red. This is the
   load-bearing rule the §3.2 #3 collapse exists to enforce.
2. **All eight fields participate in the hash.** Engine
   version, plugin ABI hash, asset-pack hash, locale, window
   size, DPI, RNG seed, target driver tier — all present, all
   hashed. Omitting a field requires a manifest schema bump
   and is a deliberate central edit, not silent.
3. **Golden refs resolved at gate time.** Every
   `GoldenStore`-relative path the manifest cites is checked
   for existence at gate time (before the runner advances any
   frame); a missing reference returns
   `E2eError::GoldenMissing`, not a per-assert failure
   later. This makes "missing golden" diagnosable up-front
   rather than mid-replay.
4. **Immutable post-load.** Manifest fields are read-only
   after parse; attempting to mutate is a compile error. The
   manifest is part of the parsed `Trace`'s identity.

#### 4.1.4 `TraceOp` — sealed sum of replay operations (value object)

**Reason to change:** the closed vocabulary of operations a
trace may carry. Adding a variant is a deliberate central
edit; this is what keeps the parser, the recorder (in
`tools`), the replay driver, and the runner in lockstep.

**Composition.** Closed `std::variant` over:

- `InputOp` — see §4.1.5.
- `AssertState` — predicate `(component_path,
  expected_fory_blob)` over a named ECS resource or
  component value at the recorded frame.
- `AssertScreenshot` — capture the swapchain readback at the
  recorded frame and compare against the named
  `GoldenImage` (§4.1.10) under the supplied
  `PixelTolerance` (§4.1.11).
- `AssertEcsSnapshot` — serialise the named `World` (or
  sub-aggregate) via Fory and require byte-equal match
  against the named `EcsSnapshot` reference in the
  `GoldenStore`.
- `AssertLogContains` — require a substring or regex to
  have appeared in the structured log channel between the
  previous assert and this frame.
- `End` — the sole termination marker.

**Identity & lifetime.** A `TraceOp` is a value object inside
its owning `Trace`'s ordered stream; it does not exist
independently.

**Public-boundary invariants.**

1. **Sealed at compile time.** Closed variant; no
   open-extension hook. Adding a variant is a central edit
   to the e2e header and the `tools::TraceRecorder`'s
   producer side.
2. **Recorded-frame-locked execution.** Each variant is
   delivered or evaluated at exactly the recorded
   `FrameIndex`; see §4.1.1 invariant 1.
3. **Per-variant payload-validity.** Parse-time validation
   (e.g. `AssertScreenshot.golden_ref` is a non-empty
   `GoldenStore`-relative path; `AssertEcsSnapshot.world_id`
   is a known world tag) is enforced at trace-parse, not at
   replay; bad payload = `E2eError::TraceParse`.
4. **No code in the op.** A `TraceOp` is data; the runner is
   the dispatcher. There is no embedded executable expression
   in any variant — `AssertState`'s predicate is a
   component-path + Fory-blob comparison, not a script.

#### 4.1.5 `InputOp` — replay-driver input event (value object)

**Reason to change:** the wire shape of a recorded input
event. Bound by `platform::InputEvent`; e2e wraps that sum
without re-deriving it.

**Composition.** Carries one `platform::InputEvent` payload
(see `platform` spec §4.2's sealed variant: `KeyDown`,
`KeyUp`, `MouseMove`, `MouseButton`, `Wheel`, `TextInput`,
`GamepadAxis`, `GamepadButton`) plus the recorded
`FrameIndex` inherited from its enclosing `(FrameIndex,
TraceOp)` tuple. Stored Fory-encoded inside the trace
stream; decoded at parse into the engine's typed
`InputEvent` value.

**Identity & lifetime.** Same as the enclosing `TraceOp`.

**Public-boundary invariants.**

1. **Re-emitted, not synthesised.** The `ReplayDriver`
   (§4.1.6) reproduces the recorded `InputEvent` byte-equal
   on the recorded frame; no field is recomputed (no
   live mouse-position substitution, no live timestamp
   stamping). This is the contract that makes the replay
   deterministic relative to the SDL3 pump.
2. **Sum bound by `platform`.** When `platform::InputEvent`
   gains a variant, the e2e Fory schema bumps and the
   trace-file version bumps; this is a deliberate
   coordinated change, not silent forward-compat (§3.3 also
   refuses to own the `InputEvent` abstraction itself).
3. **Per-device ordering preserved.** Multiple `InputOp`s on
   the same `FrameIndex` retain their recorded intra-frame
   order, mirroring `platform`'s per-device monotonic
   ordering invariant (`platform` §4.2 invariant 2).
4. **No synthetic events.** The recorder's output is the
   only source; the runner does not invent input events to
   patch around drift.

#### 4.1.6 `ReplayDriver` — frame-locked `platform::InputDriver` impl (entity)

**Reason to change:** how recorded input is yielded into the
engine's input-pump path. This is e2e's only implementation
of `platform::InputDriver`; the abstraction itself is owned
by `platform` (§3.3).

**Composition.** Reads a `Trace` (its ordered op stream) and
maintains a cursor `(next_op_index, current_frame_index)`.
When the engine's frame loop advances to frame N and queries
the input driver, the `ReplayDriver` walks forward from
`next_op_index` and yields every `InputOp` whose
`FrameIndex == N`, in recorded order, then returns control
once that frame's bucket is drained. Non-input `TraceOp`s
(asserts, `End`) are skipped here — they are dispatched by
the `TraceRunner` (§4.1.7). The driver holds no wall-clock
state.

**Identity & lifetime.** One `ReplayDriver` per trace run;
constructed by the `TraceRunner` after `EnvHash` gating
passes; installed into the engine binary in place of the
real SDL3-backed driver via the `platform::InputDriver`
seam.

**Public-boundary invariants.**

1. **Frame-locked emission.** `InputOp`s are yielded only on
   their recorded `FrameIndex`; never early, never late.
   Wall-clock is not read. This is the §3.2 #1 collapse
   reified — there is one time axis, the engine frame
   counter, and the driver honours it.
2. **`platform::InputDriver` shape unchanged.** The replay
   driver is a drop-in implementation of the existing seam
   (`platform` §4.2 `Pump` / `EventQueue<InputEvent>`); e2e
   does not extend or reshape that interface.
3. **No SDL3 traffic.** The replay driver does not call
   into SDL3, does not drain the OS event pump, and does not
   touch any window-server resource. Real input is fully
   absent during replay (`InjectionLayer::InProcess` —
   §4.1.8); per-process and OS-automation layers have their
   own driver shape but the same frame-locked emission rule.
4. **Cursor advances exactly once per op.** Each `InputOp`
   is yielded exactly once across the run; replay never
   re-yields, and frames whose bucket is empty yield zero
   events without disturbing the cursor.
5. **Deterministic on equal `EnvHash`.** Two runs of the
   same trace under the same `EnvHash` and same
   `InjectionLayer` produce byte-equal driver-side event
   sequences; divergence on this axis is a bug in the
   engine, not in the driver.

#### 4.1.7 `TraceRunner` — host process driving one trace to completion (aggregate root)

**Reason to change:** how a trace run is orchestrated end to
end — launch, gate, install driver, advance frames, dispatch
asserts, capture artefacts, emit a report. The orchestration
boundary; distinct from any single op's semantics.

**Composition.** Launches the binary under test (the engine,
or the editor for editor-mode traces); selects an
`InjectionLayer` (§4.1.8) compatible with the live
`RunnerHost` (§4.1.9); computes the live `EnvHash` and
checks it against `Trace::manifest.env_hash`; installs the
`ReplayDriver` (§4.1.6) at the `platform::InputDriver` seam;
advances the engine frame loop one frame at a time; for each
advancing frame, dispatches any `AssertOp`s recorded at that
`FrameIndex` against the live process state; collects
captured artefacts (screenshots, diff images, ECS-snapshot
blobs, log slices); produces a single `TraceReport`
(§4.1.12); returns. On any `AssertOp` failure, the run
aborts with `E2eError::AssertFailed` carrying the failing
op identifier, the `FrameIndex`, and the captured artefact.

**Identity & lifetime.** One `TraceRunner` per trace run;
constructed by the CI workflow or the developer-side
`glibre-trace run` command; destroyed when the report is
emitted. Holds no state across runs.

**Public-boundary invariants.**

1. **Gate before drive.** The `EnvHash` check (§4.1.3
   invariant 1) and the golden-presence check (§4.1.3
   invariant 3) both run *before* the `ReplayDriver` is
   installed and before any frame is advanced. A run that
   fails gating never observes a single
   `(FrameIndex, TraceOp)` pair.
2. **Frame-step deterministic.** The runner advances the
   engine's frame loop in lockstep with the trace's
   `FrameIndex` axis; it does not free-run. Each frame
   advances to N+1 only after frame N's queued asserts
   complete (failure halts immediately — see invariant 4).
3. **Single binary under test per run.** The `TraceRunner`
   targets one process; multi-process traces are out of
   scope (this is consistent with §1's refusal of
   gameplay-side networked replay and with §3.3's refusal
   of full-OS automation outside isolated CI).
4. **Fail-fast on assert.** The first failing `AssertOp`
   halts the run; the runner emits a `TraceReport` whose
   status is `Failed` with the failing op identifier,
   captured artefacts, and `FrameIndex`. There is no
   "continue-on-fail" mode in MVP.
5. **Always emits a report.** Pass, fail, parse error, env
   drift, golden-missing, driver-install, injection-refused,
   timeout, binary-crash — every terminating outcome is
   reported as a `TraceReport`. No silent exits.
6. **Frame budget enforced.** A trace that exceeds its
   declared frame budget (manifest field, derived from
   recorded length × safety multiplier) aborts with
   `E2eError::Timeout`; the runner does not run unbounded.
7. **No exceptions cross the boundary.** Per
   `reviews/decisions/error-model.md`, the runner returns
   `glibre::Result<TraceReport, glibre::Error>`; the
   `e2e::Error` arm carries `E2eError` (§4.1.13).

#### 4.1.8 `InjectionLayer` — sealed sum of input-delivery mechanisms (value object)

**Reason to change:** the menu of mechanisms by which the
runner delivers `InputOp`s into the binary under test. The
collapse from §3.2 #2 is load-bearing: harmonius scattered
four injection paths; e2e enumerates exactly three and gates
each by `RunnerHost`.

**Composition.** Closed `std::variant` over:

- `InProcess` — the `ReplayDriver` is linked directly into
  the binary under test and substitutes for the SDL3-backed
  driver at the `platform::InputDriver` seam. No OS event
  delivery; no window-server traffic. The default for
  headless and CI runs.
- `PerProcess` — events are delivered to one PID/window via
  `CGEventPostToPid` (macOS), `PostMessage` (Windows), or
  `xdotool --window` (Linux). Non-disruptive: the event
  goes to the target process only, not to the desktop. The
  only viable layer for `dev-interactive` traces against
  the running editor.
- `OsAutomation` — full-desktop control via synthesised
  global mouse/keyboard. Restricted to isolated CI runners
  by policy; explicitly refused on developer hosts.

**Identity & lifetime.** A value object selected once at
runner construction and held by the `TraceRunner` for the
duration of the run. The selection is a function of the
live `RunnerHost` (§4.1.9) and the trace's
`manifest.target_driver_tier`.

**Public-boundary invariants.**

1. **Sealed sum.** Exactly three layers, closed at compile
   time. Adding a layer is a deliberate central edit. The
   §3.2 #2 collapse is enforced here — harmonius's four
   injection surfaces are explicitly *not* re-introduced.
2. **`RunnerHost`-gated.** The runner refuses any
   `(InjectionLayer, RunnerHost)` pair the policy table
   (§4.1.9 invariant 2) does not permit; refusal returns
   `E2eError::InjectionRefused` and the run aborts before
   the driver is installed.
3. **`OsAutomation` is dev-host-refused.** On any
   `RunnerHost` other than `ci-isolated`, selecting
   `OsAutomation` returns `E2eError::InjectionRefused`
   without exception. PHILOSOPHY §1 (SRP) and §3 (minimal
   core) — full-desktop control on developer workstations
   is a category error for e2e.
4. **One layer per run.** A run does not switch layers
   mid-trace. Switching would mean two `EnvHash` worlds in
   one report, defeating the gating invariant.

#### 4.1.9 `RunnerHost` — host-environment tag gating injection (value object)

**Reason to change:** the policy table mapping where a trace
runs to which `InjectionLayer`s are permitted. Tightening
or relaxing the policy is a central edit; e2e never invents
silent permissions.

**Composition.** Closed `std::variant` over:

- `dev-headless` — developer machine, no display server in
  use for this run.
- `dev-interactive` — developer machine, display server
  active and possibly being used by the developer.
- `ci-headless` — CI worker, no display server, no other
  workloads.
- `ci-isolated` — CI worker dedicated to a single run, full
  display under the runner's control.

The `RunnerHost` is detected at runner construction from
environment markers (CI provider env vars, presence of a
window server, `--ci-isolated` opt-in flag); the detection
is conservative — when in doubt, the runner picks the more
restrictive tag.

**Identity & lifetime.** Detected once at runner
construction and held for the duration of the run.

**Public-boundary invariants.**

1. **Closed sum.** Exactly four hosts; adding a host is a
   deliberate central edit.
2. **Policy table — single source of truth.** The
   permitted-injection-layer set per host is:
   `dev-headless` → `{InProcess}`;
   `dev-interactive` → `{InProcess, PerProcess}`;
   `ci-headless` → `{InProcess}`;
   `ci-isolated` → `{InProcess, PerProcess, OsAutomation}`.
   Selecting a layer outside its host's set returns
   `E2eError::InjectionRefused`. The §3.2 #2 collapse
   demands that this table be the only place such permissions
   live.
3. **Detection is conservative.** Ambiguous environments
   (e.g. a CI runner that exposes a display server but is
   not declared isolated) downgrade to `ci-headless`, never
   upgrade to `ci-isolated`. The runner never silently
   widens permissions.
4. **Manifest target tier matches host class.** The
   trace's `manifest.target_driver_tier` declares the host
   class it was recorded for; mismatch (e.g. an
   `OsAutomation`-only trace asked to run on
   `dev-headless`) returns
   `E2eError::InjectionRefused` at gate time.

#### 4.1.10 `GoldenImage` and `GoldenStore` — reference assets for visual / state asserts

**Reason to change:** how golden references are stored,
addressed, and updated. Distinct from how a single
`AssertScreenshot` op compares pixels (§4.1.11).

**Composition.**

- `GoldenImage` — a reference PNG (sRGB, 8-bit per channel
  for MVP) checked into `tests/e2e/<ctx>/golden/` and
  addressed by the trace's relative path plus the assert id.
  Loaded read-only by `AssertScreenshot` evaluation.
- `EcsSnapshot` — a Fory-encoded reference blob (one per
  `AssertEcsSnapshot` op) with the same addressing rule.
- `GoldenStore` — the on-disk + git-tracked corpus
  containing both `GoldenImage` and `EcsSnapshot`
  references. The store is the single source of truth for
  reference artefacts; updates land through one explicit
  `golden-update` workflow, never silently. The §3.2 #4
  collapse is enforced here — five harmonius reference-image
  conventions reduce to one store with one update path.

**Identity & lifetime.** Identified by
`(trace_relative_path, assert_id)`; addressed via
`platform::CanonicalPath`. References live across runs,
across machines, across schema versions; updates are
deliberate.

**Public-boundary invariants.**

1. **Read-only at run time.** The `TraceRunner` only reads
   from the `GoldenStore`; no run mutates a reference. The
   `golden-update` workflow is the sole writer and lives
   outside the runner's surface.
2. **Address by trace path + assert id.** No content-addressing
   (which would defeat the manifest's golden-ref discipline);
   no globbing; no implicit resolution. A missing reference
   returns `E2eError::GoldenMissing` at gate time
   (§4.1.3 invariant 3), never at assert evaluation time.
3. **Updates are deliberate and audited.** The
   `golden-update` workflow updates one or more references
   in one PR with the producing trace cited in the PR body;
   a reviewer signs off on the visual or snapshot diff. No
   silent regeneration on red-to-green transition.
4. **PNG and Fory schemas are stable.** Reference encoding
   formats are fixed at engine version boundaries; a schema
   bump is a manifest field bump and changes the
   `EnvHash` per §4.1.3 invariant 2.
5. **One golden per assert id.** Each
   `(trace_relative_path, assert_id)` resolves to exactly
   one golden file; multi-platform variants address the
   multi-platform difference at the `RunnerHost` level
   (deferred post-MVP), not by storing per-platform
   shadows.

#### 4.1.11 `PixelTolerance` — per-`AssertScreenshot` comparison policy (value object)

**Reason to change:** how pixel-diff strictness is
declared. The §3.2 #4 collapse fuses five harmonius
tolerance metrics into this one policy shape.

**Composition.** A value object carrying:

- Max per-pixel ΔE (perceptual color delta; CIEDE2000 in
  MVP).
- Max % differing pixels.
- Optional region mask (a rectangle list inside which the
  comparison is enforced; outside which differences are
  ignored).
- Optional engine-wide tier name (e.g. `strict`,
  `default`, `lenient`) selecting a default triple from
  one engine-wide policy table; per-assert overrides are
  permitted.

`PixelTolerance` is owned by the `AssertScreenshot` op as
a sub-value; it never lives independently.

**Public-boundary invariants.**

1. **Policy is data, not code.** The tolerance is fully
   describable by the four fields above; no per-assert
   custom comparator is permitted. A new comparison shape
   requires a `TraceOp` schema bump.
2. **Default tier exists; per-assert override permitted.**
   Authors typically pick a tier name; the override fields
   exist for one-off masks (e.g. ignoring a clock readout
   region).
3. **Tier table is engine-wide.** The names map to one
   centrally-declared triple; per-context tier definitions
   are refused — this is what the §3.2 #4 collapse buys.
4. **Failure description is structured.** When tolerance
   is exceeded, the comparator emits a structured payload
   (max-ΔE-found, %-differing-found, masked-region
   diagnosis) consumed by the `TraceReport`'s artefact
   bundle; no prose-only failure.

#### 4.1.12 `TraceReport` and `DivergenceReport` — structured run output

**Reason to change:** the wire shape of a run's outcome.
Distinct from how the runner produces it (§4.1.7) and from
how CI consumes it (§4.1.14).

**Composition.**

- `TraceReport` — a value object carrying:
  pass/fail status (`Passed | Failed { failing_op_id,
  frame_index, e2e_error_arm } | Aborted { reason }`),
  captured artefact handles (diff image paths, ECS-snapshot
  diff blob paths, log slices), wall-clock duration
  (informational; not used in pass/fail logic), frame count
  observed, the `EnvHash` the run gated on, the
  `InjectionLayer` and `RunnerHost` tags, and the
  trace-file `CanonicalPath`. Emitted exactly once per
  `TraceRunner` invocation (§4.1.7 invariant 5).
- `DivergenceReport` — a specialised `TraceReport` shape
  produced when two replays of the same trace under the
  same `EnvHash` and `InjectionLayer` produce different
  `TraceOp` outcomes. Names the first differing
  `FrameIndex`, the diverging op, and side-by-side captured
  artefacts. Used by determinism-regression hunts; not
  produced on a normal pass/fail run.

**Public-boundary invariants.**

1. **One `TraceReport` per run.** The runner emits exactly
   one report; partial / streaming reports are not in
   scope. The report is the unit consumed by
   `ClosureGate` (§4.1.14).
2. **`DivergenceReport` requires two runs.** It is only
   produced when the runner is invoked with a comparison
   reference (`--compare <prior-report>`); on a single
   run, divergence cannot be detected and the report is a
   `TraceReport` only.
3. **Wall-clock duration is informational.** It is
   captured for telemetry (and for `Timeout` enforcement in
   §4.1.7 invariant 6) but is never an input to the
   pass/fail decision; pass/fail is a function of the
   asserts only.
4. **Stable structured fields.** Reports are Fory-encoded
   for CI consumption; field set is versioned and bumps
   the report-schema version, never silently reshapes.

#### 4.1.13 `E2eError` — closed sum of typed failures (value object)

**Reason to change:** the closed list of failure modes the
e2e context surfaces. Per `reviews/decisions/error-model.md`,
this enum lives in the engine-wide `glibre::Error` variant
as one arm and is the only e2e-internal error surface.

**Composition.** Closed enum class:

- `TraceParse` — file framing, schema-version, footer-hash,
  variant-tag, monotonic-frame-index, or per-op payload
  validation failed at parse time.
- `EnvDrift` — `manifest.env_hash` mismatched the live
  `EnvHash` at gate time.
- `AssertFailed` — an `AssertOp` evaluated to false at its
  recorded `FrameIndex`; carries the failing op id and the
  captured artefact reference.
- `DriverInstall` — installing the `ReplayDriver` at the
  `platform::InputDriver` seam failed (e.g. the binary
  under test does not expose the seam).
- `InjectionRefused` — the requested `InjectionLayer` is
  not permitted on the live `RunnerHost`.
- `GoldenMissing` — a `GoldenStore`-relative path the
  manifest cited does not exist at gate time.
- `Timeout` — the run exceeded its declared frame budget
  (§4.1.7 invariant 6).
- `BinaryCrash` — the binary under test exited non-zero
  outside an `End`-driven shutdown; carries the dump path
  (aggregation belongs to `diagnostics`, not e2e — §3.3).

**Public-boundary invariants.**

1. **Closed sum, central edit.** Adding a variant edits this
   enum and the engine-wide `glibre::Error` variant
   simultaneously; old variants are never silently
   removed. Per the engine error model, removal is a
   breaking ABI change and triggers a plugin ABI hash bump.
2. **No exceptions cross the boundary.** Every public
   fallible operation in this context returns
   `glibre::Result<T>` per
   `reviews/decisions/error-model.md`; exceptions never
   escape the runner.
3. **Errors are constructed at the failure site.** Per the
   error-model composition rule, no aggregate translates
   another aggregate's error into its own automatically;
   the call site that crosses the boundary maps explicitly.
4. **CI-consumable exit codes.** Each variant maps to a
   stable non-zero process exit code from the
   `TraceRunner`; the mapping is the public contract CI
   gates on. The mapping table itself is one source of
   truth in this header.

#### 4.1.14 `ClosureGate` — story-closure rule (entity)

**Reason to change:** the rule that flips a
`type:user-story` from "in progress" to "QA-ready". This
is the load-bearing gate cited in `AGENTS.md` § User Story
closure rules and is the externally-visible product of
e2e — it is what makes a closed story trustworthy.

**Composition.** A CI step that, for a given user-story
issue, walks the story's referenced traces, invokes the
`TraceRunner` (§4.1.7) on each, collects the resulting
`TraceReport`s, and flips the story's `qa-ready` label
only when *every* referenced trace's report status is
`Passed`. The gate is a function of the trace reports
alone; no other inputs (no human override, no manual flag)
participate.

**Identity & lifetime.** One `ClosureGate` invocation per
user-story per commit; lives for the duration of the CI
job. State is published as a CI status check the
user-story issue's GitHub workflow consults; failure to
pass the gate keeps the story out of `qa-ready` and
therefore out of manual-PASS eligibility.

**Public-boundary invariants.**

1. **All referenced traces green, or no flip.** The gate
   passes only when every trace cited by the user-story
   reports `Passed`. A single `Failed` / `Aborted`
   report keeps the story out of `qa-ready`. No
   threshold-based or quorum-based passing is permitted —
   per `AGENTS.md` § User Story closure: "E2E test must be
   authored and **passing in CI** before any human manual
   testing begins." The gate enforces that line literally.
2. **Manual-PASS is downstream of the gate, never
   upstream.** A manual-test PASS comment cannot flip the
   story closed unless `qa-ready` was set by this gate.
   The `user-story.yml` template's closure checklist
   refuses to accept a PASS comment without the gate's
   green signal — see PHILOSOPHY §4 (spec → story → test
   → code) and `AGENTS.md` § User Story closure rules.
3. **Gate runs on every commit.** A commit that drifts the
   environment (engine version bump, plugin ABI hash bump,
   asset-pack hash change) re-runs the gate; a previously
   green story can lose `qa-ready` if its environment
   drifts beneath it. There is no "frozen at last green"
   shortcut.
4. **Gate consumes `TraceReport` only.** No special
   privileged signal exists; the gate is a deterministic
   function of the reports the runner emits. Per
   PHILOSOPHY §10, this collapses any "manual override"
   shortcut into one rule with no escape hatch.
5. **Gate is the only e2e externally-visible artefact for
   closure.** All other e2e outputs (artefacts, diff
   images, logs) are diagnostic; only the gate's
   green/red signal participates in story-closure
   semantics.

### 4.2 Cross-aggregate invariants

Invariants that span more than one aggregate and must hold at every
public boundary at the seams between them:

1. **Frame-locked everything.** The only time axis e2e
   recognises is the engine `FrameIndex`; the
   `ReplayDriver` (§4.1.6) yields `InputOp`s on their
   recorded frame, the `TraceRunner` (§4.1.7) dispatches
   `AssertOp`s on their recorded frame, and the
   `TraceManifest`'s `target_driver_tier` field plus the
   `RNG seed` ensure that the same frame index produces
   the same engine state across runs. Wall-clock is
   captured in `TraceReport` only as informational
   metadata; it never participates in pass/fail logic
   (§4.1.12 invariant 3). This is the load-bearing rule
   that makes "playable evidence for an acceptance
   criterion" mean what it says.
2. **`EnvHash` gates replay before any op runs.** No
   aggregate downstream of `TraceManifest` (§4.1.3) is
   constructed or mutated when `EnvDrift` is detected.
   The `ReplayDriver` is not installed; the
   `TraceRunner` does not advance a frame; the
   `GoldenStore` is not consulted. The §3.2 #3 collapse
   reified — one hash, one refusal, one diagnosis.
3. **`InjectionLayer` choice is `RunnerHost`-gated, no
   exceptions.** The policy table in §4.1.9 invariant 2
   is the sole source of truth; selecting an unlisted
   pair returns `E2eError::InjectionRefused` and the
   runner aborts before installing the driver. The
   §3.2 #2 collapse — three layers, four hosts, one
   table — is honoured at every public boundary.
4. **`ClosureGate` requires green from every referenced
   trace, in CI.** The `qa-ready` label flip is a
   function of `TraceReport.status == Passed` for every
   trace the user-story cites, evaluated in CI. No
   manual override, no developer-host green substitute,
   no "amber" tier. This is `AGENTS.md` § User Story
   closure rule (1) reified at the spec level.
5. **`ReplayDriver` is the only e2e implementation of
   `platform::InputDriver`.** The seam itself is owned
   by `platform` (§3.3); e2e supplies one
   implementation. No second e2e-internal driver shape
   exists (e.g. a "synthetic" or "stress" driver lives
   outside e2e or not at all).
6. **`TraceFile` is read-only on the e2e side.** The
   `TraceWriter` is owned by `tools` (§3.3); e2e never
   opens a `TraceFile` for write. Trace authoring
   round-trips through the editor's record mode and a
   PR; the runner consumes the result.
7. **Per-context error model honoured.** Every aggregate's
   public fallible operation returns
   `glibre::Result<T, glibre::Error>` per
   `reviews/decisions/error-model.md`; e2e's enum lives
   in the `e2e::Error` arm cited there and is the only
   e2e-internal error surface (§4.1.13).
8. **Aggregates never mutate one another's state.** The
   `Trace` (§4.1.1) is read-only post-parse; the
   `TraceManifest` is read-only post-parse; the
   `GoldenStore` is read-only at run time; the
   `TraceReport` is constructed once per run and never
   mutated. Mutation is concentrated in the `TraceRunner`
   (§4.1.7) which orchestrates everything else; no other
   aggregate has a public mutator.

## 5. Public Interface

The header stub below is the §5 deliverable: every symbol that crosses
the e2e context's public boundary, declared in one C++23 header and
verified with `clang++ -std=c++23 -fsyntax-only -Wall -Wextra
-Wpedantic`. Bodies live inside the e2e dylib; this header is the
contract every caller (CI's `ClosureGate` step, the developer-side
`glibre-trace run` command, downstream consumers of `TraceReport`)
compiles against. Cross-context invariants enforced here:

- Every fallible operation returns `glibre::Result<T>` per
  `reviews/decisions/error-model.md`. The e2e-internal `Error` closed
  sum (§4.1.13) is the only failure surface; it rolls into the
  engine-wide `glibre::Error` variant as one arm.
- Aggregates listed in §4 (`Trace`, `TraceManifest`, `TraceRunner`,
  `ReplayDriver`, `GoldenStore`, `TraceReport`, `DivergenceReport`,
  `ClosureGate`) are forward-declared classes whose layout is owned
  inside the e2e dylib. Callers manipulate them only through the
  methods exposed below.
- `ReplayDriver` is the e2e context's only implementation of
  `platform::InputDriver` (§3.3, §4.2 cross-aggregate inv 5). The
  seam itself is owned by `platform`; e2e supplies one impl and
  exposes it via `as_platform_driver()`.
- `TraceFile` is read-only on the e2e side. The `TraceWriter` lives
  in `tools` (§3.3, §4.2 cross-aggregate inv 6) — no `open_for_write`
  symbol exists below.
- `EnvHash` gates replay before any op runs (§4.1.3 inv 1, §4.2
  cross-aggregate inv 2). The runner refuses to advance a frame
  before the live `EnvHash` matches `manifest.env_hash`.
- `InjectionLayer` choice is `RunnerHost`-gated by one policy table
  (§4.1.9 inv 2, §4.2 cross-aggregate inv 3). Selecting an unlisted
  pair returns `Error::InjectionRefused` at gate time.
- `ClosureGate` is a pure function of `TraceReport.status == Passed`
  for every cited trace (§4.1.14 inv 1, §4.2 cross-aggregate inv 4);
  no manual override, no developer-host green substitute.

The header has no Fory schemas in its public surface — `.glibre-trace`
is a versioned binary the parser consumes (§7 owns the schema), and
the runner's per-run artefact bundle is delivered as `ArtefactRef`
paths into a runner-private directory. Event types are the `TraceOp`
sealed sum (§4.1.4), the `FileEvent`-style `report_status` sub-sum
(§4.1.12), and the `Error` closed sum (§4.1.13). The e2e context
contributes one new arm to the engine-wide `glibre::Error` variant:
the closed sum `e2e::Error` defined below.

```cpp
// SPDX-License-Identifier: Apache-2.0
// glibre — e2e public interface (header-only stub).
//
// This file is the §5 deliverable of `specs/e2e/SPEC.md`. It declares
// every symbol crossing the e2e context's public boundary: the
// `.glibre-trace` consumer surface (parse → gate → replay → assert →
// report → close) cited by every `type:user-story` issue. The bodies
// live inside the e2e dylib; this header is the contract every caller
// (CI, the developer-side `glibre-trace run` command, the `ClosureGate`
// step) compiles against.
//
// Cross-context invariants embedded here:
//   * Every fallible call returns `glibre::Result<T>` per
//     `reviews/decisions/error-model.md`. `-fno-exceptions` is enforced
//     globally; this header obeys.
//   * Aggregates are opaque — `Trace`, `TraceRunner`, `ReplayDriver`,
//     `GoldenStore` are forward-declared classes whose layout is owned
//     inside the e2e dylib; callers traffic only in handles + value
//     objects.
//   * `ReplayDriver` is the e2e context's only implementation of
//     `platform::InputDriver`; the seam itself is owned by `platform`
//     (§3.3, §4.2 cross-aggregate inv 5). The `InputEvent` carried by
//     `InputOp` is forward-declared from `<glibre/platform/platform.hpp>`.
//   * The `e2e::Error` closed sum is one arm of the engine-wide
//     `glibre::Error` variant per the error-model record; e2e never
//     translates another context's error into its own automatically.
//   * `TraceFile` is read-only on the e2e side. The `TraceWriter` lives
//     in `tools` (§3.3) — no `open_for_write` symbol exists below.
//
// This stub compiles standalone with
// `clang++ -std=c++23 -fsyntax-only -Wall -Wextra -Wpedantic`.

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

// Engine-wide error type, declared in core/include/glibre/error.hpp.
// Forward-declared here so this header is self-contained for syntax
// checking; the real header pulls in <glibre/error.hpp>.
namespace glibre {
struct ErrorContext;
class  Error;
template <class T> using Result = std::expected<T, Error>;
}  // namespace glibre

// Platform context — e2e wraps `platform::InputEvent` byte-equal inside
// `InputOp` (§4.1.5 inv 1) and uses `platform::CanonicalPath` as its
// only path key (§4.1.2 inv 2). The full definitions live in
// <glibre/platform/platform.hpp>; minimal shape declarations are
// reproduced here so this header is self-contained for syntax checking.
namespace glibre::platform {

class CanonicalPath {
public:
    [[nodiscard]] static auto from_absolute(std::string_view utf8_abs) noexcept
        -> ::glibre::Result<CanonicalPath>;
    [[nodiscard]] auto view() const noexcept -> std::string_view { return view_; }
    constexpr bool operator==(const CanonicalPath&) const noexcept = default;
private:
    constexpr explicit CanonicalPath(std::string_view v) noexcept : view_{v} {}
    std::string_view view_{};
};

class InputEvent;   // sealed sum from platform §4.2; opaque at the e2e seam.
class InputDriver;  // seam from platform §4.2; e2e supplies one impl.

}  // namespace glibre::platform

namespace glibre::e2e {

// ---------------------------------------------------------------------------
// 5.1  Closed sum of typed failures (§4.1.13)
// ---------------------------------------------------------------------------
//
// `Error` is a `std::variant` so payload-bearing arms (`AssertFailed`,
// `BinaryCrash`) survive without losing the closed-sum shape. Each
// payload-free arm is a zero-sized tag struct. The engine-wide
// `glibre::Error` variant rolls this whole sum into one of its arms
// per `reviews/decisions/error-model.md`. Each variant is mapped to a
// stable non-zero process exit code by `TraceRunner` (§4.1.13 inv 4);
// the mapping table is the public CI contract.

struct FrameIndex {
    std::uint64_t value{0};
    constexpr bool operator==(const FrameIndex&) const noexcept = default;
    constexpr auto operator<=>(const FrameIndex&) const noexcept = default;
};

struct AssertOpId {
    std::uint64_t value{0};  // monotonic per-trace, assigned at parse.
    constexpr bool operator==(const AssertOpId&) const noexcept = default;
};

struct ArtefactRef {
    // Non-owning view into the runner's per-run artefact bundle.
    // Resolved against `TraceReport::artefact_root` by the consumer.
    std::string_view relative_path{};
    constexpr bool operator==(const ArtefactRef&) const noexcept = default;
};

enum class AssertKind : std::uint8_t {
    State,      // AssertState
    Screenshot, // AssertScreenshot
    EcsSnapshot,// AssertEcsSnapshot
    LogContains // AssertLogContains
};

struct TraceParse        { constexpr bool operator==(const TraceParse&)        const noexcept = default; };
struct EnvDrift          { constexpr bool operator==(const EnvDrift&)          const noexcept = default; };
struct DriverInstall     { constexpr bool operator==(const DriverInstall&)     const noexcept = default; };
struct InjectionRefused  { constexpr bool operator==(const InjectionRefused&)  const noexcept = default; };
struct GoldenMissing     { constexpr bool operator==(const GoldenMissing&)     const noexcept = default; };
struct Timeout           { constexpr bool operator==(const Timeout&)           const noexcept = default; };

struct AssertFailed {
    AssertOpId  op_id{};
    FrameIndex  frame{};
    AssertKind  kind{};
    ArtefactRef artefact{};  // diff image, snapshot diff, log slice.
    constexpr bool operator==(const AssertFailed&) const noexcept = default;
};

struct BinaryCrash {
    std::int32_t exit_code{0};
    ArtefactRef  dump{};     // path to OS crash dump; aggregation in `diagnostics`.
    constexpr bool operator==(const BinaryCrash&) const noexcept = default;
};

using Error = std::variant<
    TraceParse,
    EnvDrift,
    AssertFailed,
    DriverInstall,
    InjectionRefused,
    GoldenMissing,
    Timeout,
    BinaryCrash>;

// Convenience: every public fallible function returns Result<T>.
template <class T>
using Result = ::glibre::Result<T>;

// ---------------------------------------------------------------------------
// 5.2  Hashes, identifiers, value objects
// ---------------------------------------------------------------------------

struct Blake3Hash {
    std::array<std::uint8_t, 32> bytes{};
    constexpr bool operator==(const Blake3Hash&) const noexcept = default;
};

// EnvHash gates replay before any op runs (§4.1.3 inv 1, §4.2 inv 2).
struct EnvHash {
    Blake3Hash digest{};
    constexpr bool operator==(const EnvHash&) const noexcept = default;
};

// GoldenStore-relative path identifying a reference blob (§4.1.10 inv 2).
struct GoldenRef {
    std::string_view relative_path{};  // e.g. "render/cube/frame_42.png"
    constexpr bool operator==(const GoldenRef&) const noexcept = default;
};

struct WorldId {
    std::uint32_t value{0};  // named ECS world / sub-aggregate tag.
    constexpr bool operator==(const WorldId&) const noexcept = default;
};

// AssertState predicate target — component path inside a named world.
struct ComponentPath {
    std::string_view view{};  // e.g. "world/player/Health.value"
    constexpr bool operator==(const ComponentPath&) const noexcept = default;
};

// ---------------------------------------------------------------------------
// 5.3  PixelTolerance (§4.1.11)
// ---------------------------------------------------------------------------
//
// Per-`AssertScreenshot` policy: max per-pixel ΔE (CIEDE2000), max %
// differing pixels, optional rectangular region mask. `tier` selects
// a default triple from the engine-wide table; per-assert overrides
// are permitted (§4.1.11 inv 2). Policy is data, not code (inv 1) —
// no per-assert custom comparator is permitted.

struct RegionMask {
    // Inclusive rectangles inside which the comparison is enforced.
    // Empty span = whole-frame comparison.
    std::int32_t x{0};
    std::int32_t y{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    constexpr bool operator==(const RegionMask&) const noexcept = default;
};

enum class PixelTier : std::uint8_t { Strict, Default, Lenient };

struct PixelTolerance {
    PixelTier             tier{PixelTier::Default};
    float                 max_delta_e{0.0f};        // CIEDE2000.
    float                 max_diff_fraction{0.0f};  // [0, 1].
    std::span<const RegionMask> mask{};             // empty = whole frame.
    constexpr bool operator==(const PixelTolerance& rhs) const noexcept {
        // Spans compare by data+size; sufficient for value-object equality
        // in the same translation-unit context.
        return tier == rhs.tier
            && max_delta_e == rhs.max_delta_e
            && max_diff_fraction == rhs.max_diff_fraction
            && mask.data() == rhs.mask.data()
            && mask.size() == rhs.mask.size();
    }
};

// ---------------------------------------------------------------------------
// 5.4  TraceOp sealed sum (§4.1.4)
// ---------------------------------------------------------------------------
//
// Closed `std::variant`; adding a variant is a deliberate central edit
// (§4.1.4 inv 1). Each variant is data; the runner is the dispatcher
// (§4.1.4 inv 4). No embedded executable expression in any variant.

struct InputOp {
    // Carries one platform::InputEvent by reference into the trace's
    // arena (decoded once at parse). The wrapper is byte-equal on
    // re-emit (§4.1.5 inv 1); FrameIndex is held by the enclosing tuple.
    const ::glibre::platform::InputEvent* event{nullptr};
};

struct AssertState {
    AssertOpId    id{};
    WorldId       world{};
    ComponentPath path{};
    // Fory-encoded expected value blob; parsed once into the trace arena.
    std::span<const std::byte> expected_fory{};
};

struct AssertScreenshot {
    AssertOpId     id{};
    GoldenRef      golden{};
    PixelTolerance tolerance{};
};

struct AssertEcsSnapshot {
    AssertOpId id{};
    WorldId    world{};
    GoldenRef  reference{};  // .ecs-snapshot blob in the GoldenStore.
};

struct AssertLogContains {
    AssertOpId       id{};
    bool             is_regex{false};
    std::string_view needle{};  // substring or ECMAScript regex.
};

struct End {
    constexpr bool operator==(const End&) const noexcept = default;
};

using TraceOp = std::variant<
    InputOp,
    AssertState,
    AssertScreenshot,
    AssertEcsSnapshot,
    AssertLogContains,
    End>;

// One frame may carry zero, one, or many ops; intra-frame order
// preserved (§4.1.1 inv 1, 2).
struct FramedOp {
    FrameIndex frame{};
    TraceOp    op{};
};

// ---------------------------------------------------------------------------
// 5.5  TraceManifest (§4.1.3)
// ---------------------------------------------------------------------------
//
// All eight fields participate in EnvHash (§4.1.3 inv 2). Read-only
// post-parse (inv 4); golden refs resolved at gate time (inv 3).

enum class RunnerHostKind : std::uint8_t {
    DevHeadless,
    DevInteractive,
    CiHeadless,
    CiIsolated,
};

struct EngineVersion {
    std::uint16_t major{0};
    std::uint16_t minor{0};
    std::uint16_t patch{0};
    Blake3Hash    git_sha{};  // first 32 bytes of the engine build commit.
    constexpr bool operator==(const EngineVersion&) const noexcept = default;
};

// platform::LogicalSize / DpiScale equivalents — held by-value here so
// the manifest is self-contained. The runner cross-checks these against
// platform's live values at gate time.
struct ManifestLogicalSize {
    std::uint32_t width{0};
    std::uint32_t height{0};
    constexpr bool operator==(const ManifestLogicalSize&) const noexcept = default;
};

struct ManifestDpiScale {
    float value{1.0f};
    constexpr bool operator==(const ManifestDpiScale&) const noexcept = default;
};

struct RngSeed {
    std::uint64_t value{0};  // engine-wide deterministic seed (R-X.5.2).
    constexpr bool operator==(const RngSeed&) const noexcept = default;
};

// Frame budget = recorded length × safety multiplier; enforced by
// TraceRunner (§4.1.7 inv 6).
struct FrameBudget {
    std::uint64_t max_frames{0};
    constexpr bool operator==(const FrameBudget&) const noexcept = default;
};

class TraceManifest {
public:
    [[nodiscard]] auto engine_version() const noexcept -> EngineVersion;
    [[nodiscard]] auto plugin_abi_hash() const noexcept -> Blake3Hash;     // middleman dylib hash.
    [[nodiscard]] auto asset_pack_hash() const noexcept -> Blake3Hash;     // BLAKE3 over cooked bundle.
    [[nodiscard]] auto locale()          const noexcept -> std::string_view;  // BCP-47.
    [[nodiscard]] auto window_size()     const noexcept -> ManifestLogicalSize;
    [[nodiscard]] auto dpi_scale()       const noexcept -> ManifestDpiScale;
    [[nodiscard]] auto rng_seed()        const noexcept -> RngSeed;
    [[nodiscard]] auto target_driver()   const noexcept -> RunnerHostKind;

    // Canonical Blake3 of the manifest's Fory encoding.
    [[nodiscard]] auto env_hash() const noexcept -> EnvHash;

    // Frame budget (§4.1.7 inv 6).
    [[nodiscard]] auto frame_budget() const noexcept -> FrameBudget;

    // Every GoldenStore-relative path the trace references.
    // Checked for existence at gate time (§4.1.3 inv 3).
    [[nodiscard]] auto golden_refs() const noexcept -> std::span<const GoldenRef>;

    TraceManifest(const TraceManifest&)            = delete;
    TraceManifest& operator=(const TraceManifest&) = delete;
    TraceManifest(TraceManifest&&) noexcept;
    TraceManifest& operator=(TraceManifest&&) noexcept;
    ~TraceManifest();

private:
    friend class Trace;
    TraceManifest() noexcept = default;
    struct Impl;
    Impl* impl_{nullptr};
};

// ---------------------------------------------------------------------------
// 5.6  Trace + TraceFile (§4.1.1, §4.1.2)
// ---------------------------------------------------------------------------
//
// `Trace` is the aggregate root: immutable post-parse, owns the
// manifest by-value, owns the ordered op stream. Loaded from a
// `.glibre-trace` file via `Trace::load`; bytes never re-open for
// write on the e2e side (§4.1.2 inv 1).

class Trace {
public:
    // Read-only load. Validates magic + schema + footer Blake3
    // (§4.1.1 inv 4); rejects monotonicity / variant / End-uniqueness
    // violations with Error::TraceParse (§4.1.1 inv 2, 3, 5).
    [[nodiscard]] static auto load(const ::glibre::platform::CanonicalPath& path) noexcept
        -> Result<Trace>;

    Trace(Trace&&) noexcept;
    Trace& operator=(Trace&&) noexcept;
    Trace(const Trace&)            = delete;
    Trace& operator=(const Trace&) = delete;
    ~Trace();

    [[nodiscard]] auto manifest()    const noexcept -> const TraceManifest&;
    [[nodiscard]] auto footer_hash() const noexcept -> Blake3Hash;
    [[nodiscard]] auto file_path()   const noexcept -> const ::glibre::platform::CanonicalPath&;

    // Total ordered op count (including the terminating End).
    [[nodiscard]] auto op_count() const noexcept -> std::size_t;

    // Iterate the full ordered (FrameIndex, TraceOp) stream in record order.
    [[nodiscard]] auto ops() const noexcept -> std::span<const FramedOp>;

    // Slice ops whose frame == `frame`. Returned span is contiguous;
    // intra-frame order preserved (§4.1.1 inv 1, §4.1.5 inv 3).
    [[nodiscard]] auto ops_at(FrameIndex frame) const noexcept
        -> std::span<const FramedOp>;

private:
    Trace() noexcept = default;
    struct Impl;
    Impl* impl_{nullptr};
};

// ---------------------------------------------------------------------------
// 5.7  GoldenStore + GoldenImage (§4.1.10)
// ---------------------------------------------------------------------------
//
// Read-only at run time (inv 1). Address by trace path + assert id
// (inv 2). Updates land through the explicit `golden-update` workflow,
// which does not appear here (inv 3).

struct GoldenImage {
    // 8-bit-per-channel sRGB; rows are tightly packed, BGRA order in
    // memory to match Metal swapchain readback. Pointer is non-owning;
    // lifetime tied to the GoldenStore that vended it.
    std::span<const std::byte> bytes{};
    std::uint32_t              width{0};
    std::uint32_t              height{0};
};

struct EcsSnapshotRef {
    // Non-owning Fory-encoded reference blob (§4.1.10 reference shape).
    std::span<const std::byte> bytes{};
};

class GoldenStore {
public:
    // Open a GoldenStore rooted at `tests/e2e/<ctx>/golden/`.
    [[nodiscard]] static auto open(const ::glibre::platform::CanonicalPath& root) noexcept
        -> Result<GoldenStore>;

    GoldenStore(GoldenStore&&) noexcept;
    GoldenStore& operator=(GoldenStore&&) noexcept;
    GoldenStore(const GoldenStore&)            = delete;
    GoldenStore& operator=(const GoldenStore&) = delete;
    ~GoldenStore();

    // Lookup a reference by GoldenStore-relative path. Missing returns
    // Error::GoldenMissing (consumed at gate time per §4.1.3 inv 3).
    [[nodiscard]] auto load_image(GoldenRef)    noexcept -> Result<GoldenImage>;
    [[nodiscard]] auto load_snapshot(GoldenRef) noexcept -> Result<EcsSnapshotRef>;

    // Existence-only probe used by the gate to enumerate manifest refs
    // without loading them; returns Error::GoldenMissing on absence.
    [[nodiscard]] auto probe(GoldenRef) noexcept -> Result<void>;

private:
    GoldenStore() noexcept = default;
    struct Impl;
    Impl* impl_{nullptr};
};

// ---------------------------------------------------------------------------
// 5.8  ReplayDriver (§4.1.6)
// ---------------------------------------------------------------------------
//
// Frame-locked `platform::InputDriver` implementation. Reads a `Trace`
// and emits its `InputOp`s at their recorded `FrameIndex`. Non-input
// `TraceOp`s are skipped here — they are dispatched by the
// `TraceRunner`. The driver holds no wall-clock state.

class ReplayDriver {
public:
    // Construct a driver bound to `trace`. The driver does not own the
    // trace; the caller (TraceRunner) keeps it alive for the run.
    [[nodiscard]] static auto bind(const Trace& trace) noexcept
        -> Result<ReplayDriver>;

    ReplayDriver(ReplayDriver&&) noexcept;
    ReplayDriver& operator=(ReplayDriver&&) noexcept;
    ReplayDriver(const ReplayDriver&)            = delete;
    ReplayDriver& operator=(const ReplayDriver&) = delete;
    ~ReplayDriver();

    // Advance to `frame`. Yields every InputOp recorded at that frame
    // in recorded order (§4.1.6 inv 1, 4); never re-yields, never reads
    // wall-clock. Empty span when the frame has no inputs.
    [[nodiscard]] auto advance(FrameIndex frame) noexcept
        -> std::span<const ::glibre::platform::InputEvent* const>;

    // Adapts this driver to the platform::InputDriver seam (§3.3,
    // §4.2 cross-aggregate inv 5). The returned reference is valid
    // for the driver's lifetime.
    [[nodiscard]] auto as_platform_driver() noexcept -> ::glibre::platform::InputDriver&;

private:
    ReplayDriver() noexcept = default;
    struct Impl;
    Impl* impl_{nullptr};
};

// ---------------------------------------------------------------------------
// 5.9  InjectionLayer + RunnerHost (§4.1.8, §4.1.9)
// ---------------------------------------------------------------------------
//
// Sealed sums; closed at compile time. Selecting a layer outside its
// host's permitted set returns Error::InjectionRefused at gate time
// (§4.1.9 inv 2, 4).

namespace injection {

struct InProcess {
    constexpr bool operator==(const InProcess&) const noexcept = default;
};

struct PerProcess {
    std::uint32_t target_pid{0};
    std::uint64_t target_window_id{0};  // OS-specific window handle.
    constexpr bool operator==(const PerProcess&) const noexcept = default;
};

struct OsAutomation {
    // CI-isolated only (§4.1.8 inv 3).
    constexpr bool operator==(const OsAutomation&) const noexcept = default;
};

}  // namespace injection

using InjectionLayer = std::variant<
    injection::InProcess,
    injection::PerProcess,
    injection::OsAutomation>;

namespace host {

struct DevHeadless    { constexpr bool operator==(const DevHeadless&)    const noexcept = default; };
struct DevInteractive { constexpr bool operator==(const DevInteractive&) const noexcept = default; };
struct CiHeadless     { constexpr bool operator==(const CiHeadless&)     const noexcept = default; };
struct CiIsolated     { constexpr bool operator==(const CiIsolated&)     const noexcept = default; };

}  // namespace host

using RunnerHost = std::variant<
    host::DevHeadless,
    host::DevInteractive,
    host::CiHeadless,
    host::CiIsolated>;

// Detect the live RunnerHost from environment markers; conservative —
// ambiguous environments downgrade (§4.1.9 inv 3). The runner never
// silently widens permissions.
[[nodiscard]] auto detect_runner_host() noexcept -> RunnerHost;

// Policy table — single source of truth (§4.1.9 inv 2). Returns
// Error::InjectionRefused if the pair is not permitted.
[[nodiscard]] auto check_injection_permitted(const InjectionLayer&,
                                             const RunnerHost&) noexcept -> Result<void>;

// ---------------------------------------------------------------------------
// 5.10 TraceReport + DivergenceReport (§4.1.12)
// ---------------------------------------------------------------------------
//
// One TraceReport per run (inv 1). DivergenceReport requires two runs
// and a `--compare <prior-report>` invocation (inv 2). Wall-clock is
// informational only (inv 3).

namespace report_status {

struct Passed {
    constexpr bool operator==(const Passed&) const noexcept = default;
};

struct Failed {
    AssertOpId failing_op{};
    FrameIndex frame{};
    Error      error{TraceParse{}};  // the e2e::Error arm matching the failure.
};

struct Aborted {
    Error reason{TraceParse{}};
};

}  // namespace report_status

using ReportStatus = std::variant<
    report_status::Passed,
    report_status::Failed,
    report_status::Aborted>;

class TraceReport {
public:
    [[nodiscard]] auto status()         const noexcept -> const ReportStatus&;
    [[nodiscard]] auto trace_path()     const noexcept -> const ::glibre::platform::CanonicalPath&;
    [[nodiscard]] auto env_hash()       const noexcept -> EnvHash;
    [[nodiscard]] auto layer()          const noexcept -> const InjectionLayer&;
    [[nodiscard]] auto host()           const noexcept -> const RunnerHost&;
    [[nodiscard]] auto frames_observed()const noexcept -> std::uint64_t;
    [[nodiscard]] auto wall_duration()  const noexcept -> std::chrono::nanoseconds;
    [[nodiscard]] auto artefacts()      const noexcept -> std::span<const ArtefactRef>;
    [[nodiscard]] auto artefact_root()  const noexcept -> const ::glibre::platform::CanonicalPath&;

    TraceReport(TraceReport&&) noexcept;
    TraceReport& operator=(TraceReport&&) noexcept;
    TraceReport(const TraceReport&)            = delete;
    TraceReport& operator=(const TraceReport&) = delete;
    ~TraceReport();

private:
    friend class TraceRunner;
    TraceReport() noexcept = default;
    struct Impl;
    Impl* impl_{nullptr};
};

struct DivergenceSite {
    FrameIndex first_diff{};
    AssertOpId diverging_op{};  // zero-id when divergence is on input.
    ArtefactRef left;
    ArtefactRef right;
};

class DivergenceReport {
public:
    [[nodiscard]] auto site()  const noexcept -> DivergenceSite;
    [[nodiscard]] auto left()  const noexcept -> const TraceReport&;
    [[nodiscard]] auto right() const noexcept -> const TraceReport&;

    DivergenceReport(DivergenceReport&&) noexcept;
    DivergenceReport& operator=(DivergenceReport&&) noexcept;
    DivergenceReport(const DivergenceReport&)            = delete;
    DivergenceReport& operator=(const DivergenceReport&) = delete;
    ~DivergenceReport();

private:
    friend class TraceRunner;
    DivergenceReport() noexcept = default;
    struct Impl;
    Impl* impl_{nullptr};
};

// ---------------------------------------------------------------------------
// 5.11 TraceRunner (§4.1.7)
// ---------------------------------------------------------------------------
//
// Orchestration boundary. Gate-before-drive (inv 1); frame-step
// deterministic (inv 2); single-binary per run (inv 3); fail-fast on
// assert (inv 4); always emits a report (inv 5); frame-budget enforced
// (inv 6); no exceptions cross the boundary (inv 7).

// Host services the runner needs from the binary under test. The
// concrete adapter is injected by the caller — for in-process runs
// the adapter is the engine's frame-loop façade; for per-process /
// os-automation runs the adapter is a thin shim over the OS injection
// API. The runner does not depend on any specific binary topology.
class RunnerHostAdapter {
public:
    virtual ~RunnerHostAdapter() = default;

    // Launch / attach to the binary under test; returns an opaque
    // handle the runner uses for subsequent calls. Fails with
    // Error::DriverInstall if the binary does not expose the seam.
    [[nodiscard]] virtual auto launch(const Trace&,
                                      const InjectionLayer&) noexcept
        -> Result<void> = 0;

    // Compute the live EnvHash from the running process's environment.
    [[nodiscard]] virtual auto live_env_hash() noexcept -> Result<EnvHash> = 0;

    // Install the ReplayDriver at the platform::InputDriver seam.
    // Called after the EnvHash gate passes (§4.1.3 inv 1).
    [[nodiscard]] virtual auto install_driver(ReplayDriver&) noexcept
        -> Result<void> = 0;

    // Advance one engine frame. Returns Error::Timeout if the engine
    // failed to advance within the runner's safety window.
    [[nodiscard]] virtual auto advance_frame() noexcept -> Result<FrameIndex> = 0;

    // Evaluate a single AssertOp against the live process state.
    // Failure returns Error::AssertFailed populated by the adapter.
    [[nodiscard]] virtual auto evaluate(const AssertState&)        noexcept -> Result<void> = 0;
    [[nodiscard]] virtual auto evaluate(const AssertScreenshot&,
                                        GoldenStore&)              noexcept -> Result<void> = 0;
    [[nodiscard]] virtual auto evaluate(const AssertEcsSnapshot&,
                                        GoldenStore&)              noexcept -> Result<void> = 0;
    [[nodiscard]] virtual auto evaluate(const AssertLogContains&)  noexcept -> Result<void> = 0;

    // Gracefully end the run on End op; non-zero exit before End
    // surfaces as Error::BinaryCrash with the dump reference.
    [[nodiscard]] virtual auto shutdown() noexcept -> Result<void> = 0;
};

struct TraceRunnerConfig {
    InjectionLayer layer{injection::InProcess{}};
    // Optional override; when unset the runner calls detect_runner_host().
    std::optional<RunnerHost> host{};
    // Path to a prior TraceReport for divergence-mode runs (§4.1.12 inv 2).
    std::optional<::glibre::platform::CanonicalPath> compare_to{};
};

class TraceRunner {
public:
    [[nodiscard]] static auto create(RunnerHostAdapter& adapter,
                                     GoldenStore&       store,
                                     TraceRunnerConfig  config = {}) noexcept
        -> Result<TraceRunner>;

    TraceRunner(TraceRunner&&) noexcept;
    TraceRunner& operator=(TraceRunner&&) noexcept;
    TraceRunner(const TraceRunner&)            = delete;
    TraceRunner& operator=(const TraceRunner&) = delete;
    ~TraceRunner();

    // Drive `trace` to completion. Always emits a TraceReport
    // (§4.1.7 inv 5) — pass, fail, parse, drift, golden-missing,
    // driver-install, injection-refused, timeout, binary-crash.
    [[nodiscard]] auto run(const Trace& trace) noexcept -> Result<TraceReport>;

    // Divergence-mode run: compares against `config.compare_to` and
    // emits a DivergenceReport when the two runs diverge. Returns the
    // single-run TraceReport when they agree.
    [[nodiscard]] auto run_with_compare(const Trace& trace) noexcept
        -> Result<std::variant<TraceReport, DivergenceReport>>;

    // Stable non-zero exit codes — the public CI contract (§4.1.13 inv 4).
    [[nodiscard]] static constexpr auto exit_code(const Error& e) noexcept
        -> std::int32_t {
        return std::visit(
            [](auto const& arm) noexcept -> std::int32_t {
                using T = std::remove_cvref_t<decltype(arm)>;
                if constexpr (std::is_same_v<T, TraceParse>)        return 10;
                else if constexpr (std::is_same_v<T, EnvDrift>)     return 11;
                else if constexpr (std::is_same_v<T, AssertFailed>) return 12;
                else if constexpr (std::is_same_v<T, DriverInstall>)return 13;
                else if constexpr (std::is_same_v<T, InjectionRefused>) return 14;
                else if constexpr (std::is_same_v<T, GoldenMissing>) return 15;
                else if constexpr (std::is_same_v<T, Timeout>)      return 16;
                else if constexpr (std::is_same_v<T, BinaryCrash>)  return 17;
                else                                                return 1;
            },
            e);
    }

private:
    TraceRunner() noexcept = default;
    struct Impl;
    Impl* impl_{nullptr};
};

// ---------------------------------------------------------------------------
// 5.12 ClosureGate (§4.1.14)
// ---------------------------------------------------------------------------
//
// The CI step that flips a `type:user-story` from "in progress" to
// "qa-ready". Pure function of the TraceReports a story cites; no
// human override, no developer-host green substitute (§4.1.14 inv 1,
// 2, 4). All-green-or-no-flip rule reified at the spec level.

struct UserStoryRef {
    // GitHub issue number of the user-story whose closure this gate
    // evaluates. Resolution to the trace set lives in CI policy.
    std::uint64_t issue_number{0};
    constexpr bool operator==(const UserStoryRef&) const noexcept = default;
};

enum class ClosureDecision : std::uint8_t {
    QaReady,    // every cited trace reported Passed.
    Withhold,   // at least one trace reported Failed/Aborted; story stays open.
};

class ClosureGate {
public:
    // Evaluate the gate for `story` against `reports`. Returns
    // QaReady iff every report's status is Passed (§4.1.14 inv 1, 4).
    [[nodiscard]] static auto evaluate(UserStoryRef                       story,
                                       std::span<const TraceReport* const> reports) noexcept
        -> Result<ClosureDecision>;

    ClosureGate()                              = delete;
    ClosureGate(const ClosureGate&)            = delete;
    ClosureGate& operator=(const ClosureGate&) = delete;
};

}  // namespace glibre::e2e
```

Event types listed above are the `TraceOp` sealed sum (§4.1.4), the
`InputOp` thin wrapper around `platform::InputEvent` (§4.1.5), the
`AssertOp` family (`AssertState`, `AssertScreenshot`,
`AssertEcsSnapshot`, `AssertLogContains`, plus the terminating
`End`), the `report_status` sub-sum (`Passed | Failed | Aborted`),
and the `e2e::Error` closed sum (§4.1.13). Schemas: e2e exposes no
Fory-serialised schema in this surface — `.glibre-trace` files are
parsed by `Trace::load` into the in-memory aggregate and `TraceReport`
is consumed in-process by `ClosureGate`; the on-disk Fory schemas for
both live in §7 below. Error type: `glibre::e2e::Error`, the closed
sum from §4.1.13, contributed as one arm of the engine-wide
`glibre::Error` variant per `reviews/decisions/error-model.md`. CI
maps each variant to the stable non-zero exit codes returned by
`TraceRunner::exit_code` — the published contract `ClosureGate` and
external reviewers gate on.

## 6. Internal Architecture

Non-binding sketch for implementers.

## 7. Persistence & Schemas

The e2e context's persistence surface is the `.glibre-trace` corpus —
the **playable evidence** every `type:user-story` cites. Per §3.3 the
producer side (`tools::TraceWriter`) is owned outside e2e; this
section pins the **consumer-side bytes** the runner ingests, so
authoring tools and the runner cannot drift. Per the `Trace`
aggregate's reason-to-change boundary (§4.1.1) and the §4.2
cross-aggregate "read-only post-parse" invariant, there is no in-place
trace mutation; persistence here is read-only on the e2e side and
write-once on the authoring side.

Per `reviews/decisions/fory-codegen.md`, every persistent type below
is authored as `data/schemas/e2e/<Type>.fory` and rides the
`glibre-foryc` → `glibre-types.dylib` pipeline. FQNs are
`glibre.e2e.<Type>`. The schema-source hashes contribute to
`glibre_types_abi_hash` per data SPEC §7.2.3; a bump invalidates the
trace corpus by definition (§7.4 below). One exception is the
`GoldenImage` PNG payload, which is **not** Fory-encoded — only its
metadata index is. The collapse rationale is in §3.2 #4 (one
`GoldenStore` with one update path) and is reified by §7.1.4.

Each schema ships with at least one Catch2 round-trip test under
`tests/data/schemas/e2e/<Type>.cpp` per the data SPEC §7 mandate; the
trace-corpus migration tests live alongside under
`tests/e2e/persistence/`.

### 7.1 Persistent types

#### 7.1.1 `TraceFile` — the `.glibre-trace` byte container

**File:** `data/schemas/e2e/TraceFile.fory`
**FQN:** `glibre.e2e.TraceFile`
**Lifetime scope:** repo-checked-in evidence. Authored once per
user-story by the editor's record mode, committed under
`tests/e2e/<ctx>/<story>.glibre-trace`, read-only on the e2e side
(§4.1.2 inv 1).

`TraceFile` is the **on-disk shape** of one `Trace` (§4.1.1): a
versioned magic + Fory schema version, the embedded `TraceManifest`
(§7.1.2), the framed ordered `(FrameIndex, TraceOp)` stream (the
"frame-locked TraceOp sequence" the spike requires), the terminating
`TraceOp::End`, and a `Blake3Hash` footer over the canonical Fory
encoding of manifest + stream. The bytes are the unit the runner
parses; nothing else in e2e opens this file.

```fory
schema glibre.e2e.TraceFile {
  version 1
  since   "0.1.0"

  field magic           : u32                tag 1 since 1   // "GLTR" (0x47_4C_54_52, little-endian)
  field schema_version  : u32                tag 2 since 1
  field manifest        : TraceManifest      tag 3 since 1
  field stream          : list<TraceFrameOp> tag 4 since 1
  field footer_hash     : bytes              tag 5 since 1   // Blake3-256 of canonical(manifest + stream)
}

schema glibre.e2e.TraceFrameOp {
  version 1
  since   "0.1.0"

  field frame_index : u64     tag 1 since 1
  field op          : TraceOp tag 2 since 1
}
```

`TraceOp` and `InputOp` are sealed-sum schemas authored beside this
file; their byte layouts are pinned in §7.1.5 / §7.1.6. The order of
entries in `stream` is the recorded intra-frame order (§4.1.1
"intra-frame order preserved") and is part of the canonical encoding
the footer hashes.

**Invariants** (echoing §4.1.1 / §4.1.2 where the runtime aggregate
enforces them):

1. **Magic-first parse.** The Fory decoder reads the leading `magic`
   field before anything else; a value other than `0x47_4C_54_52`
   short-circuits with `E2eError::TraceParse`. This is the bytes-level
   form of §4.1.2 inv 3.
2. **Schema-version bumps trigger explicit re-record.** A `TraceFile`
   whose `schema_version` does not match the current build's value is
   `E2eError::TraceParse`; there is **no** silent up-conversion of
   trace bytes (§4.1.2 inv 3). Re-recording is the only path forward;
   migration of trace files across schema bumps is by replaying the
   user-story under the new build, never by codegen migration.
3. **Footer covers everything.** `footer_hash` is exactly 32 bytes
   (Blake3-256 of `canonical_encode(manifest) ++ canonical_encode(stream)`);
   any byte mutation between disk and parsed `Trace` is rejected with
   `E2eError::TraceParse` (§4.1.1 inv 4). Lengths or hashes outside
   the 32-byte fixed shape are also `TraceParse`.
4. **Stream is monotonic with one terminating `End`.** Parsed
   ordering enforces §4.1.1 inv 2 (`frame_index_i ≤ frame_index_{i+1}`)
   and inv 5 (exactly one `TraceOp::End`, last). Both are
   parse-time validations, not run-time.
5. **One trace per file (§4.1.2 inv 4).** The schema admits exactly
   one stream; bundling concerns belong to `content`, not e2e.
6. **Canonical-path discipline (§4.1.2 inv 2).** The file is opened
   through `platform::CanonicalPath`; raw paths never reach the
   decoder. The schema itself stores no path — addressing is the
   caller's concern.
7. **Authoring-side parity.** `tools::TraceWriter` (§3.3) emits
   bytes that round-trip byte-equal through this schema; a Catch2
   test under `tests/e2e/persistence/round_trip.cpp` enforces
   `parse(write(trace)) == trace` for the meta-corpus.

#### 7.1.2 `TraceManifest` — environment header + golden refs + runner-host hint

**File:** `data/schemas/e2e/TraceManifest.fory`
**FQN:** `glibre.e2e.TraceManifest`
**Lifetime scope:** embedded by-value inside every `TraceFile`;
read-only post-parse (§4.1.3 inv 4).

The manifest is the **gate** for replay — its Fory encoding is the
input to `EnvHash` (one of the two §3.2 #3 collapses), and its
`golden_refs` list is the up-front existence check that turns "missing
golden" into a gate-time refusal rather than an assert-time surprise
(§4.1.3 inv 3). The `runner_host_hint` field is the
`target_driver_tier` (§4.1.3 composition bullet 8) made byte-explicit:
the recorded host class the trace was authored under.

```fory
schema glibre.e2e.TraceManifest {
  version 1
  since   "0.1.0"

  // --- Engine identity --------------------------------------------------
  field engine_semver        : string  tag 1  since 1
  field engine_git_sha        : string  tag 2  since 1   // 40-char lowercase hex
  field plugin_abi_hash       : string  tag 3  since 1   // 64-char lowercase hex Blake3-256, mirrors data §7.2.3 abi_hash_hex
  field asset_pack_hash       : string  tag 4  since 1   // 64-char lowercase hex Blake3-256 over the cooked content bundle

  // --- Replay environment -----------------------------------------------
  field locale                : string  tag 5  since 1   // BCP-47 tag
  field window_logical_w      : u32     tag 6  since 1   // platform::LogicalSize at record
  field window_logical_h      : u32     tag 7  since 1
  field dpi_scale_x1000       : u32     tag 8  since 1   // platform::DpiScale * 1000, rounded
  field rng_seed              : u64     tag 9  since 1
  field runner_host_hint      : u8      tag 10 since 1   // RunnerHostTier (§7.1.7)

  // --- Golden references the stream cites --------------------------------
  field golden_refs           : list<GoldenRef>  tag 11 since 1

  // --- Frame budget (§4.1.7 inv 6) --------------------------------------
  field declared_frame_count  : u64     tag 12 since 1
  field frame_budget_safety_x100 : u32  tag 13 since 1   // multiplier × 100; e.g. 200 = 2×
}

schema glibre.e2e.GoldenRef {
  version 1
  since   "0.1.0"

  field assert_op_id  : u64    tag 1 since 1   // matches AssertOpId in §5
  field kind          : u8     tag 2 since 1   // GoldenKind enum: Image | EcsSnapshot
  field store_path    : string tag 3 since 1   // GoldenStore-relative, slash-separated
  field content_hash  : bytes  tag 4 since 1   // 32 bytes Blake3-256 of payload at record (§7.1.3 invariant 3)
  field tolerance     : option<PixelTolerance> tag 5 since 1   // present iff kind == Image
}

schema glibre.e2e.PixelTolerance {
  version 1
  since   "0.1.0"

  field max_per_pixel_dE_x1000   : u32   tag 1 since 1   // CIEDE2000 ΔE × 1000
  field max_pct_differing_x10000 : u32   tag 2 since 1   // percentage × 10000 (i.e. 50000 = 5%)
  field tier_name                : string tag 3 since 1   // "" iff per-assert override only
  field region_mask              : list<RegionRect> tag 4 since 1
}

schema glibre.e2e.RegionRect {
  version 1
  since   "0.1.0"

  field x : u32 tag 1 since 1
  field y : u32 tag 2 since 1
  field w : u32 tag 3 since 1
  field h : u32 tag 4 since 1
}
```

**Invariants** (echoing §4.1.3, §4.1.11):

1. **All `EnvHash` inputs frozen.** Fields tag 1-10 are the
   `EnvHash` preimage; their byte representation is the canonical
   Fory encoding of the manifest with `golden_refs`,
   `declared_frame_count`, and `frame_budget_safety_x100` masked
   out (these last three are *trace shape*, not *environment*, and
   participate in the footer hash via the stream-level encoding
   instead). `EnvHash = blake3_256(canonical_encode(manifest_with_env_fields_only))`.
   Adding, removing, or reshaping any of tags 1-10 invalidates every
   pre-existing `.glibre-trace` in the repo by construction; this is
   §4.1.3 inv 2 reified at the bytes level. See §7.4 for the
   migration consequence.
2. **Golden-ref content-addressing.** `GoldenRef.content_hash` is the
   Blake3-256 of the referenced payload **at record time** (PNG bytes
   for `Image`, Fory-encoded `EcsSnapshot` bytes for `EcsSnapshot`).
   The runner re-hashes the on-disk payload at gate time and compares;
   mismatch = `E2eError::GoldenMissing` (the same arm covers
   absent-and-substituted cases — see the §10 mapping).
3. **Hash widths fixed.** `engine_git_sha` is exactly 40 hex chars;
   `plugin_abi_hash` and `asset_pack_hash` are exactly 64 hex chars
   (matching the data §7.2.3 `abi_hash_hex` shape); `content_hash`
   and the file-level `footer_hash` are exactly 32 raw bytes. Any
   other length is `E2eError::TraceParse`.
4. **`runner_host_hint` is closed.** Value set is fixed at
   `{0=DevHeadless, 1=DevInteractive, 2=CiHeadless, 3=CiIsolated}`
   (§7.1.7). A loaded value outside this set is
   `E2eError::TraceParse`, never silently saturated — same closed-sum
   discipline `render` §7.1.1 applies to its enums.
5. **Frame budget is content-derived.** `declared_frame_count` is
   the number of frame indices observed at record;
   `frame_budget_safety_x100 ≥ 100` (the runner refuses to run a
   trace whose declared budget is below the recorded length —
   `E2eError::TraceParse`).

#### 7.1.3 `GoldenStoreIndex` — manifest-of-goldens for a trace corpus

**File:** `data/schemas/e2e/GoldenStoreIndex.fory`
**FQN:** `glibre.e2e.GoldenStoreIndex`
**Lifetime scope:** one index per repo, checked in alongside the
`.glibre-trace` corpus at `tests/e2e/golden-store/index.fory`. Read at
`TraceRunner` construction time **before** any `TraceFile` is parsed,
to amortise existence checks across all traces in a CI run (§4.1.10
inv 2 collapse).

The index is the **`(trace_path, env_hash) → (golden_image_hash,
tolerance)` map** the spike calls out. It serves three purposes:

1. **Up-front existence pre-check** — the runner can refuse the whole
   batch when an index entry is missing, rather than discovering a
   `GoldenMissing` mid-run (§4.1.3 inv 3 / §4.1.10 inv 2).
2. **Content-addressed audit** — every reference's payload hash is
   recorded once in the index, so a payload mutation produces a clean
   `IndexHashMismatch` diagnostic instead of a silent green-to-red
   flap.
3. **Tolerance pinning** — the per-`AssertScreenshot` `PixelTolerance`
   is stored both inline in the trace (§7.1.2) **and** in the index;
   the two must agree at gate time. The duplication is deliberate so
   editing the index alone (without re-recording the trace) is
   refused.

```fory
schema glibre.e2e.GoldenStoreIndex {
  version 1
  since   "0.1.0"

  field schema_version : u32                       tag 1 since 1
  field entries        : list<GoldenStoreEntry>    tag 2 since 1
  field entry_count    : u32                       tag 3 since 1   // mirrors len(entries) for cheap audits
  field index_blake3   : bytes                     tag 4 since 1   // Blake3-256 over canonical(entries) sorted by key
}

schema glibre.e2e.GoldenStoreEntry {
  version 1
  since   "0.1.0"

  // --- Key (composite, sorted lex) --------------------------------------
  field trace_path     : string  tag 1 since 1   // workspace-relative, slash-separated, NFC, no leading "./"
  field env_hash_hex   : string  tag 2 since 1   // 64-char lowercase hex Blake3-256 of the trace's TraceManifest
  field assert_op_id   : u64     tag 3 since 1   // matches GoldenRef.assert_op_id

  // --- Value ------------------------------------------------------------
  field kind           : u8      tag 4 since 1   // GoldenKind: 0=Image, 1=EcsSnapshot
  field store_path     : string  tag 5 since 1   // GoldenStore-relative, e.g. "tests/e2e/render/golden/triangle.png"
  field content_hash   : bytes   tag 6 since 1   // 32-byte Blake3-256 of the on-disk payload
  field byte_size      : u64     tag 7 since 1   // payload size in bytes (cheap drift fence)
  field tolerance      : option<PixelTolerance> tag 8 since 1   // present iff kind == Image
  field recorded_at_unix_ms : u64 tag 9 since 1  // informational only; not part of index_blake3
}
```

**Invariants:**

1. **Composite key uniqueness.** `(trace_path, env_hash_hex,
   assert_op_id)` is unique across `entries`; duplicates =
   `E2eError::TraceParse` (the index parses through the same arm
   the trace files do — there is one parse-failure surface per
   §4.1.13). The composite is the `(trace path + env)` mapping the
   spike requires.
2. **Sort order is canonical.** `entries` is sorted ascending by
   `(trace_path, env_hash_hex, assert_op_id)` (lexicographic byte
   order); out-of-order lists are `E2eError::TraceParse`. This makes
   `index_blake3` a faithful audit artefact (mirroring data §7.2.3's
   `entries` rule).
3. **`index_blake3` covers the value tuple too.** Computed as
   `blake3_256( canonical_encode( entries ) )` over fields 1-8 of
   each entry (`recorded_at_unix_ms` is excluded so re-running the
   recorder on the same payload does not perturb the hash).
4. **Cross-check against `TraceManifest.golden_refs`.** At
   `TraceRunner` construction the gate joins `index.entries` against
   `manifest.golden_refs` on `(trace_path, assert_op_id)` and
   asserts byte-equality of `(content_hash, tolerance)` between the
   two surfaces. Mismatch = `E2eError::GoldenMissing` (extended:
   the trace and the index disagree about the reference); editing
   one without the other is refused.
5. **Index missing = whole-batch refusal.** If `index.fory` is
   absent at `TraceRunner` construction, the run aborts with
   `E2eError::GoldenMissing` before any `TraceFile` is opened. This
   collapses "no index" and "missing entry" into one failure arm
   per the §3.2 #4 collapse; CI cannot silently degrade to "no
   golden checking".
6. **`entry_count` is redundant on purpose.** `entry_count` MUST
   equal `len(entries)`; mismatch = `E2eError::TraceParse`. Cheap
   bit-rot check; the field is also useful for tooling that wants
   the row count without decoding the full list.

#### 7.1.4 `GoldenImage` — PNG payload, NOT Fory-encoded

**Storage:** raw PNG files under
`tests/e2e/<ctx>/golden/<file>.png`. **No `.fory` schema.**
**Lifetime scope:** repo-checked-in reference asset; read-only at run
time (§4.1.10 inv 1).

The PNG payload is **deliberately excluded** from the Fory codegen
pipeline. Three reasons:

1. **Existing image-format tooling.** PNGs are reviewable with any
   image viewer / git-diff plugin, sized correctly by browsers, and
   linked in PR bodies. Wrapping them in a Fory envelope would be
   pure friction.
2. **`GoldenStoreIndex` already provides the metadata layer.** The
   index records every property the spine cares about
   (`content_hash`, `byte_size`, `tolerance`, `kind`); the image
   itself is opaque payload to the dispatcher.
3. **Round-trip semantics differ.** Fory schemas are migration-aware
   (`reviews/decisions/fory-codegen.md` §"Migration Mechanic"); image
   payloads are content-addressed and re-record-only — exactly the
   `PSOCacheRecord` "invalidate, never migrate" pattern from
   `render` §7.2.2.

The contract the runner relies on:

1. **PNG profile fixed.** Reference PNGs are sRGB color space, 8-bit
   per channel, no alpha for the MVP visual asserts. Other profiles
   produce a deterministic decode error mapped to
   `E2eError::TraceParse` (the same arm covers any reference-asset
   parse failure).
2. **Address by index, never by glob.** The runner addresses a PNG
   only via a `GoldenStoreEntry.store_path`; filesystem walks are
   forbidden in the runner per §4.1.10 inv 2. The corollary: a PNG
   on disk with no index entry is invisible to the runner — exactly
   the discipline that prevents silent shadow goldens.
3. **EcsSnapshot is the symmetric Fory-encoded counterpart.** Every
   `AssertEcsSnapshot` op references a Fory-encoded
   `EcsSnapshot.fory` blob beside the PNG; that one **does** ride
   the codegen pipeline because its bytes are already a Fory
   payload by construction. The schema is owned by the `World`'s
   originating context, not by e2e — e2e references it by hash, not
   by name. (This is the same "we own pointers, you own the bytes"
   pattern `render` §7.1.2 uses for `archive_blob`.)

The "PNG payload — NOT Fory; only metadata indexed" requirement from
the spike's persistent-types list is satisfied by the
§7.1.3 + §7.1.4 split above: the index is indexed, the image is not.

#### 7.1.5 `TraceOp` — sealed-sum schema, frame-locked variants

**File:** `data/schemas/e2e/TraceOp.fory`
**FQN:** `glibre.e2e.TraceOp`

The sum is closed at compile time per §4.1.4 inv 1; the schema below
pins the wire-form.

```fory
schema glibre.e2e.TraceOp {
  version 1
  since   "0.1.0"

  // Closed sum encoded as discriminator + per-variant payload union.
  // Tag values are the variant ids; new variants take new tag values
  // (additive only — see §7.2 below).
  field variant_tag : u8                  tag 1 since 1
  field input_op    : option<InputOp>             tag 2 since 1   // present iff variant_tag == 0
  field assert_state          : option<AssertStatePayload>      tag 3 since 1   // 1
  field assert_screenshot     : option<AssertScreenshotPayload> tag 4 since 1   // 2
  field assert_ecs_snapshot   : option<AssertEcsSnapshotPayload> tag 5 since 1  // 3
  field assert_log_contains   : option<AssertLogContainsPayload> tag 6 since 1  // 4
  field end_marker            : option<EndPayload> tag 7 since 1  // 5
}

schema glibre.e2e.AssertStatePayload {
  version 1
  since   "0.1.0"

  field op_id              : u64    tag 1 since 1
  field component_path     : string tag 2 since 1   // dotted path, e.g. "scene.player.transform.translation"
  field expected_fory_blob : bytes  tag 3 since 1   // canonical Fory encoding of the expected value
}

schema glibre.e2e.AssertScreenshotPayload {
  version 1
  since   "0.1.0"

  field op_id      : u64           tag 1 since 1
  field golden_ref : GoldenRef     tag 2 since 1   // the runner cross-checks against the index (§7.1.3 inv 4)
}

schema glibre.e2e.AssertEcsSnapshotPayload {
  version 1
  since   "0.1.0"

  field op_id      : u64       tag 1 since 1
  field world_id   : string    tag 2 since 1   // known world tag; validated at parse (§4.1.4 inv 3)
  field golden_ref : GoldenRef tag 3 since 1
}

schema glibre.e2e.AssertLogContainsPayload {
  version 1
  since   "0.1.0"

  field op_id  : u64    tag 1 since 1
  field needle : string tag 2 since 1
  field is_regex : bool tag 3 since 1
}

schema glibre.e2e.EndPayload {
  version 1
  since   "0.1.0"
  // intentionally empty; the marker carries no fields. Present so the
  // option<EndPayload> can be a non-null tag.
}
```

**Invariants:**

1. **Discriminator gates payload presence.** Exactly one of
   `input_op`, `assert_state`, `assert_screenshot`,
   `assert_ecs_snapshot`, `assert_log_contains`, `end_marker` is
   `Some(...)` for any given `TraceOp`; the other five are `None`.
   The chosen one corresponds to `variant_tag`. Mismatch =
   `E2eError::TraceParse` (the §4.1.4 inv 1 closed-variant rule
   reified in bytes).
2. **Unknown `variant_tag` is `TraceParse`, not forward-compat.**
   Per §4.1.1 inv 3, the parser does not silently skip unknown
   ops. Adding a variant is a deliberate central edit — see §7.2.
3. **Per-variant payload validity.** `expected_fory_blob` is a
   non-empty `bytes`; `golden_ref.content_hash` is exactly 32
   bytes; `world_id` is non-empty; `needle` length ≤ 4096 bytes.
   Failures are `E2eError::TraceParse` at parse time, not at
   replay time (§4.1.4 inv 3).

#### 7.1.6 `InputOp` — wraps `platform::InputEvent` byte-equal

**File:** `data/schemas/e2e/InputOp.fory`
**FQN:** `glibre.e2e.InputOp`

```fory
schema glibre.e2e.InputOp {
  version 1
  since   "0.1.0"

  field event : InputEvent tag 1 since 1   // glibre.platform.InputEvent (cross-context reference)
}
```

`InputEvent` is the `platform::InputEvent` schema from
`specs/platform/SPEC.md` §7; e2e references it without re-declaring
its sealed-variant set. When `platform::InputEvent` gains a variant
(§4.1.5 inv 2), its schema-version bump propagates into
`glibre_types_abi_hash`; per §7.4 below, that bump invalidates the
trace corpus by construction, which is the correct outcome — input
recordings against an old SDL3 pump are not replayable against a new
one without re-recording.

#### 7.1.7 `RunnerHostTier` — closed enum mirroring §4.1.9

```fory
# (declared inline as a u8 in TraceManifest; no separate file.)
# 0 = DevHeadless, 1 = DevInteractive, 2 = CiHeadless, 3 = CiIsolated
```

The single source of truth is the `RunnerHost` value object (§4.1.9
inv 2 policy table); the manifest field carries the **recorded** host
tier as an advisory hint. The runner uses it to detect mismatch
against the **live** `RunnerHost` and, where the policy table forbids
the pair, returns `E2eError::InjectionRefused` at gate time (§4.1.9
inv 4).

### 7.2 Migration rules

Per `reviews/decisions/fory-codegen.md` §"Migration Mechanic", every
schema-version bump emits a generated dispatcher hookup. e2e owns the
migration *bodies* for the types above; their *plumbing* is generated.

#### 7.2.1 `TraceOp` variant additions — additive only

Adding a `TraceOp` variant follows the **additive defaulted-discriminator**
pattern:

1. **Adding a variant takes the next free `variant_tag` value and a
   new `option<...Payload>` field at a new tag.** Old payload
   options remain in the schema with `since` matching their original
   version. Old `.glibre-trace` payloads decode under the new build
   because the new option<> defaults to `None` for any frame whose
   `variant_tag` predates this version.
2. **Codegen emits `migrate_TraceOp_v<N>_to_v<N+1>` as the identity
   mapping.** No hand-written body is required for the additive
   case; the codegen tool (data §7.2 / fory-codegen.md) emits the
   identity dispatcher and the round-trip golden test under
   `tests/e2e/persistence/trace_op_migration_v<N>_to_v<N+1>.cpp`
   asserts the recorded `vN` corpus replays unchanged under
   `vN+1` semantics — same pattern as `render` §7.2.1 case 1.
3. **Removing or renaming a variant is breaking.** The discriminator
   value becomes `reserved` (per data §7.1 reserved-clause rule);
   the option<> field stays but the codegen emits a hard
   `E2eError::TraceParse` for any input carrying the reserved
   value. A rename is not in scope for MVP.
4. **Variant-payload field changes follow the same additive rule
   one level deeper.** Adding a field to `AssertScreenshotPayload`
   (e.g. a `clipped_to_window` bool) takes the next free tag and a
   `default` clause; the recorded corpus replays under the new
   default.

#### 7.2.2 `AssertOp` payload additions — additive only

The four assert-payload schemas (`AssertStatePayload`,
`AssertScreenshotPayload`, `AssertEcsSnapshotPayload`,
`AssertLogContainsPayload`) follow the same additive rule. New
fields take new tags with `default` clauses; codegen synthesises the
default for older payloads. New assert kinds are *not* added by
extending an existing payload — they are new `TraceOp` variants per
§7.2.1.

#### 7.2.3 Manifest env-hash inputs — frozen, never migrated

The fields named in §7.1.2 invariant 1 (tags 1-10 of `TraceManifest`)
are the **`EnvHash` preimage**. Per §3.2 #3 (one hash, one refusal,
one diagnosis), they are **frozen**: any change — addition, removal,
or shape — invalidates every pre-existing `.glibre-trace` in the
repo by definition.

Implementation: the `TraceManifest` schema does **not** declare any
`migration` clauses for the env-input fields. Any future bump
forces the author to either (a) re-record the entire trace corpus
under the new manifest version (the only allowed path), or (b)
explicitly add an `EnvHash`-preserving migration body — which review
will reject because the whole point of `EnvDrift` is that
environment changes must be detected, not migrated through.

This is **structurally identical** to `render` §7.2.2's
"`PSOCacheRecord` — invalidate, never migrate" pattern, and matches
data §7.4's freeze-and-bump rule for breaking changes. The
golden-update workflow (§4.1.10 inv 3) is the audit trail for the
re-record.

Trace-shape fields (tags 11-13: `golden_refs`,
`declared_frame_count`, `frame_budget_safety_x100`) are *not*
`EnvHash` inputs and may be migrated additively per §7.2.1 if
needed.

#### 7.2.4 `GoldenImage` refs — content-addressed, no migration

`GoldenRef` and `GoldenStoreEntry` carry `content_hash` (Blake3-256
of the payload). Reference assets are addressed by content, not by
schema-versioned bytes:

1. **Updating a PNG payload bumps its `content_hash`.** The
   `golden-update` workflow (§4.1.10 inv 3) writes the new PNG and
   updates both `GoldenStoreIndex.content_hash` and every
   `TraceManifest.golden_refs[i].content_hash` that references the
   payload, in one atomic PR. A reviewer signs off on the visual
   diff; CI re-runs the gate.
2. **Stale traces fail loudly.** A trace whose
   `manifest.golden_refs[i].content_hash` does not match the
   index's current `content_hash` for the same key fails at gate
   time with `E2eError::GoldenMissing` (the "missing or
   substituted" arm). This is exactly the §4.1.3 inv 3 surface; no
   silent up-conversion of golden refs is permitted.
3. **No migration for the binary payload.** PNG files are not
   versioned by Fory schemas — invalidate-and-replace is the only
   path, mirroring `PSOCacheRecord`'s "invalidate, never migrate"
   discipline (`render` §7.2.2). The hash bump is the migration
   record; the PR is the audit trail.
4. **`GoldenStoreEntry` schema additions are additive only.** The
   `recorded_at_unix_ms` field already exists at v1; future
   additions (e.g. `recorded_by_engine_version`) take new tags with
   defaults and inherit the §7.2.1 identity-migration pattern.
   `index_blake3` excludes the new field unless explicitly added
   to the digest input — that change is a breaking index-schema
   bump and requires re-emitting the index under the new build.

### 7.3 Trace-corpus invalidation rule

The `glibre_types_abi_hash` is the engine-wide ABI value the plugin
loader compares (data §7.2.3, fory-codegen.md). The e2e schemas
above contribute to that hash. The corollary, made explicit:

**A `glibre_types_abi_hash` bump invalidates the entire
`.glibre-trace` corpus.** The runner's gate detects the change via
`manifest.plugin_abi_hash` (§7.1.2 tag 3); a mismatch returns
`E2eError::EnvDrift` at gate time, before any frame advances. The
`golden-update` workflow re-records the corpus under the new ABI;
there is no partial-validity middle state — the same discipline as
`render`'s "invalidate the entire archive directory wholesale".

This invalidation is **deliberate**, not accidental. The whole
point of `EnvHash` is that one hash drifts the corpus the moment
the environment drifts; the §4.2 cross-aggregate invariants make
the rule load-bearing. CI will surface the re-record requirement
when a PR bumps any schema source under `data/schemas/`.

### 7.4 What is NOT persisted

To make the boundary explicit (in line with the §1 "scope is the
consumer side of `.glibre-trace`" framing):

| Artefact            | Why not persisted                                                                                           |
|---------------------|-------------------------------------------------------------------------------------------------------------|
| `TraceRunner`       | Per-run process state; constructed fresh from `(TraceFile, GoldenStoreIndex)` and destroyed on report.      |
| `ReplayDriver`      | Per-run input pump; lives inside the `TraceRunner`.                                                         |
| `TraceReport`       | Emitted to a runner-private artefact directory and consumed by `ClosureGate`; not committed (§4.1.12).      |
| `DivergenceReport`  | Same as `TraceReport`; emitted only on `--compare` runs.                                                    |
| `RunnerHost`        | Detected fresh at runner construction (§4.1.9 inv 3); never persisted — the *trace*'s recorded tier is the only persisted host tag (§7.1.7). |
| Captured artefacts  | Diff images, snapshot diffs, log slices written to a per-run temp directory; CI uploads as build artefacts but they are not Fory-encoded. |
| Wall-clock duration | Captured only in the live `TraceReport`; never participates in pass/fail (§4.1.12 inv 3) and never persisted to disk schemas. |
| `EnvHash` (cached)  | Computed on demand at gate time; never stored as a separate artefact. The manifest's bytes are the only source of truth. |

These appear in the persistence surface only as **structured
fields inside `TraceReport`** (consumed by `ClosureGate`), never
as Fory-encoded standalone files. This keeps the persisted surface
minimal — one corpus, one index, one image format — and makes
"what survives a re-record" a one-line answer: nothing the runner
produces, everything the recorder produces.

## 8. Hot-Reload Contract

This section specialises the engine-wide hot-reload protocol
(`reviews/decisions/hot-reload-protocol.md` — drain → swap → migrate →
resume) to the **e2e context**. The specialisation has an unusual
shape because e2e differs from every other plugin in two structural
ways: (a) a running `TraceRunner` is **read-only over disk** — the
`Trace` it parses is immutable post-load (§4.1.1 inv 4) and there is
no in-memory persistent state with a `.fory` schema in e2e itself
(§7.4); and (b) e2e's whole reason to exist is to *observe* engine
behaviour deterministically, so a swap of e2e's own `.dylib`
mid-replay would corrupt the very evidence the run is trying to
produce. This section therefore answers four questions: which
reloads are categorically refused, which are tolerated when the trace
asserts them, what survives a swap on the surviving paths, and how
the observer surface synchronises with trace-asserted reload events.
Engine-wide concerns (per-plugin atomicity, observer bus event
shapes, error wrapping rules, the `enqueue_hot_reload` E2E hook) are
not re-stated here — see the protocol record. E2e adds nothing to
that machinery; it only fills in the four pluggable points the
protocol leaves to each plugin: drain side-effects, survival
inventory, migrate body, and register-time rehydration — plus one
e2e-specific clause covering the trace-asserted-reload semantics that
no other plugin needs.

### 8.1 Reload point — phase 8 only, e2e-self-swap refused mid-replay

The engine schedule (`reviews/decisions/frame-phases.md`) places the
hot-reload barrier at phase 8, **after `render-submit` (phase 7) and
before `present` (phase 9)**. e2e's reload semantics are anchored to
that one slot and refuse any other.

E2e's reload contract has two distinct halves driven by *whose*
`.dylib` is being swapped during a replay run:

1. **The e2e plugin itself (`glibre.e2e.dylib`).** A swap of e2e's
   own `.dylib` while a `TraceRunner` is mid-replay is **categorically
   refused**, regardless of phase. The `TraceRunner` (§4.1.7) is the
   process driving the trace; it owns the `ReplayDriver` (§4.1.6),
   the `(next_op_index, current_frame_index)` cursor, the live
   `EnvHash` it gated on at run start, and the partial `TraceReport`
   accumulating across frames. Replacing e2e's code under a live
   runner would invalidate every byte of that state — the cursor's
   semantics depend on the parser version, the `EnvHash` recipe is a
   build-time constant of the e2e plugin, and the in-flight report's
   `E2eError` arms are the e2e plugin's own enum (§4.1.13). A swap
   that nominally "preserved" any of those would be silent
   nondeterminism — exactly what `EnvHash` exists to prevent
   (§3.2 #3). The refusal is detected at the loader's step-2.2
   manifest check by virtue of e2e declaring the plugin-self-while-
   replaying refusal as a manifest invariant: the e2e plugin's
   manifest names `glibre.e2e.runner_active` as a boolean middleman
   singleton, and step-2.2 refuses a self-swap whenever that flag is
   true. Cause arm: `core::Error::HotReloadRefused` with nested
   `e2e::Error::EnvDrift` (the recipe of `EnvHash` itself drifted —
   the cleanest fit among the existing arms, and matching the §3.2
   #3 collapse rule that says any drift in the gating recipe routes
   through `EnvDrift`).
2. **Any other plugin (`glibre.render.dylib`, `glibre.physics.dylib`,
   `glibre.audio.dylib`, …).** Such a swap during replay is
   permitted **iff the trace recorded an explicit
   `TraceOp::ExpectReload` op at the current `FrameIndex` naming
   that plugin** (§8.6). Without the matching trace op, the swap
   is refused. The rationale is that the `EnvHash` recipe pins
   `manifest.plugin_abi_hash` (§4.1.3 inv 2); changing any plugin's
   loaded ABI hash mid-run violates that gate, and e2e's whole
   contract is "if the env drifts beneath us, we abort with
   `EnvDrift` rather than producing a misleading green/red"
   (§4.1.3 inv 1). The `ExpectReload` op is the trace's one mechanism
   for declaring "yes, I deliberately captured a reload at this
   frame; the env hash carries forward to the post-reload bytes the
   way I recorded".

Both halves of the rule consume the loader's standard refusal
mechanism (protocol §"Refusal Cases"); e2e contributes no new
umbrella arm. Mid-frame reload (any phase 1-7) is refused identically
to render's contract (`render` §8.1): the request is queued for the
next phase 8 entry, never applied mid-frame.

### 8.2 Survival inventory

The engine-wide survival rule is mechanical: **state with a `.fory`
schema in `glibre-types.dylib` survives across the swap; state
without one does not** (protocol §"State Survival Rules"; PHILOSOPHY
collapse: one check, not a per-aggregate manifest). E2e is the
unusual case where the schema'd state lives on disk only — the
`.glibre-trace` corpus (§7.1.1), the `GoldenStoreIndex` (§7.1.3),
and the PNG payloads (§7.1.4) — and every in-memory aggregate the
runner uses is built freshly from those bytes per run (§7.4). The
table below classifies every e2e-owned piece of state against the
rule and adds the e2e-specific reasoning for each survival decision.
The table only describes the **other-plugin reload** case (§8.1
half 2); the e2e-self-swap case is refused before any survival
question arises.

| E2e-owned state                                                         | Persistence path             | Survives swap? | Reasoning                                                                                                                                                                                                                                                                                                                              |
|-------------------------------------------------------------------------|------------------------------|----------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `TraceFile` on-disk bytes (§7.1.1)                                      | `.fory` schema, on disk only | Yes — never in plugin memory; the file lives under `tests/e2e/<ctx>/` and is read-only on the e2e side (§4.1.2 inv 1). The swap does not touch disk. The bytes survive trivially.                                                                                                                                                              |
| Parsed `Trace` aggregate (§4.1.1) held by the live `TraceRunner`        | None — built per run         | Yes — but only because the live `TraceRunner` is a piece of *e2e plugin* code, and e2e's own `.dylib` is refused for swap mid-replay (§8.1 half 1). Under the other-plugin reload case, the `Trace` instance is owned by an unaffected plugin's image and persists verbatim across the swap of any non-e2e dylib.                            |
| `TraceManifest` (§4.1.3) held by the live `TraceRunner`                 | None — built per run         | Yes — same reasoning as the parsed `Trace`. The manifest is the gating preimage; its bytes are immutable post-parse (§4.1.3 inv 4).                                                                                                                                                                                                            |
| `EnvHash` computed at runner start                                      | None — derived value         | Yes — same reasoning. The hash is recorded once at gate time and held immutably; a non-e2e plugin reload does not perturb it because `manifest.plugin_abi_hash` is **the recorded hash**, not the live hash (§8.4 refusal table covers the live-hash drift case).                                                                            |
| `ReplayDriver` cursor `(next_op_index, current_frame_index)` (§4.1.6)   | None — in-process            | Yes — owned by e2e plugin code, which is refused for self-swap. Across a non-e2e reload the cursor is not addressed by any other plugin and persists verbatim. The driver holds no wall-clock state (§4.1.6 inv 1) so there is no reload-induced time displacement.                                                                              |
| `GoldenStoreIndex` (§7.1.3) loaded into the runner                      | `.fory` schema, on disk only | Yes — read-only at run time; the in-memory copy is e2e-owned and persists across non-e2e swaps. The on-disk bytes are unchanged by any plugin reload.                                                                                                                                                                                                |
| `GoldenImage` PNG payloads (§7.1.4)                                     | Raw PNG, on disk only        | Yes — content-addressed by Blake3 (§7.2.4); a swap touches no PNG byte. The runtime cache, if any, is rebuilt lazily per assert.                                                                                                                                                                                                                  |
| Captured artefacts (screenshots, snapshot diffs, log slices)            | None — per run, written on fail | Yes — accumulated in the partial `TraceReport`'s arena until the run terminates; e2e plugin code owns the arena, refused for self-swap. Non-e2e reload does not touch it.                                                                                                                                                                              |
| `TraceReport` partial state                                             | None — per run               | Yes — same reasoning. The report is finalised at run end, not at a frame boundary; a non-e2e plugin reload between asserts does not invalidate the accumulated state.                                                                                                                                                                                |
| Plugin-private worker thread pools (e.g. screenshot encoder)            | None                         | Yes — survive a non-e2e swap (they are e2e-owned). On the refused e2e-self-swap path they would be drained, but that path is rejected before drain runs.                                                                                                                                                                                                |
| `OsAutomation` injection-layer kernel-event handles (§4.1.8)            | None                         | Yes — owned by e2e plugin code on the runner side; non-e2e swap does not touch them. They are tied to the OS process, not to any reloaded plugin's image.                                                                                                                                                                                            |

The rule mechanically applied: every row marked "Yes" either has a
`.fory` schema, lives on disk and is therefore untouched by any
swap, or is in-process state owned by e2e plugin code (which is
refused for self-swap). There is **no row marked "No"**: e2e has no
in-memory state that needs migration on a *non-e2e* reload, and the
*e2e-self* reload case is refused before survival is asked. This
matches §7.4's "what is NOT persisted" rule: the runner's runtime
artefacts are not migration-aware because they are not migrated at
all.

### 8.3 `migrate(...)` body — N/A for e2e itself

The protocol's `migrate` step (protocol §"Step 3 — Migrate") runs
*pure* per-row migrate functions for every persistent-component-type
schema bump on the engine's behalf. **E2e owns zero such bodies for
in-memory state** because e2e has no in-memory persistent
component types in the ECS sense — every persistent type e2e owns
(§7.1.1–§7.1.4) lives on disk only and is consumed read-only by a
fresh `TraceRunner` per run. The migration discipline that *does*
apply to those on-disk types is in §7.2 (additive `TraceOp` /
`AssertOp` payloads, frozen `EnvHash` inputs, content-addressed
golden refs) and is exercised at parse time, not at hot-reload
time. Reloads of other plugins do not invoke any e2e migrate
function.

What this section adds is the **e2e-plugin-specific portion of step
4 (resume)** — the work the new e2e plugin's
`glibre_plugin_register` would do *if and only if* e2e's own swap
were allowed (it is not, mid-replay; §8.1 half 1). The clause is
documented anyway because the engine startup path (no replay running)
*does* exercise an e2e self-swap during dev iteration on the e2e
plugin itself, and that path needs a clean register-time contract.

#### 8.3.1 Re-register `ReplayDriver` factory at the `platform::InputDriver` seam

The new e2e plugin's `glibre_plugin_register`:

1. **Publishes the new `ReplayDriver`-factory pointer** into the
   `platform::InputDriver` registry's
   `(InjectionLayer → ReplayDriver factory)` table. The table
   itself is a middleman type
   (`glibre::types::e2e::ReplayDriverRegistry`) so its slot
   identities survive; the values held in the slots are function
   pointers into the new plugin's image. The fix-up is one atomic
   store per slot, performed under the loader's exclusive phase-8
   ownership (no race with phase 1 of frame N+1's input pump,
   which has not yet begun).
2. **Re-registers the `TraceRunner` entry-point command** into
   the engine command registry (`glibre-trace run`). The command
   registry is keyed by `(command_fqn)` and is idempotent per
   protocol step 4.1; a re-registration of the same identity is a
   no-op even if the underlying function pointer is new.
3. **Does not rebuild any in-flight `TraceRunner`.** No
   `TraceRunner` is live during an allowed self-swap (the swap is
   refused if one is). `TraceRunner` instances are per-run and are
   created at `glibre-trace run` time; the new plugin builds fresh
   instances at the next invocation.
4. **Does not rebuild any in-flight `ReplayDriver`.** Same
   reasoning. The driver is per-run, refused-during-swap by the
   §8.1 half-1 rule.

The previous e2e plugin's `glibre_plugin_drain` accordingly has
nothing to flush from the runner side; its work is the worker-pool
shutdown described in §8.3.2.

#### 8.3.2 Worker pool shutdown / re-spawn

E2e's screenshot-encoder, snapshot-serialiser, and (on
`OsAutomation` runners only) kernel-event-driver workers are
plugin-private and have no `.fory` schema, so they do not survive
a swap by the §8.2 rule. Drain shuts them down; register re-spawns
them.

1. **`glibre_plugin_drain` joins every e2e worker thread** before
   returning, releases their per-thread arenas, and clears the
   pool descriptor. The drain is bounded by the longest in-flight
   encoder task; on a self-swap that path is irrelevant (refused
   mid-run) so the bound is "no work in flight" by construction.
2. **`glibre_plugin_register` re-spawns the pool** at the size
   declared in `RunnerHost`'s policy (`dev-headless`,
   `dev-interactive`, `ci-headless`, `ci-isolated` each have their
   own thread-count default — see §4.1.9). The spawn is
   deterministic w.r.t. host policy.
3. **`OsAutomation` kernel handles are re-acquired** by the new
   plugin via the existing `platform::OsAutomation` seam (refused
   on developer hosts per §4.1.9 inv 3); the seam itself is
   `platform`-owned and is not touched by the swap.

The total work in e2e's resume step on an allowed self-swap (no
runner active) is therefore bounded by **O(1) atomic-store fix-ups +
O(thread-pool size) spawn calls + zero on-disk I/O**, fitting the
protocol's budget (`hot-reload-protocol.md` §Consequences) trivially
because no runner state has to be reconstituted.

### 8.4 Refusal cases (e2e-specific)

E2e contributes no new umbrella refusal arm; every refusal is
expressed as the engine-wide `core::Error::HotReloadRefused` with a
nested cause chosen from the protocol's existing arms. E2e does
introduce three **inner causes** that the loader sees only because
e2e inspects the new plugin during step 4 (resume) — or in the
self-swap case, refuses before step 1. They are enumerated here so
the test matrix (§8.6) and the `TraceReport` diagnostic surface
(§4.1.12) can name them.

| E2e refusal cause                                                         | Detected by                                                                                                                                                | Inner-error arm                                                                                | What the operator must do                                                                                                                                                                                                                                                          |
|---------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **E2e self-swap during active replay** (§8.1 half 1)                       | Loader at step 2.2: e2e plugin manifest declares `glibre.e2e.runner_active` as a refusal predicate; the loader reads the live middleman flag set by `TraceRunner`'s constructor (cleared by its destructor) and refuses the swap when true. | `core::Error::HotReloadRefused` with cause `e2e::Error::EnvDrift` (the `EnvHash` recipe itself would drift). | Wait for the live `TraceRunner` to terminate (its `TraceReport` is emitted on shutdown, `qa-ready` flips appropriately), or re-issue the reload at engine startup before `glibre-trace run` is invoked.                                                                            |
| **Unexpected non-e2e reload mid-replay** (§8.1 half 2)                     | Loader at step 2.1 (existing arm), augmented by an e2e observer subscribed to `HotReloadStarted`. The observer checks the live `TraceRunner`'s next-op cursor; if no `TraceOp::ExpectReload` op is recorded at `current_frame_index` matching the reloading plugin's `fqn`, the observer raises a refusal. | `core::Error::HotReloadRefused` with cause `e2e::Error::EnvDrift` (the trace did not record this reload; the env therefore drifts mid-run). | Either (a) re-record the trace through the editor with the reload captured as an `ExpectReload` op (§8.6), or (b) defer the reload until after the run terminates. The prior plugin remains live; the run continues against the trace's recorded environment.                            |
| **Trace-recorded reload of an unsupported plugin**                         | E2e at parse time when the trace contains an `ExpectReload` op naming a plugin `fqn` not in the live engine's plugin registry. Detected at trace-parse, not at swap.                                  | `e2e::Error::TraceParse` (this is a parse-time refusal, not a swap-time refusal — listed here for completeness). | Re-record the trace under the current plugin set, or amend the engine build so the plugin is loaded. The trace is rejected at gate time; no frame advances.                                                                                                                                  |

Each refusal is logged exactly once at `warn` level (protocol
§"Refusal Cases") with the structured fields `plugin_fqn=glibre.e2e`
(or the offending plugin), `attempted_dylib_path`, `host_abi_hash`,
`plugin_abi_hash`, `trace_path`, `frame_index`, and the inner cause's
enumerator name. The `TraceReport`'s `status` flips to
`Aborted{cause}` (§4.1.12) for the runner-observed cases; the
self-swap refusal at startup (no live runner) reports through the
ordinary loader log surface.

### 8.5 Observers — `TraceRunner` synchronises with reload events

The loader publishes `HotReloadStarted` and `HotReloadCompleted`
events on the engine's observer bus (protocol §"Observer
Notification"). E2e's `TraceRunner` (§4.1.7) is one of the three
contexts that legitimately holds a live subscription to that bus
(alongside the editor and the `tools` profiler), and it is the only
one whose subscription affects replay correctness. E2e's hot-reload
contract requires:

1. **`TraceRunner` subscribes at run start**, before the first
   frame advances, and unsubscribes at run end. The subscription is
   held for exactly the run's lifetime; no e2e code subscribes
   outside a runner.
2. **The subscription serialises trace dispatch against reload
   completion.** When `HotReloadStarted` fires, the runner records
   the event in its frame-local accumulator and pauses dispatch
   of any further `TraceOp`s on the *same frame* (frame N) until
   `HotReloadCompleted` (or `HotReloadRefused`) fires. The
   loader's observer-notification atomicity guarantee (protocol
   §"Observer Notification" — synchronous calls on the loader
   thread) makes this single-frame pause exact: by the time the
   bus call returns, the swap is fully committed and the runner
   may resume. Frame N is therefore the *only* frame that may
   carry a reload event in the recorded trace, regardless of how
   many plugins are reloaded that frame.
3. **The runner cross-checks completion against
   `TraceOp::ExpectReload`.** For each `HotReloadCompleted` event
   observed at frame N, the runner verifies (a) an
   `ExpectReload{plugin_fqn, replacement_dylib_path}` op exists at
   `FrameIndex == N` matching the completed reload's `plugin_fqn`,
   and (b) the post-reload `glibre_types_abi_hash` matches the
   value the `ExpectReload` op recorded in its
   `expected_post_reload_abi_hash` field. Any mismatch fails the
   run with `e2e::Error::AssertFailed{op_id: ExpectReload@N}`,
   carrying both the recorded and the observed values for
   diagnostic clarity.
4. **`HotReloadRefused` is itself a trace-level event.** When
   the loader reports a refusal (e2e-self refusal, capability
   refusal, schema refusal), the runner finalises the
   `TraceReport` with status `Aborted{cause: refusal.cause}` —
   never `Failed`, because the test environment is what drifted,
   not the assertion. The distinction matters for CI triage: an
   `Aborted` report routes operators to environment fixes; a
   `Failed` report routes operators to engine fixes.
5. **No half-swapped world is ever observed by an `AssertOp`.**
   The bus's atomicity guarantee combined with rule 2 above means
   the runner's assertion dispatch sees either a fully-pre-swap
   state or a fully-post-swap state at any frame; there is no
   observable intermediate. This is the e2e-side rephrasing of
   `render` §8.5 invariant 3 and protocol §"Observer
   Notification".

The observer event types e2e *consumes* are the engine-wide
`glibre::types::core::HotReloadEvent` arms (already middleman-typed
per protocol §"Observer Notification"); e2e introduces no new arms.
The event types e2e *emits* — none. E2e's externally-visible reload
output is the `TraceReport` itself; the bus is consumer-only on
the e2e side.

### 8.6 Test hooks — `TraceOp::ExpectReload` and trace-asserted reload semantics

E2e's reload contract is verified end-to-end by trace fixtures that
exercise the loader's existing `enqueue_hot_reload` E2E entry point
(protocol §"Test Hooks") *through the trace stream itself*, not
through ad-hoc fixture code. The mechanism is one new `TraceOp`
variant that carries the loader call as recorded data.

#### 8.6.1 `TraceOp::ExpectReload` — the recorded loader call

`TraceOp` (§4.1.4) gains an `ExpectReload` variant. Per §7.2.1
(additive variant rule) this is the additive defaulted-discriminator
pattern: the variant takes the next free `variant_tag` and a new
`option<ExpectReloadPayload>` field at a new tag; old `.glibre-trace`
files decode unchanged.

```fory
schema glibre.e2e.ExpectReloadPayload {
  version 1
  since   "0.2.0"

  field plugin_fqn                     : string tag 1 since 1   // e.g. "glibre.render"
  field replacement_dylib_path         : string tag 2 since 1   // GoldenStore-relative path under tests/e2e/<ctx>/plugins/
  field expected_post_reload_abi_hash  : bytes  tag 3 since 1   // 32-byte Blake3-256, must equal glibre_types_abi_hash() after swap
  field expected_completion_frame      : u64    tag 4 since 1   // FrameIndex at which HotReloadCompleted must fire (== this op's FrameIndex)
}
```

**Recorder side (`tools::TraceWriter`).** When the editor's record
mode observes a `HotReloadCompleted` event during a recording
session, it appends an `ExpectReload` op at the current
`FrameIndex` capturing the four fields above. The recorder owns
this; e2e is the consumer (§3.3).

**Replay side (`TraceRunner`).** When the runner advances to the
op's `FrameIndex` and dispatches the op, the runner:

1. Calls `glibre::core::test::enqueue_hot_reload(plugin_fqn,
   replacement_dylib_path)` (the protocol's E2E hook). The call
   returns a `ReloadRequestId`.
2. Awaits the matching `HotReloadCompleted` (or
   `HotReloadRefused`) event via `await_reload(request_id)` —
   blocks the runner thread by §8.5 rule 2 until the loader's
   observer-bus call has returned.
3. Verifies the post-reload `glibre_types_abi_hash` matches
   `expected_post_reload_abi_hash`. Mismatch =
   `e2e::Error::AssertFailed{op_id: ExpectReload@N}` per §8.5
   rule 3.
4. Verifies `expected_completion_frame == current_frame_index`.
   The loader guarantees completion before phase 9 of the
   request's enqueueing frame (protocol §"Step 4 — Resume" sub-step
   4.4), so a same-frame completion is the only valid recording
   shape. A future relaxation (multi-frame migration) would change
   this clause; out of scope in MVP.

If the runner observes a `HotReloadStarted` event at a frame where
no `ExpectReload` op is recorded, §8.4 row 2 fires
(`e2e::Error::EnvDrift`).

#### 8.6.2 `assert_state` after reload — the post-reload world is observable

`AssertState` (§4.1.4) and its companions (`AssertScreenshot`,
`AssertEcsSnapshot`, `AssertLogContains`) are unchanged in shape,
but the trace recorder may record any of them at the same
`FrameIndex` as an `ExpectReload`, **after** the `ExpectReload` in
intra-frame order (§4.1.1 inv 5: intra-frame order preserved). The
runner dispatches asserts in recorded order, so an `AssertState`
following an `ExpectReload` runs against the post-reload world.
This is the recorded mechanism by which a trace verifies that the
new plugin's behaviour matches the expected post-reload semantics
(the e2e-context analogue of `render` §8.6's "first post-reload
frame is byte-equal to the reference trace").

The intra-frame order the recorder produces is therefore
load-bearing: an assertion captured *before* the reload event in
the same frame asserts pre-swap state, an assertion captured
*after* asserts post-swap state. The §4.1.1 inv 5 invariant
preserves this ordering on parse and the §4.1.6 driver emits the
ops in cursor order.

#### 8.6.3 Test-fixture matrix

The CI matrix exercises six scenarios under `tests/e2e/hot_reload/`:

1. **Happy-path other-plugin reload.** A trace records one
   `ExpectReload{plugin_fqn = "glibre.render"}` followed by an
   `AssertEcsSnapshot` at the same `FrameIndex`. CI asserts the
   reload completed, the post-reload snapshot is byte-equal to
   the recorded reference, and the `TraceReport` is `Passed`.
2. **Refused other-plugin reload — schema migration.** A trace
   records an `ExpectReload` whose
   `expected_post_reload_abi_hash` matches the loaded plugin, but
   CI is run against a build whose schema bump has no migrate
   function. The loader fires `HotReloadRefused
   {cause: SchemaMigrationFailed}`; the runner finalises with
   `TraceReport.status == Aborted{cause: SchemaMigrationFailed}`.
3. **Unexpected other-plugin reload mid-replay.** The trace
   records *no* `ExpectReload`, but a CI script enqueues a
   reload via the E2E hook at a frame the trace dispatches
   normally. The runner observes the `HotReloadStarted`, finds
   no matching `ExpectReload`, raises §8.4 row 2 (`EnvDrift`),
   and finalises with `TraceReport.status == Aborted{cause:
   EnvDrift}`.
4. **E2e self-swap refused mid-replay.** The trace runs normally;
   a CI script attempts to swap `glibre.e2e.dylib` while the
   `TraceRunner` is live. The loader's step-2.2 manifest check
   refuses; the run continues unaffected, and CI asserts the
   refusal log entry appears with cause arm
   `e2e::Error::EnvDrift`.
5. **E2e self-swap permitted at engine startup.** No
   `TraceRunner` is active; an automated script swaps
   `glibre.e2e.dylib`. The loader accepts the swap, e2e's
   `glibre_plugin_register` re-registers the `ReplayDriver`
   factory and the command, and a subsequent `glibre-trace run`
   loads the new e2e build and produces a `Passed` report on a
   reference trace.
6. **`ExpectReload` payload validation.** Three malformed traces
   (missing `plugin_fqn`, bad `replacement_dylib_path`, mismatched
   `expected_post_reload_abi_hash`) are rejected at parse with
   `e2e::Error::TraceParse`; the runner emits a `TraceReport`
   with `status == Aborted{cause: TraceParse}` before any frame
   advances.

All six scenarios run inside CI jobs using the in-process trigger
plus the E2E hook; no filesystem watcher is involved (protocol
§"Test Hooks"). The Catch2 cases are listed in §11 acceptance
criteria as `Hot-reload accepts trace-asserted other-plugin reload`,
`Hot-reload aborts run on schema-migration refusal`, `Hot-reload
aborts run on unexpected other-plugin reload`, `Hot-reload refuses
e2e self-swap mid-replay`, `Hot-reload accepts e2e self-swap at
startup`, and `ExpectReload payload parse-rejects malformed
records`.

### 8.7 Cross-references

- Engine protocol: `reviews/decisions/hot-reload-protocol.md`
  (drain → swap → migrate → resume; refusal arms; observer bus;
  E2E hook `enqueue_hot_reload` / `await_reload`).
- Frame slot: `reviews/decisions/frame-phases.md` (phase 8 entry /
  exit guarantees; mid-frame swap forbidden).
- Pilot specialisation: `specs/render/SPEC.md` §8 (the four-clause
  shape — drain side-effects, survival inventory, migrate body,
  register-time rehydration — used here verbatim).
- Persistence rules invoked: §7.1.1 (`TraceFile`), §7.1.2
  (`TraceManifest`), §7.1.3 (`GoldenStoreIndex`), §7.1.4
  (`GoldenImage`), §7.2.1 (additive variant rule, applied to add
  `ExpectReload`), §7.2.3 (frozen `EnvHash` inputs), §7.3
  (corpus-invalidation rule on ABI bumps), §7.4 (no in-memory
  persistence — the key invariant making §8.3 trivial).
- Aggregates touched: §4.1.1 `Trace` (intra-frame order
  load-bearing for §8.6.2), §4.1.4 `TraceOp` (gains `ExpectReload`
  variant), §4.1.6 `ReplayDriver` (factory re-registered on
  e2e self-swap at startup), §4.1.7 `TraceRunner` (subscribes to
  the observer bus per §8.5; refused-for-self-swap-while-active
  per §8.1), §4.1.12 `TraceReport` (gains `Aborted{cause}` finalisation
  for refusal arms), §4.1.13 `E2eError` (existing `EnvDrift`,
  `TraceParse`, `AssertFailed` arms cover every refusal cited
  here — no new arm).
- Errors used: `e2e::Error::EnvDrift` (self-swap-during-replay
  refusal, unexpected reload-mid-replay refusal),
  `e2e::Error::TraceParse` (`ExpectReload` payload validation,
  unsupported plugin in trace), `e2e::Error::AssertFailed`
  (post-reload `abi_hash` mismatch, completion-frame mismatch),
  each wrapped by `core::Error::HotReloadRefused` per protocol
  §"Refusal Cases" where the loader is the detection site.

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
