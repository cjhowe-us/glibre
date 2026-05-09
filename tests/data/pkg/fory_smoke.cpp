// SPDX-License-Identifier: Apache-2.0
// Fory vcpkg overlay port smoke tests (refs #218).
//
// Covers two concerns in one ctest target:
//   1. find_package(Fory CONFIG REQUIRED) resolves the overlay port.
//   2. target_link_libraries(... Fory::fory) satisfies the linker.
//   3. Transitive absl symbols propagate through Fory::fory so that an
//      absl-propagation regression (missing find_dependency(absl) in
//      ForyAliases.cmake) surfaces here rather than in consumer builds.

#include <catch2/catch_test_macros.hpp>
#include <fory/serialization/fory.h>
// Pull in an absl header that Fory's debugging subsystem depends on.
// If absl is not propagated transitively through Fory::fory, including this
// header and using the type below will produce a linker error, catching
// MED-1-class regressions in CI before they reach consumer code.
#include <absl/status/statusor.h>

TEST_CASE("fory_target_links", "[pkg][data][fory]") {
    // Build a Fory instance to confirm both the header resolves and the
    // required symbols are present in the linked library.
    using fory::serialization::Fory;
    auto instance = Fory::builder().xlang(false).track_ref(false).build();
    (void)instance;
    SUCCEED("Fory::fory links and instantiates successfully");
}

TEST_CASE("fory_absl_transitive", "[pkg][data][fory]") {
    // Construct an absl::StatusOr to exercise absl symbols that Fory depends
    // on transitively.  A missing find_dependency(absl) in ForyAliases.cmake
    // will cause a linker failure here before it reaches any real consumer.
    absl::StatusOr<int> status_or{42};
    REQUIRE(status_or.ok());
    REQUIRE(*status_or == 42);
}
