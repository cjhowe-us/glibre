cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/type_registry — unit tests for glibre::core::TypeRegistry
#
# Plan: #597 — feat(core): TypeRegistry immutable-after-init
# Authority: specs/core/SPEC.md §4.9, §6.9;
#            core/include/glibre/core/type_registry.hpp
#
# Named test cases (plan #597 Unit Test Plan):
#   - core/type_registry: lookup_unregistered_yields_TypeUnregistered
#   - core/type_registry: register_after_seal_yields_TypeRegistryClosed
#   - core/type_registry: register_during_plugin_register_admitted
#   - core/type_registry: lookup_index_based_no_string_no_reflection  (renamed from lookup_o1_*)
#   - core/type_registry: bootstrap_from_static_init_populates_count
# ---------------------------------------------------------------------------

add_executable(glibre-core-type-registry-tests
    type_registry_test.cpp
)

target_link_libraries(glibre-core-type-registry-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
        glibre::test_contract
)

target_compile_features(glibre-core-type-registry-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-type-registry-tests PROPERTIES CXX_MODULE_STD OFF)

# -fno-exceptions clean per reviews/decisions/error-model.md §Decision 3.
# Catch2 3.x supports -fno-exceptions when REQUIRE_THROWS is not used;
# this test file uses REQUIRE / CHECK only.
target_compile_options(glibre-core-type-registry-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # -Wno-c2y-extensions is inherited from glibre::test_contract (cmake/Compile.cmake).
)

# GLIBRE_TESTING is propagated PUBLIC from glibre-core when GLIBRE_BUILD_TESTS=ON,
# so no PRIVATE definition is needed here.

include(CTest)
include(Catch)
catch_discover_tests(glibre-core-type-registry-tests)
