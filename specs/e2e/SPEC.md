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
