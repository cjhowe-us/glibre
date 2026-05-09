cmake_minimum_required(VERSION 3.30)

# ---------------------------------------------------------------------------
# data/ — glibre middleman dylib (glibre-types)
#
# NOTE: data/CMakeLists.txt is a macOS-host-only context. On iOS subproject
# builds (CMAKE_SYSTEM_NAME=iOS), this entire target is skipped. The types
# are built separately for iOS frameworks without requiring this CMakeLists
# to run. See PR #994 for the iOS skip pattern precedent (applied first to
# core/CMakeLists.txt).
# ---------------------------------------------------------------------------

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    return()
endif()

# ---------------------------------------------------------------------------
# data/ — glibre middleman dylib (glibre-types)
#
# Authority: reviews/decisions/fory-codegen.md §"CMake Integration" and
#            reviews/decisions/plugin-abi.md §"Plugin file shape" rule 2.
#
# glibre-types.dylib is the ONLY engine-side library that plugins link.
# Plugins must NOT link glibre-core or any other plugin.  Cross-plugin
# communication happens through ECS components and types defined here.
#
# SONAME policy (plugin-abi.md §"Versioning Rules" point 2):
#   - Additive schema change   → bump glibre_types_abi_hash only (plan #222).
#                                SONAME unchanged.  Hash check in loader
#                                catches the smaller hash-change window.
#   - Layout-breaking change   → bump SONAME AND abi_hash.  The dynamic
#                                linker rejects mismatched SONAMEs before
#                                the loader's hash check runs.
#   Never collapse the two axes; they answer different questions.
#
# Build graph:
#   1. glibre-foryc (host tool, plan #219) must exist before
#      glibre-types-codegen runs.  The DEPENDS clause below enforces this.
#   2. glibre-types-codegen runs glibre-foryc over data/schemas/**/*.fory
#      and emits sources into ${CMAKE_BINARY_DIR}/generated/glibre-types/.
#   3. add_library(glibre-types SHARED ...) consumes those generated sources
#      PLUS the permanent anchor TU (data/src/types_anchor.cpp).
#
# Current status (plan #224):
#   glibre-foryc parser skeleton exists (plan #219) but does NOT yet emit
#   C++ headers/sources (that lands in plans #220, #225).  The codegen
#   custom_command is wired below so the build graph is complete, but the
#   GEN_SRCS glob will be empty until plan #220 ships.  types_anchor.cpp
#   ensures glibre-types.dylib can be built and linked before codegen runs.
# ---------------------------------------------------------------------------

