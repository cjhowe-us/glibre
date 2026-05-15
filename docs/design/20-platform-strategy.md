# 20 — Platform Strategy

Cites the thesis in [`00-vision.md`](00-vision.md). The vision promises a single authoring surface
that ships to every supported platform without per-platform reauthoring. This pillar covers: the
platforms the engine commits to (and refuses), the substrate the engine itself is built on, the
native interop seams where one stack stops and another starts, the runtime windowing posture, the
shader pipeline posture, the logic-graph compilation posture, and the .NET execution-model split
between editor and runtime.

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

The editor is one application, written on a cross-platform UI toolkit that runs natively on the
committed authoring platforms (macOS, Windows, Linux). Picking a cross-platform editor toolkit,
rather than three platform-specific editors, follows directly from the *no platform supremacy*
refusal in the vision: an editor that is meaningfully better on one OS would make the no-code path
uneven across the contributor base, which is exactly the failure mode the vision rules out.

The toolkit also has to embed a native rendering surface inside a normal editor window — the hosting
boundary the editor depends on. What that surface renders, and how the renderer reaches it, are
owned by [`50-rendering-strategy.md`](50-rendering-strategy.md); the per-platform mechanics of the
hosting boundary itself are owned by [`40-runtime-architecture.md`](40-runtime-architecture.md).

## Native rendering interop — three stacks, on purpose

The engine renders, stores assets, and pumps input through native platform APIs. There is no single
cross-platform abstraction below us that we can hide behind; either we choose one and pay
translation cost on every platform that doesn't natively speak it, or we accept three native interop
strategies and pay the cost of maintaining three seams.

We accept the three seams. The seams are:

1. **Windows.** D3D12 and DirectStorage are reached through a Windows-native interop
   mechanism. Both APIs are Windows-only by construction, so there is nothing to share with
   another platform's seam.
2. **Apple platforms.** Metal is reached through native interop to the Apple SDK. macOS
   and iOS are one seam, not two — they ship the same graphics API and the same OS
   frameworks underneath the engine.
3. **Linux and Android.** Vulkan is reached through a platform-native interop seam.

This is **deliberately not one abstraction**. Each platform's graphics stack is reached the way its
platform expects; the engine's own portable layer sits *above* these three seams, not below them.
Putting the abstraction below the seams would cost a translation tax on every frame on every
platform; putting it above lets each seam stay idiomatic and lets the engine's portable code stay
platform-blind.

The three-seam choice is intentional, and is the single largest non-rendering risk this pillar
carries (see [`90-risks-and-open-questions.md`](90-risks-and-open-questions.md)).

## Runtime windowing and input

The **runtime** (the shipped game) uses one cross-platform windowing-and-input layer, the same layer
on every platform. The **editor** does not — it lives inside its own UI toolkit, which already owns
its window and its input.

The two windowing stacks coexist on purpose. The runtime needs the smallest, most predictable
window-and-input surface available on every target, including the mobile targets where the editor
toolkit has no business running. The editor needs a real desktop UI toolkit. Conflating them would
force one stack to pretend to be the other; keeping them separate is cheaper.

The two-stack arrangement carries a known failure mode: during an in-editor play session both stacks
are alive, and an OS-level input event must be routed to one and only one of them. Resolving that
routing — and the focus, capture, and reentrancy questions that come with it — is owned by
[`40-runtime-architecture.md`](40-runtime-architecture.md). This pillar only commits to keeping the
stacks separate; the seam between them is described there.

## Shader pipeline

Shaders are authored once, in a single shader language, and compiled to every supported graphics API
ahead of time. The compile is a build-time step in the content pipeline (see
[`60-content-pipeline.md`](60-content-pipeline.md)); the runtime ships precompiled shaders and does
not invoke the shader compiler at runtime unless an authoring feature requires it (live shader
editing in the editor is the only current candidate).

This is the cheapest way to honor the no-code vision: makers compose materials in a visual editor,
and the engine takes responsibility for the per-platform shader artifacts. They never see the
underlying shader language.

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

- **Which specific cross-platform UI toolkit, native-interop mechanisms, runtime
  windowing layer, shader compiler, or codegen toolchain are used.** Those are tactical
  selections downstream of these stances. They are recorded as design decisions in the
  appropriate pillars, not here.
- **The exact set of shipped graphics-API features per platform.** That belongs in
  [`50-rendering-strategy.md`](50-rendering-strategy.md).
- **The seam between editor process and runtime process** — owned by
  [`40-runtime-architecture.md`](40-runtime-architecture.md).
- **Asset format choices and bake artifact layout** — owned by
  [`60-content-pipeline.md`](60-content-pipeline.md).

## Risks deferred to `90`

- Three native rendering seams multiplies maintenance cost; every renderer feature must
  ship on three stacks before it counts as shipped.
- The Windows-native interop seam is a single point of failure if the chosen interop
  mechanism stagnates.
- Console tiers are stretch; promising them without committing to one is a credibility
  risk if the engine grows users who assume console support is coming.
- AOT-only artifacts for mobile mean every runtime dependency must itself be AOT-clean.
- The shader and graph toolchains both depend on out-of-process compilers at edit time;
  losing either is an edit-time outage, not a runtime outage, but it still blocks
  authoring.
- The same out-of-process toolchains are load-bearing for CI: a CI environment without a
  display server, without a GPU, or under sandbox restrictions can fail the build because
  the shader or graph compiler cannot start. This is a build-pipeline failure mode distinct
  from the local authoring outage, and it scales worse — a stuck compile blocks every
  contributor, not just one.
