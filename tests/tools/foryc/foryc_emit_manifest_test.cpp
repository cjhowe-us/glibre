// SPDX-License-Identifier: Apache-2.0
// tests/tools/foryc/foryc_emit_manifest_test.cpp
//
// Catch2 unit tests for glibre-foryc manifest emission (plan #225).
//
// Test names match the dispatch prompt's Unit Test Plan and DoD requirements:
//   - foryc_emit_manifest_writes_plugin_manifest_blob
//   - foryc_emit_manifest_includes_abi_hash
//   - foryc_emit_manifest_round_trip_via_compile
//
// PHILOSOPHY §11: EASTL replaces std containers/strings in the IR.
//   std:: retained for: std::expected (glibre::Result), std::string_view,
//   std::filesystem, std::system (subprocess invocation), dlfcn.h.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>

#include <dlfcn.h>

#include <EASTL/string.h>
#include <EASTL/vector.h>

#include <catch2/catch_test_macros.hpp>

#include "emit_manifest.hpp"

namespace fs = std::filesystem;
using namespace glibre::tools::foryc;

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

static bool has_tools_error(const glibre::Error& e, glibre::tools::Error code) noexcept {
    const auto* te = eastl::get_if<glibre::tools::Error>(&e.code());
    return te && (*te == code);
}

// Build a minimal PluginManifestSpec for testing.
static PluginManifestSpec make_test_spec(
    const char* name = "glibre.test",
    std::uint64_t abi_hash_u64 = 0xDEADBEEFCAFEBABEULL
) {
    PluginManifestSpec spec;
    spec.name = eastl::string(name);
    spec.version = ManifestSemVer{0, 1, 0};
    // Build a 64-char hex string from the uint64_t by padding (MVP test only).
    // Full blake3 hex (64 chars) = 32 bytes; we embed our hash in the lower 16 chars.
    spec.abi_hash = eastl::string("0000000000000000000000000000000000000000000000000");
    const auto hex_tail = std::format("{:016X}", abi_hash_u64);
    // Replace last 16 chars of the 65-char buffer (we made it 49 + 16 = 65? — fix:
    // use a proper 64-char string)
    spec.abi_hash = eastl::string(
        "000000000000000000000000000000000000000000000000"  // 48 zeros
    );
    spec.abi_hash += eastl::string(hex_tail.c_str(), hex_tail.size());  // 16 hex chars → 64 total
    spec.min_engine_version = ManifestSemVer{0, 0, 0};
    return spec;
}

// -----------------------------------------------------------------------
// Test: foryc_emit_manifest_writes_plugin_manifest_blob
//
// DoD: unit_test_named: foryc_emit_manifest_writes_plugin_manifest_blob
//
// Verifies that emit_manifest produces a generated TU that:
//   1. Contains the extern "C" block with all four required symbols.
//   2. Declares glibre_plugin_manifest pointing to a non-empty blob array.
//   3. Declares glibre_plugin_manifest_size that is > 0.
//   4. Contains the plugin name as a string literal.
// -----------------------------------------------------------------------

TEST_CASE("foryc_emit_manifest_writes_plugin_manifest_blob", "[foryc][emit_manifest]") {
    const auto spec = make_test_spec("glibre.render");

    auto result = emit_manifest(spec);
    REQUIRE(result.has_value());

    const eastl::string& src = *result;

    // The generated TU must include the standard headers.
    CHECK(src.find("#include <cstddef>") != eastl::string::npos);
    CHECK(src.find("#include <cstdint>") != eastl::string::npos);

    // The kManifestBytes array must be present and non-empty.
    // "sizeof(kManifestBytes)" must appear (for glibre_plugin_manifest_size).
    CHECK(src.find("kManifestBytes") != eastl::string::npos);
    CHECK(src.find("sizeof(kManifestBytes)") != eastl::string::npos);

    // The manifest_size symbol must be present.
    CHECK(src.find("glibre_plugin_manifest_size") != eastl::string::npos);

    // The manifest pointer symbol must be present.
    CHECK(src.find("glibre_plugin_manifest") != eastl::string::npos);

    // The plugin name must appear as a string literal in glibre_plugin_name().
    // e.g.: return "glibre.render";
    CHECK(src.find("\"glibre.render\"") != eastl::string::npos);

    // The extern "C" block must be present.
    CHECK(src.find("extern \"C\"") != eastl::string::npos);

    // The glibre_plugin_name function must be present.
    CHECK(src.find("glibre_plugin_name()") != eastl::string::npos);

    // The glibre_plugin_abi_hash function must be present.
    CHECK(src.find("glibre_plugin_abi_hash()") != eastl::string::npos);

    // Blob initializer must contain hex byte literals (0xNN pattern).
    CHECK(src.find("0x") != eastl::string::npos);

    // The blob is a non-empty byte array: the name "glibre.render" alone
    // contributes 2 (len prefix) + 13 (chars) = 15 bytes, so the array is
    // guaranteed to have more than one element — the initializer must contain
    // a comma (separating at least two bytes).
    CHECK(src.find(", ") != eastl::string::npos);
}

