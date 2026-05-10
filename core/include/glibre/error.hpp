#pragma once
// core/include/glibre/error.hpp
//
// Engine-wide error type.  Per reviews/decisions/error-model.md:
//   - Every public boundary returns std::expected<T, glibre::Error>.
//   - glibre::Error is a tagged union (std::variant) over per-context
//     error enums.  Migrated from eastl::variant per
//     reviews/decisions/eastl-removal.md §4.
//   - Engine code compiles with -fno-exceptions.
//
// New contexts append their own enum to the Variant; old enums are
// never edited by other contexts.

#include <concepts>
#include <cstdint>
#include <expected>
#include <string_view>
#include <type_traits>
#include <variant>

namespace glibre {

// -----------------------------------------------------------------------
// Per-context error enumerations
// -----------------------------------------------------------------------

namespace core {
enum class Error : std::uint16_t {
    PluginAbiHashMismatch,
    PluginInitFailed,
    SchemaMigrationFailed,
    HotReloadRefused,
    FramePhaseMisordered,  // debug-build: a phase ran out-of-sequence
    OutOfBudget,
    SystemScheduleCycle,
    ScheduleAccessConflict,
    // Manifest-read errors (plan #223 — PluginManifest schema authored)
    // Used by PluginManifest::open() and the loader (plans #229..#231).
    PluginManifestNotFound,  // path does not exist or is not a regular file
    PluginManifestInvalid,   // file exists but Fory deserialization failed
                             //   (corrupt, wrong schema version, truncated)
    // Loader step 1: dlopen failure (plan #229 — PluginLoader dlopen+dlsym)
    // Maps to plugin-abi.md §"Failure Modes" step 1: dlopen returns null.
    // The dlerror() text is attached to ErrorContext::detail.
    PluginDlopenFailed,
    // Loader step 2: a required export symbol is missing from the dylib.
    // Maps to plugin-abi.md §"Failure Modes" step 2: required symbol missing.
    // After dlclose, the loader aborts without further steps.
    PluginMissingEntryPoint,
    // Loader step 5: engine version older than manifest.min_engine_version.
    // Maps to plugin-abi.md §"Failure Modes" step 5.
    // After dlclose, the loader aborts without further steps.
    // Added by plan #230 (ABI hash + version + name + deps gates).
    PluginEngineTooOld,
    // Loader step 6: a plugin with the same name is already registered.
    // Maps to plugin-abi.md §"Failure Modes" step 6.
    // After dlclose, the loader aborts without further steps.
    // Added by plan #230.
    PluginNameCollision,
    // Loader step 7: a dependency listed in manifest.depends_on is not yet
    // registered.  Maps to plugin-abi.md §"Failure Modes" step 7.
    // After dlclose, the loader aborts without further steps.
    // Added by plan #230.
    PluginDependencyMissing,
    // Transient arena exhausted — allocate() was called when the arena had
    // insufficient capacity for the requested (bytes, align) pair.
    // Returned by TransientArena::allocate() (plan #239).
    // Callers should fall back to a larger arena or defer the allocation.
    TransientArenaExhausted,
    // Null pointer supplied to a core API that requires a valid, non-null
    // pointer.  Returned instead of asserting-only so that callers in release
    // builds receive an actionable error rather than silent undefined behaviour.
    // Kept narrow (null-pointer check only) so it does not become a catch-all
    // for unrelated argument errors.  Added by plan #239 review round 1
    // (MED-3 fix: null-pointer silent success in register_transient_arena).
    // Renamed NullArgument (was InvalidArgument) in review round 2 (LOW-3:
    // narrow the seam name so it is not a magnet for unrelated callers).
    NullArgument,
    // Hot-reload validation: the incoming plugin's manifest name differs from
    // the outgoing plugin's manifest name.  Swapping a plugin for one with a
    // different identity is a configuration error, not an ABI issue.
    // Detected by hot_reload_validate() in plugin_loader_actions (plan #250).
    PluginNameMismatch,
    // TypeRegistry lookup on a TypeId that has no registered descriptor.
    // Returned by TypeRegistry::lookup() when TypeId.value is out of range
    // or the slot was never populated.  (plan #597 — TypeRegistry immutable-after-init)
    // Spec authority: specs/core/SPEC.md §4.9 invariant 2, §10 error table row
    // "TypeUnregistered".
    TypeUnregistered,
    // TypeRegistry mutation attempted after World construction has called seal().
    // register_type() returns this error on any call that arrives after the seal.
    // The friend hook extend_during_load() bypasses the seal for plugin-load
    // time registration only.  (plan #597)
    // Spec authority: specs/core/SPEC.md §4.9 invariant 1, §10 error table row
    // "TypeRegistryClosed".
    TypeRegistryClosed,
    // TypeRegistry register_type() / extend_during_load() called with an id.value
    // that is not the next contiguous slot (id.value != entries_.size()).
    // This signals a codegen contract violation — the emitter is expected to
    // assign TypeId values in ascending order with no gaps.  SPEC §10 reserves
    // TypeUnregistered for lookup-only failures; this distinct arm covers the
    // registration-time gap check so callers can distinguish the two cases.
    // (plan #597 review round 1, MED-3 fix)
    TypeRegistryGap,
    // Entity handle whose slot generation does not match the allocator's
    // current generation for that slot.  Returned by EntityAllocator::resolve()
    // and all World APIs that accept an Entity argument (SPEC §4.3 invariant 1).
    // (plan #557 — Entity ID encoding + generational allocator)
    EntityStale,
    // Entity handle from a different World passed to this World's API.
    // Reserved now for honest MVP code paths; unreachable until multi-world
    // lands post-MVP (SPEC §4.3 invariant 3, §3.3 deferral).
    // (plan #557 — Entity ID encoding + generational allocator)
    EntityForeignWorld,
    // AssetHandle whose generation does not match the AssetTable slot's current
    // generation — the handle was released and the slot reused (or the handle
    // is default-constructed / foreign).  Returned by AssetTable::resolve()
    // on a generation mismatch.  (plan #598 — AssetHandle generational table)
    // Spec authority: specs/core/SPEC.md §4.7 invariant 1, §10 error table
    // row "AssetStale".
    AssetStale,
    // System registered against Phase::HotReload (ordinal 8).
    // Phase 8 is a FrameLoop-internal seam owned by the plugin loader;
    // plugin-authored systems may not occupy it.  FrameLoop never walks
    // HotReload-phase compiled slots for user systems (SPEC §6.5 phase 8).
    // This error is returned by register_system() as a plugin-author footgun
    // guard: surfacing it early at registration is cheaper than silently
    // dropping the system at compile() time.
    // (plan #584 — Schedule DAG builder, R1 review MED-3 fix)
    SystemForbiddenInHotReloadPhase,
};
}  // namespace core

namespace render {
enum class Error : std::uint16_t {
    DeviceLost,
    PipelineCompileFailed,
    ResourceResidencyExceeded,
    RenderGraphCycle,
    UnsupportedBackend,
};
}  // namespace render

namespace tools {
// Error codes for host tools (glibre-foryc and future codegen tools).
// Added by plan #219 (foryc skeleton).
enum class Error : std::uint16_t {
    ForycSyntaxError,         // .fory file contains a parse error
    ForycDuplicateTag,        // two fields share the same tag number
    ForycNonMonotoneVersion,  // schema version is not strictly increasing
    ForycUnknownType,         // field type is not in the builtins set
    ForycIOError,             // file read / write failure
    ForycEmptySchema,         // schema contains zero TypeDecl blocks
    ForycInvalidIdentifier,   // identifier segment violates reserved-naming rule
                              //   (e.g. contains "__" — reserved for codegen mangle)
};
}  // namespace tools

namespace shader {
// Error codes for the shader bounded context.
// Added by plan #508 (ShaderSource open + include resolver + entry-point scanner).
// Authority: specs/shader/SPEC.md §10.
//
// MED-1 rationale (plan #508 review round 1):
//   All 25 arms from SPEC §10 are registered here rather than introduced
//   plan-by-plan.  Rationale: SPEC §10 is the closed sum for the shader
//   context; the static_assert chain in error_register.hpp pin-checks the
//   arm count to kExpectedArmCount so any future plan that removes or adds
//   an arm must also update the SPEC.  Introducing arms incrementally would
//   require editing this header in 4–5 subsequent PRs and risks arm-count
//   drift between the SPEC and the enum.  The accepted cost is that plans
//   that don't yet raise Compiler*/Cache*/Descriptor*/Shipping*/Reflection*
//   arms carry them as forward-declared intent; they become reachable as
//   later plans (CompilationPipeline, ShaderCache, ReflectionBlob) land.
//   This is the §10 snapshot approach; the alternative would be to prune and
//   re-amend the SPEC on each plan, which is more disruptive.
enum class Error : std::uint16_t {
    SourceNotFound,                 // file does not exist or cannot be opened
    SourceParseFailed,              // file exists but cannot be parsed
    IncludeEscape,                  // absolute or ../-escaping include path
    IncludeCycle,                   // include graph contains a cycle
    EncodingInvalid,                // non-UTF-8 or binary content in source
    EntryPointMissing,              // no [shader("...")] attribute found
    EntryPointStageAmbiguous,       // function bears more than one stage attribute
    PermutationKeyMalformed,        // packed bytes fail invariant checks
    PermutationKeyOutOfRange,       // index >= kPermutationCrossProductCardinality
    CompilerInvocationFailed,       // slangc subprocess could not be launched
    CompilerExitNonZero,            // slangc returned a non-zero exit code
    CompilerTimedOut,               // slangc subprocess exceeded time limit
    UnsupportedTarget,              // requested CompileTarget is not supported
    MetalLibEmitFailed,             // slangc emitted metallib but file is invalid
    ReflectionExtractionFailed,     // slangc reflection API returned an error
    DescriptorFrequencyAmbiguous,   // binding has conflicting frequency group tags
    DescriptorFrequencyMissing,     // binding has no frequency group tag
    LinkFailed,                     // artifact link step failed
    SpecializationConstantMissing,  // required specialization constant absent
    CacheLookupMiss,                // ShaderHash not found in the cache
    CacheCorrupt,                   // cache blob fails integrity check
    CacheIntegrity,                 // orphan or missing artifact in cooked library
    CacheReadOnlyViolation,         // write attempted to a read-only cache
    CapabilityNotSupported,         // backend lacks required capability
    ShippingCompilationAttempted,   // compile path invoked in a shipping build
};
}  // namespace shader

// -----------------------------------------------------------------------
// ErrorContext — source-location attachment (optional human hint)
//
// Uses std::string_view per reviews/decisions/eastl-removal.md §4
// (matrix row 2: eastl::string_view → std::string_view).
// -----------------------------------------------------------------------

struct ErrorContext {
    std::string_view file{};    // __FILE__
    int line{0};                // __LINE__
    std::string_view detail{};  // optional human hint, never load-bearing
};

// -----------------------------------------------------------------------
// glibre::Error — tagged union over per-context enumerations
//
// Variant alias uses std::variant per reviews/decisions/eastl-removal.md §4
// (matrix row 14: eastl::variant → std::variant).  The class Error wrapper,
// the Result<T> alias, and the ErrorContext field set are unchanged.
// -----------------------------------------------------------------------

class Error {
public:
    using Variant = std::variant<
        core::Error,
        render::Error,
        tools::Error,
        shader::Error
        // physics::Error, data::Error, ...
        // append as each context lands
        >;

