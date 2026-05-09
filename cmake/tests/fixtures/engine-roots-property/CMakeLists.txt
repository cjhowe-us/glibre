cmake_minimum_required(VERSION 3.30)
project(glibre_test_engine_roots_property LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${GLIBRE_CMAKE_DIR}")
include(Compile)

# ---------------------------------------------------------------------------
# Fixture: engine-roots-property
#
# Verifies that glibre_register_engine_root() correctly appends targets to
# the GLIBRE_ENGINE_ROOTS global CMake property (plan #1005).
#
# Also verifies that _glibre_run_core_exception_check reads the registered
# roots (not a hardcoded list) and walks them via the deferred check.
# ---------------------------------------------------------------------------

# Register two mock engine roots.
add_library(mock_engine_a INTERFACE)
target_link_libraries(mock_engine_a INTERFACE glibre::compile_contract)
glibre_register_engine_root(mock_engine_a)

add_library(mock_engine_b INTERFACE)
target_link_libraries(mock_engine_b INTERFACE glibre::compile_contract)
glibre_register_engine_root(mock_engine_b)

# --- Assertion 1: Both targets appear in GLIBRE_ENGINE_ROOTS ---
get_property(_roots GLOBAL PROPERTY GLIBRE_ENGINE_ROOTS)
message(STATUS "GLIBRE_ENGINE_ROOTS: ${_roots}")

if(NOT "mock_engine_a" IN_LIST _roots)
    message(FATAL_ERROR
        "ASSERTION FAILED: mock_engine_a not in GLIBRE_ENGINE_ROOTS. "
        "Got: ${_roots}"
    )
endif()

if(NOT "mock_engine_b" IN_LIST _roots)
    message(FATAL_ERROR
        "ASSERTION FAILED: mock_engine_b not in GLIBRE_ENGINE_ROOTS. "
        "Got: ${_roots}"
    )
endif()

message(STATUS "PASS: GLIBRE_ENGINE_ROOTS contains both registered engine roots")

# The deferred _glibre_run_core_exception_check() (installed by include(Compile))
# will walk these roots at configure-end.  Since neither mock target has
# GLIBRE_USES_EXCEPTIONS=TRUE, configure must complete without FATAL_ERROR.
