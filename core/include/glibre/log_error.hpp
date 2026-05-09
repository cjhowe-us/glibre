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
//   predictable, and allows the unit test `tostring_complete_for_core_error`
//   to fail the build the moment the enum gains an enumerator without a
//   matching arm — which is a stronger guarantee than magic_enum's runtime
//   "unknown" fallback.
//
// -fno-exceptions compatible: no throws; spdlog is compiled with
// SPDLOG_NO_EXCEPTIONS (enforced by the glibre-core compile flags).

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
// Adding a new enumerator without adding a matching arm causes a compiler
// warning (unhandled enum case in switch) that is treated as an error under
// -Werror.  This is the "fails the build" guarantee referenced in plan #236.

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
// This mapping is compile-time-stable: changing the Variant typedef order
// requires updating this function (which the unit test will flag).

[[nodiscard]] inline const char* tag_string(const Error::Variant& v) noexcept {
    switch (v.index()) {
        case 0: return "core::Error";
        case 1: return "render::Error";
        case 2: return "tools::Error";
        default: return "unknown::Error";
    }
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
