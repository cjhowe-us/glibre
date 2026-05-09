// tests/core/plugin_loader/stub_no_symbols.cpp
//
// A valid .dylib that intentionally exports NO plugin entry-point symbols.
// Built by the test suite as glibre-plugin-stub-no-symbols.dylib and used by
// the plugin_loader_open_returns_error_on_missing_symbols Catch2 test to
// verify that PluginLoader::open() returns Error::PluginMissingEntryPoint
// when the required symbol table is incomplete.
//
// This file exports one internal symbol so the linker produces a non-empty
// dylib, but it deliberately omits:
//   glibre_plugin_abi_hash
//   glibre_plugin_manifest
//   glibre_plugin_manifest_size
//   glibre_plugin_register
//
// See tests/core/plugin_loader/CMakeLists.txt for the build target.

// An internal symbol (hidden visibility): ensures the dylib is non-empty but
// guarantees dlsym("glibre_plugin_abi_hash") and friends return nullptr.
[[gnu::visibility("hidden")]]
int glibre_stub_internal_symbol = 0;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
