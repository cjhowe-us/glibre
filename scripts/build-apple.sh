#!/usr/bin/env bash
# Builds Glibre for macOS + iOS (device & simulator) via CMake/Ninja, then
# regenerates the XcodeGen project and (optionally) builds .app bundles.
#
# Requires: cmake ≥ 4.3, ninja, xcodegen, Homebrew LLVM ≥ 21, Xcode 26+ SDK.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT_DIR}"

CONFIG="${CONFIG:-debug}"   # debug | release
SKIP_XCODEBUILD="${SKIP_XCODEBUILD:-0}"

LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || true)"
if [ -n "${LLVM_PREFIX}" ] && [ -x "${LLVM_PREFIX}/bin/clang" ]; then
    export CC="${LLVM_PREFIX}/bin/clang"
    export CXX="${LLVM_PREFIX}/bin/clang++"
fi

build_preset() {
    local preset="$1"
    echo "::group::cmake $preset"
    cmake --preset "${preset}"
    cmake --build --preset "${preset}"
    echo "::endgroup::"
}

build_preset "macos-${CONFIG}"
build_preset "ios-${CONFIG}"
build_preset "ios-sim-${CONFIG}"

command -v xcodegen >/dev/null || { echo "xcodegen not found — brew install xcodegen" >&2; exit 1; }
xcodegen generate

if [ "${SKIP_XCODEBUILD}" = "0" ]; then
    XCCONFIG="$(tr '[:lower:]' '[:upper:]' <<< "${CONFIG:0:1}")${CONFIG:1}"
    xcodebuild -project Glibre.xcodeproj -scheme GlibreMacOS \
               -configuration "${XCCONFIG}" build
    xcodebuild -project Glibre.xcodeproj -scheme GlibreIOS \
               -configuration "${XCCONFIG}" \
               -destination 'generic/platform=iOS' build
fi
