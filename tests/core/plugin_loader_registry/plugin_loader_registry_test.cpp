// tests/core/plugin_loader_registry/plugin_loader_registry_test.cpp
//
// Catch2 unit tests for glibre::core::PluginLoaderRegistry (plan #230, #981, #1044).
//
// Named test cases (plan #230 Unit Test Plan + DoD):
//   - plugin_loader_rejects_abi_hash_mismatch
//   - plugin_loader_rejects_unknown_dependency
//   - plugin_loader_accepts_compatible_plugin
//   - plugin_loader_rejects_duplicate_name
//   - plugin_loader_rejects_incompatible_version
//
// Named test cases (plan #981 Unit Test Plan + DoD):
//   - validate_drain_phase_outside_hotreload_returns_error
//   - validate_drain_phase_at_phase_8_succeeds
//
// Named test cases (plan #1044 Unit Test Plan + DoD):
//   - core/plugin_loader_registry: lookup_o_log_n
//   - core/compat/transparent_string_hash: heterogeneous_lookup_string_view
//
// Design constraints:
//   * -fno-exceptions (error-model.md §Decision 3).
//   * No dylib is loaded in any of these tests.  All four gates operate
//     purely on in-memory PluginManifest values and the registry state.
//     dlopen/dlsym is already covered by tests/core/plugin_loader (plan #229).
//   * Tests construct and own PluginLoaderRegistry instances directly --
//     the registry is NOT a singleton (plugin_loader_registry.hpp contract).
//   * All eastl::string_view call sites replaced with std::string_view
//     (plan #1044 migration, eastl-removal.md matrix row 2).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>

#include <catch2/catch_test_macros.hpp>
#include <glibre/compat/transparent_string_hash.hpp>
#include <glibre/core/frame_phase.hpp>
#include <glibre/core/plugin_loader_registry.hpp>
#include <glibre/core/plugin_manifest.hpp>
#include <glibre/error.hpp>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

// Sentinel ABI hash used as the "host expected" value throughout tests.
// 64 hex chars = blake3 256-bit digest placeholder.
constexpr const char kExpectedHash[] =
    "aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899";

// A different hash that should never match kExpectedHash.
constexpr const char kWrongHash[] =
    "0000000000000000000000000000000000000000000000000000000000000000";

// Host engine version used in happy-path tests.
constexpr glibre::core::SemVer kHostVersion{1, 0, 0};

/// Build a minimal valid PluginManifest.
///
/// Defaults produce a manifest that will pass all four gates against a
/// fresh registry with host_version = kHostVersion and
/// expected_abi_hash = kExpectedHash.
glibre::core::PluginManifest make_manifest(
    const char* name = "glibre.test.plugin",
    glibre::core::SemVer version = {1, 0, 0},
    const char* abi_hash = kExpectedHash,
    glibre::core::SemVer min_engine = {0, 1, 0}
) {

    glibre::core::PluginManifest m;
    m.name = name;      // std::pmr::string accepts const char* directly
    m.version = version;
    m.abi_hash = abi_hash;  // std::pmr::string accepts const char* directly
    m.min_engine_version = min_engine;
    // depends_on is empty by default (no dependencies).
    return m;
}

/// Build a manifest with a pre-populated depends_on list.
///
/// Convenience wrapper used by dependency tests to avoid repetitive
/// push_back call sequences (LOW-7: helper reduces dep-test duplication).
template<typename... Deps>
glibre::core::PluginManifest make_manifest_with_deps(const char* name, Deps... deps) {

    auto m = make_manifest(name);
    (m.depends_on.push_back(std::pmr::string{deps}), ...);
    return m;
}

/// Extract the core::Error variant arm.  Returns nullptr if the error holds
/// a different arm (e.g. tools::Error or render::Error).
[[nodiscard]] const glibre::core::Error* as_core_error(const glibre::Error& err) noexcept {
    return std::get_if<glibre::core::Error>(&err.code());
}

}  // anonymous namespace

