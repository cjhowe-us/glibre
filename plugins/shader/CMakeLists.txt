cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# plugins/shader — Shader bounded-context plugin (glibre-shader)
#
# Plan: #508 — ShaderSource open + include resolver + entry-point scanner.
# Authority: specs/shader/SPEC.md §4.1.
#
# NOTE: This is a macOS-host-only context.  On iOS sub-project builds
# (CMAKE_SYSTEM_NAME=iOS), this entire target is skipped.  See
# data/CMakeLists.txt for the iOS skip pattern precedent.
# ---------------------------------------------------------------------------

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    return()
endif()

find_package(blake3 CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# glibre-shader STATIC library (not yet a SHARED plugin dylib — that lands
# when the plugin ABI boundary is wired in a later plan).
#
# Sources:
#   - src/source/shader_source.cpp    — ShaderSource aggregate root (§4.1)
#   - src/source/preprocessor.cpp     — #include expander
#   - src/source/include_resolver.cpp — project-rooted path validator
#   - src/source/entry_point_scanner.cpp — [shader("...")] attribute scanner
# ---------------------------------------------------------------------------

add_library(glibre-shader STATIC
    src/source/shader_source.cpp
    src/source/preprocessor.cpp
    src/source/include_resolver.cpp
    src/source/entry_point_scanner.cpp
    src/descriptor/descriptor_layout.cpp  # plan #1087 — DescriptorLayout::derive
    src/cache/blake3.cpp                  # plan #516 — BLAKE3 hasher (ShaderHash)
    src/cache/cas_store.cpp               # plan #516 — CAS filesystem store (§4.6, §7.5)
    # plan #514 — slangc reflection record ingester (§4.4, §6.3)
    src/reflection/slangc_reflection_record.cpp
    src/reflection/blob_builder.cpp
)

# Public include path: plugins/shader/include (exposes glibre/shader/shader.hpp
# and glibre/shader/shader_source.hpp to consumers).
# Core include path: core/include (for glibre/error.hpp, which declares
# glibre::shader::Error in the engine-wide variant).
target_include_directories(glibre-shader
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        # Internal source headers (preprocessor.hpp, include_resolver.hpp,
        # entry_point_scanner.hpp) are in src/source/ — not on the public path.
        ${CMAKE_CURRENT_SOURCE_DIR}/src/source
        # Internal cache headers (blake3.hpp, cas_store.hpp) — plan #516.
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cache
        # Internal reflection headers (slangc_reflection_record.hpp,
        # blob_builder.hpp) — plan #514.
        ${CMAKE_CURRENT_SOURCE_DIR}/src/reflection
)

target_link_libraries(glibre-shader
    PUBLIC
        glibre::core          # error.hpp, GLIBRE_TRY, glibre::Result<T>
    PRIVATE
        BLAKE3::blake3        # BLAKE3 content hashing (SPEC §4.1)
        # Compile-flag contract: enforces -fno-exceptions, -fno-rtti,
        # -fvisibility=hidden project-wide (cmake/Compile.cmake).
        glibre::compile_contract
)

target_compile_features(glibre-shader PUBLIC cxx_std_23)

# No CXX_MODULE_STD (no `import std;` in this translation unit).
set_target_properties(glibre-shader PROPERTIES CXX_MODULE_STD OFF)

# Warning flags for glibre-shader.
# -fno-exceptions, -fno-rtti, and -fvisibility=hidden are provided transitively
# via glibre::compile_contract (cmake/Compile.cmake) — do not duplicate them here.
target_compile_options(glibre-shader PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Werror=switch    # Catches missing to_string arms per error-model.md §Logging
    # Catch2 3.x macros use __COUNTER__ which triggers -Wpedantic C2y extension
    # warning with clang >= 22; suppress from third-party macro.
    -Wno-c2y-extensions
)

add_library(glibre::shader ALIAS glibre-shader)

# Self-register into the engine roots list so the deferred configure-time
# reachability check (cmake/Compile.cmake §4) covers this target without
# requiring a manually maintained hardcoded list.  See plan #1005.
glibre_register_engine_root(glibre-shader)
