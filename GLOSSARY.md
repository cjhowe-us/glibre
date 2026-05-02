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
- **Meshlet Pak** — cooked binary blob of meshlets + bounds + LOD links.

## Tools

- **Scene** — Fory document of entities, components, references.
- **Inspector** — auto-generated UI from reflection.
- **Trace** — `.glibre-trace` file: scripted input + assertions for E2E.

## Stories, Tasks, Issues

All live in GitHub Issues; not in the repo filesystem. Three kinds only.

- **Tracking Issue** — `type:tracking`. Non-leaf. Aggregates child
  issues. **No own story-point estimate**; total = sum of leaves.
  Source of truth for plan execution status; subagents post updates
  here as comments in English. Plays one of four roles via `role:*`:
  `role:epic` (full context slice), `role:plan` (one Claude Code
  session), `role:slice` (mid-level grouping), `role:review` (review
  iteration).
- **User Story** — `type:user-story`. Persona-grounded testable
  acceptance criterion. `As a <persona>, I want <capability>, so that
  <outcome>.` Has its own `pts:*` estimate.
- **Task Execution** — `type:task-execution`. Leaf unit of work.
  Kind label: `kind:implementation`, `kind:design`, `kind:planning`,
  `kind:bug`, or `kind:chore`. Has its own `pts:*` estimate. Satisfies
  ≥1 user story (or none for `kind:chore`); lives under exactly one
  tracking issue parent.
- **Story Point** — Fibonacci 1/2/3/5/8 relative effort. > 8 must split.
  Rolls up from leaves only.
- **Pull Request** — granular, single-purpose, Conventional Commit
  subject. One task-execution issue may have multiple PRs.
