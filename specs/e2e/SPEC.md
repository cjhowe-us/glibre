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