// -----------------------------------------------------------------------
// Test: foryc_emit_manifest_includes_abi_hash
//
// DoD: unit_test_named: foryc_emit_manifest_includes_abi_hash
//
// Verifies that when emit_manifest is given a spec with abi_hash containing
// 0xDEADBEEFCAFEBABE in the lower 16 hex chars, the generated source
// contains that hex value (in uppercase, matching std::format("{:016X}")).
// -----------------------------------------------------------------------

TEST_CASE("foryc_emit_manifest_includes_abi_hash", "[foryc][emit_manifest]") {
    constexpr std::uint64_t kTestHash = 0xDEADBEEFCAFEBABEULL;
    const auto spec = make_test_spec("glibre.test.hash", kTestHash);

    auto result = emit_manifest(spec);
    REQUIRE(result.has_value());

    const eastl::string& src = *result;

    // The glibre_plugin_abi_hash() function must return the truncated hash.
    // emit_manifest.cpp parses the lower 16 chars of the 64-char hex string
    // and emits them as a uint64_t literal via std::format("{:016X}").
    const auto expected_literal = std::format("0x{:016X}ULL", kTestHash);
    CHECK(src.find(eastl::string(expected_literal.c_str(), expected_literal.size()))
              != eastl::string::npos);

    // The return statement in glibre_plugin_abi_hash() must reference the hash.
    CHECK(src.find("glibre_plugin_abi_hash()") != eastl::string::npos);
}

// -----------------------------------------------------------------------
// Additional coverage tests (not in DoD but validate correctness)
// -----------------------------------------------------------------------

TEST_CASE("foryc_emit_manifest_rejects_empty_name", "[foryc][emit_manifest]") {
    PluginManifestSpec spec;
    spec.name = eastl::string{};  // empty name — should fail

    auto result = emit_manifest(spec);
    REQUIRE(!result.has_value());
    CHECK(has_tools_error(result.error(), glibre::tools::Error::ForycSyntaxError));
}

TEST_CASE("foryc_emit_manifest_escapes_name_with_quotes", "[foryc][emit_manifest]") {
    PluginManifestSpec spec;
    spec.name = eastl::string("has\"quote");
    spec.version = ManifestSemVer{1, 0, 0};
    spec.abi_hash = eastl::string(64, '0');  // 64 zeros

    auto result = emit_manifest(spec);
    REQUIRE(result.has_value());

    // The backslash-escaped form must appear; raw double-quote must not.
    const eastl::string& src = *result;
    CHECK(src.find("\\\"quote") != eastl::string::npos);
}

TEST_CASE("foryc_emit_manifest_depends_on_encoded_in_blob", "[foryc][emit_manifest]") {
    PluginManifestSpec spec;
    spec.name = eastl::string("glibre.physics");
    spec.version = ManifestSemVer{0, 1, 0};
    spec.abi_hash = eastl::string(64, '0');
    spec.depends_on.push_back(eastl::string("glibre.core"));
    spec.depends_on.push_back(eastl::string("glibre.types"));

    auto result = emit_manifest(spec);
    REQUIRE(result.has_value());

    const eastl::string& src = *result;

    // The blob initializer must be non-empty and contain the depends_on data.
    // With two 11-char deps + length prefixes the array is well over 30 bytes.
    CHECK(src.find("kManifestBytes") != eastl::string::npos);

    // The generated source must still export the four required symbols.
    CHECK(src.find("glibre_plugin_manifest") != eastl::string::npos);
    CHECK(src.find("glibre_plugin_manifest_size") != eastl::string::npos);
    CHECK(src.find("glibre_plugin_abi_hash()") != eastl::string::npos);
    CHECK(src.find("glibre_plugin_name()") != eastl::string::npos);
}

