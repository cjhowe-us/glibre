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
// Named test cases (plan #968 Unit Test Plan + DoD):
//   - integration_manifest_invalid_propagates
//   - integration_engine_too_old_propagates
//   - integration_dependency_missing_propagates
//
// Compile-time path macros (injected by CMakeLists.txt):
//   GLIBRE_NOOP_DYLIB_PATH                    — glibre-plugin-noop.dylib
//   GLIBRE_STUB_NO_SYMBOLS_DYLIB_PATH         — stub with NO required symbols
//   GLIBRE_STUB_WRONG_ABI_DYLIB_PATH          — stub with WRONG ABI hash symbol
//   GLIBRE_STUB_INVALID_MANIFEST_DYLIB_PATH   — stub with garbage manifest blob
//
// Build contract: GLIBRE_NOOP_DYLIB_PATH and GLIBRE_STUB_NO_SYMBOLS_DYLIB_PATH
// are defined by CMakeLists.txt only when the corresponding targets exist
// (i.e. GLIBRE_BUILD_EXAMPLES=ON, plan #229 plugin_loader subdir present).
// If a future build omits either target the dependent test reports FAIL rather
// than SKIP so the DoD-vs-execution divergence is visible in CI output
// rather than silently masked.  This is intentional: these tests are part of
// the DoD for #232 and must execute — a SKIP means coverage is missing.
//
// Design constraints:
//   • -fno-exceptions (error-model.md §Decision 3).
//   • PluginLoaderRegistry API uses std::string_view (plan #1044 migration).
//   • PluginLoader::open() still uses eastl::string_view (migrated separately).
//   • Each test owns its own PluginLoaderRegistry (not a singleton).
//
// Coverage vs. plugin-abi.md §"Loader Sequence":
//   Loader steps covered by plan #232: 1, 2, 4, 6 (failure), 1–4 + 6–7 (success).
//   Loader steps covered by plan #968: 3 (manifest_invalid), 5 (engine_too_old),
//     7 (dependency_missing — non-vacuous).
//   Steps 8, 9, 10, 11: depend on plan #231 (register + migrate); deferred.
//
// Authority: reviews/decisions/plugin-abi.md §"Loader Sequence" steps 1–7,
//            §"Failure Modes → core::Error" table.
//
// Plan: #232 — plugin loader integration — full failure-mode coverage.
// Plan: #968 — plugin loader integration — manifest_invalid / engine_too_old /
//              dependency_missing failure modes.

