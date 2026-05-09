#pragma once
// core/include/glibre/core/plugin_context.hpp
//
// Convenience umbrella header for plugin authors.
//
// Including this single header is equivalent to including both:
//   • glibre/core/plugin_api.hpp   — PluginContext aggregate definition
//   • glibre/core/plugin_entry.hpp — glibre_plugin_register prototype +
//                                     GLIBRE_PLUGIN_EXPORT macro
//
// Plugin translation units that implement glibre_plugin_register should
// include this header (or include the two constituent headers individually).
//
// The engine loader (plan #229) includes glibre/core/plugin_api.hpp directly
// since it does not need the extern "C" prototype — it resolves the symbol
// via dlsym() and casts to a function-pointer type.
//
// Both constituent headers are independently includable with no ordering
// constraint.
//
// Example plugin skeleton:
//
//   #include <glibre/core/plugin_context.hpp>
//
//   extern "C" GLIBRE_PLUGIN_EXPORT
//   glibre::Result<void>
//   glibre_plugin_register(glibre::core::PluginContext& ctx) noexcept {
//       // ctx.register_component(...)
//       // ctx.register_system(...)
//       return {};  // success
//   }

#include <glibre/core/plugin_api.hpp>
#include <glibre/core/plugin_entry.hpp>
