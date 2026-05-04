# Post-MVP Plan

Layered onto MVP without core rewrite. Each epic independent; ordering
flexible. All work tracked in GitHub Issues; this file maps epics
to issue numbers.

## Epics

| Epic                              | New context(s)              | Epic Issue |
|-----------------------------------|-----------------------------|------------|
| Virtualized geometry              | geometry (extends)          | TBD        |
| Hybrid-RT expansion (AO/refl/GI)  | render (extends)            | TBD        |
| Visual scripting (logic graph)    | scripting                   | TBD        |
| Material graph editor             | material                    | TBD        |
| Effects/VFX graph                 | vfx                         | TBD        |
| Render graph editor (viewer)      | tools (extends)             | TBD        |
| Skeletal animation                | animation                   | TBD        |
| Audio (spatial + mixer)           | audio                       | TBD        |
| Spatial indexing + NavMesh        | simulation                  | TBD        |
| Profiler UI                       | tools (extends)             | TBD        |
| Windows + Vulkan backend          | render-vulkan, platform-win | TBD        |

## Sequencing notes

- Virtualized geometry depends on MVP `geometry` + `render` epics.
- Visual scripting depends on `core` plugin + hot-reload (already in
  MVP).
- Cross-platform delays until macOS feature surface stabilizes; SDL3
  and Slang/slangc choices keep this unblocked.
