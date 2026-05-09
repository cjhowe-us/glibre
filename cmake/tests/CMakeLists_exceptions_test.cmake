# cmake/tests/CMakeLists_exceptions_test.cmake
#
# CTest-driven CMake fixture tests for the -fno-exceptions enforcement
# introduced by cmake/Compile.cmake.
#
# Included from the root CMakeLists.txt when GLIBRE_BUILD_TESTS=ON.
# Each test is a cmake --build-and-test invocation that runs a small fixture
# CMakeLists project and fails if that configure pass exits non-zero (tests 1
# and 2) or requires the configure pass to exit non-zero (test 3).
#
# Named test cases (must match the names in issue #237 Unit Test Plan):
#   ci/cmake: fno_exceptions_set_on_core_targets
#   ci/cmake: editor_ui_target_opts_into_exceptions
#   ci/cmake: configure_fails_if_engine_path_reaches_exception_target

set(_fixture_dir "${CMAKE_SOURCE_DIR}/cmake/tests/fixtures")
set(_glibre_cmake_dir "${CMAKE_SOURCE_DIR}/cmake")

# Reuse the host compiler and generator so fixtures inherit the same toolchain.
set(_cmake_exe "${CMAKE_COMMAND}")
set(_generator  "${CMAKE_GENERATOR}")
set(_cxx_compiler "${CMAKE_CXX_COMPILER}")

# ---------------------------------------------------------------------------
# Test 1: fno_exceptions_set_on_core_targets
#
# Configures cmake/tests/fixtures/core-target which inspects the
# INTERFACE_COMPILE_OPTIONS of glibre_compile_contract and FAILs if
# -fno-exceptions or -fno-rtti is absent.
# ---------------------------------------------------------------------------
add_test(
    NAME "ci/cmake: fno_exceptions_set_on_core_targets"
    COMMAND "${_cmake_exe}"
        -G "${_generator}"
        -S "${_fixture_dir}/core-target"
        -B "${CMAKE_CURRENT_BINARY_DIR}/cmake_test_core_target"
        "-DGLIBRE_CMAKE_DIR=${_glibre_cmake_dir}"
        "-DCMAKE_CXX_COMPILER=${_cxx_compiler}"
)
set_tests_properties("ci/cmake: fno_exceptions_set_on_core_targets" PROPERTIES
    LABELS "cmake;infra"
    PASS_REGULAR_EXPRESSION "PASS: -fno-exceptions and -fno-rtti present"
)

# ---------------------------------------------------------------------------
# Test 2: editor_ui_target_opts_into_exceptions
#
# Configures cmake/tests/fixtures/editor-ui-target which calls
# glibre_target_exceptions() and asserts that:
#   - GLIBRE_USES_EXCEPTIONS is TRUE
#   - -fno-exceptions is absent from COMPILE_OPTIONS
#   - -fexceptions is present in COMPILE_OPTIONS
# ---------------------------------------------------------------------------
add_test(
    NAME "ci/cmake: editor_ui_target_opts_into_exceptions"
    COMMAND "${_cmake_exe}"
        -G "${_generator}"
        -S "${_fixture_dir}/editor-ui-target"
        -B "${CMAKE_CURRENT_BINARY_DIR}/cmake_test_editor_ui_target"
        "-DGLIBRE_CMAKE_DIR=${_glibre_cmake_dir}"
        "-DCMAKE_CXX_COMPILER=${_cxx_compiler}"
)
set_tests_properties("ci/cmake: editor_ui_target_opts_into_exceptions" PROPERTIES
    LABELS "cmake;infra"
    PASS_REGULAR_EXPRESSION "PASS: editor_ui_stub has -fexceptions"
)

