# 80 — Distribution and Extensibility

Cites the thesis in [`00-vision.md`](00-vision.md), the contexts in
[`10-bounded-contexts.md`](10-bounded-contexts.md), the integration shape in
[`15-context-map.md`](15-context-map.md), the substrate stances in
[`20-platform-strategy.md`](20-platform-strategy.md), the authoring philosophy in
[`30-authoring-and-no-code.md`](30-authoring-and-no-code.md), the runtime architecture in
[`40-runtime-architecture.md`](40-runtime-architecture.md), the rendering posture in
[`50-rendering-strategy.md`](50-rendering-strategy.md), the content pipeline in
[`60-content-pipeline.md`](60-content-pipeline.md), and the accessibility/localization stance in
[`70-accessibility-localization.md`](70-accessibility-localization.md).

This pillar pins how a maker's project becomes a thing that players run, and how the engine itself
becomes a thing that other contributors extend. It covers per-target packaging, the extension and
mod model, the marketplace posture, the version-control stance, the cloud-build posture, and the
AI-provider integration posture deferred from prior pillars.

## How to read this pillar

Decisions are presented as **stances** with *committed* / *deferred* / *anticipated* tags. Failure
modes are named per Murphy.

## Distribution consumes; it does not re-bake

**Committed.**

Per S-12 and S-13 in [`15-context-map.md`](15-context-map.md), Distribution receives already-baked
artifacts from Composition (graph artifacts) and Content (asset artifacts) and assembles them into a
shipped build for a build target. Distribution does not invoke the lowering toolchain, does not run
importers, and does not call shader or texture encoders. Build correctness is established upstream,
at the bake; Distribution's job is to wrap.

**Why.** A Distribution that re-bakes is a Distribution that can produce a shipped build different
from anything tested in-editor. Keeping bake upstream of distribution means the shipped build is
byte-equivalent to what the maker saw, modulo packaging metadata.

**Failure mode absorbed.** A maker who runs the project in-editor and packages it for a build target
gets a shipped build that runs the same content. Packaging is not a rebuilding pass.

## Per-platform packaging conforms to platform expectations

**Committed.**

Distribution receives a frozen project state from Authoring at S-9 in
[`15-context-map.md`](15-context-map.md) and produces, for each committed build target, the artifact
shape that platform's ecosystem expects — its native installer, app bundle, or package format —
including signing, entitlements, and the store metadata each platform's distribution channel
requires. Distribution is a downstream conformant of Platform on this packaging-format surface (per
S-8); it does not invent a glibre-specific packaging concept.

**Why.** Players install through their platform's channel. A glibre-specific packaging format that
requires the player to do something different on each platform would be a maker-facing pain
pretending to be a platform-facing one. Conforming to each platform's conventions is what makes
"ship to where the player is" cheap.

**Anticipated.** Some platform packaging requirements (signing identities, store accounts, platform
certifications) require maker-owned credentials and platform-owned processes. Distribution provides
the mechanism; the maker provides the credentials.

**Failure mode absorbed.** A shipped build is recognized by the target platform's ecosystem without
per-platform-format adapters added by the maker.

## Extensions are first-class, the marketplace is not the boundary

**Committed.**

An **extension** (per [`10-bounded-contexts.md`](10-bounded-contexts.md)) is anything a maker adds
to a project that wasn't there before: an importer, a node library, an editor tool, an asset pack, a
mod. Extensions are first-class: the engine's built-in capabilities use the same extension
mechanisms a third-party contributor would use. There is no privileged "built-in tier" that closes a
backdoor to third-party authors.

**Why.** A first-class/third-class split would tell contributors "the engine team has better tools
than you do," which calcifies the engine. It would also let the engine ship tactical shortcuts that
community extensions cannot, which then become hidden assumptions the third-party ecosystem cannot
reproduce.

**Anticipated.** Extension installation, version compatibility, and dependency resolution are
operational concerns owned by Distribution's downstream design. A first-class extension surface
without a versioning commitment is a surface that breaks contributors silently as the engine
evolves; that risk is named in the Risks section below.

**Failure mode absorbed.** A contributor who wants to add an importer, a node library, or a tool
faces the same surface area the engine team uses. Engine evolutions that would calcify the extension
API are caught early because the engine itself uses the API.

## Editor-side extension isolation

**Committed.**

