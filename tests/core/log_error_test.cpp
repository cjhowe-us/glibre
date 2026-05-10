// tests/core/log_error_test.cpp
//
// Catch2 unit tests for glibre::log_error() — structured Error logging helper.
//
// Named test cases (per plan #236 Unit Test Plan):
//   - core/log_error: emits_structured_kv_for_core_error
//   - core/log_error: hot_reload_refusal_logs_at_warn_level
//   - core/log_error: tag_string_stable_for_core
//   - core/log_error: tostring_complete_for_core_error
//
// Test strategy: install a single-threaded spdlog::ostream_sink_st into a
// named logger.  Call log_error() with constructed glibre::Error values.
// Assert the captured sink output contains the expected structured fields.
//
// -fno-exceptions compatible: no REQUIRE_THROWS used.
// std::make_shared allocation failure is a terminate() in -fno-exceptions builds;
// this is acceptable for unit tests running in a resource-sufficient environment.

#include <memory>
#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>

#include "glibre/error.hpp"
#include "glibre/log_error.hpp"

// ---------------------------------------------------------------------------
// Fixture helpers
// ---------------------------------------------------------------------------

// make_string_logger — construct a spdlog::logger that writes into `oss`.
// Returns the logger; `oss` must outlive it.
// Uses ostream_sink_st (single-threaded, no mutex) since tests are single-threaded.
static std::shared_ptr<spdlog::logger>
make_string_logger(const std::string& name, std::ostringstream& oss) {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_st>(oss, /*force_flush=*/true);
    // Pattern: just the message, no timestamp/level decoration, so assertions
    // are unambiguous.  %v is the message payload in spdlog's pattern syntax.
    sink->set_pattern("%v");
    auto logger = std::make_shared<spdlog::logger>(name, std::move(sink));
    // Set to trace so all log levels are captured.
    logger->set_level(spdlog::level::trace);
    return logger;
}