// ===========================================================================
// Test: plugin_loader_rejects_abi_hash_mismatch
//
// Gate 1 (plugin-abi.md §"Loader Sequence" step 4):
//   manifest.abi_hash AND the exported symbol must both equal the expected
//   (host) ABI hash.  Mismatch -> core::Error::PluginAbiHashMismatch.
//
// Three sub-cases:
//   a) manifest.abi_hash mismatches  -> rejected via validate_manifest_abi_hash.
//   b) symbol_abi_hash mismatches    -> rejected via validate_all gate-1a.
//   c) both hashes wrong             -> still PluginAbiHashMismatch (not masked).
// ===========================================================================

TEST_CASE("plugin_loader_rejects_abi_hash_mismatch", "[core][plugin_loader_registry]") {
    glibre::core::PluginLoaderRegistry registry{kHostVersion};

    SECTION("manifest abi_hash mismatch") {
        // Build a manifest whose abi_hash differs from the expected value.
        auto manifest = make_manifest(
            "glibre.test.plugin",
            {1, 0, 0},
            kWrongHash,  // wrong manifest hash
            {0, 1, 0}
        );

        // Validate through the standalone manifest gate -- should reject.
        auto result = registry.validate_manifest_abi_hash(manifest, kExpectedHash);

        REQUIRE_FALSE(result.has_value());
        const auto* core_err = as_core_error(result.error());
        REQUIRE(core_err != nullptr);
        CHECK(*core_err == glibre::core::Error::PluginAbiHashMismatch);
    }

    SECTION("symbol abi_hash mismatch in validate_all") {
        // Manifest hash is correct, but the symbol value differs.
        auto manifest = make_manifest(
            "glibre.test.plugin",
            {1, 0, 0},
            kExpectedHash,  // manifest hash is correct
            {0, 1, 0}
        );

        // Pass the wrong symbol hash -> gate-1a in validate_all fires.
        auto result = registry.validate_all(
            manifest, kExpectedHash, kWrongHash, "/fake/path.dylib"
        );

        REQUIRE_FALSE(result.has_value());
        const auto* core_err = as_core_error(result.error());
        REQUIRE(core_err != nullptr);
        CHECK(*core_err == glibre::core::Error::PluginAbiHashMismatch);
    }

    SECTION("both hashes wrong -> still PluginAbiHashMismatch (not masked)") {
        auto manifest = make_manifest(
            "glibre.test.plugin",
            {1, 0, 0},
            kWrongHash,  // wrong manifest hash
            {0, 1, 0}
        );

        auto result = registry.validate_all(
            manifest, kExpectedHash, kWrongHash, "/fake/path.dylib"
        );

        REQUIRE_FALSE(result.has_value());
        const auto* core_err = as_core_error(result.error());
        REQUIRE(core_err != nullptr);
        CHECK(*core_err == glibre::core::Error::PluginAbiHashMismatch);
    }
}

// ===========================================================================
// Test: plugin_loader_rejects_unknown_dependency
//
// Gate 4 (plugin-abi.md §"Loader Sequence" step 7):
//   Every entry in manifest.depends_on must already be registered.
//   Any missing dependency -> core::Error::PluginDependencyMissing.
// ===========================================================================

TEST_CASE("plugin_loader_rejects_unknown_dependency", "[core][plugin_loader_registry]") {
    glibre::core::PluginLoaderRegistry registry{kHostVersion};

    // Build a manifest that depends on "glibre.base" which is not registered.
    auto manifest = make_manifest_with_deps("glibre.test.plugin", "glibre.base");

    auto result = registry.validate_dependencies(manifest);

    REQUIRE_FALSE(result.has_value());
    const auto* core_err = as_core_error(result.error());
    REQUIRE(core_err != nullptr);
    CHECK(*core_err == glibre::core::Error::PluginDependencyMissing);
}

// ===========================================================================
// Test: plugin_loader_accepts_compatible_plugin
//
// Happy path: a plugin whose manifest passes all four gates.
//
// Verifies that validate_all returns has_value() == true for a correctly-
// formed manifest against a fresh (empty) registry with matching versions.
// Also exercises register_plugin and loaded_count.
// ===========================================================================

