# SPDX-License-Identifier: Apache-2.0
# tools/foryc/cmake/glibre_plugin_codegen.cmake
#
# glibre_emit_plugin_manifest(<target> <plugin_fory_path>)
#
# Wires a CMake custom command that invokes glibre-foryc --emit=manifest on a
# single plugin.fory source file and compiles the emitted manifest.cpp into
# <target>.
#
# Authority: reviews/decisions/plugin-abi.md §"Manifest source-of-truth":
#   "The codegen step that already runs for data/schemas/*.fory is extended
#   to also process plugins/*/plugin.fory, emitting a generated manifest.cpp
#   that embeds the Fory-serialized manifest blob into the plugin's
#   translation unit."
#
# MED-7 fix (round-1 review): the helper takes a single .fory file path and
# passes --file <path> to glibre-foryc instead of --in <dir>.  This avoids
# the race condition that arises when multiple plugins' codegen targets each
# write to their own manifest.cpp but receive --in pointing at a shared
# source tree (causing concurrent writers to the same output path).
#
# Usage (in a plugin's CMakeLists.txt):
#   include(tools/foryc/cmake/glibre_plugin_codegen.cmake)
#   add_library(plugin-render SHARED ...)
#   glibre_emit_plugin_manifest(plugin-render
#       "${CMAKE_CURRENT_SOURCE_DIR}/plugin.fory")
#
# The emitted manifest.cpp is placed in:
#   ${CMAKE_CURRENT_BINARY_DIR}/glibre_gen/<target_name>/manifest.cpp
#
# Parameters:
#   target            — CMake target to add the generated source to.
#   plugin_fory_path  — Absolute path to the plugin.fory source file.
#
# Note: plan #225 (MVP). Real Fory serialization deferred to plan #231.
# The emitted blob uses the hand-serialized format documented in
# emit_manifest.hpp §"MVP blob format".

cmake_minimum_required(VERSION 3.30)

function(glibre_emit_plugin_manifest target plugin_fory_path)
    if(NOT TARGET glibre-foryc)
        message(FATAL_ERROR
            "glibre_emit_plugin_manifest: glibre-foryc must be a build target. "
            "Ensure tools/ is added before this function is called.")
    endif()

    if(NOT EXISTS "${plugin_fory_path}")
        message(FATAL_ERROR
            "glibre_emit_plugin_manifest: plugin.fory not found at ${plugin_fory_path}")
    endif()

    set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/glibre_gen/${target}")
    set(_manifest_out "${_gen_dir}/manifest.cpp")
    set(_stamp "${_gen_dir}/.manifest.stamp")

    file(MAKE_DIRECTORY "${_gen_dir}")

    # Pass --file <plugin_fory_path> (single-file mode) to avoid recursive
    # directory scans that could race with other targets writing to the same
    # output.  Each plugin target invokes this helper for its own plugin.fory
    # only.  (MED-7 fix, round-1 review.)
    add_custom_command(
        OUTPUT "${_manifest_out}" "${_stamp}"
        COMMAND
            "$<TARGET_FILE:glibre-foryc>"
            --file "${plugin_fory_path}"
            --out "${_gen_dir}"
            --stamp "${_stamp}"
            --emit=manifest
        DEPENDS glibre-foryc "${plugin_fory_path}"
        COMMENT "glibre-foryc: emitting manifest.cpp for ${target}"
        VERBATIM
    )

    add_custom_target(${target}-manifest-gen
        DEPENDS "${_manifest_out}" "${_stamp}"
    )

    add_dependencies(${target} ${target}-manifest-gen)
    target_sources(${target} PRIVATE "${_manifest_out}")

    message(STATUS
        "glibre_emit_plugin_manifest: wired manifest codegen for ${target} "
        "from ${plugin_fory_path}")
endfunction()
