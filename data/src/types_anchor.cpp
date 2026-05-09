// data/src/types_anchor.cpp
//
// Anchor translation unit for glibre-types.dylib.
//
// Purpose: Ensures the shared library has at least one compiled TU and
// exports the canonical ABI-version sentinel symbol.  This file is NOT
// generated — it is the permanent anchor that makes the dylib link-able
// before any codegen output (glibre-foryc, plan #220/#225) has run.
//
// Authority: reviews/decisions/fory-codegen.md §"CMake Integration"
// and reviews/decisions/plugin-abi.md §"Plugin file shape" rule 2.
//
// Plugin link surface: plugins link ONLY glibre-types.dylib, never
// glibre-core.  This file is the sole non-generated TU that makes that
// surface self-contained at the CMake level.
//
// ABI version: bumped manually only on layout-breaking schema change per
// plugin-abi.md §"Versioning Rules" point 2.  DO NOT bump this integer
// for additive schema changes — those bump the blake3 ABI hash instead
// (glibre_types_abi_hash, plan #222).  The SONAME is likewise only
// bumped for layout-breaking changes; the linker guards the SONAME
// window while glibre_types_abi_hash guards the hash-stable window.
//
// SONAME policy (mirrors CMake comment in data/CMakeLists.txt):
//   - Major additive change  → bump abi_hash only, SONAME unchanged.
//   - Layout-breaking change → bump SONAME AND abi_hash.
//
// The integer value here is separate from the Fory-codegen hash — it
// answers "can the loader dlopen this dylib at all?" before the full
// hash check runs.  Start at 1; subsequent bumps are captured in the
// PR commit history (no separate changelog file per project convention).

#include <cstdint>

extern "C" {

// glibre_types_abi_version — sentinel used by tests and the loader to
// confirm the dylib loaded and is the expected revision.
//
// Visibility: -fvisibility=hidden (set PRIVATE on glibre-types in CMake)
// applies to ALL symbols in this TU, including extern "C" ones.  The
// [[gnu::visibility("default")]] attribute below is therefore REQUIRED
// to expose this symbol to dlsym from the test executable and the loader.
// Removing it would silently hide the symbol and cause link failures.
//
// IMPORTANT: return type is int32_t so callers do not need to agree on
// platform int width.  The value 1 is the initial version.
[[gnu::visibility("default")]]
int32_t glibre_types_abi_version() noexcept {
    return 1;
}

}  // extern "C"
