// tests/core/plugin_loader_integration/plugin_loader_integration_test.cpp
//
// Integration tests for PluginLoader + PluginLoaderRegistry end-to-end.
//
// These tests exercise real .dylib files through the full loader pipeline:
//   PluginLoader::open()  (steps 1–3: dlopen, dlsym, sidecar manifest read)
//   PluginLoaderRegistry::validate_all()  (steps 4–7: hash, version, name, deps)
// and assert the exact core::Error arm mandated by plugin-abi.md §"Failure
// Modes → core::Error".
//
// Catches regressions where a unit test mocks something the integration would
// have caught (e.g. a correct symbol count that masks a wrong symbol value).
//
// Named test cases (plan #232 Unit Test Plan + DoD):
//   - integration_dlopen_failure_propagates
//   - integration_missing_symbol_propagates
//   - integration_abi_hash_mismatch_propagates
//   - integration_name_collision_propagates
//   - integration_happy_path_loads_and_validates
//
// Compile-time path macros (injected by CMakeLists.txt):
//   GLIBRE_NOOP_DYLIB_PATH            — glibre-plugin-noop.dylib (noop reference)
//   GLIBRE_STUB_NO_SYMBOLS_DYLIB_PATH — stub with NO required symbols
//   GLIBRE_STUB_WRONG_ABI_DYLIB_PATH  — stub with WRONG ABI hash symbol
//
// Design constraints:
//   • -fno-exceptions (error-model.md §Decision 3).
//   • EASTL for containers and string_view (PHILOSOPHY §11).
//   • Each test owns its own PluginLoaderRegistry (not a singleton).
//   • Tests that require a dylib path skip gracefully when the path macro is
//     not defined.
//
// Authority: reviews/decisions/plugin-abi.md §"Loader Sequence" steps 1–7,
//            §"Failure Modes → core::Error" table.
//
// Plan: #232 — plugin loader integration — full failure-mode coverage.

#include <cstddef>
#include <cstdint>

#include <EASTL/string_view.h>
#include <catch2/catch_test_macros.hpp>
#include <glibre/core/plugin_loader.hpp>
#include <glibre/core/plugin_loader_registry.hpp>
#include <glibre/core/plugin_manifest.hpp>
#include <glibre/error.hpp>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

// Host engine version used in happy-path and version-gate tests.
constexpr glibre::core::SemVer kHostVersion{1, 0, 0};

// Expected ABI hash for gate tests.  The noop plugin exports an all-zero hash;
// the wrong-hash stub exports all-f.  The unit tests check that the gate fires
// when either mismatches — we provide a sentinel that differs from both.
//
// For the happy-path test we construct a manifest with abi_hash equal to
// the noop plugin's exported value ("0000...0000") and use that same value
// as expected_abi_hash so the gate passes.
constexpr const char kNoopAbiHash[] =
    "0000000000000000000000000000000000000000000000000000000000000000";

// Sentinel expected hash that differs from the wrong-hash stub's all-f value.
// Used in integration_abi_hash_mismatch_propagates.
constexpr const char kRealExpectedHash[] =
    "aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899";

/// Return the core::Error variant arm, or nullptr if the error is a different
/// context (render::Error, tools::Error, etc.).
[[nodiscard]] const glibre::core::Error* as_core_error(const glibre::Error& err) noexcept {
    return eastl::get_if<glibre::core::Error>(&err.code());
}

/// Build a minimal valid PluginManifest that passes all registry gates.
///
/// Defaults produce a manifest that will pass all four gates against a registry
/// with host_version = kHostVersion and expected_abi_hash = kNoopAbiHash.
glibre::core::PluginManifest make_manifest(
    const char* name = "glibre.integration.noop",
    const char* abi_hash = kNoopAbiHash,
    glibre::core::SemVer version = {1, 0, 0},
    glibre::core::SemVer min_engine = {0, 1, 0}
) {
    glibre::core::PluginManifest m;
    m.name = eastl::string{name};
    m.abi_hash = eastl::string{abi_hash};
    m.version = version;
    m.min_engine_version = min_engine;
    // depends_on is empty by default.
    return m;
}

}  // namespace

