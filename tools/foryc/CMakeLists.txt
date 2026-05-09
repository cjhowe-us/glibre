cmake_minimum_required(VERSION 3.30)

# ---------------------------------------------------------------------------
# glibre-foryc — .fory schema compiler host tool (plan #219 skeleton,
#                plan #222 adds abi_hash)
#
# This target is a host-side tool only. glibre-core is linked for
# glibre::Error; EASTL is propagated from core.
#
# blake3::blake3 is linked for schema source-hash and ABI hash computation
# (plan #222). Fory::fory is NOT linked here yet — Fory reflection/
# serializer APIs will be required when codegen emission fully lands
# (plan #220 and beyond).
#
# CLI: glibre-foryc --in <dir> --out <dir> --stamp <file>
#      glibre-foryc --in <dir> --emit=abi-hash
#
# Per reviews/decisions/fory-codegen.md §CMake Integration, the caller
# (data/CMakeLists.txt, future plan) adds a custom_command that invokes
# glibre-foryc --in ... --out ... --stamp ... to trigger codegen.
# ---------------------------------------------------------------------------

find_package(blake3 CONFIG REQUIRED)

add_executable(glibre-foryc
    src/main.cpp
    src/parser.cpp
    src/abi_hash.cpp
)

target_include_directories(glibre-foryc
    PRIVATE
        src   # local parser.hpp + abi_hash.hpp are picked up here
)

target_link_libraries(glibre-foryc
    PRIVATE
        glibre::core   # provides glibre::Error + EASTL transitively
        BLAKE3::blake3 # vcpkg blake3 port — blake3_hasher_* C API
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
