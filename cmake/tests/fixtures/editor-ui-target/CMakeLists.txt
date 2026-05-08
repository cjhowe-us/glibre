cmake_minimum_required(VERSION 3.30)
project(glibre_test_editor_ui_target LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${GLIBRE_CMAKE_DIR}")
include(Compile)

# Simulate an editor UI target that opts into exceptions.
add_library(editor_ui_stub INTERFACE)
glibre_target_exceptions(editor_ui_stub)

# --- Assertions ---

# 1. GLIBRE_USES_EXCEPTIONS property must be set.
get_target_property(_uses_exc editor_ui_stub GLIBRE_USES_EXCEPTIONS)
if(NOT _uses_exc)
    message(FATAL_ERROR
        "ASSERTION FAILED: editor_ui_stub does not have "
        "GLIBRE_USES_EXCEPTIONS property set after glibre_target_exceptions()."
    )
endif()
message(STATUS "PASS: GLIBRE_USES_EXCEPTIONS is TRUE on editor_ui_stub")

# 2. The compile options must include -fexceptions (not -fno-exceptions).
#    For an INTERFACE library the property is INTERFACE_COMPILE_OPTIONS;
#    for compiled libraries it is COMPILE_OPTIONS.  Check the correct one.
get_target_property(_target_type editor_ui_stub TYPE)
if(_target_type STREQUAL "INTERFACE_LIBRARY")
    get_target_property(_opts editor_ui_stub INTERFACE_COMPILE_OPTIONS)
else()
    get_target_property(_opts editor_ui_stub COMPILE_OPTIONS)
endif()
message(STATUS "editor_ui_stub compile options (${_target_type}): ${_opts}")

if("-fno-exceptions" IN_LIST _opts)
    message(FATAL_ERROR
        "ASSERTION FAILED: editor_ui_stub has -fno-exceptions in compile options "
        "after glibre_target_exceptions() — the flag should not be present. "
        "Got: ${_opts}"
    )
endif()

if(NOT "-fexceptions" IN_LIST _opts)
    message(FATAL_ERROR
        "ASSERTION FAILED: editor_ui_stub lacks -fexceptions in compile options "
        "after glibre_target_exceptions(). "
        "Got: ${_opts}"
    )
endif()

message(STATUS "PASS: editor_ui_stub has -fexceptions and lacks -fno-exceptions")
