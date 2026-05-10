// plugins/shader/src/cache/blake3.cpp
//
// Translation unit for the BLAKE3 hasher (blake3.hpp).
//
// This file is intentionally thin — blake3.hpp is a header-only interface
// wrapping the BLAKE3 C reference library from vcpkg.  A dedicated .cpp is
// required so that the build system can list it as a source (§6.1 module
// layout: each module in cache/ has a .cpp), and to keep the pattern
// consistent with cas_store.cpp, manifest.cpp, and library.cpp which have
// meaningful non-trivial implementations.
//
// All substantive logic lives in blake3.hpp inline functions.

#include "blake3.hpp"

// No non-inline symbols to define here.
// The translation unit is a deliberate no-op body that satisfies the
// CMakeLists.txt source list (see plugins/shader/CMakeLists.txt).
