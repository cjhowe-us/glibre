# Long-term Plan

Match the full feature horizon mined from harmonius (~3,162 stories)
under a coherent architecture. Path is incremental plugin addition; SRP
keeps core untouched.

## Drives

| Drive                              | New context(s)            | Epic |
|------------------------------------|---------------------------|------|
| AI (BTs, perception, planners)     | ai                        | TBD  |
| Networking (replication, rollback) | networking                | TBD  |
| D3D12 backend                      | render-d3d12, platform-win| TBD  |
| Linux + Vulkan polish              | platform-linux            | TBD  |
| Collaborative editing              | tools (extends)           | TBD  |
| Asset versioning + build farm      | content (extends)         | TBD  |
| Procedural generation              | procgen                   | TBD  |
| Cinematics / timeline              | tools, simulation         | TBD  |
| Advanced rendering (DDGI, RTGI, volumetrics, hair, water) | render | TBD |
| Game-framework primitives (containers, graphs, tables, attributes, grids, timelines, event logs) | game-framework | TBD |

Stories for these epics remain drafts in the harmonius mirror until the
post-MVP slice that owns them lands; only after that does triage begin
in earnest.
