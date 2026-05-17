# 90 — Risks and Open Questions

The living register for risks and unresolved questions surfaced across the design set. This pillar
consolidates what each prior pillar named as deferred or risky, so the rest of the design has a
single place to point at and so the engine never loses track of a known concern.

Entries here are *known* but *not decided*. A risk in this file is a thing the design acknowledges;
a thing this file does not name is a thing the design has not yet seen.

## How to read this register

Each entry has:

- A short label.
- The pillars that raised it.
- A one-paragraph statement of the risk or open question.
- A *blast radius* tag: **slice-blocker** (must resolve before a relevant committed
  scope ships), **engine-wide** (the resolution affects the engine as a whole), or
  **growth-pressure** (the risk grows with adoption rather than landing as a single
  decision).

Entries are not numbered; they may be added, edited, or retired as the design evolves. This file is
intentionally a flat list rather than a hierarchy: items here are deliberately not yet sorted into
the contexts that will eventually own them.

## Substrate and platform

### Three-seam native interop maintenance

Pillars: [`20`](20-platform-strategy.md), [`50`](50-rendering-strategy.md),
[`15`](15-context-map.md) (S-8). **Blast radius:** engine-wide.

Three idiomatic backends — Windows, Apple, Linux/Android — mean every renderer feature must ship on
three stacks before it counts as shipped. The seam choice is intentional (`20` accepts the cost);
the *consequence* is that engine-wide rendering velocity is bounded by the slowest seam.

### Windows-native interop seam single point of failure

Pillars: [`20`](20-platform-strategy.md). **Blast radius:** engine-wide.

The Windows native interop seam is owned by a single Windows-native mechanism. If that mechanism
stagnates or loses upstream maintenance, the engine has no backup path on Windows.

### Console targets are stretch and stay stretch

Pillars: [`20`](20-platform-strategy.md). **Blast radius:** engine-wide.

Naming consoles as stretch in the platform tier table is honest, but as the engine grows its maker
base some will assume consoles are coming. The credibility risk is that "stretch" will be misread as
"soon". Holding the line is a discipline concern, not a design one.

### AOT-clean mobile dependency chain

Pillars: [`20`](20-platform-strategy.md), [`40`](40-runtime-architecture.md). **Blast radius:**
slice-blocker for any iOS-targeted scope.

Mobile AOT means every runtime dependency must itself be AOT-clean. A package that requires runtime
IL emission, dynamic assembly loading, or reflection-heavy serialization is an editor-only
dependency by definition; ensuring no such dependency leaks transitively into the runtime is an
enforcement concern.

### AOT/JIT boundary enforcement mechanism

Pillars: [`20`](20-platform-strategy.md), [`40`](40-runtime-architecture.md). **Blast radius:**
engine-wide.

The rule that the editor and runtime have distinct dependency trees is a hard design rule, but the
mechanism that catches violations at build time is not chosen.

## Viewport and runtime architecture

### Per-platform viewport handshake spikes

Pillars: [`20`](20-platform-strategy.md), [`40`](40-runtime-architecture.md),
[`50`](50-rendering-strategy.md). **Blast radius:** slice-blocker (one per committed authoring
platform).

Embedding a native rendering surface inside an editor window has a per-platform handshake on each of
the three committed authoring platforms (Apple, Windows, Linux). Each is its own spike; any one of
them not landing blocks editor viewport for that platform.

### Hot-reload edge cases per channel

Pillars: [`40`](40-runtime-architecture.md), [`60`](60-content-pipeline.md). **Blast radius:**
engine-wide.

The hot-reload contract (snapshot-boundary, per-channel) is committed; what happens to in-flight
references and long-lived state held by other systems when an artifact is hot-swapped (animation
state pointing at a re-baked skeleton, audio mixer pointing at a re-baked clip, GPU resource handles
referencing a re-baked render artifact, callers in a graph artifact being retired) is open and
per-asset-kind.

### Input routing under modal dialogs

Pillars: [`40`](40-runtime-architecture.md). **Blast radius:** engine-wide.

A modal opened by the editor while an in-editor play session has focus is a known Murphy case. The
routing rule is by focus; the right behavior when focus is forcibly relocated mid-tick is an open
question.

### Process-restart cost

Pillars: [`40`](40-runtime-architecture.md). **Blast radius:** engine-wide.

Launching a runtime process per play session is sound for isolation but pays a startup tax every
time. Caching, warm-launch, or in-place reload are tactical strategies; the cost is named so it does
not surprise a future slice.

### iOS hot-reload deficit

Pillars: [`30`](30-authoring-and-no-code.md), [`40`](40-runtime-architecture.md). **Blast radius:**
slice-blocker for any iOS-exclusive committed scope.

