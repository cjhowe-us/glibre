// tests/core/plugin_loader_integration/stub_wrong_abi_hash.cpp
//
// Stub plugin that exports all four required C symbols but uses a deliberately
// wrong ABI hash value in glibre_plugin_abi_hash.
//
// Purpose: trigger the PluginAbiHashMismatch gate (plugin-abi.md §step 4)
// when PluginLoaderRegistry::validate_symbol_abi_hash() compares the symbol's
// value against the host's expected hash.
//
// The stub exports:
//   glibre_plugin_abi_hash       — wrong 64-char sentinel ("ffff...ffff")
//   glibre_plugin_manifest       — nullptr (no Fory blob in MVP stubs)
//   glibre_plugin_manifest_size  — 0
//   glibre_plugin_register       — no-op, returns success
//
// The integration test loads this dylib via PluginLoader::open() (step 1+2
// succeed — symbols are present) then calls validate_symbol_abi_hash() against
// the sentinel hash, expecting PluginAbiHashMismatch.
//
// Authority: reviews/decisions/plugin-abi.md §"Plugin file shape",
//            §"Loader Sequence" step 4, §"Failure Modes" step 4.
//
// Plan: #232 (plugin loader integration — full failure-mode coverage).

#include <cstddef>
#include <cstdint>

#include <glibre/core/plugin_context.hpp>

// ---------------------------------------------------------------------------
// Symbol 1: glibre_plugin_abi_hash — intentionally wrong sentinel hash
//
// 64 hex chars (32 blake3 bytes) — all 'f' sentinel that never matches any
// real glibre_types_abi_hash() value.  The noop plugin uses all-zero; this
// stub uses all-f to be visually distinct in test output.
//
// Sentinel-hash hygiene note (round-1 review, LOW finding):
//   An all-f string is syntactically valid blake3 hex and has negligible
//   (2^-256) probability of colliding with a real codegen-produced digest.
//   If the project ever adopts deterministic-seeded hash testing, sentinel
//   hashes used by test stubs should migrate to a reserved-sentinel namespace
//   documented in reviews/decisions/plugin-abi.md.  Track via the deferred
//   spike opened as part of round-1 review response (refs #232 MED-1 followup).
// ---------------------------------------------------------------------------

extern "C" [[gnu::visibility("default")]]
const char* glibre_plugin_abi_hash =
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";

// ---------------------------------------------------------------------------
// Symbols 2 & 3: glibre_plugin_manifest / glibre_plugin_manifest_size
//
// Empty manifest stub — no Fory blob in MVP reference plugins.
// The integration test never reads these because it constructs the manifest
// in-memory for the validate_all() call.
// ---------------------------------------------------------------------------

extern "C" [[gnu::visibility("default")]]
const std::byte* glibre_plugin_manifest = nullptr;

extern "C" [[gnu::visibility("default")]]
std::size_t glibre_plugin_manifest_size = 0u;

// ---------------------------------------------------------------------------
// Symbol 4: glibre_plugin_register — no-op stub
//
// The ABI-hash gate fires before glibre_plugin_register is ever called in the
// integration tests.  The symbol must be exported so that PluginLoader::open()
// step 2 (dlsym for four required symbols) succeeds.
//
// -Wreturn-type-c-linkage: intentional — std::expected is ABI-safe across the
// middleman dylib boundary (plugin-abi.md §"Registration Entry-Point Signature").
// ---------------------------------------------------------------------------

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"

extern "C" [[gnu::visibility("default")]]
glibre::Result<void>
glibre_plugin_register(glibre::core::PluginContext& /*ctx*/) noexcept {
    // No-op: the integration test never reaches this call site.
    // The ABI hash gate (step 4) fires before register (step 9).
    return {};
}

#pragma clang diagnostic pop
