# Glossary

Engine-level ubiquitous language. Per-context glossaries refine these
inside each `specs/<context>/SPEC.md`. Terms used unchanged in code.

## Core

- **Entity** — opaque ID; addresses a row across one archetype's columns.
- **Component** — typed value attached to an entity; SoA-stored per
  archetype chunk.
- **Archetype** — unique component-set; owns chunked SoA storage.
- **System** — function over `World` queries; runs in a frame phase.
- **Schedule** — DAG of systems derived from read/write component sets.
- **World** — root container of archetypes, relationships, change ticks.
- **Plugin** — `.dylib` exporting `glibre_plugin_register(World&)`.
- **Middleman** — `glibre-types.dylib` shared by all plugins; ABI hash.
- **Hot-Reload Barrier** — frame-boundary point where plugins swap.
- **Frame Phase** — input / sim / physics / animation / transform / cull
  / render / reload / present.

## Data

- **Schema** — Fory-described type layout, codegen target.
- **Reflection Blob** — runtime descriptor produced by Fory codegen;
  drives inspector + serialization.
- **Migration** — function that converts old-version data to new.
- **CAS** — content-addressable store keyed by BLAKE3 hash.
- **Residency** — memory-bounded LRU policy over loaded assets.

## Rendering

- **Render Graph** — DAG of passes built each frame in C++.
- **Pass** — typed read/write resource declarations + execute lambda.
- **Resource** — transient (aliased) / persistent / imported texture or
  buffer.
- **Shader Backend** — `IShaderBackend`: compile, reflect, link.
- **Reflection** — DXIL container metadata exposing bindings, vertex IO.
- **Permutation Key** — `(ShadingModel, Features, RenderPath, LOD)`.
- **PSO** — pipeline state object; cached on `(shader_hash, state_hash)`.
- **Meshlet** — ≤64 vert / 124 prim cluster (meshoptimizer output).
- **Cluster DAG** — Nanite-style hierarchical meshlet LOD graph.
- **BLAS / TLAS** — Metal acceleration structures for hybrid RT.

## Geometry

- **Mesh** — vertex + index streams.
- **Meshlet Pak** — cooked binary blob of meshlets + bounds + LOD
  links. Vertex / index streams Draco-compressed at cook time;
  decoded into GPU-resident format at load.

## Tools

- **Scene** — Fory document of entities, components, references.
- **Inspector** — auto-generated UI from reflection.
- **Trace** — `.glibre-trace` file: scripted input + assertions for E2E.

## Stories, Tasks, Issues

All live in GitHub Issues; not in the repo filesystem. Six types; no
tracking, no roles, no kinds.

Hierarchy: **initiative → epic → sub-epic → { plan | spike }**.
Aggregators carry no estimate; leaves do; estimates roll up.

- **Initiative** — `type:initiative`. Top-level. Groups epics. No
  estimate. Source of truth for plan execution status; subagents post
  updates here in English.
- **Epic** — `type:epic`. Aggregates sub-epics / plans / spikes under
  an initiative. No estimate. SRP-bounded scope.
- **Sub-Epic** — `type:sub-epic`. Nested aggregator under an epic
  (or another sub-epic). No estimate.
- **User Story** — `type:user-story`. Persona-grounded testable
  acceptance criterion. `As a <persona>, I want <capability>, so that
  <outcome>.` Each story carries a manual test script and an E2E test
  plan (replay trace + assertions). Closure rule: E2E green in CI
  before manual testing; story closes only after manual PASS recorded.
  Has its own `pts:*` estimate.
- **Plan** — `type:plan`. Leaf implementation unit. Carries a unit
  test plan (≥1 Catch2 test name). Closed by one or more granular
  Conventional Commit PRs.
- **Spike** — `type:spike`. Time-boxed research / design. Output =
  doc, decision record, or prototype branch.
- **Story Point** — Fibonacci 1/2/3/5/8 relative effort. > 8 splits.
  Rolls up from leaves; aggregators never carry one.
- **Pull Request** — granular, single-purpose, Conventional Commit
  subject. A `type:plan` may have one or more PRs.
