// SPDX-License-Identifier: Apache-2.0
// core/src/eastl_alloc.cpp
//
// EASTL requires the project to supply custom operator new[] overloads that
// EASTL's default allocator (eastl::allocator) dispatches through.
//
// See EASTL docs §"Custom Memory Allocators" and
// vcpkg-overlay-ports/fory/ (which ships the upstream Fory library that
// similarly needs these overloads).
//
// This translation unit defines the minimal set required by
// eastl::allocator::allocate (in allocator.h lines 175-176):
//
//   operator new[](size, name, flags, debugFlags, file, line)
//   operator new[](size, alignment, alignmentOffset, name, flags, debugFlags, file, line)
//
// All overloads delegate to aligned_alloc / free (POSIX) which guarantees
// 16-byte minimum alignment (matching EASTL_ALLOCATOR_MIN_ALIGNMENT).
//
// Deallocation note: EASTL's default allocator::deallocate (allocator.h:285)
// calls plain `delete[](char*)p`, which routes through the global
// `operator delete[](void*)` — NOT through any override defined here.
// On macOS libc (libSystem), std::aligned_alloc results are free()-compatible
// and the system `operator delete[]` releases them via free(), so this is
// sound. If portability beyond macOS is ever needed, define matching
// `operator delete[]` overrides and switch allocate/deallocate to use
// std::malloc/std::free for symmetric pairing.
//
// This is a macOS-only engine (PHILOSOPHY §0: macOS-first baseline) so
// the current deallocation path is accepted for the MVP.
//
// Threading: no lock needed — aligned_alloc is thread-safe on macOS.
//
// PHILOSOPHY §11: EASTL replaces std:: containers; this file is the
// allocator substrate that makes eastl:: usable project-wide.

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
    // aligned_alloc requires alignment to be a power of two and size to be a
    // multiple of alignment. For size == 0 we return a valid unique pointer.
    if (size == 0)
        size = 1;
    // Adjust size up to a multiple of 16 so aligned_alloc accepts it.
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
    // Ensure size is a multiple of alignment.
    size = (size + alignment - 1u) & ~(alignment - 1u);
    void* p = std::aligned_alloc(alignment, size);
    if (!p)
        __builtin_trap();  // -fno-exceptions: abort on OOM
    return p;
}
