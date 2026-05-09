// tests/core/plugin_loader_register/stub_register_fails.cpp
//
// Stub plugin that exports all four required C symbols and whose
// glibre_plugin_register entry-point returns a failure (non-zero / unexpected).
//
// Purpose:
//   Trigger core::Error::PluginInitFailed in
//   PluginLoaderRegistry::call_register() (plugin-abi.md §"Loader Sequence"
//   step 9).  Tests load this stub via PluginLoader::open() (steps 1–2 succeed),
//   then call call_register() and assert the failure path.
//
// Exported symbols:
//   glibre_plugin_abi_hash       — sentinel all-zeros (matches noop plugin)
//   glibre_plugin_manifest       — nullptr (no Fory blob in MVP stubs)
//   glibre_plugin_manifest_size  — 0
//   glibre_plugin_register       — returns std::unexpected(PluginInitFailed)
//
// Authority: reviews/decisions/plugin-abi.md §"Plugin file shape",
//            §"Loader Sequence" step 9, §"Failure Modes" step 9.
//
// Plan: #231 — PluginLoaderRegistry register + schedule rebuild + migrate.

#include <cstddef>
#include <cstdint>

#include <glibre/core/plugin_context.hpp>
#include <glibre/error.hpp>

// ---------------------------------------------------------------------------
// Symbol 1: glibre_plugin_abi_hash — valid sentinel (all-zeros)
//
// Same value as the noop plugin.  Tests that use this stub for the
// call_register failure path pass the expected hash as all-zeros and use a
// constructed manifest with the same hash, so gates 1a/1b pass before
// call_register is invoked.
// ---------------------------------------------------------------------------

extern "C" [[gnu::visibility("default")]]
const char* glibre_plugin_abi_hash =
    "0000000000000000000000000000000000000000000000000000000000000000";

// ---------------------------------------------------------------------------
// Symbols 2 & 3: glibre_plugin_manifest / glibre_plugin_manifest_size
//
// Empty MVP stub — no Fory blob generated yet (plan #225).
// ---------------------------------------------------------------------------

extern "C" [[gnu::visibility("default")]]
const std::byte* glibre_plugin_manifest = nullptr;

extern "C" [[gnu::visibility("default")]]
std::size_t glibre_plugin_manifest_size = 0u;

// ---------------------------------------------------------------------------
// Symbol 4: glibre_plugin_register — always returns failure
//
// Returns std::unexpected(glibre::Error{core::Error::PluginInitFailed}) to
// simulate a plugin whose registration step fails (e.g., a system it tries to
// register conflicts with an existing one, or a required resource is absent).
//
// The test (register_failure_cleans_up_dlopen) asserts that call_register()
// wraps this inner failure in core::Error::PluginInitFailed.
//
// -Wreturn-type-c-linkage: intentional — std::expected is ABI-safe across the
// middleman dylib boundary (plugin-abi.md §"Registration Entry-Point Signature").
// ---------------------------------------------------------------------------

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"

extern "C" [[gnu::visibility("default")]]
glibre::Result<void>
glibre_plugin_register(glibre::core::PluginContext& /*ctx*/) noexcept {
    // Simulate registration failure.  The loader must treat this as
    // PluginInitFailed and run compensating cleanup (dlclose + abort load).
    return std::unexpected(glibre::Error{glibre::core::Error::PluginInitFailed});
}

#pragma clang diagnostic pop
