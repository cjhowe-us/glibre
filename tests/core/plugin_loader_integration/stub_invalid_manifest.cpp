// tests/core/plugin_loader_integration/stub_invalid_manifest.cpp
//
// Stub plugin that exports all four required C symbols and points
// glibre_plugin_manifest at a small garbage byte sequence whose size is
// non-zero, so that the loader (via PluginManifest::open on the sidecar)
// reaches the Fory deserialization path and returns PluginManifestInvalid.
//
// Note: the current loader (plan #229, step 3) reads a sidecar
// <dylib>.manifest file rather than the embedded blob.  The integration
// test pairs this stub with a corresponding sidecar file (zero bytes of
// content) created at build time in CMakeLists.txt.  PluginManifest::open()
// returns PluginManifestInvalid for any file that exists but cannot be
// deserialized (stub impl in core/src/plugin_manifest.cpp).
//
// The exported glibre_plugin_manifest and glibre_plugin_manifest_size
// symbols are also provided in non-null/non-zero form so the stub is
// plausible to any future loader path that checks the blob directly.
//
// Authority: reviews/decisions/plugin-abi.md §"Plugin file shape",
//            §"Loader Sequence" step 3, §"Failure Modes" step 3.
//
// Plan: #968 (plugin loader integration — manifest_invalid failure mode).

#include <cstddef>
#include <cstdint>

#include <glibre/core/plugin_context.hpp>

// ---------------------------------------------------------------------------
// Garbage manifest blob — ensures the stub has a non-null, non-zero
// manifest symbol so any future blob-path checks surface PluginManifestInvalid
// (corrupt bytes, not missing blob).
//
// The exact byte values are arbitrary; they are deliberately not a valid
// Fory-serialized PluginManifest so that any deserializer rejects them.
// ---------------------------------------------------------------------------

namespace {

// 8 bytes of garbage — not a valid Fory header.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)
constexpr std::byte kGarbageBlob[8]{
    std::byte{0xDE},
    std::byte{0xAD},
    std::byte{0xBE},
    std::byte{0xEF},
    std::byte{0xCA},
    std::byte{0xFE},
    std::byte{0xBA},
    std::byte{0xBE},
};

}  // namespace

// ---------------------------------------------------------------------------
// Symbol 1: glibre_plugin_abi_hash
//
// All-zero sentinel — same as the noop plugin.  This stub is not intended
// to trigger the ABI hash gate (step 4); it exercises step 3.
// The integration test is expected to detect PluginManifestInvalid before
// reaching the ABI hash check.
// ---------------------------------------------------------------------------

extern "C" [[gnu::visibility("default")]]
const char* glibre_plugin_abi_hash =
    "0000000000000000000000000000000000000000000000000000000000000000";

// ---------------------------------------------------------------------------
// Symbols 2 & 3: glibre_plugin_manifest / glibre_plugin_manifest_size
//
// Points at the garbage blob above.  A future canonical loader (after plan
// #225 emits real blobs) would attempt to Fory-deserialize these bytes and
// get PluginManifestInvalid from the decode step.
// ---------------------------------------------------------------------------

extern "C" [[gnu::visibility("default")]]
const std::byte* glibre_plugin_manifest = kGarbageBlob;

extern "C" [[gnu::visibility("default")]]
std::size_t glibre_plugin_manifest_size = sizeof(kGarbageBlob);

// ---------------------------------------------------------------------------
// Symbol 4: glibre_plugin_register — no-op stub
//
// Required to satisfy step 2 (dlsym all four symbols).
// The manifest-invalid gate fires at step 3 before register is ever called.
// ---------------------------------------------------------------------------

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"

extern "C" [[gnu::visibility("default")]]
glibre::Result<void> glibre_plugin_register(glibre::core::PluginContext& /*ctx*/) noexcept {
    // No-op: the integration test never reaches this call site.
    // The manifest-invalid gate (step 3) fires before register (step 9).
    return {};
}

#pragma clang diagnostic pop
