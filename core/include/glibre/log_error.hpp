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

#include <utility>

#include <spdlog/common.h>
#include <spdlog/logger.h>

#include "glibre/error.hpp"

// ---------------------------------------------------------------------------
// to_string overloads — hand-written per-enum (see §"to_string design choice")
// ---------------------------------------------------------------------------
//
// Each overload lives in its per-context namespace so that ADL finds the
// correct overload when the variant alternative is unwrapped by std::visit.
// External callers can also qualify explicitly: glibre::core::to_string(e).
//
// Adding a new enumerator without adding a matching arm fires -Wswitch, which
// glibre-core promotes to a build error via -Werror=switch (core/CMakeLists.txt).
// This is the compile-time coverage guarantee referenced in plan #236.

namespace glibre::core {

[[nodiscard]] constexpr const char* to_string(Error e) noexcept {
    switch (e) {
    case Error::PluginAbiHashMismatch:
        return "PluginAbiHashMismatch";
    case Error::PluginInitFailed:
        return "PluginInitFailed";
    case Error::SchemaMigrationFailed:
        return "SchemaMigrationFailed";
    case Error::HotReloadRefused:
        return "HotReloadRefused";
    case Error::FramePhaseMisordered:
        return "FramePhaseMisordered";
    case Error::OutOfBudget:
        return "OutOfBudget";
    case Error::SystemScheduleCycle:
        return "SystemScheduleCycle";
    case Error::ScheduleAccessConflict:
        return "ScheduleAccessConflict";
    case Error::PluginManifestNotFound:
        return "PluginManifestNotFound";
    case Error::PluginManifestInvalid:
        return "PluginManifestInvalid";
    case Error::PluginDlopenFailed:
        return "PluginDlopenFailed";
    case Error::PluginMissingEntryPoint:
        return "PluginMissingEntryPoint";
    case Error::PluginEngineTooOld:
        return "PluginEngineTooOld";
    case Error::PluginNameCollision:
        return "PluginNameCollision";
    case Error::PluginDependencyMissing:
        return "PluginDependencyMissing";
    case Error::TransientArenaExhausted:
        return "TransientArenaExhausted";
    case Error::NullArgument:
        return "NullArgument";
    case Error::PluginNameMismatch:
        return "PluginNameMismatch";
    // plan #597: TypeRegistry error arms.
    case Error::TypeUnregistered:
        return "TypeUnregistered";
    case Error::TypeRegistryClosed:
        return "TypeRegistryClosed";
    case Error::TypeRegistryGap:
        return "TypeRegistryGap";
    // plan #557: Entity generational handle error arms.
    case Error::EntityStale:
        return "EntityStale";
    case Error::EntityForeignWorld:
        return "EntityForeignWorld";
    // plan #598: AssetStale — stale generational asset handle.
    case Error::AssetStale:
        return "AssetStale";
    // plan #584 R1 MED-3: Phase::HotReload footgun guard.
    case Error::SystemForbiddenInHotReloadPhase:
        return "SystemForbiddenInHotReloadPhase";
    // plan #584 R2 MED-1: (phase, fqn) idempotency conflict.
    case Error::SystemDescriptorConflict:
        return "SystemDescriptorConflict";
    }
    return "Unknown";
}

}  // namespace glibre::core

namespace glibre::render {

[[nodiscard]] constexpr const char* to_string(Error e) noexcept {
    switch (e) {
    case Error::DeviceLost:
        return "DeviceLost";
    case Error::PipelineCompileFailed:
        return "PipelineCompileFailed";
    case Error::ResourceResidencyExceeded:
        return "ResourceResidencyExceeded";
    case Error::RenderGraphCycle:
        return "RenderGraphCycle";
    case Error::UnsupportedBackend:
        return "UnsupportedBackend";
    }
    return "Unknown";
}

}  // namespace glibre::render

namespace glibre::tools {

[[nodiscard]] constexpr const char* to_string(Error e) noexcept {
    switch (e) {
    case Error::ForycSyntaxError:
        return "ForycSyntaxError";
    case Error::ForycDuplicateTag:
        return "ForycDuplicateTag";
    case Error::ForycNonMonotoneVersion:
        return "ForycNonMonotoneVersion";
    case Error::ForycUnknownType:
        return "ForycUnknownType";
    case Error::ForycIOError:
        return "ForycIOError";
    case Error::ForycEmptySchema:
        return "ForycEmptySchema";
    case Error::ForycInvalidIdentifier:
        return "ForycInvalidIdentifier";
    }
    return "Unknown";
}

}  // namespace glibre::tools

