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
11. **libc++ standard library is canonical for runtime data structures**
    (see [reviews/decisions/eastl-removal.md](reviews/decisions/eastl-removal.md)).
    > Amended 2026-05-11 per reviews/decisions/flatbuffers-vs-fory.md.

    Containers, strings, smart pointers, `optional`, `variant`, `tuple`,
    `pair`, and function objects come from `std::*` or `std::pmr::*`
    (polymorphic allocators), not external libraries. The per-context
    allocator substrate is `glibre::PerContextAllocatorResource` — a
    `std::pmr::memory_resource` adapter that wraps `glibre::PerContextAllocator`
    and threads the per-tag allocation budget through every `std::pmr::*`
    container. `std::ranges` (C++20) is the canonical boundary-iteration
    vocabulary; range concepts (`std::ranges::input_range auto`,
    `std::span<const T>`) and pipe-syntax (`| std::views::filter(...) |
    std::ranges::to<std::pmr::vector<T>>()`) replace hand-rolled iterator
    pairs and loops. C++23/26 stdlib features not yet shipped by libc++ on
    the locked toolchain are polyfilled under `core/include/glibre/compat/`
    (one header per feature, deleted when libc++ catches up).

    **Plugin ABI surface rules.** Public plugin ABI surfaces never expose
    `std::` or `std::pmr::` containers — they cross the boundary as POD
    spans / handles or as **Flatbuffers-generated accessor types**
    (offset tables, `flatbuffers::Offset<T>`, `FlatBufferBuilder`,
    table-accessor pointers). The libc++ container prohibition stands
    because `std::*` layout depends on libc++ version and ABI flags,
    which we cannot pin across plugin builds. Flatbuffers-generated
    types satisfy the same offset-stable-ABI invariant via a different
    mechanism: the offset-table layout is part of the Flatbuffers
    binary format specification, so it is host-invariant by
    construction and stable across every plugin compiled against the
    same `.fbs` source. The `glibre_types_abi_hash` gate
    (`reviews/decisions/plugin-abi.md` §ABI Hash Function) enforces
    that every loaded plugin was built against an identical schema
    set. See `reviews/decisions/flatbuffers-vs-fory.md` for the
    full re-derivation.

    **Flatbuffers buffer ownership across the dylib boundary — two mutually
    exclusive regimes apply; for any given buffer, exactly one is in effect
    and the call site must document which.**

    *Clause A — Borrow semantics (default read path).* Flatbuffers buffers
    crossing the plugin ABI in the read path are passed as
    `std::span<const std::byte>` over plugin-owned, plugin-allocated memory.
    The host borrows; it **MUST NOT** call any deallocator on those bytes; the
    plugin owns the lifetime until the host's borrow returns. Mutating
    `FlatBufferBuilder` instances always live inside one dylib and are never
    passed across the ABI seam.

    *Clause B — Ownership transfer (explicit, opt-in).* Plugins that need to
    hand a buffer's ownership to the host (e.g. for cross-frame retention)
    allocate the buffer through the host's `PerContextAllocatorResource` from
    the start — obtained via `PluginContext::allocator_resource()` — and expose
    a C-ABI shim `glibre_plugin_release_<schema>(std::byte*) noexcept`. Because
    the allocator is the host's PMR resource, the host's deallocator is the
    host's own; the plugin **MUST NOT** touch those bytes after the shim
    returns. This preserves the original §11 invariant: no third-party
    deallocator runs across the dylib boundary.

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
