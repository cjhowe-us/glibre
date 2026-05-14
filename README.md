# glibre

Rust no-code game engine. Cross-platform (macOS, Windows, Linux, iOS, Android). Custom archetype
ECS, custom Chase-Lev work-stealing job system, mesh-shader + ray-tracing render path (Metal 4 /
D3D12 / Vulkan 1.4), Slang shaders through `slangc` (single IL, native reflection), codegen
middleman `.dylib` for editor hot reload + LTO static link for shipping. Built on the harmonius
design substrate (re-derived, not ported).

See:

- [PHILOSOPHY.md](PHILOSOPHY.md) — design principles (SOLID, SRP, scope)
- [ROADMAP.md](ROADMAP.md) — short / mid / long-term path
- [GLOSSARY.md](GLOSSARY.md) — ubiquitous language
- [specs/](specs/) — bounded-context specs (15 contexts)
- [specs/decisions/](specs/decisions/) — architecture decision records

User stories and tasks live in **GitHub Issues**, not in this repo. Plans live in issues; designs
live in this repo and must be updated when any change invalidates them.

Status: pre-implementation, post-pivot. C++ surface deleted 2026-05-13; Rust workspace empty until
coding lockout lifts in `.claude/skills/go/SKILL.md`. `/go` currently drives design / planning /
review only.

Licensed under the Apache License, Version 2.0 — see [LICENSE](LICENSE).