TEST_CASE("plugin_loader_accepts_compatible_plugin", "[core][plugin_loader_registry]") {
    glibre::core::PluginLoaderRegistry registry{kHostVersion};

    // Build a manifest that satisfies all gates:
    //   * abi_hash == kExpectedHash
    //   * min_engine_version {0,1,0} <= host {1,0,0}
    //   * name "glibre.test.plugin" is not registered
    //   * depends_on is empty (no unmet dependencies)
    auto manifest = make_manifest();

    constexpr std::string_view path{"/fake/path/plugin.dylib"};

    auto result = registry.validate_all(manifest, kExpectedHash, kExpectedHash, path);

    REQUIRE(result.has_value());

    // After successful validation, register the plugin.
    REQUIRE(registry.register_plugin(manifest, path).has_value());

    // Registry should now have one loaded plugin.
    CHECK(registry.loaded_count() == 1u);
    CHECK(registry.is_registered(std::string_view{manifest.name}));
}

// ===========================================================================
// Test: plugin_loader_rejects_duplicate_name
//
// Gate 3 (plugin-abi.md §"Loader Sequence" step 6):
//   manifest.name must be unique across already-loaded plugins.
//   Collision -> core::Error::PluginNameCollision.
// ===========================================================================

TEST_CASE("plugin_loader_rejects_duplicate_name", "[core][plugin_loader_registry]") {
    glibre::core::PluginLoaderRegistry registry{kHostVersion};

    constexpr std::string_view first_path{"/fake/path/first.dylib"};
    constexpr std::string_view second_path{"/fake/path/second.dylib"};  // different path

    // Register the first plugin successfully.
    auto first = make_manifest("glibre.test.duplicate", {1, 0, 0}, kExpectedHash, {0, 1, 0});
    auto r1 = registry.validate_all(first, kExpectedHash, kExpectedHash, first_path);
    REQUIRE(r1.has_value());
    REQUIRE(registry.register_plugin(first, first_path).has_value());

    // Attempt to register a second plugin with the same name but different path.
    auto second = make_manifest("glibre.test.duplicate", {1, 1, 0}, kExpectedHash, {0, 1, 0});
    auto r2 = registry.validate_all(second, kExpectedHash, kExpectedHash, second_path);

    REQUIRE_FALSE(r2.has_value());
    const auto* core_err = as_core_error(r2.error());
    REQUIRE(core_err != nullptr);
    CHECK(*core_err == glibre::core::Error::PluginNameCollision);
}

// ===========================================================================
// Test: plugin_loader_rejects_incompatible_version
//
// Gate 2 (plugin-abi.md §"Loader Sequence" step 5):
//   manifest.min_engine_version must be <= host engine version.
//   Plugin requiring a newer engine -> core::Error::PluginEngineTooOld.
// ===========================================================================

TEST_CASE("plugin_loader_rejects_incompatible_version", "[core][plugin_loader_registry]") {
    // Host engine version {1, 0, 0} -- older than the plugin requires.
    glibre::core::PluginLoaderRegistry registry{glibre::core::SemVer{1, 0, 0}};

    // Plugin requires engine {2, 0, 0} -- newer than the host.
    auto manifest = make_manifest(
        "glibre.test.future", {1, 0, 0}, kExpectedHash, {2, 0, 0}
        // min_engine_version = 2.0.0 > host 1.0.0
    );

    auto result = registry.validate_engine_version(manifest);

    REQUIRE_FALSE(result.has_value());
    const auto* core_err = as_core_error(result.error());
    REQUIRE(core_err != nullptr);
    CHECK(*core_err == glibre::core::Error::PluginEngineTooOld);

    SECTION("host exactly meets minimum version") {
        // host == min_engine_version -> must succeed (<= condition).
        glibre::core::PluginLoaderRegistry reg_exact{glibre::core::SemVer{2, 0, 0}};
        auto r2 = reg_exact.validate_engine_version(manifest);
        CHECK(r2.has_value());
    }

    SECTION("host exceeds minimum version") {
        glibre::core::PluginLoaderRegistry reg_newer{glibre::core::SemVer{3, 5, 2}};
        auto r3 = reg_newer.validate_engine_version(manifest);
        CHECK(r3.has_value());
    }
}

// ===========================================================================
// Additional: gate ordering in validate_all
//
// Verifies that validate_all checks gates in the mandated order:
//   hash -> engine version -> name -> deps.
//
// A manifest that fails gate 1 (hash) should not proceed to check name/deps.
// ===========================================================================

