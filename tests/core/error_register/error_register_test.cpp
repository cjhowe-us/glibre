// tests/core/error_register/error_register_test.cpp
//
// Catch2 unit tests for the per-context Error enum registration convention.
// Authority: core/include/glibre/error_register.hpp, plan #234,
//            reviews/decisions/error-model.md §Decision 2 + §Composition Rules.
//
// Named test cases (plan #234 Unit Test Plan + dispatch DoD):
//   - error_register_per_context_arms_unique
//   - error_register_compile_time_arm_count
//   - error_register_lists_known_contexts
//
// Design constraints:
//   - -fno-exceptions (error-model.md §Decision 3).
//   - No REQUIRE_THROWS usage.
//   - All uniqueness checks are purely compile-time (static_assert) or
//     runtime-mirrored via CHECK — the enums are fixed at compile time so
//     there is no need for dynamic introspection.

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <EASTL/array.h>
#include <catch2/catch_test_macros.hpp>
#include <glibre/error_register.hpp>

// ===========================================================================
// Test: error_register_per_context_arms_unique
//
// Verifies that within each per-context error enum, no two enumerators share
// the same underlying integer value.  Each per-context enum uses the default
// sequential assignment (0, 1, 2, …), so uniqueness holds by construction,
// but we make the invariant visible in the test report and catch future edits
// that might accidentally introduce an explicit duplicate value.
//
// Implementation note: The enums are fully enumerated at compile time.
// We use a constexpr helper that sorts a fixed-size array of values and
// checks adjacent pairs — any duplicate would appear adjacent after sorting.
// Sorting is done with a simple insertion sort since the arrays are small
// (< 20 elements).
// ===========================================================================

