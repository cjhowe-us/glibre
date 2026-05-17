# 40 — Runtime Architecture

Cites the thesis in [`00-vision.md`](00-vision.md), the substrate stances in
[`20-platform-strategy.md`](20-platform-strategy.md), the context inventory in
[`10-bounded-contexts.md`](10-bounded-contexts.md), and the integration shape in
[`15-context-map.md`](15-context-map.md).

This pillar takes the contexts whose work happens at engine-execution time — Runtime, Composition,
Simulation, Rendering, Platform, and Authoring (where it hosts a viewport into a running runtime
process) — and pins the architectural posture that lets them coexist. The decisions here are about
*shape*: how many processes, how many threads, what is allowed to mutate what, when state crosses a
boundary, and what hot-reload means in practice. Code, APIs, and threading-primitive choices are
tactical and live elsewhere.

## How to read this pillar

Architectural decisions are presented as **stances**. Each stance is one of *committed* (a decision
the rest of the design set may depend on), *deferred* (a decision pending a committed scope or an
upstream resolution), or *anticipated* (a likely shape called out so later pillars don't preclude
it). Failure modes are named for each committed stance, per Murphy.

## Editor and runtime are distinct processes

**Committed.**

The editor and the shipped runtime are separate operating-system processes, always. The editor never
embeds the shipped runtime as a library; the shipped runtime never depends on editor code.

**Why.** Three reasons compound:

- The substrate posture in [`20-platform-strategy.md`](20-platform-strategy.md) splits
  execution model (JIT for the editor everywhere, the iOS AOT runtime on iOS). A
  single process cannot satisfy both stances.
- Crash isolation. A bug in a maker's logic graph kills the runtime process, not the
  editor.
- Dependency-tree hygiene. The runtime cannot acquire editor dependencies through a shared
  process even by accident.

**What this implies for in-editor play.** When the maker "runs" a scene inside the editor, a runtime
process is launched; it renders into the editor's hosted viewport surface and receives input through
the routed channel committed in S-15 of [`15-context-map.md`](15-context-map.md). The editor and the
running session are peers-by-process, not host-and-plugin; S-15 is the only architectural channel
between them during play, and it carries only lifecycle commands, focus-transfer requests,
hot-reload notifications, and lifecycle status events — never world state or asset bytes.

**Failure mode absorbed.** A faulty graph or a misbehaving asset cannot corrupt the editor's project
state; recovering is "the play session crashed, the editor did not."

## Editor process model

**Committed.**

The editor is one process. It is not a shell + many plugin processes.

**Why.** A shell-and-plugins architecture buys crash isolation between editor tools but costs every
other operation a process boundary: drag-and-drop, undo, selection, palettes, asset queries all have
to cross IPC. The editor's value is in being *coherent*; the shell-plugin model fragments that
coherence in exchange for an isolation budget glibre does not need at this scope (per Occam).

**Plugin / extension execution.** Editor extensions (importers, node libraries, custom inspectors,
mod tooling) load as code inside the editor process. They can crash the editor; that is the cost.
The shipped runtime never loads editor extensions; the editor never loads runtime-only artifacts as
executable code.

**Failure mode absorbed.** A misbehaving editor extension can crash the editor — that cost is named
and accepted. The mitigation is that the runtime is a separate process (see the prior stance), so
the maker's running play session and the runtime's view of project state survive an editor crash.
The maker reopens the editor and the project is where they left it.

## Viewport hosting

**Committed.**

The editor hosts the running runtime's rendering output in an embedded surface, by platform-specific
arrangement. The exact mechanics — surface handle, present synchronization, OS-level focus and
capture — are per-platform and owned by Platform; the runtime renders, the editor displays.

**Why.** The vision commits to live preview at fidelity matching the shipped runtime (per
[`00-vision.md`](00-vision.md)). The cheapest way to honor that is to *use the shipped renderer*
during authoring rather than maintaining a second one. The viewport is the seam; the renderer behind
it is the same renderer the player will see.

**Anticipated.** Three per-platform handshakes — the Apple native rendering surface, the Windows
swapchain handoff, and the Linux native surface — are non-trivial and are each their own spike.
There is no Android handshake here: Android is a runtime-only target (per
[`20-platform-strategy.md`](20-platform-strategy.md)) and the editor does not run there.

