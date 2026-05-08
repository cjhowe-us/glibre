# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#
# Glibre overlay port for Apache Fory C++ — pins tag v0.17.0.
# Builds the cpp/ subdirectory and exports:
#   fory::fory          (upstream alias — everything bundled)
#   Fory::fory          (glibre canonical alias required by fory-codegen.md)
#
# The upstream project exports only fory:: (lowercase); we inject the
# capitalised alias via a post-install cmake fragment so downstream code
# can use `target_link_libraries(... PRIVATE Fory::fory)` as documented
# in reviews/decisions/fory-codegen.md §CMake Integration.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO apache/fory
    REF v${VERSION}
    SHA512 ce3cf158a184c19e9d7cfa974442846b77e1d893b8231b852b05fbd535ec1c39e30980ab5be513ada1432ef59b163681025088b902a2eb1442282348e54eeb2c
    HEAD_REF main
    PATCHES
        use-system-abseil.patch
)

# The Apache Fory repository root is NOT the CMake root; the C++ build
# lives under cpp/.  Point vcpkg at that subdirectory.
set(CPP_SOURCE_PATH "${SOURCE_PATH}/cpp")

vcpkg_cmake_configure(
    SOURCE_PATH "${CPP_SOURCE_PATH}"
    OPTIONS
        -DFORY_BUILD_TESTS=OFF
        -DFORY_BUILD_SHARED=ON
        -DFORY_BUILD_STATIC=ON
        # AVX2 is x86_64-only; our baseline is arm64-osx, disable to avoid
        # the -mavx2 flag being passed to Apple LLVM clang on ARM.
        -DFORY_USE_AVX2=OFF
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

# Fix up the installed cmake config directory so vcpkg's find_package
# machinery can locate ForyConfig.cmake via its standard search paths.
# The upstream installs to lib/cmake/fory; vcpkg expects share/<port>.
vcpkg_cmake_config_fixup(
    PACKAGE_NAME fory
    CONFIG_PATH lib/cmake/fory
)

# Remove duplicate headers from the debug tree.
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

# -------------------------------------------------------------------
# Inject the capitalised Fory:: namespace alias.
#
# The upstream ForyTargets.cmake defines fory:: targets (lowercase).
# glibre code uses Fory::fory (capital F) per fory-codegen.md.
#
# In the INSTALLED tree the main bundled target is fory::fory_lib
# (an INTERFACE IMPORTED target).  We cannot use add_library(ALIAS)
# against an IMPORTED target, so we create a second INTERFACE IMPORTED
# target Fory::fory that forwards its INTERFACE_LINK_LIBRARIES to
# fory::fory_lib.  This gives consumers a stable capitalised name
# without any linking change.
# -------------------------------------------------------------------
set(_targets_file "${CURRENT_PACKAGES_DIR}/share/fory/ForyTargets.cmake")
if(EXISTS "${_targets_file}")
    file(APPEND "${_targets_file}" "
# --- glibre overlay: capitalised namespace alias ---
# Allows downstream code to use Fory::fory as documented in
# reviews/decisions/fory-codegen.md §CMake Integration.
if(NOT TARGET Fory::fory AND TARGET fory::fory_lib)
    add_library(Fory::fory INTERFACE IMPORTED)
    set_target_properties(Fory::fory PROPERTIES
        INTERFACE_LINK_LIBRARIES \"fory::fory_lib\")
endif()
# Provide a lowercase fory::fory alias for code that uses the
# upstream name directly (the installed tree has no such alias).
if(NOT TARGET fory::fory AND TARGET fory::fory_lib)
    add_library(fory::fory INTERFACE IMPORTED)
    set_target_properties(fory::fory PROPERTIES
        INTERFACE_LINK_LIBRARIES \"fory::fory_lib\")
endif()
")
endif()

# Install license.
file(INSTALL "${SOURCE_PATH}/LICENSE"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}"
    RENAME copyright
)

# Install usage note (vcpkg convention).
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}"
)
