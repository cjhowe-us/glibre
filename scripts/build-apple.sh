#!/usr/bin/env bash
# One-shot Apple build: configures + builds the macOS preset, which in
# turn (via cmake/AppleApps.cmake) recursively builds the iOS device +
# simulator static libs, regenerates Glibre.xcodeproj, and runs
# xcodebuild for both schemes — producing GlibreMacOS.app + GlibreIOS.app.
#
# Requires: cmake ≥ 4.3, ninja, xcodegen, Xcode 26+ SDK,
#           Homebrew LLVM ≥ 21.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT_DIR}"

CONFIG="${CONFIG:-debug}"   # debug | release

LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || true)"
if [ -n "${LLVM_PREFIX}" ] && [ -x "${LLVM_PREFIX}/bin/clang" ]; then
    export CC="${LLVM_PREFIX}/bin/clang"
    export CXX="${LLVM_PREFIX}/bin/clang++"
fi

cmake --preset "macos-${CONFIG}"
cmake --build --preset "macos-${CONFIG}"