**Anticipated (separate decision).** The architecture commits to
*one viewport surface per authoring session*; multi-viewport authoring is not committed in this
pillar.

**Failure mode absorbed.** What the maker sees in the editor is what the player will see. Authoring
drift between a "preview renderer" and a "shipped renderer" cannot happen because there is only one
renderer.

## Tick and frame are decoupled

**Committed.**

The simulation tick rate and the rendering frame rate are independent. The runtime owns the tick
clock; the renderer pulls a read-only snapshot per frame.

**Why.** Tying frame rate to tick rate ties physical fidelity to display refresh, which breaks on
every device whose display is faster or slower than the simulation needs. The snapshot seam (S-7 in
[`15-context-map.md`](15-context-map.md)) is where the decoupling happens.

**What this implies.**

- Simulation phases per tick are deterministic in order; their order is owned by Runtime,
  not negotiated at runtime between subsystems.
- Rendering reads a *consistent* snapshot, not a live view; mid-frame writes to world
  state by Simulation never affect the frame being rendered.

**Failure mode absorbed.** Rendering can race or miss without corrupting simulation; simulation can
race or miss without tearing rendering.

## Hot-reload contract

**Committed shape; specific mechanics deferred per layer.**

Hot-reload is *initiated* by authoring (per S-15) and *consumed* by the runtime in the in-editor
play session. The runtime exposes machinery to consume the reload signals; authoring drives when
they fire. There are three distinct channels, by what is being reloaded:

- **Asset hot-reload.** Content publishes a re-baked artifact (per S-4 and S-14); both
  the runtime (in the in-editor play session) and Authoring (in inspectors and viewports)
  pick up the new bytes. The runtime's behavior on hot-swap is *snapshot-boundary*: a
  reload takes effect at the next tick boundary, never mid-tick.
- **Graph hot-reload.** Composition produces a fresh executable artifact from a re-bake
  (per S-2). On the JIT-host platforms, the new artifact is made available to the runtime
  and the previous artifact is retired once no execution is active in it; the retirement
  mechanism is tactical. On the iOS AOT runtime, graph hot-reload is **not available**
  in the shipped game; in the in-editor play session (which runs on a JIT-host
  platform), the maker still gets the hot-reload experience.
- **Material / shader hot-reload.** Rendering ingests a re-baked render artifact through
  S-5 and re-establishes its internal GPU state from the new artifact. No shader compile
  runs inside the shipped runtime.

**Deferred.** Atomicity guarantees for hot-swap (what happens to in-flight callbacks, what happens
to long-lived state owned by the swapped-out artifact, what happens to references held by other
systems) are per-channel concerns and are open questions tracked in
[`90-risks-and-open-questions.md`](90-risks-and-open-questions.md).

**Failure mode absorbed.** The maker is not waiting on a build to see the consequence of an edit;
the running session never tears mid-tick because of an authoring action.

## Execution-model boundary between editor and runtime

**Committed.**

The editor and the shipped runtime have **distinct dependency trees** (per
[`20-platform-strategy.md`](20-platform-strategy.md)). A package that depends on capabilities the
AOT runtime cannot provide — runtime IL emission, dynamic assembly loading, reflection-heavy
serialization — is an editor-only dependency by definition.

**How the boundary is held.** Two parts:

- **Architectural.** The runtime entry point and the editor entry point share no
  transitive assembly tree. The contexts they both consume (Composition's artifact
  loader, Content's baked-asset loader) ship in two variants if necessary: one
  AOT-friendly, one editor-friendly.
- **Enforcement.** *How* this is mechanically checked is not committed in this pillar; it is a
  tactical concern tracked in [`90-risks-and-open-questions.md`](90-risks-and-open-questions.md).

**Failure mode absorbed.** Authoring a game on the desktop and shipping it for iOS does not silently
bring in a JIT-only dependency that fails at AOT-link time three weeks before ship.

## Process-level resources are owned, not shared

**Committed.**

Within the runtime process, world state has explicit ownership at the phase boundary defined by S-6.
No shared mutable state crosses a phase boundary without going through the phase contract.

**Why.** Concurrency complects state; making ownership explicit is the cheapest defence. The phase
model in [`10-bounded-contexts.md`](10-bounded-contexts.md) is what carries this discipline.

**Anticipated.** Parallel execution of independent phases is anticipated where ownership permits.
The vehicle for that parallelism — scheduler, primitives, topology — is tactical and not committed
here.

**Failure mode absorbed.** A simulation subsystem cannot stomp on another subsystem's state by
holding a reference past its phase. The strict-phase discipline is what makes the determinism
classes in [`10-bounded-contexts.md`](10-bounded-contexts.md) holdable in practice.

## Determinism is opt-in, by subsystem

**Committed.**

A subsystem advertises a determinism class (per `10`): deterministic, best-effort, or
non-deterministic. Subsystems used in a deterministic feature (replay, rollback, automated test)
must be deterministic; other features may use any class.

**Why.** Force-determinism on every subsystem and the design budget goes to making audio mixing
reproducible bit-for-bit. Allow non-determinism without naming it and the rollback work in Slice-4
fails silently.

**Anticipated.** When the Networking context is claimed (per `10`'s deferred contexts), the
determinism contract becomes a precondition for any rollback or replay design. The runtime
architecture is intentionally Networking-agnostic until that happens.

**Failure mode absorbed.** A feature that needs determinism declares the subsystems it depends on;
the build refuses to compose deterministic features with non-deterministic subsystems.

## Input routing during in-editor play

**Committed shape.**

When an in-editor play session is active, one OS input event is delivered to **one** of the two
stacks: the editor or the running runtime. Routing is by focus: the surface that has OS-level focus
consumes events; the other does not.

**Ownership of focus.** Platform executes focus transfers — it is the only party that talks to the
OS focus primitive. Authoring may *request* a focus transfer over S-15 (acquire focus on the
viewport, release focus back to the editor); the request is acted on by Platform on the runtime
side. Authoring never manipulates the OS focus state directly, and Platform does not initiate
transfers on its own.

**Why.** The runtime windowing layer and the editor toolkit are deliberately separate (per
[`20-platform-strategy.md`](20-platform-strategy.md)). They cannot both consume OS events
simultaneously without producing double-delivery, which would be a Murphy nightmare for hotkeys,
gamepad input, and modifier keys.

**Deferred.** The per-platform mechanics — exactly which OS focus primitive is used, what happens to
keyboard repeats and gamepad polling when focus transfers mid-tick — are Platform-owned and tracked
in `90`.

**Failure mode absorbed.** No input event is delivered twice; no input event is silently dropped.
Maker confusion about "why does this hotkey do nothing" reduces to "where is focus."

## What this pillar deliberately does not decide

- **The specific tick rate, scheduler, or job-system topology.** Tactical, owned by the
  Simulation context's downstream design.
- **The exact viewport handshake per platform.** Platform-owned spikes; tracked as open
  questions in `90`.
- **The exact hot-reload atomicity rules per channel.** Per-channel concerns, tracked in
  `90`.
- **How AOT/JIT discipline is enforced.** Tactical concern; the *rule* is here, the
  enforcement mechanism is in `90`.
- **The shape of the Networking context's runtime hooks.** Networking is deferred per
  `10`.

## Risks deferred to `90`

- **Viewport handshake.** One per-platform spike for each of the three committed authoring
  platforms (macOS, Windows, Linux). Each carries independent risk; any one of them
  blocking is a slice blocker.
- **Hot-reload edge cases.** State migration, in-flight callbacks, long-lived references
  to swapped-out artifacts — none have a single right answer; each channel needs its
  own design.
- **Input routing under modal dialogs.** A modal opened by the editor while a play
  session has focus is a known Murphy case.
- **AOT/JIT boundary enforcement.** The rule is hard; the enforcement mechanism is not
  chosen.
- **Process-restart cost.** Launching a runtime process per play session is sound for
  isolation but pays a startup tax every time. Caching, warm-launch, or in-place reload
  are tactical strategies; the cost is named so it doesn't surprise a future slice.
- **iOS hot-reload deficit.** The vision commits to live feedback as a default; the iOS
  AOT runtime cannot do graph hot-reload in a shipped game. The deficit is acceptable
  for in-editor authoring (which happens on a JIT-host platform) but should be made
  visible to makers who target iOS exclusively.