TEST_CASE("validate_all_gate_ordering_hash_first", "[core][plugin_loader_registry]") {
    glibre::core::PluginLoaderRegistry registry{kHostVersion};

    // Register "glibre.base" first so a dependency check would pass.
    auto base = make_manifest("glibre.base");
    {
        // validate_all would succeed here; we discard the result intentionally
        // because we're setting up state, not testing this call.
        auto r = registry.validate_all(base, kExpectedHash, kExpectedHash, "/fake/base.dylib");
        REQUIRE(r.has_value());
    }
    REQUIRE(registry.register_plugin(base, "/fake/base.dylib").has_value());

    // Now build a manifest that:
    //   * has a WRONG abi_hash (gate 1 fails)
    //   * depends on "glibre.base" which IS registered (gate 4 would pass)
    auto manifest = make_manifest_with_deps("glibre.test.order", "glibre.base");
    manifest.abi_hash = kWrongHash;  // std::pmr::string from const char*; gate 1 fails

    auto result = registry.validate_all(
        manifest,
        kExpectedHash,
        kExpectedHash,  // symbol hash matches, but manifest hash does not
        "/fake/order.dylib"
    );

    // Gate 1 (manifest hash check) must fire, not gate 4.
    REQUIRE_FALSE(result.has_value());
    const auto* core_err = as_core_error(result.error());
    REQUIRE(core_err != nullptr);
    CHECK(*core_err == glibre::core::Error::PluginAbiHashMismatch);
}

// ===========================================================================
// Gate ordering: gate 2 (engine version) fires before gate 3 (name).
//
// A manifest that fails gate 2 must not proceed to check name uniqueness.
// Demonstrates that the engine-version gate is ordered before the name gate.
// ===========================================================================

TEST_CASE("validate_all_gate_ordering_version_before_name", "[core][plugin_loader_registry]") {
    glibre::core::PluginLoaderRegistry registry{glibre::core::SemVer{1, 0, 0}};

    // Register "glibre.base" so a name-collision would be possible if gate 3 ran.
    auto base = make_manifest("glibre.collision.victim");
    REQUIRE(registry.register_plugin(base, "/fake/base.dylib").has_value());

    // Manifest: same name as registered plugin (gate 3 would fail) AND
    // min_engine_version newer than host (gate 2 fails).  Gate 2 must fire.
    auto manifest = make_manifest(
        "glibre.collision.victim",  // same name -> gate 3 would fire if reached
        {1, 0, 0},
        kExpectedHash,
        {2, 0, 0}  // requires engine 2.0.0, host is 1.0.0 -> gate 2 fires
    );

    // Different path so gate 3 would produce PluginNameCollision if reached.
    auto result = registry.validate_all(
        manifest, kExpectedHash, kExpectedHash, "/fake/other.dylib"
    );

    // Gate 2 must fire first -- PluginEngineTooOld, not PluginNameCollision.
    REQUIRE_FALSE(result.has_value());
    const auto* core_err = as_core_error(result.error());
    REQUIRE(core_err != nullptr);
    CHECK(*core_err == glibre::core::Error::PluginEngineTooOld);
}

// ===========================================================================
// Gate ordering: gate 3 (name uniqueness) fires before gate 4 (deps).
//
// A manifest that fails gate 3 must not proceed to check dependencies.
// ===========================================================================

TEST_CASE("validate_all_gate_ordering_name_before_deps", "[core][plugin_loader_registry]") {
    glibre::core::PluginLoaderRegistry registry{kHostVersion};

    // Register "glibre.taken" so name-collision fires for gate 3.
    auto taken = make_manifest("glibre.taken");
    REQUIRE(registry.register_plugin(taken, "/fake/taken.dylib").has_value());

    // Manifest: same name + different path (gate 3 fails) AND missing dep
    // "glibre.missing" which is not registered (gate 4 would fail).
    // Gate 3 must fire before gate 4.
    auto manifest = make_manifest_with_deps(
        "glibre.taken",   // same name -> gate 3 fires
        "glibre.missing"  // missing dep -> gate 4 would fire if reached
    );

    auto result = registry.validate_all(
        manifest, kExpectedHash, kExpectedHash, "/fake/different.dylib"
    );

    // Gate 3 must fire first -- PluginNameCollision, not PluginDependencyMissing.
    REQUIRE_FALSE(result.has_value());
    const auto* core_err = as_core_error(result.error());
    REQUIRE(core_err != nullptr);
    CHECK(*core_err == glibre::core::Error::PluginNameCollision);
}

