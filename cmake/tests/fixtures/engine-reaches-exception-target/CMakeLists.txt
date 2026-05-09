cmake_minimum_required(VERSION 3.30)
project(glibre_test_engine_reaches_exception LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${GLIBRE_CMAKE_DIR}")
include(Compile)

# Deliberately-misconfigured fixture:
# An exception-using target is linked into glibre-core's public interface.
# CMake configure should FATAL_ERROR before returning.

# Simulate an editor-UI-like target that opts into exceptions.
add_library(bad_exception_lib INTERFACE)
glibre_target_exceptions(bad_exception_lib)

# glibre-core that INCORRECTLY depends on an exception-enabled target.
add_library(glibre-core INTERFACE)
target_link_libraries(glibre-core INTERFACE bad_exception_lib)

# The deferred check in Compile.cmake will fire here and issue FATAL_ERROR.
# cmake_language(DEFER CALL _glibre_run_core_exception_check) was installed
# by include(Compile) above — it will run at the end of this configure pass.
