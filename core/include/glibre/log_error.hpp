#pragma once
// core/include/glibre/log_error.hpp
//
// Structured error logging helper — plan #236.
//
// Authority: reviews/decisions/error-model.md §"Logging / Telemetry" rules 1–3.
//
// API:
//   glibre::log_error(sink, err)         — log to a specific spdlog::logger.
//   glibre::log_error(sink, err, level)  — log at caller-specified level.
//   glibre::log_error_to_default(err)    — log to spdlog::default_logger().
//
// Output format (one structured log record per error):
//   key=value pairs in the message body:
//     tag=<context>::Error  variant=<enumerator-name>
//     file=<ErrorContext::file>  line=<ErrorContext::line>
//     detail=<ErrorContext::detail>
//
// to_string design choice (plan #236 §Scope):
//   Hand-written to_string() per per-context enum is used instead of
//   magic_enum.  Rationale: avoids the dependency, keeps compile times
//   predictable, and triggers a -Wswitch compiler warning when an enum gains
//   an enumerator without a matching arm.  glibre-core compiles with
//   -Werror=switch so that warning is promoted to a build error, making the
//   guarantee compile-time.  The runtime completeness tests
//   (tostring_complete_for_{core,render,tools}_error) verify string values.
//
// -fno-exceptions compatible: log_error() is declared noexcept.  Any exception
// that attempts to escape spdlog terminates the process via std::terminate —
// the standard noexcept-on-throw behaviour.  See log_error.cpp for details.

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include "glibre/error.hpp"

