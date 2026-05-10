cmake_minimum_required(VERSION 3.30)

# ---------------------------------------------------------------------------
# glibre-core — minimal core hub
#
# Hosts the engine-wide runtime that every plugin links against:
# ECS, plugin loader, hot-reload barrier, frame loop, type registry,
# asset handles (per core/SPEC.md §1).
#
# This initial CMakeLists.txt wires the frame-loop skeleton (plan #244).
# Additional source files are added as subsequent plans land.
#
# NOTE: glibre-core is a macOS-host library only. On iOS subproject builds
# (CMAKE_SYSTEM_NAME=iOS), this entire target is skipped. iOS frameworks
# link statically against the iOS-built static libs (glibre_ios_libs target
# from cmake/AppleApps.cmake) without requiring core/CMakeLists.txt to run.
# See PR #948 for the iOS skip pattern precedent.
# ---------------------------------------------------------------------------

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    return()
endif()

add_library(glibre-core STATIC
    src/frame_loop.cpp
    src/phase_registry.cpp           # PhaseRegistry: per-phase system registration + dispatch (plan #245)
    src/plugin_manifest.cpp
    src/plugin_loader.cpp            # PluginLoader: dlopen+dlsym+manifest read (plan #229)
    src/plugin_loader_registry.cpp   # PluginLoaderRegistry: ABI+version+name+deps gates (plan #230)
    src/plugin_loader_actions.cpp    # Loader-procedure free functions: steps 9–11 (plan #231)
    src/context_tag_resolver.cpp     # Pure string→ContextTag map (SRP unit, plan #989, MED-2 round-2)
    src/log_error.cpp                # Structured error logging helper (plan #236)
    src/alloc/transient_arena.cpp    # Per-context transient bump-pointer arena (plan #239)
    src/alloc/per_context_allocator.cpp  # Tag-tracked ceiling-enforced heap allocator (plan #238)
    src/perf_budget.cpp              # Per-context perf-budget counters (plan #241)
    src/type_registry/type_registry.cpp  # TypeRegistry immutable-after-init lookup (plan #597)
    src/world/entity_allocator.cpp       # Generational entity allocator (plan #557)
    src/asset/asset_registry.cpp         # AssetRegistry process-wide singleton (plan #598)
)

# spdlog — structured logging.  Used by log_error.cpp (plan #236).
# Linked PUBLIC so consumers of glibre::core can call log_error() without a
# separate find_package(spdlog) in their own CMakeLists.txt.
find_package(spdlog CONFIG REQUIRED)

target_include_directories(glibre-core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(glibre-core
    PUBLIC
        spdlog::spdlog
    PRIVATE
        # Compile-flag contract: enforces -fno-exceptions, -fno-rtti,
        # -fvisibility=hidden project-wide (cmake/Compile.cmake).
        glibre::compile_contract
)

target_compile_features(glibre-core PUBLIC cxx_std_23)

# Do not require `import std;` — frame-loop skeleton uses traditional headers.
# CXX_MODULE_STD is set by the root CMAKE_CXX_MODULE_STD=ON; override to OFF
# here since this target uses #include-based C++23, not module import std.
set_target_properties(glibre-core PROPERTIES CXX_MODULE_STD OFF)

# Warning flags for glibre-core.
# -fno-exceptions, -fno-rtti, and -fvisibility=hidden are provided transitively
# via glibre::compile_contract (cmake/Compile.cmake) — do not duplicate them here.
# The editor-UI carve-out (ImGui) is in tools/, not here.
target_compile_options(glibre-core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # Promote -Wswitch to an error so hand-written to_string() and tag_string()
    # completeness is enforced at build time, not just by runtime unit tests.
    # This is the guarantee described in log_error.hpp §"to_string design choice".
    -Werror=switch
)

# ---------------------------------------------------------------------------
# GLIBRE_ALLOC_STRICT — hard ceiling enforcement in diagnostic/debug builds
#
# perf-budget.md §Allocator Rules #2: "when GLIBRE_ALLOC_STRICT=1, the
# allocator tracks live bytes per ContextTag and returns OutOfBudget when an
# allocation would push the tag over its cell ceiling."
#
# Wired on Debug builds; shipping builds compile with GLIBRE_ALLOC_STRICT=0
# (soft-warning path) per Allocator Rules #3.
#
# Note: the per_context_allocator unit-test CMakeLists also sets
# GLIBRE_ALLOC_STRICT=1 explicitly so that all test-suite builds exercise
# the strict-mode path regardless of CMAKE_BUILD_TYPE.
# ---------------------------------------------------------------------------
target_compile_definitions(glibre-core PUBLIC
    $<$<CONFIG:Debug>:GLIBRE_ALLOC_STRICT=1>
    $<$<NOT:$<CONFIG:Debug>>:GLIBRE_ALLOC_STRICT=0>
)

# ---------------------------------------------------------------------------
# GLIBRE_TESTING — propagate test-instrumentation macro to library TU
#
# The GLIBRE_TESTING macro gates instrumentation hooks in the library
# (e.g. FrameLoop::last_tick_phase_ordinals_) that are required by unit
# tests but must not ship in production binaries.  Instrumentation APIs
# need to be compiled INTO the library (not just the test executable)
# because the header declarations and implementation are in the library TU.
#
# Gated on the existing root-level GLIBRE_BUILD_TESTS option (default ON,
# set at the top-level CMakeLists.txt).  When tests are built, the library
# must also compile the instrumented paths; when tests are disabled (e.g.
# shipping presets or iOS), the instrumented code is compiled out:
#   (a) glibre-core itself compiles the instrumented paths when tests run, and
#   (b) consumers that link glibre::core (i.e. the test executable) receive
#       GLIBRE_TESTING=1 transitively — no PRIVATE definition needed on the
#       test target.
#
# Shipping builds (macos-release, ios-*, ios-sim-*) do not set
# GLIBRE_BUILD_TESTS=ON; the option defaults OFF for those presets so the
# instrumented code paths are compiled out in all non-test configurations.
#
# Refs: plan #997, PR #999 (preceding [!shouldfail] workaround, reverted by
# this PR), reviews/decisions/perf-budget.md §Allocator Rules #4.
# ---------------------------------------------------------------------------
if(GLIBRE_BUILD_TESTS)
    target_compile_definitions(glibre-core PUBLIC GLIBRE_TESTING=1)
endif()

# ---------------------------------------------------------------------------
# Alias for consistent glibre:: namespace usage by consumers
# ---------------------------------------------------------------------------
add_library(glibre::core ALIAS glibre-core)

# Self-register into the engine roots list so the deferred configure-time
# reachability check (cmake/Compile.cmake §4) covers this target without
# requiring a manually maintained hardcoded list.  See plan #1005.
glibre_register_engine_root(glibre-core)
