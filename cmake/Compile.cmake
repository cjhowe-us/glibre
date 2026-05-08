# cmake/Compile.cmake
#
# Compile-flag contract for glibre.
#
# RULE: every glibre target compiles with -fno-exceptions -fno-rtti by default.
# The editor UI module (tools/editor/ui) is the only permitted carve-out.
# Use glibre_target_exceptions(target) to opt that ONE target into exception
# mode.  Configuration fails if any exception-enabled target is reachable via
# glibre-core's public link interface.
#
# References: reviews/decisions/error-model.md Decision #3 + Consequences.

# ---------------------------------------------------------------------------
# 1. Interface target carrying the no-exceptions contract.
#    All glibre targets link to this (transitively via glibre-core, or
#    explicitly for standalone plugin stubs).
# ---------------------------------------------------------------------------
if(NOT TARGET glibre::compile_contract)
    add_library(glibre_compile_contract INTERFACE)
    add_library(glibre::compile_contract ALIAS glibre_compile_contract)

    target_compile_options(glibre_compile_contract INTERFACE
        -fno-exceptions
        -fno-rtti
    )
endif()

# ---------------------------------------------------------------------------
# 2. Helper: glibre_target_exceptions(target)
#
#    Opts <target> into exception + RTTI mode for ImGui / third-party interop.
#    Strips the INTERFACE no-exceptions flags inherited from glibre-core
#    (by applying the inverse flags with PUBLIC scope so they override the
#    INTERFACE propagation for this target only), then tags the target with a
#    GLIBRE_USES_EXCEPTIONS custom property so the reachability check can
#    detect it.
#
#    Permitted callers: tools/editor/ui only.  The reachability guard below
#    enforces this at configure time for the engine core path.
# ---------------------------------------------------------------------------
function(glibre_target_exceptions target)
    # Determine whether this is a header-only (INTERFACE) target so we can
    # use the correct scope keyword.  INTERFACE targets only accept INTERFACE
    # scope for compile options; compiled targets use PRIVATE.
    get_target_property(_target_type "${target}" TYPE)
    if(_target_type STREQUAL "INTERFACE_LIBRARY")
        set(_scope INTERFACE)
    else()
        set(_scope PRIVATE)
    endif()

    # Re-enable exception handling and RTTI for this target specifically.
    # The compiler sees -fexceptions after any inherited -fno-exceptions,
    # so the last flag wins (clang / GCC both honour last-flag semantics).
    target_compile_options("${target}" "${_scope}"
        -fexceptions
        -frtti
    )

    # Tag the target so the reachability check can identify it.
    set_target_properties("${target}" PROPERTIES
        GLIBRE_USES_EXCEPTIONS TRUE
    )
endfunction()

# ---------------------------------------------------------------------------
# 3. Configure-time reachability check.
#
#    Walks the transitive link closure of glibre-core (depth-first, cycle-
#    safe) and errors out if any reachable target has GLIBRE_USES_EXCEPTIONS.
#    Called via cmake_language(DEFER CALL ...) so it runs after all
#    subdirectory CMakeLists have been processed and all targets are defined.
#
#    If glibre-core does not exist yet (early-phase builds where subdirs are
#    commented out) the check is a no-op.
# ---------------------------------------------------------------------------
function(_glibre_check_exceptions_reachable_from_core root visited_var)
    # Guard against cycles.
    list(FIND ${visited_var} "${root}" _idx)
    if(NOT _idx EQUAL -1)
        return()
    endif()
    list(APPEND ${visited_var} "${root}")
    set(${visited_var} "${${visited_var}}" PARENT_SCOPE)

    # Check this node.
    if(TARGET "${root}")
        get_target_property(_uses_exc "${root}" GLIBRE_USES_EXCEPTIONS)
        if(_uses_exc)
            message(FATAL_ERROR
                "[glibre] Configure error: target '${root}' has "
                "GLIBRE_USES_EXCEPTIONS=TRUE but is reachable via glibre-core's "
                "public link interface.  The editor-UI exception carve-out must "
                "not be a transitive dependency of engine core.  "
                "See reviews/decisions/error-model.md Decision #3."
            )
        endif()

        # Walk LINK_LIBRARIES (includes both PUBLIC and INTERFACE).
        get_target_property(_libs "${root}" LINK_LIBRARIES)
        if(_libs)
            foreach(_lib IN LISTS _libs)
                # Strip generator-expression wrappers like $<LINK_ONLY:...>.
                string(REGEX REPLACE "^\\$<[A-Z_]+:(.+)>$" "\\1" _lib_clean "${_lib}")
                if(TARGET "${_lib_clean}")
                    _glibre_check_exceptions_reachable_from_core(
                        "${_lib_clean}" ${visited_var})
                    set(${visited_var} "${${visited_var}}" PARENT_SCOPE)
                endif()
            endforeach()
        endif()

        get_target_property(_iface_libs "${root}" INTERFACE_LINK_LIBRARIES)
        if(_iface_libs)
            foreach(_lib IN LISTS _iface_libs)
                string(REGEX REPLACE "^\\$<[A-Z_]+:(.+)>$" "\\1" _lib_clean "${_lib}")
                if(TARGET "${_lib_clean}")
                    _glibre_check_exceptions_reachable_from_core(
                        "${_lib_clean}" ${visited_var})
                    set(${visited_var} "${${visited_var}}" PARENT_SCOPE)
                endif()
            endforeach()
        endif()
    endif()
endfunction()

function(_glibre_run_core_exception_check)
    if(NOT TARGET glibre-core)
        return()
    endif()
    set(_visited "")
    _glibre_check_exceptions_reachable_from_core(glibre-core _visited)
endfunction()

# Defer until all CMakeLists have been processed so every target is defined.
cmake_language(DEFER CALL _glibre_run_core_exception_check)
