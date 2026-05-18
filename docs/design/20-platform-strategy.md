# 20 — Platform Strategy

Cites the thesis in [`00-vision.md`](00-vision.md). The vision promises a single authoring surface
that ships to every supported platform without per-platform reauthoring. This pillar covers: the
platforms the engine commits to (and refuses), the substrate the engine itself is built on, the
editor host, the single graphics seam, the windowing-and-input layer, the shader pipeline, the
logic-graph compilation posture, and the .NET execution-model split between editor and runtime.

This document is strategy. It does not specify APIs, file layouts, or build-script content.

## Supported platforms

| Target | Authoring | Runtime | Tier |
|---|---|---|---|
| macOS (Apple Silicon) | yes | yes | committed |
| iOS / iPadOS | no | yes | committed |
| Windows (x64) | yes | yes | committed |
| Linux (x64) | yes | yes | committed |
| Android | no | yes | committed |
| macOS (Intel) | no | best-effort | observed-only |
| Nintendo Switch | n/a | n/a | stretch |
| Xbox | n/a | n/a | stretch |
| Web / browser | n/a | n/a | non-goal (see [`00-vision.md`](00-vision.md)) |

`n/a` in the Authoring or Runtime columns means the tier's status answers the question — either the
engine hasn't decided what the target looks like (stretch) or has refused it outright (non-goal).

**Authoring** is the desktop tool the maker uses; **runtime** is the shipped game. Mobile and
console targets are runtime-only — authoring happens on a desktop platform.

Tier meaning:

- **committed** — the engine considers the target a first-class promise. A capability is not
  shipped until it works there.
- **stretch** — the engine is designed not to preclude these targets, but does not commit
  to them in the current vision horizon. Bringing them up requires platform SDKs that close
  source projects; the cost is owned by whoever picks up that effort.
- **observed-only** — known to function as a side effect of an upstream platform; not
  separately tested or supported. macOS (Intel) sits here because Apple-platform work
  carries it implicitly: the rendering, OS-framework, and AOT paths are written for the
  Apple platform, not for an ISA. The engine does not separately validate Intel macOS
  hardware, and does not promise renderer feature parity on Intel-only GPUs.
- **non-goal** — explicit refusal; see vision.

## Substrate stance

The engine, its editor, and its tooling are written on .NET with C#. This is one decision with
several follow-on consequences; this section is about *why* it earns the cost, not how it's wired
together.

**What .NET buys us:**

- A single managed language across the editor, the runtime, the tooling, and the engine's
  own contributor experience. Engine contributors do not context-switch between three
  languages to add a feature that touches the editor, the runtime, and the build pipeline.
- A mature cross-platform standard library, a credible AOT story for shipped games, and a
  credible JIT story for editor responsiveness.
- A live debugger and profiler ecosystem on every supported authoring platform.

**What .NET costs us:**

- GC pause behavior matters. Strategic answer: the runtime treats allocation pressure as a
  property to design *out* of hot paths, not to manage *around* at runtime. Tactical
  techniques live in [`40-runtime-architecture.md`](40-runtime-architecture.md).
- Native interop is non-trivial. The engine accepts this cost explicitly — see the next
  section.
- The console story is harder than for C/C++ engines. This is the dominant reason consoles
  are *stretch*, not *committed*.

The substrate is not negotiable per-feature. A feature does not get to opt out of .NET because a
different stack would be easier; doing so would fragment the contributor experience that is the
point of the choice.

## Editor host

The editor is one application that runs on every committed authoring platform (macOS, Windows,
Linux) through one substrate: SDL3 for the window and OS input, and glibre's own Vulkan renderer for
everything drawn inside that window. There is no native-widgets UI toolkit and no per-platform
editor shell — the entire editor surface (menus, palettes, inspectors, viewports) is drawn by the
renderer.

Picking one substrate, rather than three platform-specific editors or a native-widgets layer that
varies by OS, follows directly from the *no platform supremacy* refusal in the vision: an editor
that is meaningfully better on one OS would make the no-code path uneven across the contributor
base, which is exactly the failure mode the vision rules out.

Because the editor UI and the runtime's preview surface are drawn by the same renderer into the same
SDL3 window, embedding "a native rendering surface inside a normal editor window" is not the
boundary it would be under a native-widgets editor; the viewport is a region of the renderer's
frame. The per-platform mechanics of the editor↔runtime surface handoff are owned by
[`40-runtime-architecture.md`](40-runtime-architecture.md).

## One graphics seam: Vulkan via SDL3

The engine speaks Vulkan, and only Vulkan. SDL3 provides the windowing, input, and surface-creation
primitives that put a Vulkan swapchain on the screen on every supported platform.

- **Windows, Linux, Android.** Vulkan is the native graphics API. SDL3 creates the window
  and the Vulkan surface; the engine takes it from there.
- **macOS, iOS.** Vulkan is reached through MoltenVK, the Khronos-maintained translation
  layer that implements Vulkan on top of Metal. From the engine's perspective there is one
  graphics API, not two; MoltenVK is an Apple-side implementation detail. The cost of that
  translation — a feature-coverage and version-lag gap against upstream Vulkan — is named
  explicitly in [`90-risks-and-open-questions.md`](90-risks-and-open-questions.md).

This is **deliberately one stack**, not a portable abstraction over many. One API surface, one set
of pipeline objects, one shader IR. The per-platform variation lives entirely beneath SDL3 (window
and surface creation) and MoltenVK (Vulkan-to-Metal on Apple); the engine's own code does not branch
on graphics API. A single seam also means a renderer feature ships once and runs on every target,
rather than being held to the pace of the slowest of several backends.

