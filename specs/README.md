# Specs

Bounded-context design lives here. Each `<context>/SPEC.md` follows
`_TEMPLATE.md` §1–§12. Additional design files (`design-N.md`,
`integration-N.md`) live alongside.

Layout mirrors harmonius 15-subsystem decomposition (re-derived, not
ported):

- **Foundation**: `core-runtime/`, `platform/`
- **Mid-level**: `rendering/`, `physics/`, `geometry/`, `ui/`, `input/`
- **Domain**: `ai/`, `animation/`, `audio/`, `networking/`, `vfx/`
- **Data systems**: `data-systems/` (graphs, tables, attributes,
  containers)
- **Simulation**: `simulation/` (grids, awareness, timelines, event
  logs)
- **Application**: `game-framework/`, `tools/`, `content-pipeline/`
- **Cross-cutting**: `integration/` (pair-wise contracts)

## Storage rule

Designs live in this directory. Plans (task breakdowns, leaves,
spikes, stories) live in GitHub Issues. Any change that invalidates a
design here must update the affected files in the same PR.
