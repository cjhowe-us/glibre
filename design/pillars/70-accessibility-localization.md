# 70 — Accessibility and Localization

Cites the thesis in [`00-vision.md`](00-vision.md), the contexts in
[`10-bounded-contexts.md`](10-bounded-contexts.md), the integration shape in
[`15-context-map.md`](15-context-map.md), the substrate stances in
[`20-platform-strategy.md`](20-platform-strategy.md), the authoring philosophy in
[`30-authoring-and-no-code.md`](30-authoring-and-no-code.md), the runtime architecture in
[`40-runtime-architecture.md`](40-runtime-architecture.md), the rendering posture in
[`50-rendering-strategy.md`](50-rendering-strategy.md), and the content pipeline in
[`60-content-pipeline.md`](60-content-pipeline.md).

This pillar pins what "accessibility and localization are first-class" means in practice. The vision
commits to A11y & L10n as built-in properties of every authored primitive (per
[`00-vision.md`](00-vision.md)); this pillar defines the authoring-time commitments, the runtime
model, and the relationship with the platform's assistive technology — without specifying which
assistive APIs, which screen-reader semantics, or which translation formats are involved. Those are
tactical and live in downstream design.

## How to read this pillar

Decisions are presented as **stances** with *committed* / *deferred* / *anticipated* tags. Failure
modes are named per Murphy. The contracts committed here are published by A11y & L10n and conformed
to by every producing context (per S-10 in [`15-context-map.md`](15-context-map.md)); the resulting
engine-side model is delivered to OS assistive technology by Platform (per S-11).

## A11y & L10n are properties of primitives, not overlays

**Committed.**

Every authored primitive in glibre — entity, scene, prefab, graph node, material, audio clip,
animation state, UI element — carries accessibility and localization properties as part of the
primitive itself. There is no separate "accessibility pass" applied after authoring, no parallel
"localized strings file" the maker maintains by hand. The semantic metadata for a primitive and the
translatable string identities it references are part of the primitive, edited where the primitive
is edited.

**Why.** A11y and L10n bolted on after the fact are a11y and L10n that fail. The vision in
[`00-vision.md`](00-vision.md) commits to "first-class properties of the authoring model"; making
them properties of primitives is what that commitment looks like operationally.

**Failure mode absorbed.** A maker cannot author a primitive that "doesn't have" accessibility or
localization. The default is filled in; the maker can override; an intentional skip is an authored
decision the engine surfaces, not a quiet omission.

## Authoring-time linting enforces the incomplete-primitive rule

**Committed.**

A primitive without sufficient accessibility metadata, or with raw text that has no string identity,
is treated by the editor as **incomplete**. The editor surfaces this the same way it surfaces a
graph type error or a broken asset reference: visible, named, and on the maker's todo list. Whether
the editor blocks packaging at build time or surfaces incompleteness as a workflow signal is a
downstream UX decision (see Deferred below); the commitment here is that incompleteness is *named*,
not that any particular gate is in place.

**Why.** The vision's rule — "a primitive that cannot be made accessible or translatable is an
incomplete primitive" (per [`00-vision.md`](00-vision.md)) — needs a place where it is enforced. The
editor is that place; making it a linting concern means the maker sees it during authoring, not in a
player review.

**Deferred.** The exact UX of how incompleteness is surfaced — inspector badges, a project-level
health panel, build-time gates — is owned by Authoring's downstream design.

**Anticipated.** A maker may legitimately decide a primitive is intentionally not accessible (a
developer-only debug overlay, a watermark that has no semantic meaning). The editor treats such
cases as explicit opt-outs the maker authors, not silent omissions; the opt-out is itself a property
the maker sets.

**Failure mode absorbed.** A finished project does not contain primitives with missing a11y or l10n
by accident. Every gap is a maker decision the engine recorded.

## String identity, not raw text

**Committed.**