## Windowing and input

Both the editor and the shipped runtime use SDL3 for windowing and OS input, on every committed
target. The shipped runtime opens an SDL3 window on Windows, macOS, Linux, iOS, and Android; the
editor opens an SDL3 window on the desktop authoring platforms. One layer, one set of OS-input
semantics, no per-platform special case.

The shared substrate carries a known failure mode unique to in-editor play: while a play session is
active the editor process and the runtime process each have their own SDL3 window, and an OS-level
input event must be routed to one and only one of them. Resolving that routing — and the focus,
capture, and reentrancy questions that come with it — is owned by
[`40-runtime-architecture.md`](40-runtime-architecture.md). This pillar only commits to the shared
substrate; the seam between the two SDL3 windows during play is described there.

## Shader pipeline

Shaders are authored in GLSL and baked to SPIR-V by the content pipeline (see
[`60-content-pipeline.md`](60-content-pipeline.md)). The runtime consumes SPIR-V directly on every
platform; there is no per-platform shader translation step at startup, and the runtime does not
invoke the shader compiler unless an authoring feature requires it (live shader editing in the
editor is the only current candidate).

GLSL→SPIR-V is one pipeline, not many. Because Vulkan is the only graphics API the engine reaches
(per the previous section), SPIR-V is the only shader artifact the runtime needs. The maker composes
materials visually and never sees the underlying shader language; the engine takes responsibility
for producing the SPIR-V artifact.

## Logic-graph compilation toolchain

The visual logic graph (gameplay, behavior, effect parameters, audio behavior) is a build-time
compilation problem, not a runtime interpretation problem. The author edits a graph in the editor; a
downstream toolchain lowers the graph through an intermediate representation and produces a native
code artifact for the target platform. The runtime loads that artifact; it does not interpret the
graph and does not contain a JIT.

**Why AOT, not JIT or interpreted:**

- **iOS forbids runtime JIT.** A JIT-based graph would force iOS into a special case — a
  separate interpreter or a separate codegen path. Doing the AOT compile uniformly avoids
  the bifurcation entirely.
- **Determinism.** A graph that compiles once per platform is easier to make deterministic
  than a graph whose execution path varies per host.
- **Steady-state performance.** Native code outperforms an interpreted graph traversal
  with no per-frame surprise.
- **The cost is paid by the toolchain, not the runtime.** The build pipeline is where
  makers expect a "publish" step; the runtime is where they expect responsiveness.

The artifact delivery path per platform, the hot-reload model, and the per-platform variations in
how the iOS build receives its artifact are decisions owned by
[`30-authoring-and-no-code.md`](30-authoring-and-no-code.md) and
[`40-runtime-architecture.md`](40-runtime-architecture.md).

## .NET execution model on the runtime

The shipped runtime runs as a native AOT image on platforms where it must (mobile, where JIT is
disallowed or expensive), and as JIT-hosted on the desktop platforms where the JIT is the better
engineering trade. The editor always runs JIT-hosted. The editor and the runtime have distinct
dependency trees: a package that requires dynamic .NET features is an editor-only dependency by
definition, and never silently leaks into the runtime through a shared assembly.

This split is not a workaround. It is the design:

- **The runtime ships small, starts fast, and runs without a JIT** where the platform
  rewards that (iOS today; consoles eventually).
- **The runtime ships fast and benefits from JIT tiering** on desktop platforms where
  startup matters less and steady-state matters more.
- **The editor uses every dynamic capability the platform offers**, because plugin
  hot-load, reflection, and live tool authoring all rely on them.

Features that are not available under AOT — runtime IL emission, dynamic assembly loading,
reflection-heavy serialization — are not allowed in the runtime. The editor is free to depend on
them. The boundary is a hard design rule; how it is enforced is owned by
[`40-runtime-architecture.md`](40-runtime-architecture.md).

## What this pillar deliberately does not decide

- **Which specific SDL3 release line, Vulkan loader, GLSL→SPIR-V compiler, MoltenVK
  release, or codegen toolchain are used.** Those are tactical selections downstream of
  these stances. They are recorded as design decisions in the appropriate pillars, not
  here.
- **The exact set of shipped graphics-API features per platform.** That belongs in
  [`50-rendering-strategy.md`](50-rendering-strategy.md).
- **The seam between editor process and runtime process** — owned by
  [`40-runtime-architecture.md`](40-runtime-architecture.md).
- **Asset format choices and bake artifact layout** — owned by
  [`60-content-pipeline.md`](60-content-pipeline.md).

## Risks deferred to `90`

- MoltenVK is the engine's only path to the Apple GPU; its feature coverage and version
  cadence lag upstream Vulkan in places, and a feature the engine adopts on native Vulkan
  may need a workaround or wait on Apple platforms.
- Console tiers are stretch; promising them without committing to one is a credibility
  risk if the engine grows users who assume console support is coming.
- AOT-only artifacts for mobile mean every runtime dependency must itself be AOT-clean.
- The GLSL→SPIR-V and graph toolchains both depend on out-of-process compilers at edit
  time; losing either is an edit-time outage, not a runtime outage, but it still blocks
  authoring.
- The same out-of-process toolchains are load-bearing for CI: a CI environment without a
  display server, without a GPU, or under sandbox restrictions can fail the build because
  the shader or graph compiler cannot start. This is a build-pipeline failure mode distinct
  from the local authoring outage, and it scales worse — a stuck compile blocks every
  contributor, not just one.