An editor extension the *maker* has marked trusted (a built-in extension or one the maker installs
from a source they choose) runs in-process inside the editor. An editor extension the maker has not
yet trusted (third-party on first install, or under quarantine) runs in an isolated process with a
published-language boundary back to the editor.

**Who decides.** The maker, at install time. The engine does not silently elevate trust.

**Why.** An in-process extension that throws kills the editor (a known cost named in
[`40-runtime-architecture.md`](40-runtime-architecture.md)); paying IPC overhead for every extension
because of that risk is the wrong trade for the common case where the maker actually trusts what
they installed. Splitting trusted/untrusted lets the engine isolate code the maker has not yet
vouched for, without taxing what they have.

**Deferred.** Exactly how trust is evaluated (per-extension, per-publisher, signed-only,
quarantine-on-first-run) is a downstream UX concern tracked in
[`90-risks-and-open-questions.md`](90-risks-and-open-questions.md).

**Failure mode absorbed.** The engine does not run not-yet-trusted code in-process. A
trusted-but-buggy extension can still crash the editor — trust is a performance optimization, not a
safety guarantee — but the runtime process is separate (per `40`), so the maker's running play
session and the project on disk survive an editor crash.

## Runtime-side mod isolation

**Committed.**

A mod loaded into a shipped runtime always runs in an isolated process with a published-language
boundary back to the host runtime. There is no in-process mod path in the shipped runtime,
regardless of trust state.

**Who decides.** The *maker* decides which mod surface a shipped build exposes (which hooks mods may
use, where mod content may be loaded from). The *player* decides which specific mods they install.
The maker ships the boundary; the player chooses what crosses it.

**Why.** A player installing a mod cannot be a security event for the maker. Allowing in-process
mods would make the maker's shipped game dependent on the player's mod-curation discipline, which
the maker has no ability to enforce. Out-of-process is the only stance that keeps the player free to
mod and the maker free from inheriting the player's trust decisions.

**Failure mode absorbed.** A mod that misbehaves does not corrupt the running game; the worst
outcome is the mod's own isolated process failing.

## Marketplace is a delivery channel, not the gatekeeper

**Committed.**

