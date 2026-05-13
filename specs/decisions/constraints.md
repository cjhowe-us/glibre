# ADR-0001: Project Constraints (Rust on harmonius substrate)

- **Status**: accepted
- **Date**: 2026-05-13
- **Supersedes**: every prior C++ ADR (eastl-removal, error-model,
  plugin-abi, frame-phases, hot-reload-protocol, perf-budget,
  fory-codegen, flatbuffers-vs-fory, physics-error-arm-joint-body,
  resourceresidency-srp, slot-table-overflow) — all deleted in the
  C++→Rust pivot.

## Context

Project pivots from C++23/26 + CMake + vcpkg + metal-cpp + Slang +
Flatbuffers to Rust stable on the harmonius design substrate. ADR
captures rolled-up constraints; per-domain ADRs branch from here.

## Language and toolchain

- Rust stable (1.80+). No nightly.
- No C, C++, Objective-C, Objective-C++, Swift source in the engine
  or runtime.
- Cargo workspace, `resolver = "3"`.
- LLVM-backed shipping; bundled `rustc` + `cargo` drive editor hot
  reload of the codegen middleman `.dylib`.

## Graphics and shading

- Metal 4 (macOS/iOS), Direct3D 12 (Windows), Vulkan 1.4 (Linux,
  Android). Mesh shaders + ray tracing required; no legacy pipeline.
- **Slang** is the sole shader IL. `slangc` (subprocess) emits
  metallib (Apple), DXIL (Windows), and SPIR-V (Linux / Android).
  Native reflection from `slangc` drives binding layout and
  permutation enumeration.
- No HLSL, no `dxc`, no `metal-shaderconverter`. One IL, one
  compiler, one reflection source.
- `metal-cpp` is **forbidden**; use `objc2` family for Apple
  platforms.

## Runtime architecture

- Custom archetype ECS with AoSoA tiled storage; SIMD-aware query
  plans.
- Custom job system on `crossbeam-deque` (Chase-Lev work stealing);
  no `tokio`, `mio`, `rayon`, `compio` in engine or game runtime.
- Three thread roles: main (OS + I/O), workers (game loop + ECS),
  render (GPU).
- Deterministic fixed timestep (30 / 60 / 120 fps tiers).
- One shared spatial BVH; physics owns a private BVH.

## Concurrency and async

- `async`/`await`, `Future`, async runtimes are **forbidden** in
  engine, editor, and game runtime. Allowed only in backend services.
- No singletons; dependency injection. No `dyn` dispatch unless
  justified (cold-init `dyn Plugin`, command buffer callbacks,
  editor-only).

## Reflection and codegen

- Zero runtime reflection: no `dyn Reflect`, no `TypeRegistry`, no
  `TypeId`-based dispatch.
- All visual graphs (logic, materials, AI, animation, VFX) codegen
  Rust source into the middleman `.dylib`.
- Middleman is hot-reloaded in editor; statically linked under LTO
  for shipping.

## Serialization

- `rkyv` only. Zero-copy mmap for baked assets and save files.
- No `serde`. No other binary serialization libraries.
- Custom text scene format for diff/merge.

## Platform I/O

- Linux: `io_uring` via `rustix`.
- Windows: IOCP + DirectStorage via `windows-rs`.
- Apple: GCD `dispatch_io` + Metal I/O via `dispatch2` + `objc2`.
- Main thread polls completions; zero blocking calls.
- No `winit`; custom windowing (NSWindow, Win32, X11/Wayland)
  directly.

## Networking

- QUIC unified transport (`quinn-proto` on Linux,
  `Networking.framework` on Apple, MsQuic on Windows).
- No TCP, no custom UDP, no DTLS, no HTTPS, no WebSocket in engine.

## Data structures

- No `HashMap` on deterministic hot paths. Use sorted `Vec`,
  `BTreeMap`, or index-based lookup.

## Testing

- TDD. No mocking libraries, no mock objects. Real dependencies
  preferred; full fakes only when unavoidable.
- Tests trace back to requirement / feature / user-story IDs in
  GitHub.

## Storage of artifacts

- Plans (task breakdowns, `[PLAN]`, `[STORY]`, `[SPIKE]`, sub-epic,
  epic, initiative bodies) live in GitHub Issues only.
- Designs (specs, ADRs, integration contracts, decision records)
  live in this repository under `specs/` and `specs/decisions/`.
- Any change that invalidates existing design must update affected
  design files in the same PR.

## Out of scope (until coding re-enables)

- All Rust crates. `Cargo.toml` workspace stays empty
  (`members = []`).
- All code PRs. `/go` skill is locked to design / planning / review.