Per [`40`](40-runtime-architecture.md), the iOS-shipped runtime cannot do graph hot-reload (the
platform forbids the underlying runtime mechanism). The in-editor play session (always on a JIT-host
platform) keeps the authoring loop intact; the maker-facing communication of the deficit for
iOS-exclusively-targeted projects is owned by [`30`](30-authoring-and-no-code.md) but deferred to a
scope that commits to iOS-only.

## Rendering

### Tier-vs-feature tension

Pillars: [`50`](50-rendering-strategy.md). **Blast radius:** growth-pressure.

A maker who wants a single feature from a higher tier without paying that tier's full cost is a
known wish. Granting it would break the tier model; holding the line is a discipline concern.

### Shared-renderer renderer-internal drift

Pillars: [`50`](50-rendering-strategy.md). **Blast radius:** engine-wide.

The shared-renderer commitment is structurally enforced by the editor/runtime process split (per
[`40`](40-runtime-architecture.md)) for editor overlays, but *renderer-internal* drift — features
active in one build configuration and not another — is a discipline concern the structural boundary
does not catch.

### Shader-language version churn

Pillars: [`20`](20-platform-strategy.md), [`50`](50-rendering-strategy.md),
[`60`](60-content-pipeline.md). **Blast radius:** engine-wide.

The chosen shader language is an external project with its own release cadence. A version change is
an engine-level concern, not a maker-level one, but its handling is named here so it cannot surprise
a later slice.

### First-launch shader prewarm on some distribution channels

Pillars: [`50`](50-rendering-strategy.md). **Blast radius:** slice-blocker on the affected channels.

Even with build-time bakes, some platforms may require on-device prewarm passes (driver-specific
shader caches). The maker-facing surface for this is downstream; the renderer's no-runtime-compile
commitment is not weakened, but the platform's prewarm is not glibre's compilation.

### Material-vs-shader contributor path discipline

Pillars: [`50`](50-rendering-strategy.md). **Blast radius:** growth-pressure.

Letting contributors write shader source for custom material nodes is the engine's escape hatch from
the no-code constraint at the rendering surface; without discipline, that hatch widens into a
parallel authoring surface the vision refuses.

## Content pipeline

### Large-binary version control

Pillars: [`60`](60-content-pipeline.md), [`80`](80-distribution-and-extensibility.md).
**Blast radius:** slice-blocker for any real-project scope.

The pipeline assumes source assets live in VC. Real projects contain meshes, textures, and audio
that are enormous; conventional VC struggles. How source assets too large for conventional VC are
managed without compromising the VC-as-source-of-truth stance is named in
[`60`](60-content-pipeline.md) but not designed.

### Bake nondeterminism

Pillars: [`60`](60-content-pipeline.md). **Blast radius:** engine-wide.

Some tools used in the bake have nondeterminism modes that can produce subtly different output run
to run. The reproducibility commitment requires choosing deterministic settings consistently; an
enforcement strategy is open.

### Edit-time compiler outage during authoring

Pillars: [`20`](20-platform-strategy.md), [`60`](60-content-pipeline.md). **Blast radius:**
engine-wide.

The shader and graph toolchains depend on out-of-process compilers that the editor invokes during
authoring. If either compiler is missing, mis-installed, or crashes, authoring is blocked even
though the maker's machine is otherwise functional. This is a local-authoring failure mode distinct
from the CI variant below and from a runtime outage (the shipped runtime never invokes the
compiler).

### CI/no-display bake

Pillars: [`20`](20-platform-strategy.md), [`60`](60-content-pipeline.md). **Blast radius:**
engine-wide.

Shader and graph compilation under sandboxed CI environments (no display server, no GPU, locked-down
sandboxes) can fail the build. The pipeline's posture on headless CI is the authoritative place to
resolve this; the resolution is open.

### Importer extension surface

Pillars: [`60`](60-content-pipeline.md), [`80`](80-distribution-and-extensibility.md).
**Blast radius:** growth-pressure.

A growing third-party importer ecosystem creates a wide surface for project-state assumptions to
leak in. Versioning the importer interface is a concern that grows with adoption.

### Asset identity across renames and merges

Pillars: [`60`](60-content-pipeline.md). **Blast radius:** engine-wide.

Maintaining identity through refactoring of a project's filesystem layout is straightforward;
maintaining it across branch merges where two makers have renamed the same asset differently is not.

## Composition and authoring

### Single-substrate cost

Pillars: [`30`](30-authoring-and-no-code.md). **Blast radius:** engine-wide.