// ---------------------------------------------------------------------------
// Test: core/log_error: emits_structured_kv_for_core_error
//
// Constructs a glibre::Error with core::Error::PluginDlopenFailed and a
// populated ErrorContext.  Calls log_error() at err level.  Asserts the
// captured sink string contains the expected tag, variant, file, line, and
// detail fields.
// ---------------------------------------------------------------------------
TEST_CASE("core/log_error: emits_structured_kv_for_core_error", "[core][log_error]") {
    std::ostringstream oss;
    auto logger = make_string_logger("test.log_error.kv", oss);

    const glibre::ErrorContext ctx{
        .file = "core/src/plugin_loader.cpp",
        .line = 87,
        .detail = "dlopen failed",
    };
    const glibre::Error err{glibre::core::Error::PluginDlopenFailed, ctx};

    glibre::log_error(*logger, err, spdlog::level::err);

    const std::string captured = oss.str();

    // The log line must contain every structured field.
    CHECK(captured.find("tag=core::Error") != std::string::npos);
    CHECK(captured.find("variant=PluginDlopenFailed") != std::string::npos);
    CHECK(captured.find("file=core/src/plugin_loader.cpp") != std::string::npos);
    CHECK(captured.find("line=87") != std::string::npos);
    CHECK(captured.find("detail=dlopen failed") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Test: core/log_error: hot_reload_refusal_logs_at_warn_level
//
// Verifies that a HotReloadRefused error can be logged at warn level.
// Per error-model.md §"Logging / Telemetry" rule 3, hot-reload refusals
// must not escalate to error level; the caller supplies spdlog::level::warn.
//
// Test strategy: install two spdlog::logger instances sharing the same sink
// (oss).  Log at warn then at err.  Assert warn-level message appears in
// captured output and that the level distinction is respected.
// Since we use pattern "%v" (message only), we use two separate oss instances
// to isolate the two log calls and assert each individually.
// ---------------------------------------------------------------------------
TEST_CASE("core/log_error: hot_reload_refusal_logs_at_warn_level", "[core][log_error]") {
    // Part 1: log at warn level — verify the message is emitted.
    {
        std::ostringstream oss_warn;
        auto logger = make_string_logger("test.log_error.hot_reload.warn", oss_warn);

        const glibre::ErrorContext ctx{
            .file = "core/src/hot_reload.cpp",
            .line = 42,
            .detail = "abi hash mismatch",
        };
        const glibre::Error err{glibre::core::Error::HotReloadRefused, ctx};

        glibre::log_error(*logger, err, spdlog::level::warn);

        const std::string captured = oss_warn.str();
        CHECK(!captured.empty());
        CHECK(captured.find("variant=HotReloadRefused") != std::string::npos);
        CHECK(captured.find("detail=abi hash mismatch") != std::string::npos);
    }

    // Part 2: logger set to err level — warn messages must be suppressed.
    {
        std::ostringstream oss_suppressed;
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_st>(oss_suppressed, true);
        sink->set_pattern("%v");
        auto logger =
            std::make_shared<spdlog::logger>("test.log_error.hot_reload.suppress", std::move(sink));
        // Logger level set to err — warn is below threshold, so nothing is written.
        logger->set_level(spdlog::level::err);

        const glibre::Error err{glibre::core::Error::HotReloadRefused};
        glibre::log_error(*logger, err, spdlog::level::warn);

        // No output expected: the logger filtered the warn message.
        CHECK(oss_suppressed.str().empty());
    }
}

// ---------------------------------------------------------------------------
// Test: core/log_error: tag_string_stable_for_core
//
// Verifies that tag_string() returns "core::Error" for any core::Error
// variant, "render::Error" for any render::Error variant, and
// "tools::Error" for any tools::Error variant.
//
// This is the compile-time-stability test: adding a new context enum arm
// to Error::Variant without updating tag_string() will fail this test
// at the variant index boundary.
// ---------------------------------------------------------------------------
TEST_CASE("core/log_error: tag_string_stable_for_core", "[core][log_error]") {
    // core::Error — variant index 0.
    {
        const glibre::Error err{glibre::core::Error::PluginAbiHashMismatch};
        const char* tag = glibre::tag_string(err);
        REQUIRE(tag != nullptr);
        CHECK(std::string{tag} == "core::Error");
    }

    // render::Error — variant index 1.
    {
        const glibre::Error err{glibre::render::Error::DeviceLost};
        const char* tag = glibre::tag_string(err);
        REQUIRE(tag != nullptr);
        CHECK(std::string{tag} == "render::Error");
    }

    // tools::Error — variant index 2.
    {
        const glibre::Error err{glibre::tools::Error::ForycSyntaxError};
        const char* tag = glibre::tag_string(err);
        REQUIRE(tag != nullptr);
        CHECK(std::string{tag} == "tools::Error");
    }

    // shader::Error — variant index 3 (added by plan #508).
    {
        const glibre::Error err{glibre::shader::Error::SourceNotFound};
        const char* tag = glibre::tag_string(err);
        REQUIRE(tag != nullptr);
        CHECK(std::string{tag} == "shader::Error");
    }
}

// ---------------------------------------------------------------------------
// Test: core/log_error: tostring_complete_for_core_error
//
// Verifies that glibre::core::to_string(core::Error) returns a non-null,
// non-empty string for every enumerator in core::Error.
//
// If a new enumerator is added to core::Error without adding a matching arm
// to to_string(core::Error), -Werror=switch (core/CMakeLists.txt) turns the
// -Wswitch diagnostic into a build error.  This test provides runtime
// confirmation that every enumerator maps to the correct string literal.
// ---------------------------------------------------------------------------
TEST_CASE("core/log_error: tostring_complete_for_core_error", "[core][log_error]") {
    using E = glibre::core::Error;

    // Each enumerator must produce a non-null, non-empty string.
    // The expected string is checked against the enumerator name to guard
    // against copy-paste errors in the to_string arm bodies.
    const auto check = [](E e, const char* expected) {
        const char* s = glibre::core::to_string(e);
        REQUIRE(s != nullptr);
        CHECK(*s != '\0');
        CHECK(std::string{s} == std::string{expected});
    };

    check(E::PluginAbiHashMismatch, "PluginAbiHashMismatch");
    check(E::PluginInitFailed, "PluginInitFailed");
    check(E::SchemaMigrationFailed, "SchemaMigrationFailed");
    check(E::HotReloadRefused, "HotReloadRefused");
    check(E::FramePhaseMisordered, "FramePhaseMisordered");
    check(E::OutOfBudget, "OutOfBudget");
    check(E::SystemScheduleCycle, "SystemScheduleCycle");
    check(E::ScheduleAccessConflict, "ScheduleAccessConflict");
    check(E::PluginManifestNotFound, "PluginManifestNotFound");
    check(E::PluginManifestInvalid, "PluginManifestInvalid");
    check(E::PluginDlopenFailed, "PluginDlopenFailed");
    check(E::PluginMissingEntryPoint, "PluginMissingEntryPoint");
    check(E::PluginEngineTooOld, "PluginEngineTooOld");
    check(E::PluginNameCollision, "PluginNameCollision");
    check(E::PluginDependencyMissing, "PluginDependencyMissing");
    // plan #239: transient arena exhaustion error
    check(E::TransientArenaExhausted, "TransientArenaExhausted");
    // plan #239 r2: NullArgument (renamed from InvalidArgument in round 2 LOW-3)
    check(E::NullArgument, "NullArgument");
    // plan #250: PluginNameMismatch — hot-reload swap candidate has different name
    check(E::PluginNameMismatch, "PluginNameMismatch");
    // plan #597: TypeUnregistered — lookup on unregistered TypeId
    check(E::TypeUnregistered, "TypeUnregistered");
    // plan #597: TypeRegistryClosed — register_type() called after seal()
    check(E::TypeRegistryClosed, "TypeRegistryClosed");
    // plan #597: TypeRegistryGap — codegen contract violation (gap in TypeId sequence)
    check(E::TypeRegistryGap, "TypeRegistryGap");
    // plan #557: EntityStale — stale generational entity handle
    check(E::EntityStale, "EntityStale");
    // plan #557: EntityForeignWorld — entity from a different World (reserved, unreachable MVP)
    check(E::EntityForeignWorld, "EntityForeignWorld");
    // plan #598: AssetStale — stale generational asset handle
    check(E::AssetStale, "AssetStale");
    // plan #584 R1 MED-3: Phase::HotReload footgun guard
    check(E::SystemForbiddenInHotReloadPhase, "SystemForbiddenInHotReloadPhase");
    // plan #584 R2 MED-1: (phase, fqn) idempotency conflict
    check(E::SystemDescriptorConflict, "SystemDescriptorConflict");
}

// ---------------------------------------------------------------------------
// Test: core/log_error: tostring_complete_for_render_error
//
// Verifies that glibre::render::to_string(render::Error) returns a non-null,
// non-empty string for every enumerator in render::Error.
//
// Symmetric to tostring_complete_for_core_error.  -Werror=switch catches
// missing arms at build time; this test provides runtime string-value checks.
// ---------------------------------------------------------------------------
TEST_CASE("core/log_error: tostring_complete_for_render_error", "[core][log_error]") {
    using E = glibre::render::Error;

    const auto check = [](E e, const char* expected) {
        const char* s = glibre::render::to_string(e);
        REQUIRE(s != nullptr);
        CHECK(*s != '\0');
        CHECK(std::string{s} == std::string{expected});
    };

    check(E::DeviceLost, "DeviceLost");
    check(E::PipelineCompileFailed, "PipelineCompileFailed");
    check(E::ResourceResidencyExceeded, "ResourceResidencyExceeded");
    check(E::RenderGraphCycle, "RenderGraphCycle");
    check(E::UnsupportedBackend, "UnsupportedBackend");
}

// ---------------------------------------------------------------------------
// Test: core/log_error: tostring_complete_for_tools_error
//
// Verifies that glibre::tools::to_string(tools::Error) returns a non-null,
// non-empty string for every enumerator in tools::Error.
//
// Symmetric to tostring_complete_for_core_error.
// ---------------------------------------------------------------------------
TEST_CASE("core/log_error: tostring_complete_for_tools_error", "[core][log_error]") {
    using E = glibre::tools::Error;

    const auto check = [](E e, const char* expected) {
        const char* s = glibre::tools::to_string(e);
        REQUIRE(s != nullptr);
        CHECK(*s != '\0');
        CHECK(std::string{s} == std::string{expected});
    };

    check(E::ForycSyntaxError, "ForycSyntaxError");
    check(E::ForycDuplicateTag, "ForycDuplicateTag");
    check(E::ForycNonMonotoneVersion, "ForycNonMonotoneVersion");
    check(E::ForycUnknownType, "ForycUnknownType");
    check(E::ForycIOError, "ForycIOError");
    check(E::ForycEmptySchema, "ForycEmptySchema");
}

// ---------------------------------------------------------------------------
// Test: core/log_error: tag_string_stable_for_shader
//
// Verifies that tag_string() returns "shader::Error" for shader::Error variants.
// Added by plan #508 (ShaderSource open + include resolver + entry-point scanner).
// ---------------------------------------------------------------------------
TEST_CASE("core/log_error: tag_string_stable_for_shader", "[core][log_error]") {
    // shader::Error — variant index 3.
    const glibre::Error err{glibre::shader::Error::SourceNotFound};
    const char* tag = glibre::tag_string(err);
    REQUIRE(tag != nullptr);
    CHECK(std::string{tag} == "shader::Error");
}

// ---------------------------------------------------------------------------
// Test: core/log_error: tostring_complete_for_shader_error
//
// Verifies that glibre::shader::to_string(shader::Error) returns a non-null,
// non-empty string for every enumerator in shader::Error.
// Added by plan #508.
// ---------------------------------------------------------------------------
TEST_CASE("core/log_error: tostring_complete_for_shader_error", "[core][log_error]") {
    using E = glibre::shader::Error;

    const auto check = [](E e, const char* expected) {
        const char* s = glibre::shader::to_string(e);
        REQUIRE(s != nullptr);
        CHECK(*s != '\0');
        CHECK(std::string{s} == std::string{expected});
    };

    check(E::SourceNotFound, "SourceNotFound");
    check(E::SourceParseFailed, "SourceParseFailed");
    check(E::IncludeEscape, "IncludeEscape");
    check(E::IncludeCycle, "IncludeCycle");
    check(E::EncodingInvalid, "EncodingInvalid");
    check(E::EntryPointMissing, "EntryPointMissing");
    check(E::EntryPointStageAmbiguous, "EntryPointStageAmbiguous");
    check(E::PermutationKeyMalformed, "PermutationKeyMalformed");
    check(E::PermutationKeyOutOfRange, "PermutationKeyOutOfRange");
    check(E::CompilerInvocationFailed, "CompilerInvocationFailed");
    check(E::CompilerExitNonZero, "CompilerExitNonZero");
    check(E::CompilerTimedOut, "CompilerTimedOut");
    check(E::UnsupportedTarget, "UnsupportedTarget");
    check(E::MetalLibEmitFailed, "MetalLibEmitFailed");
    check(E::ReflectionExtractionFailed, "ReflectionExtractionFailed");
    check(E::DescriptorFrequencyAmbiguous, "DescriptorFrequencyAmbiguous");
    check(E::DescriptorFrequencyMissing, "DescriptorFrequencyMissing");
    check(E::LinkFailed, "LinkFailed");
    check(E::SpecializationConstantMissing, "SpecializationConstantMissing");
    check(E::CacheLookupMiss, "CacheLookupMiss");
    check(E::CacheCorrupt, "CacheCorrupt");
    check(E::CacheIntegrity, "CacheIntegrity");
    check(E::CacheReadOnlyViolation, "CacheReadOnlyViolation");
    check(E::CapabilityNotSupported, "CapabilityNotSupported");
    check(E::ShippingCompilationAttempted, "ShippingCompilationAttempted");
}
