cmake_minimum_required(VERSION 3.30)

# ---------------------------------------------------------------------------
# glibre-foryc — .fory schema compiler host tool (plan #219)
#
# This target is a host-side tool only. It links Fory::fory privately
# (only foryc needs it; no consumer links foryc as a library).
# glibre-core is linked for glibre::Error; EASTL is propagated from core.
#
# CLI: glibre-foryc --in <dir> --out <dir> --stamp <file>
#
# Per reviews/decisions/fory-codegen.md §CMake Integration, the caller
# (data/CMakeLists.txt, future plan) adds a custom_command that invokes
# glibre-foryc --in ... --out ... --stamp ... to trigger codegen.
# ---------------------------------------------------------------------------

find_package(Fory CONFIG REQUIRED)

add_executable(glibre-foryc
    src/main.cpp
    src/parser.cpp
)

target_include_directories(glibre-foryc
    PRIVATE
        src   # local parser.hpp is picked up here
)

target_link_libraries(glibre-foryc
    PRIVATE
        Fory::fory
        glibre::core   # provides glibre::Error + EASTL transitively
)

target_compile_features(glibre-foryc PRIVATE cxx_std_23)

# glibre-foryc is a host tool, not shipping engine code.
# It is allowed to link with exceptions (Fory may use exceptions internally).
# However we still disable them for the parser code which uses std::expected.
# The CLI entry point uses STL streams / std::format (C++23, no exceptions
# path) so -fno-exceptions is safe for the full executable.
target_compile_options(glibre-foryc PRIVATE
    -fno-exceptions
    -fno-rtti
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # Suppress -Wpedantic on C2y extension warnings from EASTL macros
    -Wno-c2y-extensions
)

# Disable CXX_MODULE_STD for the same reason as other targets in this
# project: Catch2 + clang on macOS 26 has duplicate-symbol issues when
# std module import is enabled. Since foryc uses #include headers only
# this flag is consistently OFF project-wide.
set_target_properties(glibre-foryc PROPERTIES CXX_MODULE_STD OFF)