Insisting on one logic substrate for gameplay, materials, audio, animation, and tooling means every
subsystem's authoring surface depends on Composition working. A Composition regression hits
everything at once.

### Discoverability scales sublinearly with palette size

Pillars: [`30`](30-authoring-and-no-code.md). **Blast radius:** growth-pressure.

A growing node palette without growing discoverability investment becomes a directory of options the
maker cannot navigate.

### Maker-vs-contributor tension

Pillars: [`30`](30-authoring-and-no-code.md). **Blast radius:** engine-wide.

Maker convenience and contributor convenience pull in opposite directions repeatedly. The named
priority gives the engine a way to notice and choose; it does not eliminate the recurring decision.

### Bake-step UX cost

Pillars: [`30`](30-authoring-and-no-code.md). **Blast radius:** engine-wide.

Compiled graphs are an engine-level win and a maker-level cost. Live preview hides the cost most of
the time; the cases where it doesn't (first build, clean reload, target switch) need explicit
attention.

### Fragment versioning across project history

Pillars: [`30`](30-authoring-and-no-code.md), [`60`](60-content-pipeline.md). **Blast radius:**
engine-wide.

A graph that worked at version N of a fragment may not work at N+1; the fragment's parameter shape
can change. Without a versioning answer, maker projects rot when shared fragments evolve.

### AI-assist drift toward generation-not-assist

Pillars: [`30`](30-authoring-and-no-code.md). **Blast radius:** engine-wide.

"Generate graphs from natural language" is a tempting feature; it contradicts the commitment that
the maker authors and AI accelerates. Holding the line is discipline.

## Accessibility and localization

### Authoring-tax of always-on linting

Pillars: [`70`](70-accessibility-localization.md). **Blast radius:** growth-pressure.

The incomplete-primitive rule is committed; the rate at which it is enforced (real-time, on save, on
build) is a maker-experience trade-off requiring real-project data.

### String-identity churn under refactoring

Pillars: [`70`](70-accessibility-localization.md). **Blast radius:** engine-wide.

A translator hands back catalogs keyed on identities; if identities are renamed during authoring,
translations are silently orphaned. The mechanism for stable-identity-through-rename is open.

### Per-platform assistive divergence

Pillars: [`70`](70-accessibility-localization.md). **Blast radius:** engine-wide.

Two committed platforms with substantially different assistive surfaces may require per-platform
shaping that exceeds what Platform's S-11 conformist seam absorbs cheaply.

### Reduced-motion authoring overhead

Pillars: [`70`](70-accessibility-localization.md). **Blast radius:** growth-pressure.

A maker authoring elaborate motion has to declare reduced-motion behavior for each, which is real
work. Sensible defaults reduce — but do not eliminate — the cost.

### Localization-aware layout boundary

Pillars: [`70`](70-accessibility-localization.md), [`50`](50-rendering-strategy.md).
**Blast radius:** slice-blocker for any RTL- or vertical-script-language scope.

The pillar commits to making text identity the unit of translation, but layout-level localization
(RTL mirroring, text-growth flex) lives at the boundary between A11y & L10n's identity model and the
rendering / UI authored layout. That boundary needs explicit design before a slice ships an RTL
language.

### Maker-facing accessibility coverage gaps

Pillars: [`70`](70-accessibility-localization.md). **Blast radius:** growth-pressure.

Even with the incomplete-primitive rule, the engine cannot verify that the maker's *semantics* are
accurate. Detection of this class of error is outside what authoring-time linting can do.

## Distribution and extensibility

### Extension API versioning

Pillars: [`80`](80-distribution-and-extensibility.md). **Blast radius:** growth-pressure.

Versioning the extension surface so contributors can keep working as the engine evolves is a concern
that grows with the third-party ecosystem.

### Mod trust in a shipped runtime

Pillars: [`80`](80-distribution-and-extensibility.md). **Blast radius:** engine-wide.

The out-of-process isolation stance helps, but a player installing mods is still a trust problem the
maker inherits in part. Curation tooling for mod sources is open.

### Marketplace governance

Pillars: [`80`](80-distribution-and-extensibility.md). **Blast radius:** growth-pressure.

A non-gatekeeping marketplace still has questions about featuring, legal removal, and dispute
arbitration. The vision refuses gating; it does not commit to laissez-faire.

### Cloud-build cost allocation

Pillars: [`80`](80-distribution-and-extensibility.md). **Blast radius:** slice-blocker for any
committed scope that promises cloud build.

A hosted build service has real per-build costs; the business model — bundled, subscription,
pay-per-build, self-hosted-only — is open and will shape adoption.

### AI-provider lock-in pressure