namespace glibre {

// ---------------------------------------------------------------------------
// to_string overloads — hand-written per-enum (see §"to_string design choice")
// ---------------------------------------------------------------------------
//
// Each overload returns a string literal.  The caller must never store this
// pointer past the lifetime of the program; the literals have static storage
// duration and are always valid.
//
// Namespace placement: all overloads live in namespace glibre — NOT in the
// per-context namespaces (glibre::core, glibre::render, …).  ADL on a value
// of type render::Error will NOT find glibre::to_string; call sites must
// qualify: `glibre::to_string(e)`.  The implementation (variant_code_string)
// already qualifies correctly; external callers must do the same.
//
// Adding a new enumerator without adding a matching arm fires -Wswitch, which
// glibre-core promotes to a build error via -Werror=switch (core/CMakeLists.txt).
// This is the compile-time coverage guarantee referenced in plan #236.

[[nodiscard]] constexpr const char* to_string(core::Error e) noexcept {
    switch (e) {
        case core::Error::PluginAbiHashMismatch:   return "PluginAbiHashMismatch";
        case core::Error::PluginInitFailed:        return "PluginInitFailed";
        case core::Error::SchemaMigrationFailed:   return "SchemaMigrationFailed";
        case core::Error::HotReloadRefused:        return "HotReloadRefused";
        case core::Error::FramePhaseMisordered:    return "FramePhaseMisordered";
        case core::Error::OutOfBudget:             return "OutOfBudget";
        case core::Error::SystemScheduleCycle:     return "SystemScheduleCycle";
        case core::Error::ScheduleAccessConflict:  return "ScheduleAccessConflict";
        case core::Error::PluginManifestNotFound:  return "PluginManifestNotFound";
        case core::Error::PluginManifestInvalid:   return "PluginManifestInvalid";
        case core::Error::PluginDlopenFailed:      return "PluginDlopenFailed";
        case core::Error::PluginMissingEntryPoint: return "PluginMissingEntryPoint";
        case core::Error::PluginEngineTooOld:      return "PluginEngineTooOld";
        case core::Error::PluginNameCollision:     return "PluginNameCollision";
        case core::Error::PluginDependencyMissing: return "PluginDependencyMissing";
    }
    return "Unknown";
}

[[nodiscard]] constexpr const char* to_string(render::Error e) noexcept {
    switch (e) {
        case render::Error::DeviceLost:                  return "DeviceLost";
        case render::Error::PipelineCompileFailed:       return "PipelineCompileFailed";
        case render::Error::ResourceResidencyExceeded:   return "ResourceResidencyExceeded";
        case render::Error::RenderGraphCycle:            return "RenderGraphCycle";
        case render::Error::UnsupportedBackend:          return "UnsupportedBackend";
    }
    return "Unknown";
}

[[nodiscard]] constexpr const char* to_string(tools::Error e) noexcept {
    switch (e) {
        case tools::Error::ForycSyntaxError:        return "ForycSyntaxError";
        case tools::Error::ForycDuplicateTag:       return "ForycDuplicateTag";
        case tools::Error::ForycNonMonotoneVersion: return "ForycNonMonotoneVersion";
        case tools::Error::ForycUnknownType:        return "ForycUnknownType";
        case tools::Error::ForycIOError:            return "ForycIOError";
        case tools::Error::ForycEmptySchema:        return "ForycEmptySchema";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// tag_string — maps a glibre::Error::Variant index to the context name string
// ---------------------------------------------------------------------------
//
// Returns the stable context name tag for a given Variant alternative index.
// The index matches the order of alternating types in Error::Variant:
//   0 → core::Error   → "core::Error"
//   1 → render::Error → "render::Error"
//   2 → tools::Error  → "tools::Error"
//
// Compile-time guard: the static_assert below fires if Error::Variant grows
// beyond the currently-known 3 alternatives without this function being
// updated.  Adding a 4th alternative to Error::Variant will fail the build
// with an actionable message ("extend tag_string when Error::Variant grows").
//
// Once tag_string is extended for the new alternative, increment the
// static_assert count to match.

static_assert(
    eastl::variant_size_v<Error::Variant> == 3,
    "extend tag_string() when Error::Variant grows (add a new case and bump "
    "the static_assert count in log_error.hpp)"
);

[[nodiscard]] inline const char* tag_string(const Error::Variant& v) noexcept {
    switch (v.index()) {
        case 0: return "core::Error";
        case 1: return "render::Error";
        case 2: return "tools::Error";
    }
    // v.index() == eastl::variant_npos only when the variant holds
    // valueless_by_exception state, which cannot occur in -fno-exceptions
    // builds.  __builtin_unreachable() eliminates any dead-code warning and
    // asserts this path is logically impossible.
    __builtin_unreachable();
}

// ---------------------------------------------------------------------------
// variant_code_string — extract the enumerator name from the active alternative
// ---------------------------------------------------------------------------

[[nodiscard]] inline const char* variant_code_string(const Error::Variant& v) noexcept {
    // eastl::visit is the correct way to dispatch on an eastl::variant.
    // We use a lambda that calls the overloaded to_string for each alternative.
    return eastl::visit(
        [](auto&& e) -> const char* { return glibre::to_string(e); },
        v
    );
}

// ---------------------------------------------------------------------------
// log_error — structured log a glibre::Error to a specific logger
// ---------------------------------------------------------------------------
//
// Logs at the specified level (default: spdlog::level::err).
// Output format:
//   tag=<tag_string>  variant=<enumerator>  file=<file>  line=<line>  detail=<detail>
//
// Per error-model.md §"Logging / Telemetry" rule 3:
//   Hot-reload refusals (core::Error::HotReloadRefused) must be logged at
//   warn level.  The caller is responsible for choosing the correct level;
//   log_error() logs at whatever level it is given.
//
// Per error-model.md §"Logging / Telemetry" rule 1:
//   Errors are logged exactly once at the boundary where they are *handled*
//   (not at the raise site).  Callers must not double-log.

void log_error(
    spdlog::logger& sink,
    const glibre::Error& err,
    spdlog::level::level_enum level = spdlog::level::err
) noexcept;

// ---------------------------------------------------------------------------
// log_error_to_default — log to spdlog::default_logger()
// ---------------------------------------------------------------------------
//
// Convenience overload for call sites that do not manage a named logger.
// Same structured output as log_error(sink, err, level).

void log_error_to_default(
    const glibre::Error& err,
    spdlog::level::level_enum level = spdlog::level::err
) noexcept;

}  // namespace glibre