# ---------------------------------------------------------------------------
# Test 3: configure_fails_if_engine_path_reaches_exception_target
#
# Configures cmake/tests/fixtures/engine-reaches-exception-target which
# deliberately links an exception-enabled target into glibre-core.
# The Compile.cmake deferred check must make CMake exit non-zero.
# We mark WILL_FAIL TRUE so CTest expects failure and records PASS.
# ---------------------------------------------------------------------------
add_test(
    NAME "ci/cmake: configure_fails_if_engine_path_reaches_exception_target"
    COMMAND "${_cmake_exe}"
        -G "${_generator}"
        -S "${_fixture_dir}/engine-reaches-exception-target"
        -B "${CMAKE_CURRENT_BINARY_DIR}/cmake_test_engine_exception_reach"
        "-DGLIBRE_CMAKE_DIR=${_glibre_cmake_dir}"
        "-DCMAKE_CXX_COMPILER=${_cxx_compiler}"
)
set_tests_properties(
    "ci/cmake: configure_fails_if_engine_path_reaches_exception_target"
    PROPERTIES
        LABELS "cmake;infra"
        WILL_FAIL TRUE
        # Require the specific [glibre] FATAL_ERROR message so that unrelated
        # configure failures (missing compiler, bad generator, etc.) do not
        # silently masquerade as a PASS.
        FAIL_REGULAR_EXPRESSION "\\[glibre\\] Configure error: target '.*' has GLIBRE_USES_EXCEPTIONS=TRUE"
)

# ---------------------------------------------------------------------------
# Test 4: engine_roots_property_self_registration
#
# Configures cmake/tests/fixtures/engine-roots-property which calls
# glibre_register_engine_root() for two mock targets and asserts that
# GLIBRE_ENGINE_ROOTS global property contains both registered targets.
# Introduced by plan #1005 (invert control: self-register engine roots).
# ---------------------------------------------------------------------------
add_test(
    NAME "ci/cmake: engine_roots_property_self_registration"
    COMMAND "${_cmake_exe}"
        -G "${_generator}"
        -S "${_fixture_dir}/engine-roots-property"
        -B "${CMAKE_CURRENT_BINARY_DIR}/cmake_test_engine_roots_property"
        "-DGLIBRE_CMAKE_DIR=${_glibre_cmake_dir}"
        "-DCMAKE_CXX_COMPILER=${_cxx_compiler}"
)
set_tests_properties("ci/cmake: engine_roots_property_self_registration" PROPERTIES
    LABELS "cmake;infra"
    PASS_REGULAR_EXPRESSION "PASS: GLIBRE_ENGINE_ROOTS contains both registered engine roots"
)

# ---------------------------------------------------------------------------
# Test 5: engine_roots_property_rejects_exception_root
#
# Configures cmake/tests/fixtures/engine-roots-property-rejects-exceptions
# which registers two roots and marks one GLIBRE_USES_EXCEPTIONS=TRUE.
# The deferred check must FATAL_ERROR because the exception-flagged root is
# itself in GLIBRE_ENGINE_ROOTS — negative coverage for plan #1005 / #237.
# ---------------------------------------------------------------------------
add_test(
    NAME "ci/cmake: engine_roots_property_rejects_exception_root"
    COMMAND "${_cmake_exe}"
        -G "${_generator}"
        -S "${_fixture_dir}/engine-roots-property-rejects-exceptions"
        -B "${CMAKE_CURRENT_BINARY_DIR}/cmake_test_engine_roots_rejects_exc"
        "-DGLIBRE_CMAKE_DIR=${_glibre_cmake_dir}"
        "-DCMAKE_CXX_COMPILER=${_cxx_compiler}"
)
set_tests_properties(
    "ci/cmake: engine_roots_property_rejects_exception_root"
    PROPERTIES
        LABELS "cmake;infra"
        WILL_FAIL TRUE
        # Require the specific [glibre] FATAL_ERROR so that unrelated configure
        # failures (missing compiler, bad generator, etc.) do not silently
        # masquerade as a PASS.
        FAIL_REGULAR_EXPRESSION "\\[glibre\\] Configure error: target '.*' has GLIBRE_USES_EXCEPTIONS=TRUE"
)
