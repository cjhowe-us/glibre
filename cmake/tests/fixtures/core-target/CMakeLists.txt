cmake_minimum_required(VERSION 3.30)
project(glibre_test_core_target LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${GLIBRE_CMAKE_DIR}")
include(Compile)

# Simulate a core engine target.
add_library(glibre-core-stub INTERFACE)
target_link_libraries(glibre-core-stub INTERFACE glibre::compile_contract)

# A concrete "engine" target that links glibre-core-stub.
add_library(engine_module INTERFACE)
target_link_libraries(engine_module INTERFACE glibre-core-stub)

# --- Assertion ---
# After all targets are defined, inspect INTERFACE_COMPILE_OPTIONS of
# glibre::compile_contract to confirm the no-exceptions flags are present.
get_target_property(_opts glibre_compile_contract INTERFACE_COMPILE_OPTIONS)
message(STATUS "glibre_compile_contract INTERFACE_COMPILE_OPTIONS: ${_opts}")

if(NOT "-fno-exceptions" IN_LIST _opts)
    message(FATAL_ERROR
        "ASSERTION FAILED: -fno-exceptions not found in "
        "glibre_compile_contract INTERFACE_COMPILE_OPTIONS. "
        "Got: ${_opts}"
    )
endif()

if(NOT "-fno-rtti" IN_LIST _opts)
    message(FATAL_ERROR
        "ASSERTION FAILED: -fno-rtti not found in "
        "glibre_compile_contract INTERFACE_COMPILE_OPTIONS. "
        "Got: ${_opts}"
    )
endif()

message(STATUS "PASS: -fno-exceptions and -fno-rtti present on core compile contract")
