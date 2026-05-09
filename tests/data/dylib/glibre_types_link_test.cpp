// tests/data/dylib/glibre_types_link_test.cpp
//
// Unit tests for plan #224 §"Unit Test Plan":
//   - glibre_types_dylib_links_minimal_consumer
//   - glibre_types_dylib_excludes_fory_and_blake3_symbols
//
// Verifies that:
//  1. glibre-types.dylib exposes glibre_types_abi_version().
//  2. The function returns the expected sentinel value (1).
//  3. The test executable links ONLY glibre-types (no glibre-core),
//     satisfying plugin-abi.md §"Plugin file shape" rule 2.
//  4. The dylib's public symbol table contains no Fory or BLAKE3 symbols,
//     meaning Fory::fory and blake3::blake3 are not accidentally exposed
//     on the plugin link surface.
//
// Authority:
//   plan #224 §"Unit Test Plan" — glibre_types_dylib_links_minimal_consumer,
//                                   links_only_fory_and_blake3_privately
//   reviews/decisions/plugin-abi.md §"Plugin file shape" rule 2
//
// Note: glibre_types_abi_version() is the permanent anchor symbol defined in
// data/src/types_anchor.cpp.  It is distinct from glibre_types_abi_hash()
// (plan #222): the version integer answers "can the loader dlopen this dylib?",
// while the hash answers "does the plugin's schema contract match the host?".

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <catch2/catch_test_macros.hpp>

// Forward-declare the extern "C" function exported by glibre-types.dylib.
// Including a generated header is intentionally avoided here so this test
// validates the raw link surface, not any header dependency.
extern "C" int32_t glibre_types_abi_version() noexcept;

// ---------------------------------------------------------------------------
// TEST CASE: glibre_types_dylib_links_minimal_consumer
//
// This is the canonical DoD test for plan #224.  The exact string
// "glibre_types_dylib_links_minimal_consumer" must match the
// unit_test_named: entry in the issue's ## Definition of Done block.
// ---------------------------------------------------------------------------

TEST_CASE("glibre_types_dylib_links_minimal_consumer", "[data][dylib][abi]") {
    // The sentinel value is 1 per data/src/types_anchor.cpp.
    // Future layout-breaking schema changes bump this integer and update
    // the corresponding SONAME per plugin-abi.md §"Versioning Rules" point 2.
    REQUIRE(glibre_types_abi_version() == 1);
}

// ---------------------------------------------------------------------------
// TEST CASE: glibre_types_dylib_excludes_fory_and_blake3_symbols
//
// Guards the PRIVATE-link-surface invariant from plugin-abi.md §"Plugin file
// shape" rule 2: Fory::fory and blake3::blake3 must be PRIVATE deps of
// glibre-types.dylib, never re-exported onto the plugin link surface.
//
// Mechanism: runs `nm -gU <dylib>` (macOS; -g = extern symbols, -U = defined
// only) and asserts that no line in the output matches the Fory or BLAKE3
// namespace prefixes.  The test passes today because neither is linked yet
// (plans #220/#222 are pending); it will catch a future regression where a
// misconfigured glibre-types accidentally exposes these symbols publicly.
//
// The dylib path is injected at compile time via the GLIBRE_TYPES_DYLIB_PATH
// compile definition set in tests/data/dylib/CMakeLists.txt.
//
// Note: `nm -gU` on macOS lists only externally-visible, defined symbols —
// the exact set that a downstream `dlopen` / dynamic linker would see.
// This is equivalent to otool-based checks but narrower and more portable
// within Apple platforms.
// ---------------------------------------------------------------------------

TEST_CASE("glibre_types_dylib_excludes_fory_and_blake3_symbols", "[data][dylib][abi]") {
#ifndef GLIBRE_TYPES_DYLIB_PATH
    FAIL(
        "GLIBRE_TYPES_DYLIB_PATH compile definition not set — "
        "check tests/data/dylib/CMakeLists.txt"
    );
#else
    // Build the nm command. The path is a string literal injected at compile
    // time; no shell-escaping is needed for well-formed CMake build paths.
    const std::string cmd = "nm -gU " GLIBRE_TYPES_DYLIB_PATH " 2>&1";

    // NOLINTNEXTLINE(cert-env33-c) — popen used intentionally for nm inspection
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) — popen is POSIX, not std
    std::FILE* pipe = ::popen(cmd.c_str(), "r");  // POSIX; not in std:: namespace
    REQUIRE(pipe != nullptr);

    std::string nm_output;
    std::array<char, 256> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        nm_output += buf.data();
    }
    ::pclose(pipe);

    // Fory C++ symbols are mangled under the `fory` namespace; the common
    // prefixes on macOS are `__ZN4fory` (mangled) or `_fory_` (C linkage).
    // BLAKE3 symbols are exported under `_blake3_` (C linkage from the C
    // implementation).  Neither should appear in glibre-types' public table
    // until plans #220/#222 land AND explicitly choose to re-export them
    // (which they must not: they are PRIVATE per plugin-abi.md).
    REQUIRE_FALSE(nm_output.find("__ZN4fory") != std::string::npos);
    REQUIRE_FALSE(nm_output.find("_fory_") != std::string::npos);
    REQUIRE_FALSE(nm_output.find("_blake3_") != std::string::npos);
#endif
}
