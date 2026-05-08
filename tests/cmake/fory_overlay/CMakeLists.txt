# tests/cmake/fory_overlay/CMakeLists.txt
#
# Stand-alone CMake configure-time verification that the vcpkg overlay port
# for Apache Fory exports both the upstream fory::fory target and the glibre
# canonical Fory::fory alias (see reviews/decisions/fory-codegen.md).
#
# This file is NOT a subdirectory of the main glibre CMake tree.  It is
# invoked as a separate cmake configure step by the CI "fory-overlay-check"
# job, which passes -DCMAKE_TOOLCHAIN_FILE pointing at the vcpkg cmake helper.
# The configure step fails (non-zero exit) if either target is absent.

cmake_minimum_required(VERSION 3.30)
project(fory_overlay_check LANGUAGES CXX)

find_package(fory CONFIG REQUIRED)

if(NOT TARGET fory::fory)
    message(FATAL_ERROR
        "vcpkg overlay port did not export 'fory::fory'.  "
        "Check vcpkg-overlay-ports/fory/portfile.cmake.")
endif()

if(NOT TARGET Fory::fory)
    message(FATAL_ERROR
        "vcpkg overlay port did not export 'Fory::fory' alias.  "
        "Check the glibre alias fragment in portfile.cmake (fory-codegen.md).")
endif()

message(STATUS "fory_overlay_check: fory::fory and Fory::fory are both present -- OK")
