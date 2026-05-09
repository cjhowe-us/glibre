// tests/core/plugin_loader/plugin_loader_test.cpp
//
// Catch2 unit tests for glibre::core::PluginLoader (plan #229).
//
// Named test cases (plan #229 Unit Test Plan, DoD):
//   - plugin_loader_open_loads_noop_plugin
//   - plugin_loader_open_returns_error_on_missing_path
//   - plugin_loader_open_returns_error_on_missing_symbols
//
// Additional coverage aligned with issue #229 Unit Test Plan:
//   - dlopen_failure_returns_plugin_dlopen_failed
//     (alias / equivalent to plugin_loader_open_returns_error_on_missing_path)
//   - missing_symbol_returns_plugin_missing_entry_point
//     (alias / equivalent to plugin_loader_open_returns_error_on_missing_symbols)
//
// Design constraints:
//   • -fno-exceptions (error-model.md §Decision 3).
//   • GLIBRE_NOOP_DYLIB_PATH — compile-time path to glibre-plugin-noop.dylib,
//     injected by CMakeLists.txt.
//   • GLIBRE_STUB_NO_SYMBOLS_DYLIB_PATH — path to the stub dylib that exports
//     no plugin symbols, injected by CMakeLists.txt.
//   • Tests that depend on a dylib path skip gracefully if the path macro is
//     not defined (e.g. when building without examples).

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <EASTL/string_view.h>
#include <catch2/catch_test_macros.hpp>
#include <glibre/core/plugin_loader.hpp>
#include <glibre/error.hpp>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/// Extract the core::Error from a glibre::Error by visiting the variant.
/// Returns true if the error holds the expected core::Error arm.
[[nodiscard]] bool holds_core_error(const glibre::Error& err, glibre::core::Error expected) {
    const auto& var = err.code();
    const auto* ptr = eastl::get_if<glibre::core::Error>(&var);
    return (ptr != nullptr) && (*ptr == expected);
}

}  // namespace

// ---------------------------------------------------------------------------
// Test: helpers_negative_holds_core_error
//
// (Covers LOW finding round-1: holds_core_error had no negative-arm test.)
//
// Verifies that holds_core_error returns false when the glibre::Error holds a
// non-core variant arm (tools::Error).  Without this check a future variant
// reordering that aliases discriminator indices could silently pass tests.
// ---------------------------------------------------------------------------

TEST_CASE("helpers_negative_holds_core_error", "[core][plugin_loader]") {
    // Construct an error from a different variant arm (tools::Error).
    const glibre::Error tools_err{glibre::tools::Error::ForycIOError};

    // holds_core_error must return false: the error is NOT a core::Error.
    CHECK_FALSE(holds_core_error(tools_err, glibre::core::Error::PluginDlopenFailed));
    CHECK_FALSE(holds_core_error(tools_err, glibre::core::Error::PluginMissingEntryPoint));
    CHECK_FALSE(holds_core_error(tools_err, glibre::core::Error::PluginManifestNotFound));

    // Construct a core::Error and confirm it IS recognized.
    const glibre::Error core_err{glibre::core::Error::PluginDlopenFailed};
    CHECK(holds_core_error(core_err, glibre::core::Error::PluginDlopenFailed));
    CHECK_FALSE(holds_core_error(core_err, glibre::core::Error::PluginMissingEntryPoint));
}

// ---------------------------------------------------------------------------
// Test: plugin_loader_open_loads_noop_plugin
//
// (Satisfies DoD assertion: unit_test_named: plugin_loader_open_loads_noop_plugin)
//
// Verifies that PluginLoader::open() succeeds on a well-formed plugin dylib:
//   • The result has_value().
//   • abi_hash() is a non-null pointer (the noop plugin exports a 64-char
//     placeholder blake3 hex string).
//   • register_fn() is non-null (the symbol was resolved).
//   • manifest_blob_size() matches the noop stub's exported zero size.
// ---------------------------------------------------------------------------

TEST_CASE("plugin_loader_open_loads_noop_plugin", "[core][plugin_loader]") {
#ifndef GLIBRE_NOOP_DYLIB_PATH
    SKIP("GLIBRE_NOOP_DYLIB_PATH not defined; build with GLIBRE_BUILD_EXAMPLES=ON");
#else
    const eastl::string_view path{GLIBRE_NOOP_DYLIB_PATH};
    REQUIRE_FALSE(path.empty());

    auto result = glibre::core::PluginLoader::open(path);

    REQUIRE(result.has_value());

    const glibre::core::PluginLoader& loader = *result;

    // The noop plugin exports glibre_plugin_abi_hash as a 64-char hex string.
    REQUIRE(loader.abi_hash() != nullptr);
    CHECK(std::strlen(loader.abi_hash()) == 64u);  // blake3 hex = 64 chars

    // register_fn must resolve (noop plugin exports glibre_plugin_register).
    CHECK(loader.register_fn() != nullptr);

    // noop plugin exports glibre_plugin_manifest = nullptr and
    // glibre_plugin_manifest_size = 0.
    CHECK(loader.manifest_blob_size() == 0u);

    // dylib_path accessor must round-trip the input path.
    CHECK(loader.dylib_path() == eastl::string{path.data(), path.size()});
#endif
}

