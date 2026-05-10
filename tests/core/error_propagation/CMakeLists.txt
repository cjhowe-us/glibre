cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/error_propagation — C-ABI boundary contract tests (plan #240)
#
# Authority: reviews/decisions/error-model.md §Decision 1 ("Every public
# boundary returns std::expected<T, glibre::Error>"), §Decision 3
# ("-fno-exceptions on engine code"), §Composition Rules.
#
# Named test cases (plan #240 Unit Test Plan + DoD):
#   - c_abi_boundary_returns_error_code_no_exception
#   - error_propagation_preserves_error_context
#   - terminating_exception_aborts_plugin_load
#
# Stub dylib:
#   glibre-stub-error-returns — small C-ABI dylib that constructs
#   glibre::Result<void> and glibre::Error internally and exposes the
#   results as plain C integers / POD structs.  Compiled with -fno-exceptions
#   so any throw expression is compiled as std::terminate.
#
# The stub is NOT a plugin (does not export glibre_plugin_register etc.).
# It is a minimal ABI-boundary fixture for these contract tests only.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# glibre-stub-error-returns — C-ABI stub for the contract tests
#
# Does NOT link glibre-core — the stub represents a plugin dylib that only
# sees the header-only glibre::Error type (std::string_view + std::variant,
# libc++ stdlib only per reviews/decisions/eastl-removal.md §4).
#
# glibre::compile_contract supplies -fno-exceptions/-fno-rtti and visibility
# flags.  No EASTL dependency: stub_error_returns.cpp was migrated to
# libc++ stdlib by plan #1049 (eastl-removal.md §4 migration sweep).
# ---------------------------------------------------------------------------

add_library(glibre-stub-error-returns MODULE
    stub_error_returns.cpp
)

target_include_directories(glibre-stub-error-returns
    PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
)

target_link_libraries(glibre-stub-error-returns PRIVATE glibre::compile_contract)

target_compile_features(glibre-stub-error-returns PRIVATE cxx_std_23)

set_target_properties(glibre-stub-error-returns PROPERTIES
    SUFFIX ".dylib"
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    CXX_MODULE_STD OFF
)

target_compile_options(glibre-stub-error-returns PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # Catch2 3.x macros use __COUNTER__ which triggers -Wpedantic C2y
    # extension warning with clang >= 22; not directly relevant here but
    # kept consistent with the rest of the test sub-trees.
    -Wno-c2y-extensions
)

# ---------------------------------------------------------------------------
# glibre-core-error-propagation-tests — Catch2 contract test binary
# ---------------------------------------------------------------------------

add_executable(glibre-core-error-propagation-tests
    error_propagation_test.cpp
)

target_link_libraries(glibre-core-error-propagation-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
        glibre::compile_contract
)

target_compile_features(glibre-core-error-propagation-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-error-propagation-tests PROPERTIES
    CXX_MODULE_STD OFF
)

# -fno-exceptions and -fno-rtti are provided via glibre::compile_contract.
# The terminating_exception_aborts_plugin_load test case audits at compile
# time (static_assert) that this binary is built without exceptions.
target_compile_options(glibre-core-error-propagation-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Wno-c2y-extensions
)

# ---------------------------------------------------------------------------
# Inject stub dylib path at build time so the test can locate it without
# hardcoding an absolute path.
#
# GLIBRE_STUB_ERROR_RETURNS_DYLIB_PATH is injected unconditionally because
# glibre-stub-error-returns is always built as part of this sub-tree.
# If the CMake configuration somehow omits the target, the test will FAIL
# (not SKIP) to surface the misconfiguration loudly.
# ---------------------------------------------------------------------------

target_compile_definitions(glibre-core-error-propagation-tests PRIVATE
    GLIBRE_STUB_ERROR_RETURNS_DYLIB_PATH="$<TARGET_FILE:glibre-stub-error-returns>"
    # -fno-exceptions turns Catch2 throw-based failure into std::terminate.
    # CATCH_CONFIG_DISABLE_EXCEPTIONS switches Catch2 to its longjmp-based
    # failure path so REQUIRE failures are recorded rather than aborting the runner.
    CATCH_CONFIG_DISABLE_EXCEPTIONS
)

add_dependencies(glibre-core-error-propagation-tests glibre-stub-error-returns)

include(CTest)
include(Catch)
catch_discover_tests(glibre-core-error-propagation-tests)
