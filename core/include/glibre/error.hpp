#pragma once
// core/include/glibre/error.hpp
//
// Engine-wide error type.  Per reviews/decisions/error-model.md:
//   - Every public boundary returns std::expected<T, glibre::Error>.
//   - glibre::Error is a tagged union (eastl::variant) over per-context
//     error enums (PHILOSOPHY §11 — EASTL is the substrate for runtime
//     data structures; std::variant is not permitted in engine code).
//   - Engine code compiles with -fno-exceptions.
//
// New contexts append their own enum to the Variant; old enums are
// never edited by other contexts.

#include <concepts>
#include <cstdint>
#include <expected>
#include <type_traits>

// EASTL substrate types — PHILOSOPHY §11 mandates eastl:: for variant
// and string_view in engine code (not std::).
#include <EASTL/string_view.h>
#include <EASTL/variant.h>

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
};
}  // namespace tools

// -----------------------------------------------------------------------
// ErrorContext — source-location attachment (optional human hint)
//
// Uses eastl::string_view per PHILOSOPHY §11 and error-model.md
// §Type-Sketch (which shows eastl::string_view in ErrorContext fields).
// -----------------------------------------------------------------------

struct ErrorContext {
    eastl::string_view file{};    // __FILE__
    int line{0};                  // __LINE__
    eastl::string_view detail{};  // optional human hint, never load-bearing
};

// -----------------------------------------------------------------------
// glibre::Error — tagged union over per-context enumerations
//
// Uses eastl::variant per PHILOSOPHY §11 and error-model.md §Type-Sketch
// (which explicitly shows "eastl::variant" in the Variant typedef) and
// SPEC §1322.  std::variant is not permitted in engine code outside
// tools/editor.
// -----------------------------------------------------------------------

class Error {
public:
    using Variant = eastl::variant<
        core::Error,
        render::Error,
        tools::Error
        // physics::Error, data::Error, shader::Error, ...
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