// ---------------------------------------------------------------------------
// Test: plugin_loader_open_returns_error_on_missing_path
//
// (Satisfies DoD assertion:
//   unit_test_named: plugin_loader_open_returns_error_on_missing_path)
//
// Alias for the dlopen_failure_returns_plugin_dlopen_failed case from the
// issue #229 Unit Test Plan.  A path that does not exist on disk causes
// dlopen() to return nullptr; the loader must return PluginDlopenFailed.
// ---------------------------------------------------------------------------

TEST_CASE("plugin_loader_open_returns_error_on_missing_path", "[core][plugin_loader]") {
    const eastl::string_view nonexistent{"/tmp/glibre-nonexistent-plugin-229.dylib"};

    auto result = glibre::core::PluginLoader::open(nonexistent);

    REQUIRE_FALSE(result.has_value());
    CHECK(holds_core_error(result.error(), glibre::core::Error::PluginDlopenFailed));
}

// ---------------------------------------------------------------------------
// Test: dlopen_failure_returns_plugin_dlopen_failed
//
// (Satisfies issue #229 Unit Test Plan: dlopen_failure_returns_plugin_dlopen_failed)
//
// Exercises the same dlopen failure path as above through a second named
// test case matching the plan's unit test table exactly.
// ---------------------------------------------------------------------------

TEST_CASE("dlopen_failure_returns_plugin_dlopen_failed", "[core][plugin_loader]") {
    auto result = glibre::core::PluginLoader::open("/no/such/path/plugin.dylib");

    REQUIRE_FALSE(result.has_value());
    CHECK(holds_core_error(result.error(), glibre::core::Error::PluginDlopenFailed));
}

// ---------------------------------------------------------------------------
// Test: plugin_loader_open_returns_error_on_missing_symbols
//
// (Satisfies DoD assertion:
//   unit_test_named: plugin_loader_open_returns_error_on_missing_symbols)
//
// Opens a valid .dylib that does NOT export any of the four required plugin
// entry-point symbols.  The loader must return PluginMissingEntryPoint and
// must NOT leak the dlopen handle (destructor-of-error path).
// ---------------------------------------------------------------------------

TEST_CASE("plugin_loader_open_returns_error_on_missing_symbols", "[core][plugin_loader]") {
#ifndef GLIBRE_STUB_NO_SYMBOLS_DYLIB_PATH
    SKIP("GLIBRE_STUB_NO_SYMBOLS_DYLIB_PATH not defined");
#else
    const eastl::string_view stub_path{GLIBRE_STUB_NO_SYMBOLS_DYLIB_PATH};
    REQUIRE_FALSE(stub_path.empty());

    auto result = glibre::core::PluginLoader::open(stub_path);

    REQUIRE_FALSE(result.has_value());
    CHECK(holds_core_error(result.error(), glibre::core::Error::PluginMissingEntryPoint));
#endif
}

// ---------------------------------------------------------------------------
// Test: missing_symbol_returns_plugin_missing_entry_point
//
// (Satisfies issue #229 Unit Test Plan:
//   missing_symbol_returns_plugin_missing_entry_point)
//
// Same failure scenario as plugin_loader_open_returns_error_on_missing_symbols
// but named per the issue's unit test plan table for exact DoD matching.
// ---------------------------------------------------------------------------

TEST_CASE("missing_symbol_returns_plugin_missing_entry_point", "[core][plugin_loader]") {
#ifndef GLIBRE_STUB_NO_SYMBOLS_DYLIB_PATH
    SKIP("GLIBRE_STUB_NO_SYMBOLS_DYLIB_PATH not defined");
#else
    const eastl::string_view stub_path{GLIBRE_STUB_NO_SYMBOLS_DYLIB_PATH};

    auto result = glibre::core::PluginLoader::open(stub_path);

    REQUIRE_FALSE(result.has_value());
    CHECK(holds_core_error(result.error(), glibre::core::Error::PluginMissingEntryPoint));
#endif
}

// ---------------------------------------------------------------------------
// Test: success_manifest_result_is_populated_after_open
//
// (Satisfies issue #229 Unit Test Plan:
//   success_manifest_result_is_populated_after_open)
//
// Verifies that after a successful open() the manifest_result() accessor is
// populated.  The noop plugin has no sidecar .manifest file, so we expect
// the manifest_result to hold an error (PluginManifestNotFound) — which is
// the correct stub behaviour documented in plugin_manifest.cpp.
// This test confirms the step-3 data-collection semantics: the manifest read
// is attempted and the result (success or error) is accessible via the loader.
//
// Renamed from "success_reads_manifest_with_expected_fields" (MED finding
// round-1): the old name implied happy-path field assertions, but the body
// checks PluginManifestNotFound (the correct result when no sidecar is present).
// The field-assertion test lands in plan #230 once the in-dylib blob ships.
// DoD on issue #229 updated to match.
// ---------------------------------------------------------------------------

