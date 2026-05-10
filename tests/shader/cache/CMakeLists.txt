cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/shader/cache — unit tests for BLAKE3 hasher + CAS store
#
# Plan: #516 — feat(shader): BLAKE3 ShaderHash + CAS store.
# Authority: specs/shader/SPEC.md §4.6, §7.5.
#
# Named test cases (plan #516 Unit Test Plan + DoD):
#   - shader/cache: blake3_hash_is_deterministic_across_repeated_calls
#   - shader/cache: blake3_hash_input_composition_matches_spec_section_2
#   - shader/cache: cas_store_insert_if_absent_is_idempotent
#   - shader/cache: cas_store_path_layout_matches_two_byte_prefix_sharding
#   - shader/cache: cas_store_get_returns_byte_equal_payload_for_inserted_hash
#   - shader/cache: cas_store_returns_CacheCorrupt_on_blake3_self_check_failure
#   - shader/cache: cas_store_get_returns_CacheCorrupt_when_stored_content_hash_mismatches_key
#   - shader/cache: cas_store_insert_if_absent_returns_CacheIntegrity_when_caller_hash_mismatches_data
#   - shader/cache: cas_store_insert_if_absent_returns_CacheCorrupt_when_existing_entry_content_mismatches_key
# ---------------------------------------------------------------------------

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    return()
endif()

add_executable(glibre-shader-cache-tests
    shader_cache_test.cpp
)

target_link_libraries(glibre-shader-cache-tests
    PRIVATE
        glibre::shader
        Catch2::Catch2WithMain
        glibre::compile_contract
)

target_compile_features(glibre-shader-cache-tests PRIVATE cxx_std_23)

set_target_properties(glibre-shader-cache-tests PROPERTIES CXX_MODULE_STD OFF)

target_compile_options(glibre-shader-cache-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # Catch2 3.x macros use __COUNTER__ which triggers -Wpedantic C2y
    # extension warning with clang >= 22; suppress from third-party macro.
    -Wno-c2y-extensions
)

# Expose the cache internal headers to the test (blake3.hpp, cas_store.hpp).
# These are private to the shader plugin; the test is the sole consumer outside
# the plugin itself.
target_include_directories(glibre-shader-cache-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/plugins/shader/src/cache
)

include(CTest)
include(Catch)
catch_discover_tests(glibre-shader-cache-tests)