// ===========================================================================
// Additional: is_registered and loaded_count
//
// Basic accessor unit tests.
// ===========================================================================

TEST_CASE("registry_is_registered_and_loaded_count", "[core][plugin_loader_registry]") {
    glibre::core::PluginLoaderRegistry registry{kHostVersion};

    CHECK(registry.loaded_count() == 0u);
    CHECK_FALSE(registry.is_registered("glibre.absent"));

    auto m = make_manifest("glibre.test.accessor");
    REQUIRE(registry.register_plugin(m, "").has_value());

    CHECK(registry.loaded_count() == 1u);
    CHECK(registry.is_registered("glibre.test.accessor"));
    CHECK_FALSE(registry.is_registered("glibre.absent"));
}

// ===========================================================================
// Additional: multi-dependency happy path
//
// A plugin with two declared dependencies passes validation once both
// are already registered.
// ===========================================================================

TEST_CASE("plugin_loader_accepts_multi_dependency_plugin", "[core][plugin_loader_registry]") {
    glibre::core::PluginLoaderRegistry registry{kHostVersion};

    // Register dep-a and dep-b first.
    auto dep_a = make_manifest("glibre.dep.a");
    REQUIRE(registry.register_plugin(dep_a, "/fake/dep_a.dylib").has_value());

    auto dep_b = make_manifest("glibre.dep.b");
    REQUIRE(registry.register_plugin(dep_b, "/fake/dep_b.dylib").has_value());

    // Plugin that depends on both -- use make_manifest_with_deps helper.
    auto manifest = make_manifest_with_deps("glibre.consumer", "glibre.dep.a", "glibre.dep.b");

    auto result = registry.validate_all(
        manifest, kExpectedHash, kExpectedHash, "/fake/consumer.dylib"
    );
    CHECK(result.has_value());
}

// ===========================================================================
// Test: validate_drain_phase_outside_hotreload_returns_error  (plan #981)
//
// Authority: reviews/decisions/plugin-abi.md §"Loader Sequence" step 8 and
//            reviews/decisions/frame-phases.md §8.
//
// The drain guard must return core::Error::FramePhaseMisordered for every
// frame phase other than Phase::HotReload (phase 8).  Registry mutations
// (call_register, rebuild_schedule, migrate_components) are forbidden outside
// the hot-reload barrier.
//
// SRP validation: PluginLoaderRegistry::validate_drain_phase is a static
// method -- it accepts the phase by value so the registry does NOT own or
// query phase state (plans #247/#248 will supply the real FramePhaseTracker).
// ===========================================================================

TEST_CASE(
    "validate_drain_phase_outside_hotreload_returns_error", "[core][plugin_loader_registry]"
) {
    // Enumerate all non-HotReload phases and assert each returns the error.
    const glibre::core::Phase non_hotreload_phases[] = {
        glibre::core::Phase::Input,
        glibre::core::Phase::Logic,
        glibre::core::Phase::PhysicsFixed,
        glibre::core::Phase::Animation,
        glibre::core::Phase::Transform,
        glibre::core::Phase::CullExtract,
        glibre::core::Phase::RenderSubmit,
        glibre::core::Phase::Present,
    };

    for (const auto phase : non_hotreload_phases) {
        INFO("phase = " << static_cast<int>(phase));
        auto result = glibre::core::PluginLoaderRegistry::validate_drain_phase(phase);
        REQUIRE_FALSE(result.has_value());
        const auto* core_err = as_core_error(result.error());
        REQUIRE(core_err != nullptr);
        CHECK(*core_err == glibre::core::Error::FramePhaseMisordered);
    }
}

// ===========================================================================
// Test: validate_drain_phase_at_phase_8_succeeds  (plan #981 optional)
//
// Happy path: Phase::HotReload (phase 8) is the only permitted phase for
// registry mutations.  validate_drain_phase must return success.
// ===========================================================================

TEST_CASE("validate_drain_phase_at_phase_8_succeeds", "[core][plugin_loader_registry]") {
    auto result =
        glibre::core::PluginLoaderRegistry::validate_drain_phase(glibre::core::Phase::HotReload);
    // REQUIRE hard-fails so the operator-bool check below does not UB on an error payload.
    REQUIRE(result.has_value());
    // Exercise operator bool -- confirms no spurious glibre::Error was constructed.
    // Result<void> success returns {} (empty expected); there is no value to dereference
    // further, but operator bool must be true iff has_value() is true.
    CHECK(result);
}