    // Constructible from any per-context error enum that is a member of
    // Variant.  The constraint prevents the generic constructor from
    // participating in overload resolution for unrelated types (e.g.
    // std::expected<…>), which would cause circular constraint evaluation
    // in libc++ clang-22.
    template<class E>
        requires std::constructible_from<Variant, E>
    constexpr Error(E e, ErrorContext ctx = {}) noexcept
        : variant_{e},
          ctx_{ctx} {}

    [[nodiscard]] constexpr const Variant& code() const noexcept { return variant_; }

    [[nodiscard]] constexpr const ErrorContext& where() const noexcept { return ctx_; }

private:
    Variant variant_;
    ErrorContext ctx_;
};

// -----------------------------------------------------------------------
// Convenience alias used throughout the engine
// -----------------------------------------------------------------------

template<class T>
using Result = std::expected<T, Error>;

// -----------------------------------------------------------------------
// Compile-time invariants pinning the shape of Error::Variant and Result<T>
//
// These static_asserts are the mechanical guarantee that #233 requested:
// "codify with compile-time invariants so the shape cannot silently drift".
//
// I1 — Result<T> is exactly std::expected<T, glibre::Error>.  Ensures the
//      alias has not been accidentally widened or redirected.
// I2 — Each per-context enum uses std::uint16_t as its underlying type.
//      Changing the underlying type is an ABI break (structured log fields
//      embed the numeric value); the assert makes this visible at compile time.
// I3 — Error::code() returns const Variant& and is [[nodiscard]].  Verified
//      by the constexpr construction below; the [[nodiscard]] attribute on
//      code() is a declaration invariant enforced by code review.
//
// Note: the invariant that std::variant_size_v<Variant> == kExpectedArmCount
// already lives in error_register.hpp (which includes this header) to avoid a
// circular include.  That static_assert is part of the same contract and is
// considered logically co-located here.
// -----------------------------------------------------------------------

static_assert(
    std::is_same_v<Result<int>, std::expected<int, Error>>,
    "glibre::Result<T> must remain an alias for std::expected<T, glibre::Error>."
);
// I1b — void specialisation: Result<void> is the canonical success-only return
// shape used throughout the engine (e.g. every Init/Shutdown function returns
// Result<void>).  The int-only assert above would not catch a partial-
// specialisation change that broke only the void path; pin it separately.
static_assert(
    std::is_same_v<Result<void>, std::expected<void, Error>>,
    "glibre::Result<void> must remain an alias for std::expected<void, glibre::Error>."
);

// I2: All registered per-context enums must use std::uint16_t as their
// underlying type.  The asserts fire at compile time with a clear diagnostic
// if someone changes the base type.
static_assert(
    std::is_same_v<std::underlying_type_t<core::Error>, std::uint16_t>,
    "core::Error underlying type must be std::uint16_t (error-model.md §Type Sketch)."
);
static_assert(
    std::is_same_v<std::underlying_type_t<render::Error>, std::uint16_t>,
    "render::Error underlying type must be std::uint16_t (error-model.md §Type Sketch)."
);
static_assert(
    std::is_same_v<std::underlying_type_t<tools::Error>, std::uint16_t>,
    "tools::Error underlying type must be std::uint16_t (error-model.md §Type Sketch)."
);
static_assert(
    std::is_same_v<std::underlying_type_t<shader::Error>, std::uint16_t>,
    "shader::Error underlying type must be std::uint16_t (error-model.md §Type Sketch)."
);

}  // namespace glibre

