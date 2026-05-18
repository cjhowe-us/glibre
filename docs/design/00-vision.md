# 00 — Vision

## Thesis

**glibre is a cross-platform engine where the path from idea to shipped game contains no
programming.** Makers — designers, artists, writers, hobbyists — compose, simulate, and ship
interactive experiences end-to-end inside one tool. The maker never writes or reads source code.

This is not a "code-friendly visual scripting layer on top of a programmer engine." The no-code
surface is the canonical authoring surface. Every primitive the engine offers is expressible there.
Anything that cannot be expressed visually is not yet a feature.

## Who the engine is for

- **The maker who has the game in their head, not the code.** Their fluency is in
  systems, mechanics, levels, characters, story, and feel — not in C-family syntax.
- **The small team that needs leverage.** A handful of generalists shipping a project
  that would otherwise need a dedicated programmer per subsystem.
- **The hobbyist who wants to finish.** Lower the activation energy from "learn a
  programming stack" to "open the editor and start composing."

The engine is **not** aimed at AAA studios with bespoke pipelines and in-house engine programmers.
Those teams will outgrow the no-code constraint; the engine does not bend to keep them.

The no-code surface has a ceiling, and Murphy guarantees someone hits it. When that happens the
honest answer is *contribute to the engine* (which is open source) or *use a different engine* — not
bolt a scripting language onto the user-facing surface. Where this ceiling sits and how contributors
extend the engine itself are out of scope for this pillar.

## What "no-code" means here

- **Authoring is composition, not transcription.** Mechanics, logic, materials,
  animation behaviors, audio behaviors, and AI behaviors are all assembled from typed,
  composable primitives in visual editors.
- **The visual surface is total.** Gameplay, shaders, audio behavior, animation control, and
  tool automation are all authored in the same visual idiom. Learn it once, apply it everywhere.
- **Authoring tools are first-class engine features, not afterthoughts.** Every primitive the
  engine exposes has an inspector, an editor, and a preview at the same quality bar as the
  runtime that uses it.
- **Live feedback is the default authoring mode.** Authoring is interactive composition, not
  an edit-build-run cycle. Where the runtime can show the result of a change as the change is
  made, it does.
- **Programming is unnecessary, not forbidden.** Shipping a finished game never requires
  writing or reading source code.

## What the engine commits to

- **Cross-platform without compromise.** A game authored once ships to every platform the
  engine supports — same authoring, no per-platform reauthoring. The set of supported
  platforms is a strategic decision deferred to platform strategy; the parity promise is not.
- **Accessibility and localization are first-class properties of the authoring model,**
  not an opt-in overlay added after the fact. A primitive that cannot be made accessible or
  translatable is an incomplete primitive.
- **Open by default.** The engine is open source; user projects are owned by their
  authors; no per-seat fees and no revenue share.
- **Determinism where it earns its keep.** Subsystems that need it (replays, lockstep
  multiplayer, automated testing) get a deterministic path; the rest of the engine stays
  pragmatic.
- **Live preview anywhere a preview makes sense.** Materials, entities, scenes,
  animations, audio — previewed inside the editor with fidelity matching what the shipped
  runtime produces.

## What the engine refuses

- **No "first write some boilerplate."** No required scripting language step, no
  required scene file hand-editing, no required build pipeline configuration.
- **No half-built visual editors used as marketing while the real work happens in code.**
  If a capability is shipped, it is fully expressible no-code.
- **No platform supremacy.** Authoring is not better on the host OS than on the others.
- **No abandoned authoring features.** Every editor included is maintained at the same
  bar as the runtime it authors.
- **No closed marketplaces gating basic features.** Core authoring works without an
  account, a subscription, or an asset store transaction.

## Non-goals

- **Replacing programmer engines.** Teams who want to write engine extensions in code
  are better served by other tools. glibre's leverage comes from the no-code constraint;
  removing it removes the leverage.
- **Universal genre coverage.** The engine grows by **committed scopes** — small, named,
  end-to-end vertical proofs that each unlock a new class of game. A genre is supported
  when a committed scope claims it, and not before.
- **Web/browser authoring or runtime.** Out of scope for this vision horizon. The editor is
  a desktop application; runtime targets are platform-strategy concerns, and the browser is
  not among them.
- **AAA-grade rendering as the headline.** Rendering must be credible and modern; it is
  not the differentiator. The no-code path is the differentiator.
- **Live multi-user collaborative authoring.** Real-time co-edit is not part of the
  engine this vision describes. How a team divides work across authors is a downstream
  concern.

## How success is judged

A non-programmer opens glibre, authors a small interactive 3D scene with logic, materials, input,
and audio, runs it inside the editor, then packages it for a second platform — all without writing
or reading source code, all in a single session. Until that path is real and pleasant, nothing else
about the engine matters.

The engine grows from there by **committed scopes** (defined above). A scope is not finished until
its acceptance signals are real for a real maker; the engine does not move past a scope by claiming
it on a roadmap.

Every later pillar in this design set cites this thesis. When a tactical decision is in tension with
the no-code path, the no-code path wins.
