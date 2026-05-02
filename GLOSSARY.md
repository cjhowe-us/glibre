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

All live in GitHub Issues; not in the repo filesystem.

- **User Story** — GitHub issue, `type:user-story`. Persona-grounded
  testable acceptance criterion. `As a <persona>, I want <capability>,
  so that <outcome>.` Includes acceptance criteria + story points.
- **Task** — GitHub issue, `type:task`. Unit of implementation work;
  story-pointed; satisfies ≥1 user story; sub-issued under an epic.
- **Epic** — GitHub issue, `type:epic`. Milestone-scale; tracks an
  entire context's MVP / post-MVP / long-term slice.
- **Spike** — `type:spike`. Time-boxed research with concrete output.
- **Story Point** — Fibonacci 1/2/3/5/8 relative effort. > 8 must split.
- **Pull Request** — granular, single-purpose, conventional-commit
  subject (`feat(core):`, `fix(render):`, `chore(repo):`, etc.). One
  task issue may have multiple PRs.
