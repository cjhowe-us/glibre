cmake_minimum_required(VERSION 3.30)

# ---------------------------------------------------------------------------
# tests/data — data bounded-context unit and pkg tests
# ---------------------------------------------------------------------------

# The pkg smoke tests require Catch2 and Fory installed for the host triplet
# (arm64-osx). iOS presets use arm64-ios / arm64-ios-simulator triplets for
# which Catch2 is not — and should not be — installed. Skip on all iOS
# targets; the fory_smoke test is a host-machine link verification only.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_subdirectory(pkg)
endif()