Every piece of player-facing text in the project is a **string identity**, not a literal string (per
[`10-bounded-contexts.md`](10-bounded-contexts.md)'s ubiquitous language). The actual text is
resolved at display time from a per-language **string catalog** owned by A11y & L10n. The maker
authors and edits text in the editor; the editor manages the identity behind the scenes.

**Why.** A literal string in a graph or a property is a string the maker has to find and update
twice when localization is added — and is exactly what doesn't get found. An identity is the unit of
translation; making it the only kind of text the engine knows about means no translation effort
starts by hunting for raw strings.

**Anticipated.** Pluralization rules, grammatical-gender concerns, message ordering for languages
with different syntax, and date/number formatting are part of the string-identity model rather than
separate features. Specific catalog formats and translation-workflow integrations are tactical and
downstream.

**Failure mode absorbed.** A localization pass on a finished project is a translator's job, not a
maker's hunting expedition. This promise depends on stable identity through rename and refactor,
which is an open risk tracked in the Risks section below; absent that, translations can be silently
orphaned.

## The semantic tree is a property of the running scene

**Committed.**

At runtime, the world (per [`10-bounded-contexts.md`](10-bounded-contexts.md)) projects a
**semantic tree** for assistive technologies to consume. Each entry is a **semantic node** (per the
ubiquitous language in `10`) describing a visible, interactive, or audible element of the current
scene — its role, its focusable structure, and its state. The semantic tree is *derived* from the
same primitives the renderer and the simulator are operating on; it is not authored separately.

**Why.** A separately-authored semantic tree would drift from the primitives it describes — every
scene change a maker makes would have to be re-mirrored in the semantic tree by hand. Deriving the
tree from the primitives means the tree updates automatically as the maker edits.

**Deferred.** Specific semantic roles, focus models, and the granularity at which the tree updates
are owned by A11y & L10n's downstream design.

**Failure mode absorbed.** A scene that looks accessible in the editor is accessible at runtime; a
maker change to a primitive shows up in the semantic tree without a separate authoring step.

## Platform delivers, A11y & L10n shapes

**Committed.**

Per S-11 in [`15-context-map.md`](15-context-map.md), A11y & L10n is a downstream conformant of
Platform's OS assistive-technology seam: the engine-side semantic tree and translated strings are
delivered to the player's OS assistive stack by Platform, in the shape that platform expects. A11y &
L10n does not call OS assistive APIs directly.

**Why.** OS assistive APIs differ per platform (screen-reader semantics, focus models, voice-control
hooks) and evolve independently of the engine. Treating Platform as the interop seam and A11y & L10n
as the engine-side model keeps the model coherent across platforms while letting each platform's
interop seam adapt to its OS.

**Anticipated.** Platforms whose assistive surfaces differ enough to require a per-platform shaping
pass on the semantic tree are a Platform/A11y boundary concern; the engine-side model commits to
producing data Platform can shape, not to anticipating every platform's quirks.

**Failure mode absorbed.** An engine-side change to the semantic model does not require edits in
every OS-specific assistive integration; the per-platform shaping pass is owned by Platform, not the
model.

## Reduced-motion, color-aware, and contrast modes are first-class authoring concerns

**Committed.**

The engine treats reduced-motion, color-perception differences, and contrast preferences as authored
properties of the primitives that respond to them. A maker authoring a particle effect declares its
reduced-motion behavior. A maker authoring a material declares its color-independence affordance. A
maker authoring a UI element declares its contrast posture. These are not "settings the engine flips
on the maker's behalf"; they are authoring decisions with engine support.

**Why.** A blanket reduced-motion mode that just disables animations is the wrong shape: it removes
communicative animation along with decorative animation. The maker is the only one who knows the
difference. The engine's role is to make declaring that distinction cheap and to surface it in the
inspector.

**Deferred.** The default behavior of each authored property in the absence of explicit
configuration is a sensible-default decision owned by A11y & L10n's downstream design; the
*commitment* to authored-property-shape is here.

**Failure mode absorbed.** A reduced-motion player gets the maker's intended reduced experience, not
a blanket disable; a color-blind player gets the shape redundancy the maker authored, not a
heuristic guess.

## Localization is structural, not retrofitted

**Committed.**

A project can be authored in one language and shipped in many without re-authoring. All authoring
surfaces work the same way regardless of which languages the project ships in; adding a language is
adding a translated catalog, not re-touching the graphs, scenes, or prefabs.

**Why.** Localization-as-retrofit is the failure mode this commitment refuses. A graph or a scene
that "works in English and breaks in German" because of hard-coded layout assumptions is a primitive
that wasn't authored localization-aware. The engine pushes those assumptions out at the primitive
level (string identity, layout that flexes, right-to-left support, language-dependent formatting) so
they don't accrete.

**Anticipated.** Right-to-left support, vertical-script support, and font-fallback chains are
downstream concerns owned by the rendering and UI material design; A11y & L10n commits the
*property-level* shape, not the rendering.

**Failure mode absorbed.** Adding a language to a project does not become a port of the project. It
is a translation pass against an already-localization-aware project.

## What this pillar deliberately does not decide

- **Which specific assistive APIs the engine targets per platform.** Owned by Platform's
  downstream design.
- **The catalog format for string catalogs.** Tactical; owned by Content's downstream
  design.
- **Specific accessibility audit checklists** (WCAG levels, platform-specific
  certifications). Tactical; downstream.
- **The translation workflow integrations** (translator tools, vendor handoffs).
  Tactical; owned by `80-distribution-and-extensibility.md` when written.
- **The visual style of accessibility affordances** (focus indicators, large-text
  rendering). Owned by the editor and rendering downstream designs.
- **Voice-input or alternative-input device handling.** Authored as input maps in
  Runtime; the assistive surface is here in spirit, the mechanism is in Runtime.

## Risks deferred to `90`

- **Authoring-tax of always-on linting.** Treating incompleteness as a project-level
  error message costs the maker attention. The engine commits to the rule; the rate at
  which the rule is enforced (real-time, on save, on build) is a maker-experience
  trade-off that requires real-project data.
- **String-identity churn under refactoring.** Renaming or restructuring an identity
  during authoring without breaking already-translated catalogs is a workflow concern
  the maker hits during real projects. The mechanism for stable-identity-through-rename
  is open.
- **Per-platform assistive divergence.** Two committed platforms with substantially
  different assistive surfaces (e.g. desktop OS screen readers vs mobile screen readers)
  may require per-platform shaping that goes beyond what Platform's S-11 conformist
  seam absorbs cheaply.
- **Reduced-motion authoring overhead.** A maker authoring elaborate motion has to
  declare reduced-motion behavior for each, which is real work. The engine should keep
  the default sensible enough that most primitives need no override.
- **Localization-aware layout in 2D and UI.** The pillar commits to making text identity
  the unit of translation, but layout-level localization (mirroring for RTL, text growth
  in some languages) lives at the boundary between A11y & L10n's identity model and the
  rendering / UI authored layout. That boundary needs explicit design before a slice
  ships an RTL language.
- **Maker-facing accessibility coverage gaps.** Even with the incomplete-primitive rule,
  the engine cannot verify that the maker's *semantics* are accurate (the label says
  "play button" but it is in fact a quit button). Detection of this class of error is
  outside what authoring-time linting can do; it is a downstream concern.