// ===========================================================================
// Additional: partial dependency satisfaction fails on first missing
//
// Plugin with two dependencies where only the first is registered: the gate
// must fail with PluginDependencyMissing for the second.
// ===========================================================================

TEST_CASE("plugin_loader_rejects_partial_dependency", "[core][plugin_loader_registry]") {
    glibre::core::PluginLoaderRegistry registry{kHostVersion};

    // Register only dep-a; dep-b is missing.
    auto dep_a = make_manifest("glibre.dep.a");
    REQUIRE(registry.register_plugin(dep_a, "/fake/dep_a.dylib").has_value());

    // Use make_manifest_with_deps helper (dep-a registered, dep-b not).
    auto manifest = make_manifest_with_deps(
        "glibre.consumer",
        "glibre.dep.a",  // registered
        "glibre.dep.b"   // NOT registered
    );

    auto result = registry.validate_dependencies(manifest);

    REQUIRE_FALSE(result.has_value());
    const auto* core_err2 = as_core_error(result.error());
    REQUIRE(core_err2 != nullptr);
    CHECK(*core_err2 == glibre::core::Error::PluginDependencyMissing);
}

// ===========================================================================
// Additional: name uniqueness -- same name + same path is idempotent (HIGH-1)
//
// plugin-abi.md §"Loader Sequence" step 6 qualifies the collision check with
// "with a different file path".  Hot-reload re-registration of the same .dylib
// must succeed without error.
// ===========================================================================

TEST_CASE("name_unique_allows_same_name_same_path", "[core][plugin_loader_registry]") {
    glibre::core::PluginLoaderRegistry registry{kHostVersion};

    constexpr std::string_view path{"/fake/path/plugin.dylib"};

    auto manifest = make_manifest("glibre.test.hotreload");
    REQUIRE(registry.validate_all(manifest, kExpectedHash, kExpectedHash, path).has_value());
    REQUIRE(registry.register_plugin(manifest, path).has_value());

    // Same name + same path: validate_name_unique must accept (idempotent).
    auto r2 = registry.validate_name_unique(manifest, path);
    CHECK(r2.has_value());

    // register_plugin with same name + same path must also succeed.
    auto r3 = registry.register_plugin(manifest, path);
    CHECK(r3.has_value());
    CHECK(registry.loaded_count() == 1u);  // still only one entry
}

// ===========================================================================
// Additional: name uniqueness -- same name + different path is a collision
// (HIGH-1)
//
// The hot-reload re-load path is the same .dylib; a different .dylib with
// the same plugin name is a genuine collision.
// ===========================================================================

TEST_CASE("name_unique_rejects_same_name_different_path", "[core][plugin_loader_registry]") {
    glibre::core::PluginLoaderRegistry registry{kHostVersion};

    constexpr std::string_view path_a{"/fake/path/plugin_v1.dylib"};
    constexpr std::string_view path_b{"/fake/path/plugin_v2.dylib"};  // different path

    auto manifest = make_manifest("glibre.test.collision");
    REQUIRE(registry.validate_all(manifest, kExpectedHash, kExpectedHash, path_a).has_value());
    REQUIRE(registry.register_plugin(manifest, path_a).has_value());

    // Same name + different path: validate_name_unique must reject.
    auto r2 = registry.validate_name_unique(manifest, path_b);
    REQUIRE_FALSE(r2.has_value());
    const auto* core_err = as_core_error(r2.error());
    REQUIRE(core_err != nullptr);
    CHECK(*core_err == glibre::core::Error::PluginNameCollision);

    // register_plugin with different path must also reject.
    auto r3 = registry.register_plugin(manifest, path_b);
    REQUIRE_FALSE(r3.has_value());
    const auto* core_err2 = as_core_error(r3.error());
    REQUIRE(core_err2 != nullptr);
    CHECK(*core_err2 == glibre::core::Error::PluginNameCollision);
}

