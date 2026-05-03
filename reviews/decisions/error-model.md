# Decision Record — Error Model

## Status

Accepted (research spike #9, Epic #2 cross-cutting foundation).
Supersedes nothing. Inputs to follow-on plan issues that introduce
`glibre::Error` into `core` and stamp every public boundary header.

## Context

Glibre is a C++23 engine made of a thin core plus per-domain plugin
dylibs (render, physics, audio, scripting, editor UI, …). Plugins
load and unload at frame-boundary 8 (hot-reload barrier), and the
ABI is gated by a middleman dylib hash.

Several requirements collapse onto a single error-handling decision:

1. Engine code must be exception-free. Plugins are loaded across a
   C ABI; throwing across that boundary is undefined. Determinism
   forbids the non-local control-flow that exceptions imply.
2. Editor UI links ImGui, which is exception-tolerant; we cannot
   blanket-ban exceptions there without forking the dependency.
3. Hot-reload has structured refusal cases (ABI hash mismatch,
   schema migration failure, plugin init returns error). These must
   be first-class, inspectable, and serializable into telemetry.
4. Per-context SPEC §10 enumerates that context's failure modes;
   the engine-wide error must roll those up without losing the
   per-context semantics.
5. Logging is centralized through `spdlog`; errors must survive a
   round-trip through structured logs without prose-only encoding.

The C++23 successor to this problem space is `std::expected<T, E>`,
which gives a value-or-error sum type with no allocation, no RTTI,
and no unwinding.

## Decision

1. Every public boundary in the engine returns
   `std::expected<T, glibre::Error>`. "Public boundary" = any symbol
   exported from `glibre-core` or any plugin dylib, plus any header
   reachable from outside its owning context.
2. `glibre::Error` is a tagged union (`std::variant`) over per-context
   error enums. Each context owns its enum (`enum class
   render::Error`, `physics::Error`, …) and lists its variants in
   that context's SPEC §10. New contexts add a new enum and extend
   the variant; old contexts never edit each other's enums.
3. Engine code (everything outside `tools/editor/ui/`) compiles with
   `-fno-exceptions`. The editor UI module is the lone exception:
   it may use exceptions internally for ImGui interop, but it must
   convert any exception that would cross its module boundary into
   `std::expected<T, glibre::Error>` before returning.
4. Hot-reload refusal is encoded as a dedicated `core::Error` arm
   (see Type Sketch). The hot-reload phase logs the refusal and
   leaves the previous plugin instance live; no half-loaded state.
5. Errors flow through `spdlog` via a single
   `glibre::log_error(const Error&)` helper that formats the variant
   tag, the context-specific enumerator, and any attached payload
   (path, hash, schema version) as structured key/value fields.

## Type Sketch

```cpp
// core/include/glibre/error.hpp
#pragma once
#include <expected>
#include <string_view>
#include <variant>
#include <cstdint>

namespace glibre {

namespace core {
enum class Error : std::uint16_t {
    PluginAbiHashMismatch,
    PluginInitFailed,
    SchemaMigrationFailed,
    HotReloadRefused,
    FramePhaseMisordered,
    OutOfBudget,
};
}

namespace render {
enum class Error : std::uint16_t {
    DeviceLost,
    PipelineCompileFailed,
    ResourceResidencyExceeded,
    RenderGraphCycle,
    UnsupportedBackend,
};
}

// Per-context enums declared in their own headers. The engine-wide
// Error rolls them up as a tagged union; std::variant gives us the
// tag-plus-payload semantics with no allocation and no RTTI.

struct ErrorContext {
    std::string_view file;       // __FILE__
    int              line;       // __LINE__
    std::string_view detail;     // optional human hint, never load-bearing
};

class Error {
public:
    using Variant = std::variant<
        core::Error,
        render::Error
        // physics::Error, data::Error, shader::Error, ...
        // appended as each context lands
    >;

    template <class E>
    constexpr Error(E e, ErrorContext ctx = {}) noexcept
        : variant_{e}, ctx_{ctx} {}

    constexpr const Variant&       code() const noexcept { return variant_; }
    constexpr const ErrorContext&  where() const noexcept { return ctx_; }

private:
    Variant       variant_;
    ErrorContext  ctx_;
};

template <class T>
using Result = std::expected<T, Error>;

}  // namespace glibre
```

Sample boundary signature:

```cpp
// render/include/glibre/render/device.hpp
glibre::Result<DeviceHandle> create_device(const DeviceDesc&) noexcept;
```

## Composition Rules

1. Per-context enums are leaves; nothing nests another context's
   enum inside its own. Cross-context translation happens at the
   call site that crosses the boundary.
2. When context A calls into context B and wants to surface B's
   failure as A's failure, A maps the inner enumerator to one of A's
   own enumerators. The mapping is local, explicit, and unit-tested.
   No automatic up-cast.
3. `std::expected<T, glibre::Error>` propagates with the C++23
   monadic chain: `.and_then`, `.or_else`, `.transform`. Engine code
   prefers these over manual `if (!result)` ladders for boundary
   plumbing, but inner hot loops keep the explicit form to avoid
   surprising codegen.
4. Functions that cannot fail return plain `T` (or `void`) and do
   not wrap. `std::expected` is reserved for actual failure modes
   listed in SPEC §10.
5. The variant grows monotonically as new contexts ship. Removing
   an enumerator is a breaking ABI change and triggers the plugin
   ABI hash bump (Epic #2 SubE C decision).

## Logging / Telemetry

1. Every `glibre::Error` constructed inside engine code is logged
   exactly once at the boundary where it is *handled* (not at the
   boundary where it is *raised*). The handler calls
   `glibre::log_error(err, level)` which dispatches to `spdlog`.
2. `log_error` formats:
   - `error.tag` — the variant index, mapped to a stable string
     (`"render::Error"`, `"core::Error"`, …).
   - `error.code` — the enumerator name (compile-time string from
     `magic_enum` or a hand-written `to_string` per enum; decision
     deferred to the implementation plan, but stable strings are
     required).
   - `error.detail`, `error.file`, `error.line` from `ErrorContext`.
3. Hot-reload refusals are logged at `warn` level with the rejected
   dylib path and the offending hash / schema version. They never
   escalate to `error` because the previous-good plugin keeps running.
4. Telemetry sinks (post-MVP) consume the same structured fields;
   no separate telemetry path is introduced.

## Rationale

- `std::expected` is the C++23 idiom; we get value semantics, zero
  allocation, no RTTI, and clean monadic chaining without inventing
  a bespoke `Result<T, E>`.
- A tagged union over per-context enums preserves SRP: each context
  owns the meaning of its failures. The engine-wide alias gives us
  one type to thread through generic code without flattening
  context-specific semantics.
- `-fno-exceptions` on engine code is enforced by the build, not by
  convention. The editor-UI carve-out is bounded by a single module
  boundary and a single conversion point.
- Hot-reload refusal as a first-class `core::Error` arm is what
  makes the "refuse load on hash mismatch" rule (PHILOSOPHY #9)
  actually testable.

## Consequences

Positive:

- Public APIs are self-documenting about failure modes; SPEC §10
  and the enum definition stay in lockstep.
- No exception machinery on the hot path; deterministic codegen.
- Errors flow into `spdlog` through one helper — telemetry stays
  uniform across contexts.
- Plugin authors know exactly how to fail: pick an enumerator, wrap
  in `std::unexpected`, return.

Negative / accepted costs:

- Adding a new context requires editing the `glibre::Error` variant
  alias in core. This is a deliberate central-registration point;
  the alternative (type-erased error) loses the per-context
  semantics we want.
- `std::expected` chains can get verbose; we accept that and lean
  on monadic helpers + a small set of macros (`GLIBRE_TRY(expr)`)
  to keep boundary code legible. Macro shape decided in the
  implementation plan, not here.
- Exception-aware third-party libraries (Jolt, FBX SDK, Fory) must
  be wrapped at their first ingress point. Each wrapper translates
  thrown exceptions into `glibre::Error`; the wrapper module is the
  only place in its context that compiles with `-fexceptions`.

## Open Questions

1. `magic_enum` vs hand-written `to_string` per enum for the
   logging string mapping. `magic_enum` adds a dependency but
   removes a maintenance tax; decision deferred to the
   implementation plan that introduces `core/error.hpp`.
2. Macro shape for `GLIBRE_TRY` — single statement vs expression
   form. C++23 `co_await`-style chaining is not on the table
   (no coroutines in engine code). Decided in the implementation
   plan.
3. Should `ErrorContext` capture `std::source_location` instead of
   manual `__FILE__` / `__LINE__`? Likely yes; verify the
   `source_location` overhead in a release build before committing.
4. How does the editor UI's exception-to-`Error` conversion handle
   `std::bad_alloc`? Either map to a dedicated `core::Error::OutOfMemory`
   arm or terminate; resolve when the editor module lands.
5. Per-plugin error enums (third-party plugins outside the MVP
   contexts) — do they extend `glibre::Error` directly, or do they
   surface through a `plugin::Error` arm that carries an opaque
   plugin-defined code? Defer until the plugin SDK story is opened.
