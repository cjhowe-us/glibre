// tests/core/hot_reload/hot_reload_validate_test.cpp
//
// Catch2 unit tests for glibre::core::hot_reload_validate().
//
// Authority: core/include/glibre/core/plugin_loader_actions.hpp,
//            reviews/decisions/hot-reload-protocol.md §"Step 2 — Swap".
//
// Plan: #250 — feat(core): hot-reload plugin manifest + ABI hash gate.
//
// Named test cases (plan #250 Unit Test Plan + DoD):
//   - hot_reload_rejects_abi_hash_mismatch
//   - hot_reload_accepts_compatible_swap
//   - hot_reload_rejects_name_mismatch
//   - hot_reload_rejects_major_version_change
//
// Design constraints:
//   - -fno-exceptions (error-model.md §Decision 3).
//   - No REQUIRE_THROWS usage.
//   - No filesystem access — manifests are constructed in-memory.
//   - EASTL per PHILOSOPHY §11 (eastl::string for manifest fields).

#include <EASTL/string.h>
#include <catch2/catch_test_macros.hpp>

#include "glibre/core/plugin_loader_actions.hpp"
#include "glibre/core/plugin_manifest.hpp"
#include "glibre/error.hpp"

namespace {

// ---------------------------------------------------------------------------
// Fixture helpers — build minimal PluginManifest values for tests.
// ---------------------------------------------------------------------------

// A stable 64-char lowercase hex string used as a "known good" ABI hash.
// Represents a fictional blake3 digest; content is irrelevant for the check.
constexpr const char* kGoodHash =
    "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2";

// A different 64-char hex string representing a mismatched ABI hash.
constexpr const char* kBadHash = "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef";

/// Build a baseline PluginManifest with the given name, abi_hash, and version.
/// All other fields are defaulted (empty).
glibre::core::PluginManifest make_manifest(
    const char* name,
    const char* abi_hash,
    std::uint16_t major,
    std::uint16_t minor = 0,
    std::uint16_t patch = 0
) {
    glibre::core::PluginManifest m;
    m.name = name;          // std::pmr::string from const char*
    m.abi_hash = abi_hash;  // std::pmr::string from const char*
    m.version = glibre::core::SemVer{major, minor, patch};
    return m;
}

}  // anonymous namespace

// ===========================================================================
// Test: hot_reload_rejects_abi_hash_mismatch
//
// Verifies that hot_reload_validate() returns core::Error::PluginAbiHashMismatch
// when the incoming manifest's abi_hash differs from the outgoing manifest's
// abi_hash (hot-reload-protocol.md §"Step 2 — Swap" step 2.1).
//
// Both manifests carry the same name and same major version so that only the
// ABI hash check fires.
// ===========================================================================

TEST_CASE("hot_reload_rejects_abi_hash_mismatch", "[core][hot_reload]") {
    // Outgoing plugin: good hash.
    const auto outgoing = make_manifest("glibre.test", kGoodHash, /*major=*/1);

    // Incoming plugin: same name, same major, but different (bad) ABI hash.
    const auto incoming = make_manifest("glibre.test", kBadHash, /*major=*/1);

    const auto result = glibre::core::hot_reload_validate(outgoing, incoming);

    // Must fail with PluginAbiHashMismatch.
    REQUIRE_FALSE(result.has_value());

    const auto& err = result.error();
    const auto* code = std::get_if<glibre::core::Error>(&err.code());
    REQUIRE(code != nullptr);
    CHECK(*code == glibre::core::Error::PluginAbiHashMismatch);
}

// ===========================================================================
// Test: hot_reload_accepts_compatible_swap
//
// Verifies that hot_reload_validate() returns success when the incoming
// manifest is a compatible swap candidate:
//   - Same name as outgoing.
//   - Same ABI hash as outgoing.
//   - Same major version (minor/patch may advance).
//
// Exercises both the exact-version case (same minor/patch) and the
// minor-advance case (incoming minor > outgoing minor).
// ===========================================================================