#include <cstddef>
#include <cstdint>
#include <string_view>

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
    return std::get_if<glibre::core::Error>(&err.code());
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
    m.name = name;          // std::pmr::string from const char*
    m.abi_hash = abi_hash;  // std::pmr::string from const char*
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
    // FAIL rather than SKIP: this test is part of the DoD for #232.
    // A missing macro means the CMake integration is incomplete — that is a
    // build configuration error, not a valid skip condition.
    // Rebuild with the tests/core/plugin_loader subdir in scope (plan #229).
    FAIL(
        "GLIBRE_STUB_NO_SYMBOLS_DYLIB_PATH not defined — "
        "rebuild with tests/core/plugin_loader subdir (plan #229 target required)"
    );
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
    // GLIBRE_STUB_WRONG_ABI_DYLIB_PATH is injected unconditionally by
    // CMakeLists.txt (not conditional on any target guard).  Reaching this
    // branch would indicate a CMake misconfiguration — fail loudly.
    FAIL(
        "GLIBRE_STUB_WRONG_ABI_DYLIB_PATH not defined — "
        "this macro is unconditional; check CMakeLists.txt for glibre-plugin-stub-wrong-abi"
    );
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
    // Keep eastl::string_view for EASTL-side comparisons (loader.abi_hash() returns const char*).
    const eastl::string_view symbol_hash_eastl{loader.abi_hash()};
    CHECK(
        symbol_hash_eastl ==
        eastl::string_view{"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"}
    );

    // Step 4 (symbol-side gate): validate_symbol_abi_hash must reject the stub.
    // PluginLoaderRegistry API now takes std::string_view (plan #1044).
    glibre::core::PluginLoaderRegistry registry{kHostVersion};

    SECTION("validate_symbol_abi_hash rejects wrong hash") {
        auto result = registry.validate_symbol_abi_hash(
            std::string_view{loader.abi_hash()},  // symbol hash via std::string_view
            kRealExpectedHash                      // const char* -> std::string_view
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
            kRealExpectedHash,                    // expected (const char* -> std::string_view)
            std::string_view{loader.abi_hash()},  // symbol value (also wrong)
            std::string_view{stub_path.data(), stub_path.size()}  // eastl -> std::string_view
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
    // FAIL rather than SKIP: this test is part of the DoD for #232.
    // A missing macro means the noop plugin was not built (GLIBRE_BUILD_EXAMPLES=OFF
    // or examples/plugins/ subdir not included).  That is a build configuration
    // error for this test suite — fail loudly so CI surfaces it.
    FAIL(
        "GLIBRE_NOOP_DYLIB_PATH not defined — "
        "rebuild with GLIBRE_BUILD_EXAMPLES=ON (noop plugin required for #232 DoD)"
    );
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
            kNoopAbiHash,  // expected = noop's hash (const char* → std::string_view)
            kNoopAbiHash,  // symbol   = noop's hash (const char* → std::string_view)
            std::string_view{noop_path.data(), noop_path.size()}  // eastl → std::string_view
        );
        REQUIRE(vr.has_value());
    }
    REQUIRE(
        registry.register_plugin(manifest_a, std::string_view{noop_path.data(), noop_path.size()})
            .has_value()
    );
    CHECK(registry.loaded_count() == 1u);

    // Step 3: attempt to load a second plugin with the SAME manifest name but
    // a different file path.  Gate-3 (name uniqueness) must fire.
    constexpr std::string_view other_path{"/tmp/glibre-integration-other-collision.dylib"};
    auto manifest_b = make_manifest("glibre.integration.collision");  // same name

    auto result = registry.validate_all(
        manifest_b,
        kNoopAbiHash,  // expected (const char* → std::string_view)
        kNoopAbiHash,  // symbol value (const char* → std::string_view)
        other_path     // different path → collision
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
// Exercises loader steps 1–4 and 6 along the success path, plus step 7
// vacuously (depends_on is empty — no deps to resolve):
//   1. PluginLoader::open(noop_path) → success (dlopen + dlsym).
//   2. abi_hash(), register_fn() accessors return valid values.
//   3. PluginLoaderRegistry::validate_all() with matching expected_abi_hash,
//      correct engine version, no prior registrations, no deps →
//      has_value() == true.
//   4. register_plugin() succeeds; loaded_count() == 1.
//
// Steps NOT exercised here:
//   • Step 3 (manifest deserialization) — covered by plan #968:
//       integration_manifest_invalid_propagates.
//   • Step 5 (engine_too_old check)     — covered by plan #968:
//       integration_engine_too_old_propagates.
//   • Step 7 non-vacuously              — covered by plan #968:
//       integration_dependency_missing_propagates.
//   • Steps 8 (drain), 9 (register()) — await plan #231 (register + migrate).
//
// This test catches regressions where unit-test mocking masks an integration
// failure (e.g. dlsym resolving the right symbol name but to a wrong type,
// or the registry gate logic depending on state that mocks pre-seed).
//
// Refs: plugin-abi.md §"Loader Sequence" steps 1–4, 6–7 (success path).
// DoD: unit_test_named: integration_happy_path_loads_and_validates
// ===========================================================================

TEST_CASE("integration_happy_path_loads_and_validates", "[core][integration]") {
#ifndef GLIBRE_NOOP_DYLIB_PATH
    // FAIL rather than SKIP: this test is part of the DoD for #232.
    // See integration_name_collision_propagates for the same rationale.
    FAIL(
        "GLIBRE_NOOP_DYLIB_PATH not defined — "
        "rebuild with GLIBRE_BUILD_EXAMPLES=ON (noop plugin required for #232 DoD)"
    );
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
    // Gate 4:  depends_on is empty → pass (step 7 vacuously satisfied).
    auto validate_result = registry.validate_all(
        manifest,
        kNoopAbiHash,                                              // expected hash (const char*)
        std::string_view{loader.abi_hash()},                      // symbol hash (std::string_view)
        std::string_view{noop_path.data(), noop_path.size()}       // eastl → std::string_view
    );

    REQUIRE(validate_result.has_value());

    // register_plugin records the plugin in the table.
    REQUIRE(
        registry
            .register_plugin(manifest, std::string_view{noop_path.data(), noop_path.size()})
            .has_value()
    );

    CHECK(registry.loaded_count() == 1u);
    CHECK(registry.is_registered("glibre.integration.happy"));
#endif
}

// ===========================================================================
// Test: integration_manifest_invalid_propagates
//
// Exercises loader step 3 (plugin-abi.md §"Loader Sequence"):
//   PluginLoader::open() reads a sidecar <dylib>.manifest file via
//   PluginManifest::open().  If the sidecar file exists but cannot be
//   Fory-deserialized, the manifest_result_ holds PluginManifestInvalid.
//
// Steps exercised end-to-end:
//   1. PluginLoader::open(stub_path) → dlopen succeeds; all four symbols
//      resolve (glibre-plugin-stub-invalid-manifest exports them all).
//   2. Sidecar read: the .manifest file created at build time alongside the
//      stub dylib is a regular file but contains zero valid Fory bytes.
//      PluginManifest::open() returns PluginManifestInvalid (stub impl in
//      core/src/plugin_manifest.cpp).
//   3. loader.manifest_result() carries the PluginManifestInvalid error.
//
// No mocking: uses the real OS dlopen/dlsym path and the real sidecar read.
//
// Build contract: GLIBRE_STUB_INVALID_MANIFEST_DYLIB_PATH is injected
// unconditionally by CMakeLists.txt (not conditional on any target guard).
// Reaching the #ifndef branch indicates a CMake misconfiguration — fail loudly.
//
// Refs: plugin-abi.md §"Failure Modes → core::Error" step 3.
// DoD: unit_test_named: integration_manifest_invalid_propagates
// ===========================================================================

TEST_CASE("integration_manifest_invalid_propagates", "[core][integration]") {
#ifndef GLIBRE_STUB_INVALID_MANIFEST_DYLIB_PATH
    // GLIBRE_STUB_INVALID_MANIFEST_DYLIB_PATH is injected unconditionally by
    // CMakeLists.txt.  Reaching here indicates a CMake misconfiguration — fail
    // loudly so CI surfaces the gap rather than silently skipping DoD coverage.
    FAIL(
        "GLIBRE_STUB_INVALID_MANIFEST_DYLIB_PATH not defined — "
        "this macro is unconditional; check CMakeLists.txt for "
        "glibre-plugin-stub-invalid-manifest"
    );
#else
    const eastl::string_view stub_path{GLIBRE_STUB_INVALID_MANIFEST_DYLIB_PATH};
    REQUIRE_FALSE(stub_path.empty());

    // Steps 1–2: dlopen + dlsym must succeed — the stub exports all four symbols.
    auto loader_result = glibre::core::PluginLoader::open(stub_path);
    REQUIRE(loader_result.has_value());

    const glibre::core::PluginLoader& loader = *loader_result;

    // Verify that the stub's ABI hash symbol resolved (step 2 passed).
    REQUIRE(loader.abi_hash() != nullptr);

    // Step 3: the sidecar <stub>.manifest file exists but fails deserialization.
    // PluginManifest::open() must return PluginManifestInvalid.
    const glibre::Result<glibre::core::PluginManifest>& manifest_res = loader.manifest_result();

    // The manifest result must be an error.
    REQUIRE_FALSE(manifest_res.has_value());

    const auto* core_err = as_core_error(manifest_res.error());
    REQUIRE(core_err != nullptr);
    CHECK(*core_err == glibre::core::Error::PluginManifestInvalid);

    // Coverage note (refs #225): this test depends on PluginManifest::open()
    // returning PluginManifestInvalid for ANY regular file, which is the current
    // stub behaviour in core/src/plugin_manifest.cpp:41.  The empty sidecar
    // created by `cmake -E touch` satisfies the gate trivially — we are not yet
    // testing that garbage bytes fail Fory deserialization, only that the loader
    // propagates the PluginManifestInvalid error correctly.
    //
    // Once plan #225 lands real Fory decoders, this test must be refreshed:
    //   • write a non-empty garbage payload to the sidecar (or use the
    //     kGarbageBlob already embedded in stub_invalid_manifest.cpp), and
    //   • update the sidecar creation in CMakeLists.txt accordingly.
    // At that point the test will verify the actual deserialise-failure path.
#endif
}

// ===========================================================================
// Test: integration_engine_too_old_propagates
//
// Exercises loader step 5 (plugin-abi.md §"Loader Sequence"):
//   PluginLoaderRegistry::validate_engine_version() rejects a plugin whose
//   manifest.min_engine_version exceeds the host engine version.
//
// Steps exercised end-to-end:
//   1. PluginLoader::open(noop_path) → success (all four symbols present;
//      the noop plugin is a valid dylib).
//   2. Synthesise a PluginManifest with min_engine_version = {99, 0, 0},
//      which exceeds the test fixture's kHostVersion = {1, 0, 0}.
//   3. PluginLoaderRegistry::validate_all() runs gate 2 (engine version):
//      host {1,0,0} < min_engine_version {99,0,0} → PluginEngineTooOld.
//
// No new stub is needed: the noop plugin is reused for the dlopen/dlsym path;
// the version incompatibility is injected via the synthetic manifest.
//
// Build contract: GLIBRE_NOOP_DYLIB_PATH is injected conditionally (requires
// GLIBRE_BUILD_EXAMPLES=ON).  Fail loudly if missing — same rationale as
// integration_name_collision_propagates.
//
// Refs: plugin-abi.md §"Failure Modes → core::Error" step 5.
// DoD: unit_test_named: integration_engine_too_old_propagates
// ===========================================================================

TEST_CASE("integration_engine_too_old_propagates", "[core][integration]") {
#ifndef GLIBRE_NOOP_DYLIB_PATH
    // FAIL rather than SKIP: this test is part of the DoD for #968.
    // A missing macro means the noop plugin was not built; that is a build
    // configuration error for this test suite — fail loudly so CI surfaces it.
    FAIL(
        "GLIBRE_NOOP_DYLIB_PATH not defined — "
        "rebuild with GLIBRE_BUILD_EXAMPLES=ON (noop plugin required for #968 DoD)"
    );
#else
    const eastl::string_view noop_path{GLIBRE_NOOP_DYLIB_PATH};
    REQUIRE_FALSE(noop_path.empty());

    // Step 1–2: dlopen + dlsym must succeed — the noop plugin exports all four
    // required symbols.
    auto loader_result = glibre::core::PluginLoader::open(noop_path);
    REQUIRE(loader_result.has_value());

    // Anchor the dlopen: verify the ABI hash symbol resolved to the noop
    // sentinel.  Without this check the open() call is performative; this
    // assertion confirms dlsym reached the correct dylib before we feed the
    // synthetic manifest into the registry gates below.
    REQUIRE(loader_result->abi_hash() != nullptr);
    CHECK(eastl::string_view{loader_result->abi_hash()} == eastl::string_view{kNoopAbiHash});

    // Registry with host engine version {1, 0, 0}.
    glibre::core::PluginLoaderRegistry registry{kHostVersion};

    // Synthesise a manifest whose min_engine_version far exceeds the host.
    // min_engine_version {99, 0, 0} > host {1, 0, 0} → gate 2 fires.
    auto manifest = make_manifest(
        "glibre.integration.engine_too_old",
        kNoopAbiHash,
        /*version=*/{1, 0, 0},
        /*min_engine=*/{99, 0, 0}
    );

    // Step 5 (gate 2): validate_all must fail at the engine-version gate.
    // Gates 1a and 1b pass because the manifest and symbol both carry kNoopAbiHash.
    auto result = registry.validate_all(
        manifest,
        kNoopAbiHash,                                         // expected (const char*)
        kNoopAbiHash,                                         // symbol value (const char*)
        std::string_view{noop_path.data(), noop_path.size()}  // eastl → std::string_view
    );

    REQUIRE_FALSE(result.has_value());
    const auto* core_err = as_core_error(result.error());
    REQUIRE(core_err != nullptr);
    CHECK(*core_err == glibre::core::Error::PluginEngineTooOld);
#endif
}

// ===========================================================================
// Test: integration_dependency_missing_propagates
//
// Exercises loader step 7 (plugin-abi.md §"Loader Sequence"):
//   PluginLoaderRegistry::validate_dependencies() refuses to load a plugin
//   whose depends_on lists a name that is not yet registered.
//
// Steps exercised end-to-end:
//   1. PluginLoader::open(noop_path) → success.
//   2. Construct an empty registry (no plugins registered).
//   3. Synthesise a PluginManifest for plugin B that depends_on a plugin
//      named "glibre.integration.missing_dep" — which has NOT been registered.
//   4. PluginLoaderRegistry::validate_all() runs gate 4 (dependency resolution):
//      "glibre.integration.missing_dep" is absent → PluginDependencyMissing.
//
// Distinction from the vacuous happy-path case: the happy-path test has
// depends_on empty, so gate 4 always passes.  This test adds one entry to
// depends_on that is genuinely unmet, exercising the non-vacuous code path.
//
// No new stub is needed: the noop plugin is reused; the dependency mismatch
// is injected via the synthetic manifest.
//
// Build contract: GLIBRE_NOOP_DYLIB_PATH is injected conditionally — fail
// loudly if missing (same rationale as integration_name_collision_propagates).
//
// Refs: plugin-abi.md §"Failure Modes → core::Error" step 7.
// DoD: unit_test_named: integration_dependency_missing_propagates
// ===========================================================================

TEST_CASE("integration_dependency_missing_propagates", "[core][integration]") {
#ifndef GLIBRE_NOOP_DYLIB_PATH
    // FAIL rather than SKIP: this test is part of the DoD for #968.
    FAIL(
        "GLIBRE_NOOP_DYLIB_PATH not defined — "
        "rebuild with GLIBRE_BUILD_EXAMPLES=ON (noop plugin required for #968 DoD)"
    );
#else
    const eastl::string_view noop_path{GLIBRE_NOOP_DYLIB_PATH};
    REQUIRE_FALSE(noop_path.empty());

    // Step 1–2: dlopen + dlsym must succeed.
    auto loader_result = glibre::core::PluginLoader::open(noop_path);
    REQUIRE(loader_result.has_value());

    // Anchor the dlopen: verify the ABI hash symbol resolved to the noop
    // sentinel.  Without this check the open() call is performative; this
    // assertion confirms dlsym reached the correct dylib before we feed the
    // synthetic manifest into the registry gates below.
    REQUIRE(loader_result->abi_hash() != nullptr);
    CHECK(eastl::string_view{loader_result->abi_hash()} == eastl::string_view{kNoopAbiHash});

    // Registry is empty — "glibre.integration.missing_dep" is not registered.
    glibre::core::PluginLoaderRegistry registry{kHostVersion};

    // Synthesise a manifest for plugin B that lists an unmet dependency.
    glibre::core::PluginManifest manifest = make_manifest("glibre.integration.dep_consumer");
    manifest.depends_on.push_back(
        "glibre.integration.missing_dep"
    );  // std::pmr::string from literal

    // Step 7 (gate 4): validate_all must fail at the dependency gate.
    // Gates 1a, 1b, 2, and 3 pass: abi hash matches, engine version is fine,
    // and "glibre.integration.dep_consumer" is not yet registered.
    auto result = registry.validate_all(
        manifest,
        kNoopAbiHash,                                         // expected (const char*)
        kNoopAbiHash,                                         // symbol value (const char*)
        std::string_view{noop_path.data(), noop_path.size()}  // eastl → std::string_view
    );

    REQUIRE_FALSE(result.has_value());
    const auto* core_err = as_core_error(result.error());
    REQUIRE(core_err != nullptr);
    CHECK(*core_err == glibre::core::Error::PluginDependencyMissing);
#endif
}