// ===========================================================================
// Test: integration_dlopen_failure_propagates
//
// Exercises loader step 1 (plugin-abi.md §"Loader Sequence"):
//   dlopen a path that does not exist on disk → PluginDlopenFailed.
//
// No mocking: PluginLoader::open() calls the real OS dlopen() and the real
// dlerror().  The error propagates from dlopen() through the loader to the
// caller unchanged.
//
// Refs: plugin-abi.md §"Failure Modes → core::Error" step 1.
// DoD: unit_test_named: integration_dlopen_failure_propagates
// ===========================================================================

TEST_CASE("integration_dlopen_failure_propagates", "[core][integration]") {
    const eastl::string_view nonexistent{"/tmp/glibre-integration-nonexistent-plugin.dylib"};

    auto result = glibre::core::PluginLoader::open(nonexistent);

    REQUIRE_FALSE(result.has_value());

    const auto* core_err = as_core_error(result.error());
    REQUIRE(core_err != nullptr);
    CHECK(*core_err == glibre::core::Error::PluginDlopenFailed);
}

// ===========================================================================
// Test: integration_missing_symbol_propagates
//
// Exercises loader step 2 (plugin-abi.md §"Loader Sequence"):
//   dlopen a valid .dylib that exports NONE of the four required symbols.
//   PluginLoader::open() must return PluginMissingEntryPoint and must
//   NOT leak the dlopen handle.
//
// No mocking: uses a real stub dylib built at configure time from
// tests/core/plugin_loader/stub_no_symbols.cpp (reused from plan #229).
// The stub exports one hidden symbol so the dylib is non-empty but the four
// required symbols are absent.
//
// Refs: plugin-abi.md §"Failure Modes → core::Error" step 2.
// DoD: unit_test_named: integration_missing_symbol_propagates
// ===========================================================================

TEST_CASE("integration_missing_symbol_propagates", "[core][integration]") {
#ifndef GLIBRE_STUB_NO_SYMBOLS_DYLIB_PATH
    SKIP("GLIBRE_STUB_NO_SYMBOLS_DYLIB_PATH not defined; rebuild with full test suite");
#else
    const eastl::string_view stub_path{GLIBRE_STUB_NO_SYMBOLS_DYLIB_PATH};
    REQUIRE_FALSE(stub_path.empty());

    auto result = glibre::core::PluginLoader::open(stub_path);

    REQUIRE_FALSE(result.has_value());

    const auto* core_err = as_core_error(result.error());
    REQUIRE(core_err != nullptr);
    CHECK(*core_err == glibre::core::Error::PluginMissingEntryPoint);
#endif
}

// ===========================================================================
// Test: integration_abi_hash_mismatch_propagates
//
// Exercises loader step 4 (plugin-abi.md §"Loader Sequence"):
//   Load a real dylib whose exported glibre_plugin_abi_hash value ("ffff...f")
//   does not match the host's expected ABI hash (kRealExpectedHash).
//
// Steps exercised end-to-end:
//   1. PluginLoader::open(stub_wrong_abi_path) — succeeds (all four symbols
//      are present; the stub is a valid dylib).
//   2. PluginLoaderRegistry::validate_symbol_abi_hash(loader.abi_hash(),
//      kRealExpectedHash) — fails → PluginAbiHashMismatch.
//
// Also verifies the validate_all() path using a constructed manifest whose
// abi_hash matches the stub's exported value ("ffff...f") — step 4a
// (manifest hash mismatch against kRealExpectedHash) fires.
//
// Refs: plugin-abi.md §"Failure Modes → core::Error" step 4.
// DoD: unit_test_named: integration_abi_hash_mismatch_propagates
// ===========================================================================

