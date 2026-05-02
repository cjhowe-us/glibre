cmake_minimum_required(VERSION 3.28)

project(glibre
    VERSION 0.0.0
    DESCRIPTION "No-code game engine"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT APPLE)
    message(FATAL_ERROR "glibre MVP is macOS-only; cross-platform deferred")
endif()

option(GLIBRE_BUILD_TESTS  "Build Catch2 tests"     ON)
option(GLIBRE_BUILD_EDITOR "Build editor binary"    ON)
option(GLIBRE_BUILD_RUNTIME "Build runtime binary" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(GNUInstallDirs)

# Subdirectories will be added once stubs land.
# add_subdirectory(core)
# add_subdirectory(plugins)
# add_subdirectory(tools)
# add_subdirectory(runtime)
# if(GLIBRE_BUILD_TESTS)
#     enable_testing()
#     add_subdirectory(tests)
# endif()