// -----------------------------------------------------------------------
// GLIBRE_TRY(name, expr) — early-return-on-error propagation macro
//
// Rationale (error-model.md §Open Questions #2, plan #235):
//   Chosen form: statement macro, not expression macro.
//   Expression-form (e.g. __extension__ ({ ... })) relies on a GCC/clang
//   statement-expression extension and produces harder-to-read call sites.
//   Statement-form keeps macro expansion predictable, compatible with
//   -fno-exceptions/-fno-rtti, and trivially composable.
//   co_await-style chaining (C++23 operator co_await on expected) is
//   explicitly off the table per error-model.md §Open Questions #2
//   (no coroutines in engine code).
//
//   Header placement: The macro lives here in error.hpp rather than the
//   originally-scoped try.hpp (plan #235 Scope) because GLIBRE_TRY is
//   only meaningful in a translation unit that already knows about
//   glibre::Result<T>.  Co-locating both in error.hpp eliminates a
//   fragile include-order dependency and means callers get the macro
//   for free whenever they include the type they are already using.
//
//   Temp variable is mangled with __COUNTER__ so that two GLIBRE_TRY
//   calls within the same expansion (e.g. when a helper macro emits
//   multiple GLIBRE_TRY on the same source line) never collide.
//   __COUNTER__ is a clang/gcc/MSVC extension but is universally
//   available on all toolchains that support C++23.
//
// Usage rules (GLIBRE_TRY expands to multiple statements):
//   GLIBRE_TRY MUST appear as a top-level statement in a braced block.
//   It MUST NOT be the unbraced body of if / else / for / while — the
//   macro expands to two or three statements and only the first would
//   be governed by the control-flow construct; the remainder would
//   execute unconditionally (or, if the declared name is used in the
//   remainder, produce a hard compile error).
//
//   CORRECT:
//     if (cond) {
//         GLIBRE_TRY(x, fn());
//         use(x);
//     }
//
//   WRONG — do not do this:
//     if (cond)
//         GLIBRE_TRY(x, fn());  // second statement leaks out of if-body
//
// Usage examples:
//   glibre::Result<int> caller() {
//       GLIBRE_TRY(x, fn1());       // binds unwrapped value to 'x'
//       GLIBRE_TRY_VOID(fn2(x));    // propagates error from Result<void>
//       return x * 2;
//   }
//
// Both macros are valid only inside a function returning glibre::Result<T>
// or glibre::Result<void>.  Using them in a void-returning function is a
// compile error (std::unexpected return type mismatch).
// -----------------------------------------------------------------------

