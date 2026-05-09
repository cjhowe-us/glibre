// tests/data/dylib/glibre_types_link_test.cpp
//
// Unit test: glibre_types_dylib_links_minimal_consumer
//
// Verifies that:
//  1. glibre-types.dylib exposes glibre_types_abi_version().
//  2. The function returns the expected sentinel value (1).
//  3. The test executable links ONLY glibre-types (no glibre-core),
//     satisfying plugin-abi.md §"Plugin file shape" rule 2.
//
// Authority:
//   plan #224 §"Unit Test Plan" — glibre_types_dylib_links_minimal_consumer
//   reviews/decisions/plugin-abi.md §"Plugin file shape" rule 2
//
// Note: glibre_types_abi_version() is the permanent anchor symbol defined in
// data/src/types_anchor.cpp.  It is distinct from glibre_types_abi_hash()
// (plan #222): the version integer answers "can the loader dlopen this dylib?",
// while the hash answers "does the plugin's schema contract match the host?".

#include <cstdint>

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
