cmake_minimum_required(VERSION 3.30)

# Apple-clang on macOS 26 is too old for `import std;`. Auto-pick Homebrew
# LLVM (clang ≥ 21) before `project()` so its CXX-detection step picks it
# up. User-supplied -DCMAKE_CXX_COMPILER / CC / CXX always wins.
if(APPLE AND NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    find_program(_brew_exe brew)
    if(_brew_exe)
        execute_process(
            COMMAND ${_brew_exe} --prefix llvm
            OUTPUT_VARIABLE _llvm_prefix
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(_llvm_prefix AND EXISTS "${_llvm_prefix}/bin/clang++")
            set(CMAKE_C_COMPILER   "${_llvm_prefix}/bin/clang"   CACHE FILEPATH "")
            set(CMAKE_CXX_COMPILER "${_llvm_prefix}/bin/clang++" CACHE FILEPATH "")
        endif()
    endif()
endif()

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
