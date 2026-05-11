cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/shader/permutation — unit tests for PermutationKey codec,
# PermutationIndex bijection, and EnumerationTable cross-product walker.
#
# Plan: #509 — feat(shader/permutation): PermutationKey codec + cross-product enumerator.
# Authority: specs/shader/SPEC.md §4.2.
#
# Named test cases (plan #509 Unit Test Plan + DoD):
#   - shader/permutation: permutation_key_encoding_is_total_injective_and_bit_stable
#   - shader/permutation: permutation_key_from_bytes_rejects_out_of_range_with_PermutationKeyMalformed
#   - shader/permutation: permutation_index_ordering_matches_tuple_field_ascending_order
#   - shader/permutation: permutation_index_round_trips_through_key_bijectively
#   - shader/permutation: enumeration_table_yields_full_cross_product_in_canonical_order
#   - shader/permutation: feature_set_bit_positions_are_stable_across_revs
# ---------------------------------------------------------------------------

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    return()
endif()

add_executable(glibre-shader-permutation-tests
    permutation_test.cpp
)

target_link_libraries(glibre-shader-permutation-tests
    PRIVATE
        glibre::shader
        Catch2::Catch2WithMain
        glibre::compile_contract
)

target_compile_features(glibre-shader-permutation-tests PRIVATE cxx_std_23)

set_target_properties(glibre-shader-permutation-tests PROPERTIES CXX_MODULE_STD OFF)

target_compile_options(glibre-shader-permutation-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # Catch2 3.x macros use __COUNTER__ which triggers -Wpedantic C2y
    # extension warning with clang >= 22; suppress from third-party macro.
    -Wno-c2y-extensions
)

# Expose the permutation internal headers to the test.
# (axes.hpp, packed_key.hpp, permutation_index.hpp, enumeration_table.hpp)
# These are private to the shader plugin; the test is the sole consumer outside
# the plugin itself.
target_include_directories(glibre-shader-permutation-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/plugins/shader/src/permutation
)

include(CTest)
include(Catch)
catch_discover_tests(glibre-shader-permutation-tests)
