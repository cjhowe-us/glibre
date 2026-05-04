# Design Philosophy

## Principles

1. **SOLID — SRP first**. Every module owns one responsibility. Split
   when two reasons to change appear.
2. **Cohesion AND completeness**. Not at odds — well-chosen abstractions
   build the foundation that completeness rests on. Small bounded
   contexts with clean seams; each context complete within its scope.
   Reject the false trade-off that asks us to ship half-built modules
   for the sake of breadth.
3. **Minimal core, plugin-only growth**. Core hosts the codegen-driven
   archetype ECS, plugin loader, hot-reload barrier, frame loop, type
   registry, asset handles. Every domain (render, physics, audio,
   scripting, editor UI) is a plugin `.dylib`.
4. **Spec → story → test → code**. Stories are testable acceptance
   criteria; tests come from stories; code comes from tests.
5. **Greatly reduced MVP scope**. A fraction of the long-term horizon,
   designed end-to-end before any feature creep.
6. **Static codegen everywhere it fits, zero runtime reflection** in
   shipping builds. ECS archetype storage, component access, schema
   serialization, plugin manifest types, visual graphs (logic,
   material, effects), shader permutations all emit hand-written-shape
   C++ / Slang at build time. No third-party ECS library; the engine
   owns its archetype layout end-to-end.
7. **Determinism by default**. Physics + ECS world snapshots byte-equal
   across hosts and runs. No platform intrinsics in simulation. Fixed
   container iteration order.
8. **Hot-reload at frame boundaries**. Drain → swap → migrate → resume.
   Never mid-frame.
9. **Plugin ABI gated by middleman dylib hash**. Refuse load on hash
   mismatch.
10. **Occam's razor at every decision**. Two collapsing requirements
    become one primitive. Record the collapse in the spec.
11. **EASTL replaces the C++ standard library for runtime data
    structures**. All containers, strings, smart pointers, `optional`,
    `variant`, `tuple`, `pair`, and `function` come from `eastl::`,
    not `std::`. Reasons: explicit allocator-by-value (per-system
    arenas, no global heap), no exceptions in the hot path, frame /
    fixed / inline allocators, slot-map and intrusive list primitives,
    deterministic iteration where required, debug instrumentation that
    matches game-development workloads. `std::` is retained only for
    language/runtime utilities EASTL does not own: `std::expected`,
    `std::format`, `std::chrono`, `std::filesystem`, `std::thread` /
    `std::mutex` / `std::atomic`, `std::source_location`,
    `std::span` (when interop with non-EASTL APIs is required),
    type-traits / concepts, `std::move` / `std::forward`. Public
    plugin ABI surfaces never expose `std::` containers or
    `eastl::` containers — they cross the boundary as POD spans /
    handles only (see plugin-abi decision record).

## Anti-patterns we reject

- Cross-domain abstractions invented before two concrete users exist.
- Per-domain reinventions of graph runtimes, hot-reload, or error types.
- Reflection-driven runtime VMs for gameplay logic.
- Serialized render-graph files. Render graph is C++ code, visualized
  live by the editor.
- Time estimates. We use story points only.

## How harmonius is used

`/Users/cjhowe/Code/harmonius/` is unreliable prior art produced by an
older model. Treated as **input** for fresh research, not authority.
Stories and design sketches mined; conclusions independently re-derived.
Glibre is not a port and does not "preserve" harmonius decisions.
