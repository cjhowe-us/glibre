cmake_minimum_required(VERSION 3.30)

# ---------------------------------------------------------------------------
# tests/core — unit tests for glibre::core
# ---------------------------------------------------------------------------
# GLIBRE_TESTING=1 is propagated PUBLIC from glibre-core when GLIBRE_BUILD_TESTS
# is ON (the same option that enables this subdirectory at the root level), so
# every test target in this tree receives GLIBRE_TESTING=1 transitively via
# target_link_libraries(... glibre::core).  No separate guard is needed — the
# root CMakeLists.txt never adds tests/ when GLIBRE_BUILD_TESTS is OFF.
# ---------------------------------------------------------------------------

add_subdirectory(plugin_api)
add_subdirectory(plugin_loader)
add_subdirectory(plugin_loader_registry)         # plan #230 — ABI hash + version + name + deps gates; plan #981 — phase-8 drain guard
add_subdirectory(plugin_loader_register)         # plan #231 — register call + schedule rebuild + migrate
add_subdirectory(error)                          # plan #235 — GLIBRE_TRY macro
add_subdirectory(plugin_loader_integration)      # plan #232 — end-to-end failure-mode coverage
add_subdirectory(error_register)                 # plan #234 — per-context Error enum registration convention
add_subdirectory(error_propagation)             # plan #240 — C-ABI error-propagation contract tests
add_subdirectory(error_variant)                 # plan #233 — Error variant shape + Result<T> alias contract
add_subdirectory(per_context_allocator)         # plan #238 — PerContextAllocator with per-tag heap ceiling
add_subdirectory(perf_budget)                   # plan #241 — per-context perf-budget counters
add_subdirectory(hot_reload)                    # plan #250 — hot-reload manifest + ABI hash gate
add_subdirectory(frame_loop_integration)         # plan #248 — frame-loop nine-phase ordering integration test
add_subdirectory(perf_s1_fixture)               # plan #1003 — canonical S1 fixture loader + invariant tests
add_subdirectory(world)                         # plan #944 — macOS PMC sampler helper
add_subdirectory(phase_registry)                # plan #245 — phase ownership + system register API
add_subdirectory(type_registry)                 # plan #597 — TypeRegistry immutable-after-init
add_subdirectory(entity)                        # plan #557 — Entity value object encoding
add_subdirectory(entity_allocator)              # plan #557 — generational entity allocator

add_executable(glibre-core-tests
    frame_loop_test.cpp
    plugin_manifest/plugin_manifest_test.cpp
    log_error_test.cpp          # structured error logging helper (plan #236)
    transient_arena_test.cpp    # per-context transient arena (plan #239)
)

target_link_libraries(glibre-core-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
        glibre::compile_contract
)

target_compile_features(glibre-core-tests PRIVATE cxx_std_23)

# Same as glibre-core: no `import std;` in test files.
set_target_properties(glibre-core-tests PROPERTIES CXX_MODULE_STD OFF)

# -fno-exceptions and -fno-rtti are provided via glibre::compile_contract.
# Catch2 3.x supports -fno-exceptions builds when CATCH_CONFIG_DISABLE_EXCEPTIONS
# is defined (see Catch2 docs §"Compile-time configuration").
# LOW-9 review finding: Catch2 internals are compatible with -fno-exceptions when
# REQUIRE_THROWS is not used; glibre-core-tests never uses REQUIRE_THROWS.
target_compile_options(glibre-core-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # Catch2 3.x macros use __COUNTER__ which triggers -Wpedantic C2y
    # extension warning with clang >= 22; suppress from third-party macro.
    -Wno-c2y-extensions
)

# GLIBRE_TESTING is now propagated PUBLIC from glibre-core (core/CMakeLists.txt)
# when GLIBRE_BUILD_TESTS=ON (the existing root-level option), so
# glibre-core-tests receives it transitively via target_link_libraries(glibre::core).
# No PRIVATE definition needed here.
# See plan #997 for the fix history (was previously PRIVATE-only on the test
# executable, causing the library TU to compile without the instrumentation paths).

include(CTest)
include(Catch)
catch_discover_tests(glibre-core-tests)