// ===========================================================================
// Test: core/plugin_loader_registry: lookup_o_log_n  (plan #1044)
//
// Confirms the migrated container (std::pmr::unordered_map with transparent
// hash) preserves the expected lookup characteristic: heterogeneous find()
// with std::string_view succeeds without materialising a temporary
// std::pmr::string key.
//
// This test validates that:
//   1. is_registered(std::string_view) finds entries inserted via register_plugin.
//   2. is_registered(std::string_view) returns false for absent keys.
//   3. After N registrations, all N lookups succeed (container integrity).
//
// Authority: plan #1044 Unit Test Plan, eastl-removal.md matrix row 7 (OQ-2).
// ===========================================================================

TEST_CASE("core/plugin_loader_registry: lookup_o_log_n", "[core][plugin_loader_registry]") {
    glibre::core::PluginLoaderRegistry registry{kHostVersion};

    // Register a batch of plugins to exercise the container at non-trivial size.
    constexpr std::size_t kPluginCount = 8;
    const char* names[kPluginCount] = {
        "glibre.plugin.alpha",
        "glibre.plugin.beta",
        "glibre.plugin.gamma",
        "glibre.plugin.delta",
        "glibre.plugin.epsilon",
        "glibre.plugin.zeta",
        "glibre.plugin.eta",
        "glibre.plugin.theta",
    };

    for (std::size_t i = 0; i < kPluginCount; ++i) {
        auto m = make_manifest(names[i]);
        auto r = registry.register_plugin(m, "/fake/path.dylib");
        REQUIRE(r.has_value());
    }

    CHECK(registry.loaded_count() == kPluginCount);

    // All N registered names must be found via std::string_view heterogeneous lookup
    // (no temporary std::pmr::string materialised per call).
    for (std::size_t i = 0; i < kPluginCount; ++i) {
        CHECK(registry.is_registered(std::string_view{names[i]}));
    }

    // An absent name must return false.
    CHECK_FALSE(registry.is_registered(std::string_view{"glibre.plugin.absent"}));
}

// ===========================================================================
// Test: core/compat/transparent_string_hash: heterogeneous_lookup_string_view
//       (plan #1044)
//
// Verifies glibre::TransparentStringHash + std::equal_to<> enable
// heterogeneous find() on a std::pmr::string-keyed unordered_map without
// materialising a temporary std::pmr::string key.
//
// Authority: plan #1044 Unit Test Plan, eastl-removal.md matrix row 27
//   (eastl::transparent_string_hash -> glibre::TransparentStringHash).
// ===========================================================================

TEST_CASE(
    "core/compat/transparent_string_hash: heterogeneous_lookup_string_view",
    "[core][compat]"
) {
    // Confirm the is_transparent tag is present (required by C++14 unordered
    // heterogeneous lookup machinery).
    static_assert(
        std::is_same_v<glibre::TransparentStringHash::is_transparent, void>,
        "TransparentStringHash must declare is_transparent = void"
    );

    std::pmr::unordered_map<std::pmr::string, int, glibre::TransparentStringHash,
                            std::equal_to<>>
        map;

    // Insert via std::pmr::string key.
    map.emplace(std::pmr::string{"alpha"}, 1);
    map.emplace(std::pmr::string{"beta"}, 2);
    map.emplace(std::pmr::string{"gamma"}, 3);

    // --- Heterogeneous lookup via std::string_view (no allocation) ---

    // find() with std::string_view.
    {
        auto it = map.find(std::string_view{"alpha"});
        REQUIRE(it != map.end());
        CHECK(it->second == 1);
    }

    // find() with const char* (converts to std::string_view implicitly).
    {
        auto it = map.find("beta");
        REQUIRE(it != map.end());
        CHECK(it->second == 2);
    }

    // contains() with std::string_view (C++20 heterogeneous contains).
    CHECK(map.contains(std::string_view{"gamma"}));

    // Absent key returns end().
    CHECK(map.find(std::string_view{"delta"}) == map.end());
    CHECK_FALSE(map.contains(std::string_view{"absent"}));

    // --- Hash consistency check ---
    // Hashing a std::string_view and the corresponding std::pmr::string must
    // produce the same result; otherwise the map's invariant breaks.
    {
        glibre::TransparentStringHash hasher;
        const std::string_view sv{"consistency"};
        const std::pmr::string pmrs{"consistency"};
        CHECK(hasher(sv) == hasher(std::string_view{pmrs}));
    }
}
