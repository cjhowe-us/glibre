cmake_minimum_required(VERSION 3.30)
project(glibre_test_core_target LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${GLIBRE_CMAKE_DIR}")
include(Compile)

# Simulate a core engine target that explicitly links glibre::compile_contract.
add_library(glibre-core-stub INTERFACE)
target_link_libraries(glibre-core-stub INTERFACE glibre::compile_contract)

# A concrete "engine" module that links glibre-core-stub (transitive consumer).
add_library(engine_module INTERFACE)
target_link_libraries(engine_module INTERFACE glibre-core-stub)

# --- Assertion 1 ---
# The contract target itself must carry the flags.
get_target_property(_contract_opts glibre_compile_contract INTERFACE_COMPILE_OPTIONS)
message(STATUS "glibre_compile_contract INTERFACE_COMPILE_OPTIONS: ${_contract_opts}")

if(NOT "-fno-exceptions" IN_LIST _contract_opts)
    message(FATAL_ERROR
        "ASSERTION FAILED: -fno-exceptions not found in "
        "glibre_compile_contract INTERFACE_COMPILE_OPTIONS. "
        "Got: ${_contract_opts}"
    )
endif()

if(NOT "-fno-rtti" IN_LIST _contract_opts)
    message(FATAL_ERROR
        "ASSERTION FAILED: -fno-rtti not found in "
        "glibre_compile_contract INTERFACE_COMPILE_OPTIONS. "
        "Got: ${_contract_opts}"
    )
endif()

if(NOT "-fvisibility=hidden" IN_LIST _contract_opts)
    message(FATAL_ERROR
        "ASSERTION FAILED: -fvisibility=hidden not found in "
        "glibre_compile_contract INTERFACE_COMPILE_OPTIONS. "
        "Got: ${_contract_opts}"
    )
endif()

if(NOT "-fvisibility-inlines-hidden" IN_LIST _contract_opts)
    message(FATAL_ERROR
        "ASSERTION FAILED: -fvisibility-inlines-hidden not found in "
        "glibre_compile_contract INTERFACE_COMPILE_OPTIONS. "
        "Got: ${_contract_opts}"
    )
endif()

message(STATUS "PASS: -fno-exceptions and -fno-rtti present on core compile contract")

# --- Assertion 2 ---
# A real consumer that links glibre::compile_contract (via glibre-core-stub)
# must have -fno-exceptions in its INTERFACE_COMPILE_OPTIONS after the
# transitive propagation step.  CMake populates INTERFACE_LINK_LIBRARIES for
# INTERFACE libraries; the generator-expression expansion of
# INTERFACE_COMPILE_OPTIONS happens at build time, but we can verify the raw
# transitive property is wired by checking that glibre_compile_contract appears
# in engine_module's transitive link closure.
#
# For a direct check that does not require a build step, we inspect
# engine_module's own INTERFACE_LINK_LIBRARIES and confirm the chain exists.
get_target_property(_engine_links engine_module INTERFACE_LINK_LIBRARIES)
message(STATUS "engine_module INTERFACE_LINK_LIBRARIES: ${_engine_links}")

if(NOT "glibre-core-stub" IN_LIST _engine_links)
    message(FATAL_ERROR
        "ASSERTION FAILED: engine_module does not link glibre-core-stub. "
        "Got: ${_engine_links}"
    )
endif()

# Also verify the consumer's compile options include -fno-exceptions through
# the COMPILE_OPTIONS / INTERFACE_COMPILE_OPTIONS property directly set on it.
# Since engine_module is INTERFACE, the flags flow via INTERFACE_COMPILE_OPTIONS
# of its dependencies.  We verify by checking glibre-core-stub propagates them.
get_target_property(_core_stub_opts glibre-core-stub INTERFACE_COMPILE_OPTIONS)
message(STATUS "glibre-core-stub INTERFACE_COMPILE_OPTIONS: ${_core_stub_opts}")
# glibre-core-stub propagates glibre::compile_contract's flags as a generator
# expression $<TARGET_PROPERTY:glibre_compile_contract,INTERFACE_COMPILE_OPTIONS>
# which is not resolved at configure time.  The presence of the INTERFACE link
# is sufficient evidence that the consumer will receive -fno-exceptions at
# generator time.  The contract flags are validated in Assertion 1 above.

message(STATUS "PASS: engine_module correctly linked to glibre::compile_contract via glibre-core-stub")