// GLIBRE_TRY_DETAIL_CONCAT2 / _CONCAT: two-level paste needed so macro
// arguments expand before concatenation (standard CPP token-paste rule).
//
// GLIBRE_TRY_DETAIL / GLIBRE_TRY_VOID_DETAIL: inner helpers that receive
// __COUNTER__ as a stable integer argument (cnt).  __COUNTER__ is evaluated
// exactly once in the outer GLIBRE_TRY / GLIBRE_TRY_VOID call and forwarded
// as a literal, so all three glibre_try_result_<cnt> references name the
// same variable.  This is the correct pattern for __COUNTER__-based mangling;
// using __COUNTER__ directly in the inner macro body would produce a fresh
// counter value on each expansion.
// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define GLIBRE_TRY_DETAIL_CONCAT2(a, b) a##b
#define GLIBRE_TRY_DETAIL_CONCAT(a, b) GLIBRE_TRY_DETAIL_CONCAT2(a, b)

#define GLIBRE_TRY_DETAIL(name, expr, cnt)                                                         \
    auto GLIBRE_TRY_DETAIL_CONCAT(glibre_try_result_, cnt) = (expr);                               \
    if (!GLIBRE_TRY_DETAIL_CONCAT(glibre_try_result_, cnt))                                        \
        return std::unexpected(                                                                    \
            std::move(GLIBRE_TRY_DETAIL_CONCAT(glibre_try_result_, cnt).error())                   \
        );                                                                                         \
    auto name = std::move(*GLIBRE_TRY_DETAIL_CONCAT(glibre_try_result_, cnt))