Pillars: [`30`](30-authoring-and-no-code.md), [`80`](80-distribution-and-extensibility.md).
**Blast radius:** engine-wide.

Even with AI providers as extensions, a provider whose features the engine integrations rely on may
create a soft lock-in. The provider-neutrality commitment is easy to make and hard to keep.

### Per-platform certification overhead

Pillars: [`80`](80-distribution-and-extensibility.md). **Blast radius:** slice-blocker for any
certified-platform scope.

Distribution surfaces the certification cost but does not absorb it; the maker still owns
credentials and processes for each certified platform.

### Trust-evaluation model for extensions

Pillars: [`80`](80-distribution-and-extensibility.md). **Blast radius:** engine-wide.

How trust is evaluated for editor extensions (per-extension, per-publisher, signed-only,
quarantine-on-first-run) is a downstream UX and security concern.

### Editor's runtime shader compiler need

Pillars: [`20`](20-platform-strategy.md), [`50`](50-rendering-strategy.md). **Blast radius:**
slice-blocker for any scope that promises live shader authoring in the editor.

Whether the editor needs a runtime shader compiler for live shader editing is a maker-experience
trade-off; the runtime never needs one.

## Integration shape

### S-1 open host service width

Pillars: [`15`](15-context-map.md). **Blast radius:** engine-wide.

The open host service Authoring publishes is the most widely-consumed contract in the engine.
Versioning it as contexts grow primitives is a load-bearing concern.

### S-3 ACL count scales with contexts

Pillars: [`15`](15-context-map.md). **Blast radius:** growth-pressure.

Every new context that contributes nodes adds another node-library ACL. The cost is per-context, not
per-node; the model must stay legible as the count grows.

### S-8 conformist coupling with Platform

Pillars: [`15`](15-context-map.md), [`20`](20-platform-strategy.md). **Blast radius:** engine-wide.

Conformist coupling with Platform is the right trade, but it means OS-level platform changes can
propagate into multiple downstream contexts. The cost is inherited from the substrate, not added by
the context map.

### S-10 obligates every producing context

Pillars: [`15`](15-context-map.md), [`70`](70-accessibility-localization.md). **Blast radius:**
engine-wide.

Treating A11y & L10n as the publisher of the semantic-metadata contract puts compliance pressure on
every producing context. Discipline on both sides is required.

### S-11 dependence on platform assistive APIs

Pillars: [`15`](15-context-map.md), [`70`](70-accessibility-localization.md). **Blast radius:**
engine-wide.

OS-level assistive technology evolves independently; a Platform-level change can force A11y & L10n
into a silent failure mode if Platform's seam shifts without notice.

### S-14 vs S-4 divergence discipline

Pillars: [`15`](15-context-map.md), [`60`](60-content-pipeline.md). **Blast radius:** engine-wide.

Authoring and Runtime resolve assets through separate Content seams. Drift between the two
(identity, dependency, metadata) is a discipline concern the design does not structurally prevent.

### Anticipated Networking seams

Pillars: [`10`](10-bounded-contexts.md), [`15`](15-context-map.md). **Blast radius:** slice-blocker
for Slice-4-and-later.

The shape of Networking's future seams is anticipated, not committed. A Slice-4 commit may
force-revise the context map.

## Determinism

### Determinism enforcement across subsystems

Pillars: [`40`](40-runtime-architecture.md), [`10`](10-bounded-contexts.md),
[`20`](20-platform-strategy.md), [`30`](30-authoring-and-no-code.md). **Blast radius:**
slice-blocker for replay / rollback / deterministic-test scopes.

Subsystems declare a determinism class; deterministic features require deterministic subsystems.
Detecting and refusing nondeterministic compositions is an enforcement concern.

### AOT codegen math reproducibility

Pillars: [`40`](40-runtime-architecture.md), [`20`](20-platform-strategy.md). **Blast radius:**
engine-wide.

Per-platform math lowering and CPU math-mode flags in the AOT compilation toolchain can vary between
targets; without consistent control, the determinism commitment for graph-authored code is
compromised.

## What this file deliberately does not include

- **Risks that are not yet design risks.** Once a risk lands in a pillar's stance with
  a named failure mode, it stops being an open risk and starts being a committed
  acceptance. This file does not duplicate those.
- **Tactical bugs or implementation defects.** Those belong in the codebase tracker, not
  the design set.
- **Risks of committed scopes that have not been claimed.** Slice-2/3/4 specifics are
  not enumerated here; only the design-level risks that block them.

## Retiring entries

An entry is retired when the design commits to its resolution — either a new committed stance covers
it, or a committed scope claims it and absorbs its cost. The entry is deleted (not crossed out); the
git history is the audit log.