namespace glibre::shader {

[[nodiscard]] constexpr const char* to_string(Error e) noexcept {
    switch (e) {
    case Error::SourceNotFound:
        return "SourceNotFound";
    case Error::SourceParseFailed:
        return "SourceParseFailed";
    case Error::IncludeEscape:
        return "IncludeEscape";
    case Error::IncludeCycle:
        return "IncludeCycle";
    case Error::EncodingInvalid:
        return "EncodingInvalid";
    case Error::EntryPointMissing:
        return "EntryPointMissing";
    case Error::EntryPointStageAmbiguous:
        return "EntryPointStageAmbiguous";
    case Error::PermutationKeyMalformed:
        return "PermutationKeyMalformed";
    case Error::PermutationKeyOutOfRange:
        return "PermutationKeyOutOfRange";
    case Error::CompilerInvocationFailed:
        return "CompilerInvocationFailed";
    case Error::CompilerExitNonZero:
        return "CompilerExitNonZero";
    case Error::CompilerTimedOut:
        return "CompilerTimedOut";
    case Error::UnsupportedTarget:
        return "UnsupportedTarget";
    case Error::MetalLibEmitFailed:
        return "MetalLibEmitFailed";
    case Error::ReflectionExtractionFailed:
        return "ReflectionExtractionFailed";
    case Error::DescriptorFrequencyAmbiguous:
        return "DescriptorFrequencyAmbiguous";
    case Error::DescriptorFrequencyMissing:
        return "DescriptorFrequencyMissing";
    case Error::LinkFailed:
        return "LinkFailed";
    case Error::SpecializationConstantMissing:
        return "SpecializationConstantMissing";
    case Error::CacheLookupMiss:
        return "CacheLookupMiss";
    case Error::CacheCorrupt:
        return "CacheCorrupt";
    case Error::CacheIntegrity:
        return "CacheIntegrity";
    case Error::CacheReadOnlyViolation:
        return "CacheReadOnlyViolation";
    case Error::CapabilityNotSupported:
        return "CapabilityNotSupported";
    case Error::ShippingCompilationAttempted:
        return "ShippingCompilationAttempted";
    }
    return "Unknown";
}

}  // namespace glibre::shader

namespace glibre {

// ---------------------------------------------------------------------------
// tag_string — maps a glibre::Error to its context name string
// ---------------------------------------------------------------------------
//
// Returns the stable context name tag for the active alternative in err.code().
// The index matches the order of alternating types in Error::Variant:
//   0 → core::Error   → "core::Error"
//   1 → render::Error → "render::Error"
//   2 → tools::Error  → "tools::Error"
//   3 → shader::Error → "shader::Error"
//
// Compile-time guard: the static_assert below fires if Error::Variant grows
// beyond the currently-known 4 alternatives without this function being
// updated.  Adding a 5th alternative to Error::Variant will fail the build
// with an actionable message ("extend tag_string when Error::Variant grows").
//
// Once tag_string is extended for the new alternative, increment the
// static_assert count to match.

// Migrated from eastl::variant_size_v to std::variant_size_v per
// reviews/decisions/eastl-removal.md §4 (matrix row 19).
static_assert(
    std::variant_size_v<Error::Variant> == 4,
    "extend tag_string() when Error::Variant grows (add a new case and bump "
    "the static_assert count in log_error.hpp)"
);

[[nodiscard]] inline const char* tag_string(const Error& err) noexcept {
    switch (err.code().index()) {
    case 0:
        return "core::Error";
    case 1:
        return "render::Error";
    case 2:
        return "tools::Error";
    case 3:
        return "shader::Error";
    }
    // err.code().index() == std::variant_npos only when the variant holds
    // valueless_by_exception state, which cannot occur in -fno-exceptions
    // builds.  std::unreachable() (C++23) eliminates any dead-code warning and
    // asserts this path is logically impossible.
    std::unreachable();
}

// ---------------------------------------------------------------------------
// variant_code_string — extract the enumerator name from the active alternative
// ---------------------------------------------------------------------------

[[nodiscard]] inline const char* variant_code_string(const Error& err) noexcept {
    // std::visit dispatches on the active alternative in err.code().
    // ADL finds the correct to_string overload in each per-context namespace.
    // Migrated from eastl::visit per reviews/decisions/eastl-removal.md §4.
    return std::visit([](auto&& e) -> const char* { return to_string(e); }, err.code());
}

// ---------------------------------------------------------------------------
// log_error — structured log a glibre::Error to a specific logger
// ---------------------------------------------------------------------------
//
// Logs at the specified level (default: spdlog::level::err).
// Output format (zero heap allocation — args passed directly to spdlog/fmtlib):
//   [tag=<tag_string>  variant=<enumerator>  file=<file>  line=<line>  detail=<detail>]
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
    const glibre::Error& err, spdlog::level::level_enum level = spdlog::level::err
) noexcept;

}  // namespace glibre