The engine ships with a marketplace surface that allows makers to find, install, and update
extensions and asset packs. The marketplace is *one* channel for extensions; it is not the only way
a maker can install one. A maker who chooses to install an extension from a source they trust (a
personal repository, a colleague's package, a community mirror) can do so without going through the
marketplace.

**Why.** A marketplace that is the *only* path makes the engine a permission system over the maker's
project. The vision in [`00-vision.md`](00-vision.md) refuses that posture explicitly ("no closed
marketplaces gating basic features"). Keeping the marketplace as a channel, not a gatekeeper,
preserves the maker's authority over their own project.

**Anticipated.** The engine may offer integrity and provenance signals on marketplace extensions
(signed-by-marketplace, has-N-installs, last-updated). Those are informational; they do not become
enforcement.

**Failure mode absorbed.** Core authoring works without a marketplace account, a subscription, or a
transaction, per the vision's refusal list.

## Version control is the project's source of truth

**Committed.**

A glibre project's source-of-truth is its version-controlled source assets, scenes, graphs, prefabs,
and configuration (per [`60-content-pipeline.md`](60-content-pipeline.md)). The frozen project state
Distribution receives at S-9 is sourced from this VC-managed project; Distribution does not maintain
its own state separate from that snapshot.

**Why.** A maker who reverts to last week's commit gets last week's project, including last week's
build outputs. A Distribution context with hidden state separate from VC would invalidate that
property.

**Anticipated.** Specific version-control vendor support, branch / tag conventions, and LFS-style
large-binary handling are tactical and out of scope for this pillar. Per
[`60-content-pipeline.md`](60-content-pipeline.md), the side-channel for large binaries is a
deferred concern.

**Failure mode absorbed.** A maker's VC reflects what they have; Distribution does not introduce
surprises that aren't in the repo.

## Cloud build is a deployment of the local pipeline

**Committed shape; specific mechanics deferred.**

When a maker uses a hosted build service to package for a target the maker's machine cannot build
for itself (the obvious example: shipping an iOS build from a non-Apple authoring platform), the
hosted service receives the same S-9 frozen project snapshot the local Distribution context would
receive, runs the same content pipeline, and produces artifacts through the same S-12 and S-13
paths. The hosted service is a deployment of glibre's local tooling, not a separate system.

**Why.** A second build system parallel to the local one is a second source of disagreement; bugs in
cloud builds would not reproduce locally. Reusing the same pipeline means a local repro is always
possible.

**Anticipated.** The hosting story (self-hosted vs glibre-hosted vs third-party-hosted), its cost
model, and its integration into the marketplace are operational concerns deferred to a committed
scope that promises cloud build.

**Deferred.** Whether cloud build ships as part of an early committed scope or waits is itself a
scope-prioritization decision.

**Failure mode absorbed.** The shared-pipeline commitment gives the maker a local repro path for any
cloud build failure. A cloud system that diverges from the local tooling violates this commitment;
that divergence is a defect to fix, not an acceptable trade-off.

## AI providers are extensions

**Committed.**

Per [`30-authoring-and-no-code.md`](30-authoring-and-no-code.md), AI-assist accelerates authoring
but is not a separate authoring surface. The integration of specific AI providers — which providers
the engine talks to, how authentication is handled, what privacy posture each integration adopts,
how cost is allocated — is the operational surface of that commitment. Each provider integration is
itself an extension (per the Extensions-are-first-class and Editor-side extension isolation stances
above, and per the extension definition in [`10-bounded-contexts.md`](10-bounded-contexts.md)) with
the same trust evaluation, the same installation flow, and the same maker-can-uninstall property as
any other extension.

**Why.** AI is not different from other engine integrations in any way that justifies a separate
mechanism. Treating it as just another extension keeps the engine from growing a parallel
integration surface specifically for AI, which would have to be re-decided every time a new provider
matters.

**Anticipated.** Privacy posture (where prompts and content go, what is retained, who sees it),
cost-allocation model (per-maker, per-organization, marketplace-mediated), and provider-neutrality
are extension-instance concerns: each provider's extension carries its own posture, and the engine
does not adopt one engine-level stance covering all of them. This resolves the forward-reference
from [`30-authoring-and-no-code.md`](30-authoring-and-no-code.md): no engine-wide AI-provider policy
is committed here.

**Deferred.** The default set of AI providers shipped with the engine — if any — is a
scope-prioritization decision.

**Failure mode absorbed.** A new AI provider does not require an engine-level change to integrate;
the engine does not bind to any specific provider; the maker can install, trust, and uninstall AI
providers like any other extension.

## What this pillar deliberately does not decide

- **The specific package format, installer mechanism, or signing mechanism used per
  platform.** Tactical; downstream.
- **The specific version-control vendor.** Tactical; downstream.
- **The hosting infrastructure for cloud builds.** Tactical; downstream and
  scope-dependent.
- **Specific marketplace UI, search, and rating mechanics.** Tactical; downstream.
- **Specific AI provider integrations.** Each is its own extension's concern.
- **The shape of the trust-evaluation model for extensions.** Owned downstream; this
  pillar commits to the trusted/untrusted distinction, not to its mechanics.

## Risks deferred to `90`

- **Extension API versioning.** A growing third-party extension ecosystem creates a wide
  surface for project-state assumptions to leak into. Versioning the extension surface so
  contributors can keep working as the engine evolves is a serious concern that grows
  with adoption.
- **Mod-trust-in-a-shipped-runtime.** Letting players install mods into a shipped game
  is a player-side trust problem the maker inherits. The out-of-process isolation
  stance helps but does not absolve the maker of curating mod sources.
- **Marketplace governance.** A non-gatekeeping marketplace still has questions about
  what gets featured, what gets removed for legal reasons, and who arbitrates disputes.
  The vision refuses gating; it does not commit to laissez-faire.
- **Cloud-build cost allocation.** A hosted build service has real per-build costs;
  whether those are bundled, subscription-based, pay-per-build, or self-hosted-only is
  a business model question that will shape adoption.
- **AI-provider lock-in pressure.** Even with AI providers as extensions, a provider
  whose features the engine integrations rely on may create a soft lock-in. The
  provider-neutrality commitment is easy to make and hard to keep.
- **Per-platform certification overhead.** Platforms whose distribution channels
  require maker-owned credentials and certification processes — mobile, console, and
  some desktop distribution platforms — impose a maker-facing cost that Distribution
  surfaces but does not absorb. Surfacing this clearly before a slice commits to a
  certified target is a maker-facing concern.
- **VC-of-large-binaries side-channel.** Carried forward from
  [`60-content-pipeline.md`](60-content-pipeline.md). Distribution depends on it
  working but does not own it.