TEST_CASE("integration_abi_hash_mismatch_propagates", "[core][integration]") {
#ifndef GLIBRE_STUB_WRONG_ABI_DYLIB_PATH
    SKIP("GLIBRE_STUB_WRONG_ABI_DYLIB_PATH not defined; rebuild with full test suite");
#else
    const eastl::string_view stub_path{GLIBRE_STUB_WRONG_ABI_DYLIB_PATH};
    REQUIRE_FALSE(stub_path.empty());

    // Step 1-2: dlopen + dlsym must succeed — the stub exports all four symbols.
    auto loader_result = glibre::core::PluginLoader::open(stub_path);
    REQUIRE(loader_result.has_value());

    const glibre::core::PluginLoader& loader = *loader_result;

    // Verify that the loaded stub's ABI hash is the wrong sentinel ("ffff...").
    // This confirms we loaded the stub and not some other dylib.
    REQUIRE(loader.abi_hash() != nullptr);
    const eastl::string_view symbol_hash{loader.abi_hash()};
    CHECK(symbol_hash == eastl::string_view{
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
    });

    // Step 4 (symbol-side gate): validate_symbol_abi_hash must reject the stub.
    glibre::core::PluginLoaderRegistry registry{kHostVersion};

    SECTION("validate_symbol_abi_hash rejects wrong hash") {
        auto result = registry.validate_symbol_abi_hash(
            symbol_hash,
            eastl::string_view{kRealExpectedHash}
        );
        REQUIRE_FALSE(result.has_value());
        const auto* core_err = as_core_error(result.error());
        REQUIRE(core_err != nullptr);
        CHECK(*core_err == glibre::core::Error::PluginAbiHashMismatch);
    }

    SECTION("validate_all rejects wrong manifest abi_hash") {
        // Build a manifest whose abi_hash matches the stub's exported wrong value.
        // validate_all gate-1a (manifest hash check) fires before gate-1b.
        auto manifest = make_manifest(
            "glibre.integration.wrong_abi",
            "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
        );

        auto result = registry.validate_all(
            manifest,
            eastl::string_view{kRealExpectedHash},  // expected
            symbol_hash,                              // symbol value (also wrong)
            stub_path
        );
        REQUIRE_FALSE(result.has_value());
        const auto* core_err = as_core_error(result.error());
        REQUIRE(core_err != nullptr);
        CHECK(*core_err == glibre::core::Error::PluginAbiHashMismatch);
    }
#endif
}

// ===========================================================================
// Test: integration_name_collision_propagates
//
// Exercises loader step 6 (plugin-abi.md §"Loader Sequence"):
//   Register one plugin with name "glibre.integration.collision", then
//   attempt to validate a second plugin file with the same manifest name
//   but a different file path → PluginNameCollision.
//
// Steps exercised end-to-end:
//   1. PluginLoader::open(noop_path) — loads the noop plugin.
//   2. registry.register_plugin(manifest_a, noop_path) — first registration.
//   3. registry.validate_all(manifest_b, ..., different_path) — second load
//      with same name but different path → PluginNameCollision.
//
// The noop plugin is used as the real dylib for step 1; the second plugin
// is represented by a synthetic PluginManifest with the same name but a
// different (fabricated) path.
//
// Refs: plugin-abi.md §"Failure Modes → core::Error" step 6.
// DoD: unit_test_named: integration_name_collision_propagates
// ===========================================================================

TEST_CASE("integration_name_collision_propagates", "[core][integration]") {
#ifndef GLIBRE_NOOP_DYLIB_PATH
    SKIP("GLIBRE_NOOP_DYLIB_PATH not defined; build with GLIBRE_BUILD_EXAMPLES=ON");
#else
    const eastl::string_view noop_path{GLIBRE_NOOP_DYLIB_PATH};
    REQUIRE_FALSE(noop_path.empty());

    glibre::core::PluginLoaderRegistry registry{kHostVersion};

    // Step 1: load the noop plugin via PluginLoader::open().
    auto loader_result = glibre::core::PluginLoader::open(noop_path);
    REQUIRE(loader_result.has_value());

    // Step 2: build a synthetic manifest for the noop plugin and register it.
    // The noop plugin's abi_hash symbol is all-zeros; we use kNoopAbiHash as
    // both the expected hash and the manifest hash so that gate-1 passes.
    auto manifest_a = make_manifest("glibre.integration.collision");

    // Validate then register the first plugin (must succeed).
    {
        auto vr = registry.validate_all(
            manifest_a,
            eastl::string_view{kNoopAbiHash},  // expected = noop's hash
            eastl::string_view{kNoopAbiHash},  // symbol   = noop's hash
            noop_path
        );
        REQUIRE(vr.has_value());
    }
    REQUIRE(registry.register_plugin(manifest_a, noop_path).has_value());
    CHECK(registry.loaded_count() == 1u);

    // Step 3: attempt to load a second plugin with the SAME manifest name but
    // a different file path.  Gate-3 (name uniqueness) must fire.
    const eastl::string_view other_path{"/tmp/glibre-integration-other-collision.dylib"};
    auto manifest_b = make_manifest("glibre.integration.collision");  // same name

    auto result = registry.validate_all(
        manifest_b,
        eastl::string_view{kNoopAbiHash},
        eastl::string_view{kNoopAbiHash},
        other_path  // different path → collision
    );

    REQUIRE_FALSE(result.has_value());
    const auto* core_err = as_core_error(result.error());
    REQUIRE(core_err != nullptr);
    CHECK(*core_err == glibre::core::Error::PluginNameCollision);
#endif
}

