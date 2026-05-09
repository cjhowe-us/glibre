cmake_minimum_required(VERSION 3.30)

# ---------------------------------------------------------------------------
# tests/ — Catch2 unit test suite
#
# Each bounded context owns a subdirectory here.  Add new contexts as
# their first unit tests land.
# ---------------------------------------------------------------------------

find_package(Catch2 3 REQUIRED)

add_subdirectory(core)
add_subdirectory(data)

if(GLIBRE_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
