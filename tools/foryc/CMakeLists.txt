cmake_minimum_required(VERSION 3.30)

# ---------------------------------------------------------------------------
# glibre-foryc — .fory schema compiler host tool (plans #219, #220)
#
# This target is a host-side tool only. glibre-core is linked for
# glibre::Error; EASTL is propagated from core.
#
# Plan #219: parser skeleton (parser.{hpp,cpp}).
# Plan #220: header emission pass (emit_header.{hpp,cpp}, builtin_map.hpp).
#            Adds --emit=header flag to the CLI; outputs one .hpp per TypeDecl
#            to <out>/include/glibre/types/<ctx>/<Type>.hpp per
#            reviews/decisions/fory-codegen.md §Pipeline.
#
# Fory::fory is NOT linked here yet — codegen uses a hand-written type
# mapping table (fory-codegen.md §"Rules").  Fory reflection/serializer
# APIs will be required for ABI hash export (plan #222) and migration
# dispatcher (plan #221). Add back: find_package(Fory CONFIG REQUIRED) +
# Fory::fory in target_link_libraries once those plans are implemented.
#
# CLI: glibre-foryc --in <dir> --out <dir> --stamp <file> [--emit=header]
#
# Per reviews/decisions/fory-codegen.md §CMake Integration, the caller
# (data/CMakeLists.txt) adds a custom_command that invokes
# glibre-foryc --in ... --out ... --stamp ... --emit=header.
# ---------------------------------------------------------------------------

add_executable(glibre-foryc
    src/main.cpp
    src/parser.cpp
    src/emit_header.cpp  # plan #220: header emission pass
)

target_include_directories(glibre-foryc
    PRIVATE
        src   # local parser.hpp is picked up here
)

target_link_libraries(glibre-foryc
    PRIVATE
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
