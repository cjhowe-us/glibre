// SPDX-License-Identifier: Apache-2.0
// Smoke link-test for the Fory::fory vcpkg overlay port (refs #218).
//
// This translation unit instantiates one Fory symbol to confirm that:
//   1. find_package(Fory CONFIG REQUIRED) resolves the overlay port.
//   2. target_link_libraries(... Fory::fory) satisfies the linker.
//
// No serialization round-trip is exercised here; that belongs in the
// data-plugin unit tests once glibre-types-codegen lands.

#include <catch2/catch_test_macros.hpp>
#include <fory/serialization/fory.h>

TEST_CASE("fory_target_links", "[pkg][data][fory]") {
    // Build a Fory instance to confirm both the header resolves and the
    // required symbols are present in the linked library.
    using fory::serialization::Fory;
    auto instance = Fory::builder().xlang(false).track_ref(false).build();
    (void)instance;
    SUCCEED("Fory::fory links and instantiates successfully");
}
