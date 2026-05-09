cmake_minimum_required(VERSION 3.30)
project(glibre_test_engine_roots_rejects_exception_root LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${GLIBRE_CMAKE_DIR}")
include(Compile)

# ---------------------------------------------------------------------------
# Fixture: engine-roots-property-rejects-exception-root
#
# Negative-path coverage for plan #1005 / #237 §"Register engine roots".
#
# Registers two mock engine roots, then marks one with
# GLIBRE_USES_EXCEPTIONS=TRUE.  The deferred
# _glibre_run_core_exception_check() must fire FATAL_ERROR because a
# registered root carries the exceptions flag — even if it is not reachable
# via another root's link interface.
#
# Distinct from engine-reaches-exception-target: in that fixture the
# exception-bearing target (bad_exception_lib) is NOT in GLIBRE_ENGINE_ROOTS
# but IS reachable via glibre-core's INTERFACE link; here the exception-
# bearing target (mock_root_exc) IS directly in GLIBRE_ENGINE_ROOTS but is
# NOT linked by any other registered root.  The two fixtures together cover
# both reachability axes of the deferred check.
#
# CTest marks this test WILL_FAIL TRUE so that CMake's non-zero exit
# maps to CTest PASS.
# ---------------------------------------------------------------------------

# Root A: clean engine root — no exceptions.
add_library(mock_root_clean INTERFACE)
target_link_libraries(mock_root_clean INTERFACE glibre::compile_contract)
glibre_register_engine_root(mock_root_clean)

# Root B: engine root that erroneously opts into exceptions.
add_library(mock_root_exc INTERFACE)
glibre_target_exceptions(mock_root_exc)
glibre_register_engine_root(mock_root_exc)

# The deferred check walks both roots.  mock_root_exc has
# GLIBRE_USES_EXCEPTIONS=TRUE so configure must abort with FATAL_ERROR.