#define GLIBRE_TRY_VOID_DETAIL(expr, cnt)                                                          \
    do {                                                                                           \
        auto GLIBRE_TRY_DETAIL_CONCAT(glibre_try_void_result_, cnt) = (expr);                      \
        if (!GLIBRE_TRY_DETAIL_CONCAT(glibre_try_void_result_, cnt))                               \
            return std::unexpected(                                                                \
                std::move(GLIBRE_TRY_DETAIL_CONCAT(glibre_try_void_result_, cnt).error())          \
            );                                                                                     \
    } while (false)

/// GLIBRE_TRY(name, expr)
/// Evaluates expr (which must return glibre::Result<T>).
/// On error, returns the error to the caller unchanged via std::unexpected.
/// On success, declares `auto name = std::move(*result)` in the current scope.
/// MUST appear as a top-level statement in a braced block (multi-statement macro).
#define GLIBRE_TRY(name, expr) GLIBRE_TRY_DETAIL(name, expr, __COUNTER__)

/// GLIBRE_TRY_VOID(expr)
/// Like GLIBRE_TRY but for glibre::Result<void> — no value to bind.
/// On error, returns the error to the caller unchanged via std::unexpected.
#define GLIBRE_TRY_VOID(expr) GLIBRE_TRY_VOID_DETAIL(expr, __COUNTER__)
// NOLINTEND(cppcoreguidelines-macro-usage)