TEST_CASE("success_manifest_result_is_populated_after_open", "[core][plugin_loader]") {
#ifndef GLIBRE_NOOP_DYLIB_PATH
    SKIP("GLIBRE_NOOP_DYLIB_PATH not defined; build with GLIBRE_BUILD_EXAMPLES=ON");
#else
    const eastl::string_view path{GLIBRE_NOOP_DYLIB_PATH};

    auto result = glibre::core::PluginLoader::open(path);
    REQUIRE(result.has_value());

    const glibre::core::PluginLoader& loader = *result;

    // The noop plugin has no sidecar .manifest file; the stub PluginManifest::open()
    // returns PluginManifestNotFound for a missing file.  Plan #230 treats this as
    // a hard error; at this loader layer we only collect the result.
    const auto& manifest_res = loader.manifest_result();
    REQUIRE_FALSE(manifest_res.has_value());
    CHECK(holds_core_error(manifest_res.error(), glibre::core::Error::PluginManifestNotFound));
#endif
}

// ---------------------------------------------------------------------------
// Test: invalid_manifest_returns_plugin_manifest_invalid
//
// (Satisfies issue #229 Unit Test Plan:
//   invalid_manifest_returns_plugin_manifest_invalid)
//
// Verifies that when a sidecar .manifest file EXISTS but cannot be deserialized,
// the manifest_result holds PluginManifestInvalid.
//
// The current PluginManifest::open() stub (core/src/plugin_manifest.cpp) returns
// PluginManifestInvalid for any existing regular file.  We use a temp file to
// exercise this path without requiring Fory toolchain.
// ---------------------------------------------------------------------------

TEST_CASE("invalid_manifest_returns_plugin_manifest_invalid", "[core][plugin_loader]") {
#ifndef GLIBRE_NOOP_DYLIB_PATH
    SKIP("GLIBRE_NOOP_DYLIB_PATH not defined; build with GLIBRE_BUILD_EXAMPLES=ON");
#else
    // Create a sidecar .manifest file alongside a copy of the noop dylib.
    // We place the temp dylib under /tmp so we can write a companion .manifest.
    //
    // Strategy: use std::filesystem to create a temp dir, symlink or copy
    // the noop dylib, then write a corrupt .manifest file next to it.
    // PluginManifest::open() sees the file exists → returns PluginManifestInvalid.
    //
    // std::filesystem is an explicit std:: carve-out per PHILOSOPHY §11.

    namespace fs = std::filesystem;

    std::error_code ec;
    const fs::path tmp_dir = fs::temp_directory_path(ec) / "glibre_test_229";
    if (ec) {
        SKIP("Could not get temp directory");
    }

    fs::create_directories(tmp_dir, ec);
    if (ec) {
        SKIP("Could not create temp directory");
    }

    // Write a dummy .manifest file (corrupt — not valid Fory data).
    const fs::path manifest_path = tmp_dir / "corrupt.dylib.manifest";
    {
        // Write 4 junk bytes so the file exists as a regular file.
        // PluginManifest::open() sees it exists → returns PluginManifestInvalid.
        FILE* f = fopen(manifest_path.c_str(), "wb");  // NOLINT(cppcoreguidelines-owning-memory)
        if (f == nullptr) {
            SKIP("Could not create temp manifest file");
        }
        const unsigned char garbage[4] = {0xDE, 0xAD, 0xBE, 0xEF};
        fwrite(garbage, 1, 4, f);
        fclose(f);  // NOLINT(cppcoreguidelines-owning-memory)
    }

    // Copy or hard-link the noop dylib into the temp dir so the loader can
    // dlopen it, then check that the sidecar .manifest is read and returns Invalid.
    const fs::path noop_src{GLIBRE_NOOP_DYLIB_PATH};
    const fs::path noop_dst = tmp_dir / "corrupt.dylib";
    fs::copy_file(noop_src, noop_dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        // Clean up before skipping.
        fs::remove_all(tmp_dir, ec);
        SKIP("Could not copy noop dylib to temp dir");
    }

    const std::string noop_dst_str = noop_dst.string();
    const eastl::string_view loader_path{noop_dst_str.data(), noop_dst_str.size()};

    auto result = glibre::core::PluginLoader::open(loader_path);

    // Cleanup regardless of result.
    fs::remove_all(tmp_dir, ec);

    REQUIRE(result.has_value());

    const glibre::core::PluginLoader& loader = *result;
    const auto& manifest_res = loader.manifest_result();
    REQUIRE_FALSE(manifest_res.has_value());
    CHECK(holds_core_error(manifest_res.error(), glibre::core::Error::PluginManifestInvalid));
#endif
}
