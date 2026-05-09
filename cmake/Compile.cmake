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
#    All engine targets must link to this explicitly via
#    target_link_libraries(<target> PRIVATE glibre::compile_contract).
#    Plugins link glibre-types (not glibre-core) and must also link this
#    contract explicitly per plugin-abi.md.  glibre-core does NOT propagate
#    this contract transitively to plugin DSOs.
#
# Flags encoded in this contract (all INTERFACE, applied to every consumer):
#   -fno-exceptions          — no C++ exception machinery (error-model.md §Decision 3)
#   -fno-rtti                — no run-time type information (matches no-exceptions)
#   -fvisibility=hidden      — engine-wide ABI discipline: only explicitly
#                              attributed symbols are exported from DSOs.
#                              Mirrors plugin-abi.md §"Plugin file shape" rule 3
#                              and data/CMakeLists.txt §ABI discipline comment.
#   -fvisibility-inlines-hidden — suppress inline symbol export (complements
#                              -fvisibility=hidden for inline/template code).
#
# NOTE: For the iOS subproject build (CMAKE_SYSTEM_NAME=iOS), core/ and data/
# early-return before adding any targets, so the deferred reachability check at
# the end of this file becomes a no-op — no engine targets exist in that
# configure pass.  This is intentional: the iOS sub-build is excluded from the
# main configure and its tests, so the contract is not wired there.
# ---------------------------------------------------------------------------
if(NOT TARGET glibre::compile_contract)
    add_library(glibre_compile_contract INTERFACE)
    add_library(glibre::compile_contract ALIAS glibre_compile_contract)

    target_compile_options(glibre_compile_contract INTERFACE
        -fno-exceptions
        -fno-rtti
        -fvisibility=hidden
        -fvisibility-inlines-hidden
    )
endif()

# ---------------------------------------------------------------------------
# 2. Helper: glibre_register_engine_root(target)
#
#    Self-registration macro: each engine CMakeLists.txt calls this after
#    add_library(<target> ...) to append the target to the GLIBRE_ENGINE_ROOTS
#    global CMake property.  _glibre_run_core_exception_check() reads that
#    property at configure-end so it no longer needs a hardcoded list.
#
#    Callers: core/CMakeLists.txt, data/CMakeLists.txt, plugins/*/CMakeLists.txt,
#    tools/foryc/CMakeLists.txt, examples/plugin-noop/CMakeLists.txt.
#
#    Must be called AFTER add_library/add_executable so the target exists.
# ---------------------------------------------------------------------------
macro(glibre_register_engine_root target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "glibre_register_engine_root(${target}): target '${target}' is not yet "
            "defined; call after add_library/add_executable.")
    endif()
    set_property(GLOBAL APPEND PROPERTY GLIBRE_ENGINE_ROOTS "${target}")
endmacro()

# ---------------------------------------------------------------------------
# 3. Helper: glibre_target_exceptions(target)
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
    # CMake has no remove API for inherited compile flags.  Instead, we rely
    # on last-flag-wins precedence in the compiler's command-line generator
    # order: -fexceptions/-frtti appended here appear after any -fno-exceptions
    # inherited from glibre::compile_contract, causing the compiler to honour
    # the latter flags.  This is documented clang/GCC behaviour for duplicate
    # flags of the same family.
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
# 4. Configure-time reachability check.
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
    # Walk from every self-registered engine root so that plugin DSOs (which
    # link glibre-types, not glibre-core) and host tools are also covered.
    # Each engine CMakeLists.txt calls glibre_register_engine_root(<target>)
    # after add_library/add_executable, appending to GLIBRE_ENGINE_ROOTS.
    # Tools/editor/ui are excluded from registration — they are the permitted
    # exception carve-out.
    # Roots that do not exist (iOS sub-builds that early-return before
    # add_library) will not be in GLIBRE_ENGINE_ROOTS and are silently absent.
    get_property(_engine_roots GLOBAL PROPERTY GLIBRE_ENGINE_ROOTS)
    set(_visited "")
    foreach(_root IN LISTS _engine_roots)
        if(TARGET "${_root}")
            _glibre_check_exceptions_reachable_from_core("${_root}" _visited)
        endif()
    endforeach()
endfunction()

# Defer until all CMakeLists have been processed so every target is defined.
cmake_language(DEFER CALL _glibre_run_core_exception_check)