// ===========================================================================
// Test: integration_happy_path_loads_and_validates
//
// Exercises loader steps 1–4 and 6 along the success path:
//   1. PluginLoader::open(noop_path) → success.
//   2. abi_hash(), register_fn() accessors return valid values.
//   3. PluginLoaderRegistry::validate_all() with matching expected_abi_hash,
//      correct engine version, no prior registrations, no deps →
//      has_value() == true.
//   4. register_plugin() succeeds; loaded_count() == 1.
//
// This test catches regressions where unit-test mocking masks an integration
// failure (e.g. dlsym resolving the right symbol name but to a wrong type,
// or the registry gate logic depending on state that mocks pre-seed).
//
// Refs: plugin-abi.md §"Loader Sequence" steps 1–7 (success path).
// DoD: unit_test_named: integration_happy_path_loads_and_validates
// ===========================================================================

TEST_CASE("integration_happy_path_loads_and_validates", "[core][integration]") {
#ifndef GLIBRE_NOOP_DYLIB_PATH
    SKIP("GLIBRE_NOOP_DYLIB_PATH not defined; build with GLIBRE_BUILD_EXAMPLES=ON");
#else
    const eastl::string_view noop_path{GLIBRE_NOOP_DYLIB_PATH};
    REQUIRE_FALSE(noop_path.empty());

    // Step 1–2: load the noop plugin via the real OS dlopen/dlsym path.
    auto loader_result = glibre::core::PluginLoader::open(noop_path);

    REQUIRE(loader_result.has_value());
    const glibre::core::PluginLoader& loader = *loader_result;

    // Verify the four required symbol accessors are populated.
    REQUIRE(loader.abi_hash() != nullptr);
    CHECK(loader.register_fn() != nullptr);

    // The noop plugin exports glibre_plugin_manifest_size = 0 (no blob in MVP).
    CHECK(loader.manifest_blob_size() == 0u);

    // dylib_path round-trips the input.
    CHECK(loader.dylib_path() == eastl::string{noop_path.data(), noop_path.size()});

    // Step 3 (registry): construct a manifest whose abi_hash matches the noop
    // plugin's exported value.  Since the noop exports all-zeros, we use
    // kNoopAbiHash for both the manifest field and the expected_abi_hash arg.
    glibre::core::PluginLoaderRegistry registry{kHostVersion};

    auto manifest = make_manifest("glibre.integration.happy");

    // Gate 1a: manifest.abi_hash == expected → pass.
    // Gate 1b: symbol value    == expected → pass.
    // Gate 2:  min_engine_version {0,1,0} ≤ host {1,0,0} → pass.
    // Gate 3:  "glibre.integration.happy" not yet registered → pass.
    // Gate 4:  depends_on is empty → pass.
    auto validate_result = registry.validate_all(
        manifest,
        eastl::string_view{kNoopAbiHash},                         // expected hash
        eastl::string_view{loader.abi_hash()},                    // symbol hash
        noop_path
    );

    REQUIRE(validate_result.has_value());

    // register_plugin records the plugin in the table.
    REQUIRE(registry.register_plugin(manifest, noop_path).has_value());

    CHECK(registry.loaded_count() == 1u);
    CHECK(registry.is_registered(eastl::string_view{"glibre.integration.happy"}));
#endif
}
