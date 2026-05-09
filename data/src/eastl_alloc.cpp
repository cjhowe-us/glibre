// SPDX-License-Identifier: Apache-2.0
// data/src/eastl_alloc.cpp
//
// EASTL operator new[] substrate for glibre-types.dylib.
//
// Per reviews/decisions/plugin-abi.md §"Plugin file shape" rule 2:
//   "Each plugin links ONLY glibre-types.dylib from the engine's side.
//    It must not link glibre-core or any other plugin."
//
// EASTL requires the project to supply custom operator new[] overloads that
// eastl::allocator dispatches through.  Because plugins link ONLY
// glibre-types.dylib (not glibre-core), glibre-types must carry these
// overloads so that any EASTL container used inside glibre-types and by
// plugins via the PUBLIC EASTL link resolves correctly.
//
// Duplication note: core/src/eastl_alloc.cpp carries identical overloads
// for the glibre-core STATIC library.  The duplication is intentional and
// accepted: moving the overloads from glibre-core to glibre-types would
// cause glibre-core to link glibre-types (introducing a circular-ish
// dependency between the core hub and the middleman dylib).  Keeping both
// copies avoids that coupling.  Both TUs define the same global ::operator
// new[] signatures; only one definition is resolved per dylib boundary
// at link time, so there is no ODR violation.
//
// References:
//   EASTL docs §"Custom Memory Allocators"
//   core/src/eastl_alloc.cpp — canonical model for this copy
//   PHILOSOPHY §11 — EASTL replaces std:: containers project-wide
//
// Threading: no lock needed — aligned_alloc is thread-safe on macOS.
// Deallocation: EASTL deallocate calls operator delete[](void*), which
// correctly releases aligned_alloc results via macOS libSystem's free().

#include <cstddef>
#include <cstdlib>
#include <new>

// ---------------------------------------------------------------------------
// EASTL allocator new[] overloads (global scope, not in any namespace)
// ---------------------------------------------------------------------------

// Simple overload: allocate 'size' bytes with 16-byte alignment.
void* operator new
    [](std::size_t size,
       const char* /*pName*/,
       int /*flags*/,
       unsigned /*debugFlags*/,
       const char* /*file*/,
       int /*line*/) {
    if (size == 0)
        size = 1;
    size = (size + 15u) & ~static_cast<std::size_t>(15u);
    void* p = std::aligned_alloc(16u, size);
    if (!p)
        __builtin_trap();  // -fno-exceptions: abort on OOM
    return p;
}

// Aligned overload: allocate 'size' bytes with the requested alignment.
void* operator new
    [](std::size_t size,
       std::size_t alignment,
       std::size_t /*alignmentOffset*/,
       const char* /*pName*/,
       int /*flags*/,
       unsigned /*debugFlags*/,
       const char* /*file*/,
       int /*line*/) {
    if (size == 0)
        size = 1;
    if (alignment < 16u)
        alignment = 16u;
    size = (size + alignment - 1u) & ~(alignment - 1u);
    void* p = std::aligned_alloc(alignment, size);
    if (!p)
        __builtin_trap();  // -fno-exceptions: abort on OOM
    return p;
}