namespace {

/// Returns true if all elements in the array are distinct.
/// Uses an O(n^2) pairwise comparison — arrays are small (< 20 entries).
template<typename T, std::size_t N>
constexpr bool all_distinct(const eastl::array<T, N>& arr) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
        for (std::size_t j = i + 1; j < N; ++j) {
            if (arr[i] == arr[j]) {
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// core::Error — enumerator values
// ---------------------------------------------------------------------------

/// All core::Error enumerator values, listed in declaration order.
/// When a new enumerator is added to core::Error, add it here too.
constexpr eastl::array<std::underlying_type_t<glibre::core::Error>, 20> kCoreErrorValues{{
    static_cast<std::uint16_t>(glibre::core::Error::PluginAbiHashMismatch),
    static_cast<std::uint16_t>(glibre::core::Error::PluginInitFailed),
    static_cast<std::uint16_t>(glibre::core::Error::SchemaMigrationFailed),
    static_cast<std::uint16_t>(glibre::core::Error::HotReloadRefused),
    static_cast<std::uint16_t>(glibre::core::Error::FramePhaseMisordered),
    static_cast<std::uint16_t>(glibre::core::Error::OutOfBudget),
    static_cast<std::uint16_t>(glibre::core::Error::SystemScheduleCycle),
    static_cast<std::uint16_t>(glibre::core::Error::ScheduleAccessConflict),
    static_cast<std::uint16_t>(glibre::core::Error::PluginManifestNotFound),
    static_cast<std::uint16_t>(glibre::core::Error::PluginManifestInvalid),
    static_cast<std::uint16_t>(glibre::core::Error::PluginDlopenFailed),
    static_cast<std::uint16_t>(glibre::core::Error::PluginMissingEntryPoint),
    static_cast<std::uint16_t>(glibre::core::Error::PluginEngineTooOld),
    static_cast<std::uint16_t>(glibre::core::Error::PluginNameCollision),
    static_cast<std::uint16_t>(glibre::core::Error::PluginDependencyMissing),
    // plan #239: TransientArenaExhausted — transient arena capacity exhausted.
    static_cast<std::uint16_t>(glibre::core::Error::TransientArenaExhausted),
    // plan #239 r1/r2: NullArgument — null pointer to core API (was InvalidArgument).
    static_cast<std::uint16_t>(glibre::core::Error::NullArgument),
    // plan #250: PluginNameMismatch — hot-reload swap candidate has a different name.
    static_cast<std::uint16_t>(glibre::core::Error::PluginNameMismatch),
    // plan #597: TypeUnregistered — lookup on a TypeId with no registered descriptor.
    static_cast<std::uint16_t>(glibre::core::Error::TypeUnregistered),
    // plan #597: TypeRegistryClosed — register_type() called after seal().
    static_cast<std::uint16_t>(glibre::core::Error::TypeRegistryClosed),
    // When a new enumerator is added to core::Error, add it here and
    // increment the array size template argument above.
}};

// Compile-time uniqueness: core::Error values must be distinct.
// Fires if someone adds `ExampleError = PluginDlopenFailed` accidentally.
static_assert(
    all_distinct(kCoreErrorValues),
    "core::Error has duplicate enumerator values — each enumerator must have "
    "a unique underlying integer value."
);

// ---------------------------------------------------------------------------
// render::Error — enumerator values
// ---------------------------------------------------------------------------

constexpr eastl::array<std::underlying_type_t<glibre::render::Error>, 5> kRenderErrorValues{{
    static_cast<std::uint16_t>(glibre::render::Error::DeviceLost),
    static_cast<std::uint16_t>(glibre::render::Error::PipelineCompileFailed),
    static_cast<std::uint16_t>(glibre::render::Error::ResourceResidencyExceeded),
    static_cast<std::uint16_t>(glibre::render::Error::RenderGraphCycle),
    static_cast<std::uint16_t>(glibre::render::Error::UnsupportedBackend),
}};

static_assert(all_distinct(kRenderErrorValues), "render::Error has duplicate enumerator values.");

// ---------------------------------------------------------------------------
// tools::Error — enumerator values
// ---------------------------------------------------------------------------

constexpr eastl::array<std::underlying_type_t<glibre::tools::Error>, 6> kToolsErrorValues{{
    static_cast<std::uint16_t>(glibre::tools::Error::ForycSyntaxError),
    static_cast<std::uint16_t>(glibre::tools::Error::ForycDuplicateTag),
    static_cast<std::uint16_t>(glibre::tools::Error::ForycNonMonotoneVersion),
    static_cast<std::uint16_t>(glibre::tools::Error::ForycUnknownType),
    static_cast<std::uint16_t>(glibre::tools::Error::ForycIOError),
    static_cast<std::uint16_t>(glibre::tools::Error::ForycEmptySchema),
}};

static_assert(all_distinct(kToolsErrorValues), "tools::Error has duplicate enumerator values.");

// ---------------------------------------------------------------------------
// shader::Error — enumerator values
// ---------------------------------------------------------------------------
// Added by plan #508 (ShaderSource open + include resolver + entry-point scanner).

constexpr eastl::array<std::underlying_type_t<glibre::shader::Error>, 25> kShaderErrorValues{{
    static_cast<std::uint16_t>(glibre::shader::Error::SourceNotFound),
    static_cast<std::uint16_t>(glibre::shader::Error::SourceParseFailed),
    static_cast<std::uint16_t>(glibre::shader::Error::IncludeEscape),
    static_cast<std::uint16_t>(glibre::shader::Error::IncludeCycle),
    static_cast<std::uint16_t>(glibre::shader::Error::EncodingInvalid),
    static_cast<std::uint16_t>(glibre::shader::Error::EntryPointMissing),
    static_cast<std::uint16_t>(glibre::shader::Error::EntryPointStageAmbiguous),
    static_cast<std::uint16_t>(glibre::shader::Error::PermutationKeyMalformed),
    static_cast<std::uint16_t>(glibre::shader::Error::PermutationKeyOutOfRange),
    static_cast<std::uint16_t>(glibre::shader::Error::CompilerInvocationFailed),
    static_cast<std::uint16_t>(glibre::shader::Error::CompilerExitNonZero),
    static_cast<std::uint16_t>(glibre::shader::Error::CompilerTimedOut),
    static_cast<std::uint16_t>(glibre::shader::Error::UnsupportedTarget),
    static_cast<std::uint16_t>(glibre::shader::Error::MetalLibEmitFailed),
    static_cast<std::uint16_t>(glibre::shader::Error::ReflectionExtractionFailed),
    static_cast<std::uint16_t>(glibre::shader::Error::DescriptorFrequencyAmbiguous),
    static_cast<std::uint16_t>(glibre::shader::Error::DescriptorFrequencyMissing),
    static_cast<std::uint16_t>(glibre::shader::Error::LinkFailed),
    static_cast<std::uint16_t>(glibre::shader::Error::SpecializationConstantMissing),
    static_cast<std::uint16_t>(glibre::shader::Error::CacheLookupMiss),
    static_cast<std::uint16_t>(glibre::shader::Error::CacheCorrupt),
    static_cast<std::uint16_t>(glibre::shader::Error::CacheIntegrity),
    static_cast<std::uint16_t>(glibre::shader::Error::CacheReadOnlyViolation),
    static_cast<std::uint16_t>(glibre::shader::Error::CapabilityNotSupported),
    static_cast<std::uint16_t>(glibre::shader::Error::ShippingCompilationAttempted),
}};

static_assert(all_distinct(kShaderErrorValues), "shader::Error has duplicate enumerator values.");

}  // anonymous namespace

TEST_CASE("error_register_per_context_arms_unique", "[core][error_register]") {
    // Runtime mirrors of the file-scope static_asserts above.
    // Making them visible in the Catch2 report means they appear in CI output
    // and are tracked as named test cases in the DoD.

    // core::Error: 20 enumerators with sequential values 0..19 (plan #597 added
    // TypeUnregistered+TypeRegistryClosed)
    CHECK(all_distinct(kCoreErrorValues));

    // render::Error: 5 enumerators with sequential values 0..4
    CHECK(all_distinct(kRenderErrorValues));

    // tools::Error: 6 enumerators with sequential values 0..5
    CHECK(all_distinct(kToolsErrorValues));

    // shader::Error: 25 enumerators with sequential values 0..24
    CHECK(all_distinct(kShaderErrorValues));
}

// ===========================================================================
// Test: error_register_compile_time_arm_count
//
// Verifies that the Variant arm count matches kExpectedArmCount from the
// registry.  This is a runtime mirror of the file-scope static_assert in
// error_register.hpp so the invariant appears in the Catch2 test report.
//
// Context: the static_assert in error_register.hpp fires at compile time when
// the variant arm count and kExpectedArmCount are out of sync.  This runtime
// CHECK surfaces the same fact in CI output and test-results artifacts.
// ===========================================================================

// Compile-time: verified in error_register.hpp via static_assert.
// Runtime mirror below for Catch2 visibility.
static_assert(
    eastl::variant_size_v<glibre::Error::Variant> == glibre::kExpectedArmCount,
    "Variant arm count does not match kExpectedArmCount (runtime mirror — "
    "should be caught by error_register.hpp static_assert first)."
);

TEST_CASE("error_register_compile_time_arm_count", "[core][error_register]") {
    // The static_assert above already fires at compile time.
    // The runtime CHECK below makes the invariant visible in the test report.
    constexpr std::size_t actual = eastl::variant_size_v<glibre::Error::Variant>;
    CHECK(actual == glibre::kExpectedArmCount);

    // Also verify the kAllErrorContexts array has the same length —
    // consistent with the second static_assert in error_register.hpp.
    CHECK(glibre::kAllErrorContexts.size() == glibre::kExpectedArmCount);
}

// ===========================================================================
// Test: error_register_lists_known_contexts
//
// Verifies that kAllErrorContexts contains the expected context name strings
// in the correct order (matching the Variant arm order).
//
// Minimum required: "core" (index 0) and "tools" are present (per dispatch).
// This test also checks "render" since it is currently registered.
// ===========================================================================

TEST_CASE("error_register_lists_known_contexts", "[core][error_register]") {
    // The registry must list at least "core" and "tools".
    // Order must match Variant arm indices so that variant_index maps
    // correctly to a log tag (error-model.md §Logging).

    // The compile-time static_assert in error_register.hpp already guarantees
    // kAllErrorContexts.size() == kExpectedArmCount — this runtime REQUIRE
    // makes the same contract visible in the Catch2 report and OOB-safe.
    static_assert(
        glibre::kExpectedArmCount >= 4,
        "kExpectedArmCount must be >= 4 (core + render + tools + shader are registered)."
    );
    REQUIRE(glibre::kAllErrorContexts.size() == glibre::kExpectedArmCount);

    // Index 0 corresponds to core::Error (first Variant arm).
    CHECK(glibre::kAllErrorContexts[0] == eastl::string_view{"core"});

    // Index 1 corresponds to render::Error.
    CHECK(glibre::kAllErrorContexts[1] == eastl::string_view{"render"});

    // Index 2 corresponds to tools::Error.
    CHECK(glibre::kAllErrorContexts[2] == eastl::string_view{"tools"});

    // Index 3 corresponds to shader::Error (added by plan #508).
    CHECK(glibre::kAllErrorContexts[3] == eastl::string_view{"shader"});

    // No duplicates in the context names list.
    bool has_duplicates = false;
    for (std::size_t i = 0; i < glibre::kAllErrorContexts.size(); ++i) {
        for (std::size_t j = i + 1; j < glibre::kAllErrorContexts.size(); ++j) {
            if (glibre::kAllErrorContexts[i] == glibre::kAllErrorContexts[j]) {
                has_duplicates = true;
            }
        }
    }
    CHECK_FALSE(has_duplicates);
}
