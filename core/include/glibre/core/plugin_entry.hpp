#pragma once
// core/include/glibre/core/plugin_entry.hpp
//
// C-ABI entry-point declaration for glibre plugins.
//
// DESIGN (reviews/decisions/plugin-abi.md §"Registration Entry-Point Signature"):
//
//   Every plugin .dylib exports the following C symbol:
//
//     extern "C" std::expected<void, glibre::Error>
//     glibre_plugin_register(glibre::core::PluginContext& ctx) noexcept;
//
//   The return type crosses the middleman dylib boundary.  Both sides of
//   that boundary compile against the same libc++ that built
//   glibre-types.dylib (plugin-abi.md §"Registration Entry-Point
//   Signature" footnote, and fory-codegen.md §"Open Questions" #4),
//   so std::expected<void, glibre::Error> is ABI-safe here.
//
//   The optional paired unregister symbol:
//
//     extern "C" std::expected<void, glibre::Error>
//     glibre_plugin_unregister(glibre::core::PluginContext& ctx) noexcept;
//
//   is optional in MVP, mandatory once hot-reload migrations land
//   (plugin-abi.md §"Open Questions" point 1).
//
// Symbol visibility:
//   Both symbols carry [[gnu::visibility("default")]] so that even when
//   the .dylib is built with -fvisibility=hidden the entry-points remain
//   exported.  Plugins MUST compile their translation unit that defines
//   these functions with at least -fvisibility=default or use the
//   GLIBRE_PLUGIN_EXPORT macro defined below.
//
// Usage — plugin side:
//   #include <glibre/core/plugin_entry.hpp>   // or plugin_context.hpp
//
//   // Definition (GLIBRE_PLUGIN_EXPORT guards the visibility attribute):
//   extern "C" GLIBRE_PLUGIN_EXPORT
//   glibre::Result<void>
//   glibre_plugin_register(glibre::core::PluginContext& ctx) noexcept {
//       // ... register components, systems, passes, panels
//       return {};
//   }
//
// Usage — engine/loader side (plan #229):
//   using RegisterFn = glibre::Result<void>(*)(glibre::core::PluginContext&);
//   auto* fn = reinterpret_cast<RegisterFn>(dlsym(handle, "glibre_plugin_register"));
//
// Compilation requirements:
//   -fno-exceptions (error-model.md §Decision 3)
//   C++23

#include <glibre/core/plugin_api.hpp>
#include <glibre/error.hpp>

// ---------------------------------------------------------------------------
// GLIBRE_PLUGIN_EXPORT — macro for plugin translation units
//
// Apply to the definitions of glibre_plugin_register and
// glibre_plugin_unregister to ensure they are exported from the .dylib
// regardless of the plugin's default symbol-visibility compiler flag.
//
// Guarded on GCC/Clang because [[gnu::visibility]] is a GCC extension.
// On MSVC (future post-MVP Windows support) the equivalent would be
// __declspec(dllexport) — tracked in reviews/decisions/build-portability.md
// (post-MVP issue).
// ---------------------------------------------------------------------------

#if defined(__GNUC__) || defined(__clang__)
#define GLIBRE_PLUGIN_EXPORT [[gnu::visibility("default")]]
#else
#define GLIBRE_PLUGIN_EXPORT
#endif

// ---------------------------------------------------------------------------
// glibre_plugin_register — prototype
//
// Declared extern "C" to give the symbol a stable, unmangled name that the
// engine loader (plan #229) can dlsym() at runtime.  The C linkage applies
// only to the name; the function body is full C++23 and may use RAII,
// templates, and constexpr internally.
//
// The context is passed by (non-const) reference rather than pointer:
//   • The plugin cannot take ownership or persist the reference past the call.
//   • The loader guarantees the context outlives the call.
//   • Reference semantics make "null context" a compile-time impossibility.
//
// noexcept: engine core compiles with -fno-exceptions; the boundary must
// not leak any exception state even if a plugin is compiled with exceptions
// enabled.  Plugins must catch and convert any thrown exception to a
// glibre::Error before returning.
// ---------------------------------------------------------------------------

// The extern "C" block uses C linkage (unmangled name) for dlsym() resolution.
// The return type std::expected<void, glibre::Error> is NOT a C type, but this
// is intentional: both sides of the boundary compile against the same libc++
// that built glibre-types.dylib (plugin-abi.md §"Registration Entry-Point
// Signature" footnote, fory-codegen.md §"Open Questions" #4).  The clang
// diagnostic below suppresses the expected -Wreturn-type-c-linkage warning.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"

extern "C" {

/// Entry-point every plugin .dylib must export.
/// The engine loader calls this once during phase 8 (HotReload) after the
/// manifest has been verified and all dependencies are already registered.
///
/// @param ctx  engine-owned context; valid only for the duration of the call.
/// @returns    std::expected<void, glibre::Error>{}  on success,
///             std::unexpected(err)                   on failure.
///
/// On failure the loader runs compensating unregister of any partial
/// registration the plugin made, then dlclose()s the dylib.
GLIBRE_PLUGIN_EXPORT
glibre::Result<void> glibre_plugin_register(glibre::core::PluginContext& ctx) noexcept;

/// Optional paired teardown, required once hot-reload migrations land.
/// (plugin-abi.md §"Open Questions" point 1 — optional in MVP)
///
/// The loader calls this during phase 8 (drain) before unloading a plugin.
/// Plugins that do not export this symbol are unloadable only via process
/// restart in MVP.
GLIBRE_PLUGIN_EXPORT
glibre::Result<void> glibre_plugin_unregister(glibre::core::PluginContext& ctx) noexcept;

}  // extern "C"

#pragma clang diagnostic pop

namespace glibre::core {

// ---------------------------------------------------------------------------
// RegisterFn — single source of truth for the loader-side function-pointer
// type corresponding to glibre_plugin_register.
//
// noexcept is intentionally absent: since C++17, noexcept is part of the
// function type.  A plugin compiled against a header that omits noexcept
// would produce a different function pointer type, causing a silent mismatch.
// The loader (plugin_loader.hpp) imports this alias; plugin_entry.hpp (this
// file) is the canonical definition.
//
// Usage (loader side):
//   #include <glibre/core/plugin_entry.hpp>
//   RegisterFn fn{nullptr};
//   std::memcpy(&fn, &sym_register, sizeof(fn));
// ---------------------------------------------------------------------------
using RegisterFn = glibre::Result<void> (*)(glibre::core::PluginContext&);

}  // namespace glibre::core
