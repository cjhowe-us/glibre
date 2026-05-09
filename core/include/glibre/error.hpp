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