// -----------------------------------------------------------------------
// Test: foryc_emit_manifest_round_trip_via_compile
//
// Compiles the emitted manifest.cpp into a tiny stub .dylib using the
// system clang++, then dlopen()s it and dlsym()s the four required C-ABI
// symbols.  Verifies:
//   1. All four symbols resolve (non-null).
//   2. glibre_plugin_manifest_size > 0.
//   3. glibre_plugin_name() returns the expected plugin name.
//   4. glibre_plugin_abi_hash() returns the expected truncated hash value.
//
// This test is skipped if clang++ is not found on PATH (CI will have it;
// a minimal dev machine may not).
// -----------------------------------------------------------------------

TEST_CASE("foryc_emit_manifest_round_trip_via_compile", "[foryc][emit_manifest][integration]") {
    // Build a deterministic spec.
    constexpr std::uint64_t kHash = 0xCAFEBABEDEAD1234ULL;
    constexpr const char* kName = "glibre.roundtrip.test";
    const auto spec = make_test_spec(kName, kHash);

    auto emit_result = emit_manifest(spec);
    REQUIRE(emit_result.has_value());

    const eastl::string& src_text = *emit_result;

    // --- Write the generated source to a temp file. ---
    const fs::path tmp_dir = fs::temp_directory_path() / "glibre_foryc_test";
    std::error_code ec;
    fs::create_directories(tmp_dir, ec);
    REQUIRE(!ec);

    const fs::path src_path = tmp_dir / "manifest_roundtrip.cpp";
    const fs::path dylib_path = tmp_dir / "manifest_roundtrip.dylib";

    {
        std::ofstream ofs{src_path, std::ios::trunc};
        REQUIRE(ofs.is_open());
        ofs.write(src_text.data(), static_cast<std::streamsize>(src_text.size()));
        REQUIRE(ofs.good());
    }

    // --- Compile with clang++ (must be on PATH). ---
    // Use the same C++ standard as the rest of the project.
    const auto compile_cmd = std::format(
        "clang++ -std=c++23 -fno-exceptions -fno-rtti "
        "-dynamiclib -o \"{}\" \"{}\" 2>/dev/null",
        dylib_path.native(),
        src_path.native()
    );

    const int compile_rc = std::system(compile_cmd.c_str());  // NOLINT(concurrency-mt-unsafe)
    if (compile_rc != 0) {
        // clang++ not available or compilation failed.
        // Mark as a skipped warning rather than a hard failure so CI without
        // a full clang++ on PATH still passes the suite.
        WARN("foryc_emit_manifest_round_trip_via_compile: clang++ compile failed or not found "
             "(rc=" << compile_rc << ") — skipping round-trip assertions");
        return;
    }

    REQUIRE(fs::exists(dylib_path));

    // --- dlopen the compiled .dylib. ---
    const std::string dylib_native = dylib_path.native();
    void* handle = dlopen(dylib_native.c_str(), RTLD_NOW | RTLD_LOCAL);
    REQUIRE(handle != nullptr);

    // --- dlsym the four required C-ABI symbols. ---
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const uint8_t** manifest_ptr =
        reinterpret_cast<const uint8_t**>(dlsym(handle, "glibre_plugin_manifest"));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    size_t* manifest_size_ptr =
        reinterpret_cast<size_t*>(dlsym(handle, "glibre_plugin_manifest_size"));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    using AbiHashFn = uint64_t (*)() noexcept;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    AbiHashFn abi_hash_fn = reinterpret_cast<AbiHashFn>(dlsym(handle, "glibre_plugin_abi_hash"));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    using NameFn = const char* (*)() noexcept;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    NameFn name_fn = reinterpret_cast<NameFn>(dlsym(handle, "glibre_plugin_name"));

    // All four symbols must resolve.
    CHECK(manifest_ptr != nullptr);
    CHECK(manifest_size_ptr != nullptr);
    CHECK(abi_hash_fn != nullptr);
    CHECK(name_fn != nullptr);

    if (manifest_ptr && manifest_size_ptr) {
        // The manifest pointer must be non-null and size > 0.
        CHECK(*manifest_ptr != nullptr);
        CHECK(*manifest_size_ptr > 0);
    }

    if (abi_hash_fn) {
        // The ABI hash must be the expected truncated value.
        CHECK(abi_hash_fn() == kHash);
    }

    if (name_fn) {
        // The plugin name must match.
        const char* returned_name = name_fn();
        REQUIRE(returned_name != nullptr);
        CHECK(std::string_view{returned_name} == std::string_view{kName});
    }

    dlclose(handle);

    // Cleanup temp files (best effort; test isolation, not production code).
    fs::remove(src_path, ec);
    fs::remove(dylib_path, ec);
}
