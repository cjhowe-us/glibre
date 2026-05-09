cmake_minimum_required(VERSION 3.30)

# ---------------------------------------------------------------------------
# tests/data/pkg — vcpkg overlay port smoke tests
#
# Verifies that packages added via vcpkg-overlay-ports/ resolve correctly
# and that their CMake targets satisfy the linker.  Refs #218.
# ---------------------------------------------------------------------------

find_package(Catch2 CONFIG REQUIRED)
find_package(Fory CONFIG REQUIRED)

add_executable(fory_target_links fory_target_links.cpp)

target_link_libraries(fory_target_links
    PRIVATE
        Catch2::Catch2WithMain
        Fory::fory
)

# Disable C++ module std import for this target — it uses classical headers only.
set_target_properties(fory_target_links PROPERTIES CXX_MODULE_STD OFF)

include(CTest)
include(Catch)
catch_discover_tests(fory_target_links)
