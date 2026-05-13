# Design Philosophy

## Principles

1. **SOLID — SRP first**. Every module owns one responsibility. Split
   when two reasons to change appear.
2. **Cohesion AND completeness**. Not at odds — well-chosen
   abstractions build the foundation that completeness rests on.
   Small bounded contexts with clean seams; each context complete
   within its scope.
3. **Minimal core, plugin-only growth**. Core hosts the codegen-driven
   archetype ECS, plugin loader, hot-reload barrier, frame loop, type
   registry, and asset handles. Every domain ships as a Rust crate
   feeding the codegen middleman `.dylib` in editor builds and
   statically linking under LTO in shipping builds.
4. **Spec → story → test → code**. Stories are testable acceptance
   criteria; tests come from stories; code comes from tests.
5. **Greatly reduced MVP scope**. A fraction of the long-term horizon,
   designed end-to-end before any feature creep.
6. **Static codegen everywhere it fits, zero runtime reflection** in
   shipping builds. ECS archetype storage, component access, schema
   serialization, plugin manifest types, visual graphs (logic,
   material, effects), shader permutations all emit hand-written-shape
   Rust / Slang at build time. No third-party ECS; the engine owns its
   archetype layout end-to-end. No `dyn Reflect`, no `TypeRegistry`,
   no `TypeId`-based dispatch.
7. **Determinism by default**. Physics + ECS world snapshots byte-equal
   across hosts and runs. No platform intrinsics in simulation. Fixed
   container iteration order. No `HashMap` on deterministic hot paths.
8. **Hot-reload at frame boundaries**. Drain → swap → migrate →
   resume. Never mid-frame. Bundled `rustc` + `cargo` recompile the
   middleman `.dylib` only; the engine binary stays stable.
9. **Plugin ABI gated by middleman dylib hash**. Refuse load on hash
   mismatch. Public surfaces cross the seam as `#[repr(C)]` POD
   structs, opaque handles, or `rkyv` zero-copy buffers — never `std`
   collections, never `String`, never `Box<dyn Trait>`.
10. **Occam's razor at every decision**. Two collapsing requirements
    become one primitive. Record the collapse in the spec.
11. **No async in the engine**. `async` / `await`, `Future`, async
    runtimes (`tokio`, `mio`, `compio`) are forbidden in engine,
    editor, and game runtime. Backend services may use async. The
    engine schedules work through a custom Chase-Lev work-stealing
    job system on `crossbeam-deque`; platform I/O is polled on the
    main thread (`io_uring` on Linux, IOCP + DirectStorage on
    Windows, GCD `dispatch_io` + Metal I/O on Apple).
12. **`rkyv` is the sole binary serialization**. Zero-copy mmap of
    baked assets and save files. No `serde`. A custom text scene
    format handles diff / merge.

## Anti-patterns we reject

- Cross-domain abstractions invented before two concrete users exist.
- Per-domain reinventions of graph runtimes, hot-reload, or error
  types.
- Reflection-driven runtime VMs for gameplay logic.
- Serialized render-graph files. Render graph is Rust code,
  visualized live by the editor.
- Time estimates. We use story points only.
- C, C++, Objective-C, Objective-C++, Swift, or `metal-cpp` anywhere
  in the engine, runtime, editor, or tools.
- `winit`, `SDL`, `glfw`. Custom windowing (NSWindow, Win32,
  X11/Wayland) directly.
- Mocking libraries and mock objects in tests. Real dependencies
  preferred; full fakes only when unavoidable.

## Storage of design artifacts

- **Plans** (task breakdowns, `[PLAN]`, `[STORY]`, `[SPIKE]`,
  sub-epic, epic, initiative bodies, progress comments) live in
  GitHub Issues. No plan content checked into the repo.
- **Designs** (specs, ADRs, integration contracts, decision records,
  diagrams) live in this repository under `specs/` and
  `specs/decisions/`.
- Any change — design, plan, or code (once coding re-enables) — that
  invalidates an existing design must update affected files in the
  same PR. If that exceeds scope, open an `[SPIKE] iterate-*` issue
  first.

## How harmonius is used

`/Users/cjhowe/Code/harmonius/` is unreliable prior art produced by an
older model. Treated as **input** for fresh research, not authority.
Stories and design sketches mined; conclusions independently
re-derived. Glibre is not a port and does not "preserve" harmonius
decisions — it inherits only the substrate (Rust, custom ECS + jobs,
Slang shaders through `slangc`, `rkyv`, codegen middleman, no async,
no reflection).