TEST_CASE("hot_reload_accepts_compatible_swap", "[core][hot_reload]") {
    SECTION("identical version — same minor and patch") {
        const auto outgoing = make_manifest("glibre.render", kGoodHash, 1, 2, 3);
        const auto incoming = make_manifest("glibre.render", kGoodHash, 1, 2, 3);

        const auto result = glibre::core::hot_reload_validate(outgoing, incoming);
        CHECK(result.has_value());
    }

    SECTION("minor version advances — still compatible") {
        const auto outgoing = make_manifest("glibre.render", kGoodHash, 1, 0, 0);
        const auto incoming = make_manifest("glibre.render", kGoodHash, 1, 5, 0);

        const auto result = glibre::core::hot_reload_validate(outgoing, incoming);
        CHECK(result.has_value());
    }

    SECTION("patch version advances — still compatible") {
        const auto outgoing = make_manifest("glibre.physics", kGoodHash, 2, 1, 0);
        const auto incoming = make_manifest("glibre.physics", kGoodHash, 2, 1, 7);

        const auto result = glibre::core::hot_reload_validate(outgoing, incoming);
        CHECK(result.has_value());
    }
}

// ===========================================================================
// Test: hot_reload_rejects_name_mismatch
//
// Verifies that hot_reload_validate() returns core::Error::PluginNameMismatch
// when the incoming manifest's name differs from the outgoing manifest's name.
//
// Both manifests carry the same ABI hash and same version so that only the
// name check fires.  This guards against an operator accidentally supplying
// the wrong dylib as a swap candidate.
// ===========================================================================

TEST_CASE("hot_reload_rejects_name_mismatch", "[core][hot_reload]") {
    const auto outgoing = make_manifest("glibre.render", kGoodHash, 1);
    const auto incoming = make_manifest("glibre.physics", kGoodHash, 1);

    const auto result = glibre::core::hot_reload_validate(outgoing, incoming);

    // Must fail with PluginNameMismatch.
    REQUIRE_FALSE(result.has_value());

    const auto& err = result.error();
    const auto* code = std::get_if<glibre::core::Error>(&err.code());
    REQUIRE(code != nullptr);
    CHECK(*code == glibre::core::Error::PluginNameMismatch);
}

// ===========================================================================
// Test: hot_reload_rejects_major_version_change
//
// Verifies that hot_reload_validate() returns core::Error::HotReloadRefused
// when the incoming manifest's major version differs from the outgoing's
// (hot-reload-protocol.md §"Step 2 — Swap" step 2.2 compatible-swap rule).
//
// A major-version bump indicates a breaking redesign that cannot be hot-reloaded
// into a running world; it requires a fresh world (process restart).
// Both manifests carry the same name and same ABI hash so that only the
// major-version check fires.
// ===========================================================================

TEST_CASE("hot_reload_rejects_major_version_change", "[core][hot_reload]") {
    SECTION("major increases") {
        const auto outgoing = make_manifest("glibre.audio", kGoodHash, 1, 0, 0);
        const auto incoming = make_manifest("glibre.audio", kGoodHash, 2, 0, 0);

        const auto result = glibre::core::hot_reload_validate(outgoing, incoming);

        REQUIRE_FALSE(result.has_value());
        const auto& err = result.error();
        const auto* code = std::get_if<glibre::core::Error>(&err.code());
        REQUIRE(code != nullptr);
        CHECK(*code == glibre::core::Error::HotReloadRefused);
    }

    SECTION("major decreases — also refused") {
        const auto outgoing = make_manifest("glibre.audio", kGoodHash, 3, 0, 0);
        const auto incoming = make_manifest("glibre.audio", kGoodHash, 2, 0, 0);

        const auto result = glibre::core::hot_reload_validate(outgoing, incoming);

        REQUIRE_FALSE(result.has_value());
        const auto& err = result.error();
        const auto* code = std::get_if<glibre::core::Error>(&err.code());
        REQUIRE(code != nullptr);
        CHECK(*code == glibre::core::Error::HotReloadRefused);
    }
}
