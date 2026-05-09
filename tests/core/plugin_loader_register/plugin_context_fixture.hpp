#pragma once
// tests/core/plugin_loader_register/plugin_context_fixture.hpp
//
// Shared fixture header — minimal empty shell definitions for the pending
// PluginContext reference types.
//
// PluginContext (glibre/core/plugin_api.hpp) carries references to World,
// TypeRegistry, SystemRegistry, PassRegistry, PanelRegistry, and LogSink.
// Each of those types is forward-declared as an opaque tag in plugin_api.hpp;
// the real definitions land in their respective bounded-context plans
// (#ECS-world, #type-registry, #system-registry, #pass-registry,
// #panel-registry, #log-sink).
//
// Until those plans land, test TUs that need to construct a real PluginContext
// must provide definitions.  This header is the single source of truth for
// those shell classes, replacing per-TU inline definitions that were an ODR
// landmine (MED-4, round-1 review): if two TUs in the same link unit both
// defined `class glibre::core::World {}` the definitions would silently
// violate ODR under the "all definitions identical" exception, but any
// divergence (a field added in one TU during refactoring) would be UB with
// no diagnostic.
//
// When the real types land (plans listed above), delete this file and update
// the test TUs to include the real headers.  The compiler will report any
// remaining uses of the now-deleted shell definitions.
//
// Authority: reviews/decisions/plugin-abi.md §"Registration Entry-Point
//            Signature" (PluginContext field list).
// Plan: #231.

namespace glibre::core {

// Empty shell — real definition: plan #ECS-world (pending).
class World {};

// Empty shell — real definition: plan #type-registry (pending).
class TypeRegistry {};

// Empty shell — real definition: plan #system-registry (pending).
class SystemRegistry {};

// Empty shell — real definition: plan #pass-registry (pending).
class PassRegistry {};

// Empty shell — real definition: plan #panel-registry (pending).
class PanelRegistry {};

// Empty shell — real definition: plan #log-sink (pending).
class LogSink {};

}  // namespace glibre::core