find_package(EASTL CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Codegen custom command — invokes glibre-foryc over data/schemas/**/*.fory
#
# Guards:
#   1. if(EXISTS …/data/schemas) — data/schemas/ is tracked via a .gitkeep
#      but the guard prevents breakage if someone manually deletes it.
#      Without this guard, a missing directory causes glibre-foryc to return
#      EXIT_FAILURE, which fails cmake --build even on a clean checkout.
#   2. if(TARGET glibre-foryc) — GLIBRE_BUILD_TOOLS=OFF omits tools/ from
#      the build; the codegen block must not reference a non-existent target.
# ---------------------------------------------------------------------------

set(GEN_DIR "${CMAKE_BINARY_DIR}/generated/glibre-types")

if(TARGET glibre-foryc AND EXISTS "${CMAKE_SOURCE_DIR}/data/schemas")
    file(GLOB_RECURSE GLIBRE_SCHEMAS
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/data/schemas/*.fory"
    )

    # glibre-foryc is built by tools/foryc/CMakeLists.txt (plans #219, #220).
    # It accepts --in <schema-dir> --out <gen-dir> --stamp <stamp-file>
    # and optionally --emit=header (plan #220).
    #
    # --emit=header: writes <GEN_DIR>/include/glibre/types/<stem>.fory.h
    # for each .fory file.  These headers are the foundation for the
    # glibre-types middleman dylib (fory-codegen.md §"Pipeline" step 1).
    #
    # Use $<TARGET_FILE:glibre-foryc> (a generator expression) so the full path
    # to the binary is resolved at build time regardless of the working directory
    # from which Ninja invokes the command.  This avoids "command not found" when
    # glibre-foryc is not on PATH.
    add_custom_command(
        OUTPUT  "${GEN_DIR}/.stamp"
        COMMAND "$<TARGET_FILE:glibre-foryc>"
                --in    "${CMAKE_SOURCE_DIR}/data/schemas"
                --out   "${GEN_DIR}"
                --stamp "${GEN_DIR}/.stamp"
                --emit=header
        DEPENDS glibre-foryc ${GLIBRE_SCHEMAS}
        COMMENT "glibre-foryc: regenerating middleman types (--emit=header)"
        VERBATIM
    )

    add_custom_target(glibre-types-codegen
        DEPENDS "${GEN_DIR}/.stamp"
    )
else()
    # No schemas directory or tools are disabled: create a no-op codegen target
    # so that add_dependencies(glibre-types glibre-types-codegen) below still
    # resolves.  This allows schema-less builds (current state before plans
    # #220/#225 land) and GLIBRE_BUILD_TOOLS=OFF configurations.
    add_custom_target(glibre-types-codegen)
endif()

# ---------------------------------------------------------------------------
# Generated sources glob (populated after codegen runs, plans #220/#225)
# ---------------------------------------------------------------------------

# CONFIGURE_DEPENDS: CMake re-globs on every incremental build; acceptable
# for tens of schemas.  Revisit if schema count climbs past ~200.
file(GLOB_RECURSE GEN_SRCS
    CONFIGURE_DEPENDS
    "${GEN_DIR}/src/*.cpp"
)

# ---------------------------------------------------------------------------
# glibre-types SHARED library
#
# Sources:
#   - data/src/types_anchor.cpp : permanent anchor TU; exports
#     glibre_types_abi_version() so the dylib is non-empty before codegen.
#   - data/src/eastl_alloc.cpp  : EASTL operator new[] substrate; duplicated
#     from core/src/eastl_alloc.cpp so plugins can link glibre-types without
#     linking glibre-core (plugin-abi.md §"Plugin file shape" rule 2).
#   - ${GEN_SRCS}               : generated serialise/deserialise TUs
#     emitted by glibre-foryc (plans #220/#225). Empty until those plans land.
# ---------------------------------------------------------------------------

add_library(glibre-types SHARED
    src/types_anchor.cpp
    src/eastl_alloc.cpp
    ${GEN_SRCS}
)

# Depend on codegen so Ninja rebuilds the generated sources when schemas change.
add_dependencies(glibre-types glibre-types-codegen)

# Core headers (glibre/error.hpp, glibre/core/plugin_manifest.hpp, …) are
# exposed PUBLIC so that consumers of glibre-types get the include path
# without having to separately link glibre-core.
target_include_directories(glibre-types
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/core/include>
        $<BUILD_INTERFACE:${GEN_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# EASTL is a PUBLIC dependency: consumers of glibre-types (plugins) inherit
# the EASTL include path so they can use eastl:: containers in plugin code
# that is type-compatible with types defined here.
target_link_libraries(glibre-types
    PUBLIC
        EASTL
    PRIVATE
        # Compile-flag contract: enforces -fno-exceptions, -fno-rtti,
        # -fvisibility=hidden project-wide (cmake/Compile.cmake).
        glibre::compile_contract
    # Fory::fory and blake3::blake3 are PRIVATE (not exposed on the plugin
    # link surface): plugins must NOT link Fory or BLAKE3 directly — they
    # go through the generated wrapper functions in glibre-types.  These
    # deps are added here as a comment placeholder; they become live when
    # plans #220/#222 land and the generated TUs reference them.
    # PRIVATE
    #     Fory::fory
    #     blake3::blake3
)

target_compile_features(glibre-types PUBLIC cxx_std_23)

# Warning flags for glibre-types.
# -fno-exceptions, -fno-rtti, and -fvisibility=hidden are provided transitively
# via glibre::compile_contract (cmake/Compile.cmake) — do not duplicate them here.
# Only symbols with explicit __attribute__((visibility("default"))) (or extern "C"
# with the attribute in types_anchor.cpp) are visible to dlsym.
target_compile_options(glibre-types PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # -Wno-c2y-extensions: omitted here — neither types_anchor.cpp nor
    # eastl_alloc.cpp use __COUNTER__.  Re-add when generated TUs under
    # ${GEN_SRCS} (plans #220/#225) actually trigger the warning.
)

# SONAME versioning (plugin-abi.md §"Versioning Rules" point 2).
# VERSION  = major.minor.patch library file version.
# SOVERSION = major-only; the dynamic linker uses this as the SONAME.
# Both start at 0.1.0 / 0 for the initial pre-codegen release.
# Bump SOVERSION only on layout-breaking schema changes; bump VERSION on
# every release.  The loader's hash check covers hash-stable window
# changes; dyld SONAME check covers the layout-breaking window.
set_target_properties(glibre-types PROPERTIES
    VERSION   0.1.0
    SOVERSION 0
    CXX_MODULE_STD OFF
)

# ---------------------------------------------------------------------------
# Alias for consumers
# ---------------------------------------------------------------------------

add_library(glibre::types ALIAS glibre-types)

# Self-register into the engine roots list so the deferred configure-time
# reachability check (cmake/Compile.cmake §4) covers this target without
# requiring a manually maintained hardcoded list.  See plan #1005.
glibre_register_engine_root(glibre-types)
