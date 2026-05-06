cmake_minimum_required(VERSION 3.30)

project(glibre
    VERSION 0.0.0
    DESCRIPTION "No-code game engine"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# C++23 modules — see cmake-cxxmodules(7).
# Ninja generator + per-target dependency scan + `import std;` support.
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_CXX_MODULE_STD ON)

if(NOT APPLE)
    message(FATAL_ERROR "glibre MVP is macOS-only; cross-platform deferred")
endif()

option(GLIBRE_BUILD_TESTS  "Build Catch2 tests"     ON)
option(GLIBRE_BUILD_EDITOR "Build editor binary"    ON)
option(GLIBRE_BUILD_RUNTIME "Build runtime binary" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(GNUInstallDirs)

if(GLIBRE_BUILD_TESTS)
    enable_testing()
endif()

# Subdirectories will be added once stubs land.
# add_subdirectory(core)
# add_subdirectory(plugins)
# add_subdirectory(tools)
# add_subdirectory(runtime)
# if(GLIBRE_BUILD_TESTS)
#     add_subdirectory(tests)
# endif()

# CMake-driven xcodegen + xcodebuild → produces Glibre.xcodeproj plus
# GlibreMacOS.app / GlibreIOS.app from a single `cmake --build`.
include(AppleApps)
