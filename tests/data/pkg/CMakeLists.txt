cmake_minimum_required(VERSION 3.30)

# ---------------------------------------------------------------------------
# tests/data/pkg — vcpkg overlay port smoke tests
#
# Verifies that packages added via vcpkg-overlay-ports/ resolve correctly
# and that their CMake targets satisfy the linker.  Refs #218.
# ---------------------------------------------------------------------------

find_package(Catch2 CONFIG REQUIRED)
find_package(Fory CONFIG REQUIRED)
find_package(absl CONFIG REQUIRED)

add_executable(fory_smoke fory_smoke.cpp)

target_link_libraries(fory_smoke
    PRIVATE
        Catch2::Catch2WithMain
        Fory::fory
        absl::status
)

# CXX_MODULE_STD OFF: Catch2's generated main calls std:: symbols via classical
# headers.  Apple-Clang (Homebrew clang >= 21, currently 22.x on macOS 26
# Apple Silicon) and Catch2's catch_all.hpp are incompatible with std module
# import — enabling it causes duplicate symbol linker errors on this toolchain.
# This property must remain OFF until Catch2 ships explicit module support.
set_target_properties(fory_smoke PROPERTIES CXX_MODULE_STD OFF)

include(CTest)
include(Catch)
catch_discover_tests(fory_smoke)
