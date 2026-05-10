#pragma once
// core/include/glibre/core/plugin_manifest.hpp
//
// PluginManifest — canonical in-memory schema for a glibre plugin's identity
// and declared surface.
//
// Authority: reviews/decisions/plugin-abi.md §"Plugin Manifest Schema".
// This header is the C++ mirror of the Fory schema authored in
// data/schemas/core/PluginManifest.fory (plan #222 / #225).  Field names,
// types, and ordering below must stay in sync with that schema.
//
// Wire-format stability: scalar fields (uint8_t, uint16_t) are fixed-width, so
// the Fory tag-sorted layout is stable across builds on the same ABI.
// std::pmr::string and std::pmr::vector members are pointer-indirected — they
// are NOT part of the in-memory POD footprint.  The wire format (Fory blob) is
// stable; the in-memory layout is not byte-transferable across address spaces.
// Serialization / deserialization of these fields is the responsibility of the
// glibre-foryc codegen pipeline (plan #225); the loader (plan #229..#231) reads
// the Fory blob and populates these fields correctly.
//
// SCOPE — this header is SCHEMA ONLY:
//   - No Fory codegen is invoked here (plans #225, #222).
//   - No loader logic (plans #229, #230, #231).
//   - No ABI hash computation (reaffirmed by fory-codegen.md; lands #222).
//   - No migration mechanics (plan #231).
//
// Field tags are assigned per plugin-abi.md §"Plugin Manifest Schema" and are
// immutable once shipped.  New fields must use the next available tag number.
// Never reuse a retired tag; mark it `// RESERVED tag N` instead.
//
// Container and string types per reviews/decisions/eastl-removal.md:
//   std::pmr::string  — matrix row 1 (engine code → PMR variant)
//   std::string_view  — matrix row 2 (non-PMR; string_view does not own storage)
//   std::pmr::vector  — matrix row 3 (engine code → PMR variant)
//   Backed by glibre::PerContextAllocatorResource (alloc.hpp §3).
//
// Public plugin ABI surfaces never expose std::pmr:: containers directly — they
// cross the boundary as POD spans / handles only (plugin-abi.md §rationale).
// The struct members here are the in-memory representation used by core after
// deserialisation; they are not the wire boundary.

#include <cstdint>
#include <filesystem>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "glibre/error.hpp"

namespace glibre::core {

// ---------------------------------------------------------------------------
// SemVer — semantic version triple
//
// Field tags (plugin-abi.md §"glibre.core.SemVer"):
//   major tag 1 since 1
//   minor tag 2 since 1
//   patch tag 3 since 1
// ---------------------------------------------------------------------------

struct SemVer {
    std::uint16_t major{0};  // tag 1
    std::uint16_t minor{0};  // tag 2
    std::uint16_t patch{0};  // tag 3

    [[nodiscard]] constexpr bool operator==(const SemVer&) const noexcept = default;

    [[nodiscard]] constexpr bool operator<(const SemVer& rhs) const noexcept {
        if (major != rhs.major)
            return major < rhs.major;
        if (minor != rhs.minor)
            return minor < rhs.minor;
        return patch < rhs.patch;
    }

    [[nodiscard]] constexpr bool operator<=(const SemVer& rhs) const noexcept {
        return !(rhs < *this);
    }
};

// ---------------------------------------------------------------------------
// ComponentDecl — descriptor for one component type registered by a plugin
//
// Field tags (plugin-abi.md §"glibre.core.ComponentDecl"):
//   fqn          tag 1 since 1   — fully-qualified type name, e.g. "glibre.render.Camera"
//   schema_hash  tag 2 since 1   — blake3 hex (64 chars) of the .fory source
//   storage_hint tag 3 since 1   — 0=archetype, 1=sparse, 2=singleton (byte enum)
// ---------------------------------------------------------------------------

struct ComponentDecl {
    std::pmr::string fqn;          // tag 1
    std::pmr::string schema_hash;  // tag 2 — 64-char blake3 hex
    std::uint8_t storage_hint{0};  // tag 3 — archetype=0, sparse=1, singleton=2

    [[nodiscard]] bool operator==(const ComponentDecl&) const noexcept = default;
};

// ---------------------------------------------------------------------------
// SystemDecl — descriptor for one ECS system registered by a plugin
//
// Field tags (plugin-abi.md §"glibre.core.SystemDecl"):
//   name   tag 1 since 1   — system name (unique within plugin)
//   phase  tag 2 since 1   — 1..=9 from reviews/decisions/frame-phases.md
//   reads  tag 3 since 1   — component fqns this system reads
//   writes tag 4 since 1   — component fqns this system writes
//   after  tag 5 since 1   — system names this must execute after (intra-phase)
//   before tag 6 since 1   — system names this must execute before (intra-phase)
// ---------------------------------------------------------------------------

struct SystemDecl {
    std::pmr::string name;                      // tag 1
    std::uint8_t phase{0};                      // tag 2 — 1..=9
    std::pmr::vector<std::pmr::string> reads;   // tag 3
    std::pmr::vector<std::pmr::string> writes;  // tag 4
    std::pmr::vector<std::pmr::string> after;   // tag 5
    std::pmr::vector<std::pmr::string> before;  // tag 6

    [[nodiscard]] bool operator==(const SystemDecl&) const noexcept = default;
};

// ---------------------------------------------------------------------------
// PassDecl — descriptor for one render-graph pass registered by a plugin
//
// Passes are only meaningful for plugins operating in render phases 6 and 7
// (CullExtract / RenderSubmit per frame-phases.md).
//
// Field tags (plugin-abi.md §"glibre.core.PassDecl"):
//   name         tag 1 since 1   — pass name (unique within plugin)
//   render_phase tag 2 since 1   — 6 (cull-extract) or 7 (render-submit)
//   inputs       tag 3 since 1   — resource names consumed
//   outputs      tag 4 since 1   — resource names produced
// ---------------------------------------------------------------------------

struct PassDecl {
    std::pmr::string name;                       // tag 1
    std::uint8_t render_phase{0};                // tag 2 — 6 or 7
    std::pmr::vector<std::pmr::string> inputs;   // tag 3
    std::pmr::vector<std::pmr::string> outputs;  // tag 4

    [[nodiscard]] bool operator==(const PassDecl&) const noexcept = default;
};

// ---------------------------------------------------------------------------
// PanelDecl — descriptor for one editor-UI panel registered by a plugin
//
// No-op for non-editor builds; included in the manifest for forward-compat.
//
// Field tags (plugin-abi.md §"glibre.core.PanelDecl"):
//   id    tag 1 since 1   — stable panel identifier (kebab-case)
//   title tag 2 since 1   — human-readable panel title
//   area  tag 3 since 1   — docked area enum (0..=N per future editor spec)
// ---------------------------------------------------------------------------

struct PanelDecl {
    std::pmr::string id;     // tag 1
    std::pmr::string title;  // tag 2
    std::uint8_t area{0};    // tag 3 — docked-area byte enum

    [[nodiscard]] bool operator==(const PanelDecl&) const noexcept = default;
};

// ---------------------------------------------------------------------------
// PluginManifest — top-level manifest aggregate for a glibre plugin
//
// Canonical field set per plugin-abi.md §"Plugin Manifest Schema".
// Serialized as a Fory blob baked into plugin .rodata by the codegen
// pipeline (plans #222 / #225).  The loader reads this blob before invoking
// any plugin C++ code (plans #229..#231).
//
// Field tags (plugin-abi.md §"glibre.core.PluginManifest"):
//   name               tag 1 since 1
//   version            tag 2 since 1
//   abi_hash           tag 3 since 1
//   min_engine_version tag 4 since 1
//   components         tag 5 since 1
//   systems            tag 6 since 1
//   passes             tag 7 since 1
//   panels             tag 8 since 1
//   depends_on         tag 9 since 1
//
// PMR allocator threading (reviews/decisions/eastl-removal.md §3):
//   PluginManifest members allocate from std::pmr::get_default_resource()
//   when default-constructed — acceptable for tests and stub callers.
//   Production callers (plan #229 loader) should wire a
//   glibre::PerContextAllocatorResource backed by ContextTag::core so
//   that manifest string/vector storage is tracked under the core context
//   ceiling per perf-budget.md §Allocator Rules #1.
// ---------------------------------------------------------------------------

struct PluginManifest {
    // tag 1 — fully-qualified plugin id, e.g. "glibre.render".
    // Unique across loaded plugins; collision → core::Error::PluginNameCollision.
    std::pmr::string name;

    // tag 2 — plugin's own SemVer.  Independent of abi_hash (see
    //   plugin-abi.md §"Versioning Rules").
    SemVer version{};

    // tag 3 — 64-char lowercase blake3 hex, copied from
    //   glibre_types_abi_hash() at plugin compile time.
    //   Loader compares this against the host's hash at load time.
    std::pmr::string abi_hash;

    // tag 4 — minimum glibre-core SemVer this plugin tolerates.
    //   Loader refuses load if engine version < min_engine_version.
    SemVer min_engine_version{};

    // tag 5 — every component type the plugin registers into the type registry.
    std::pmr::vector<ComponentDecl> components;

    // tag 6 — every ECS system the plugin registers.
    std::pmr::vector<SystemDecl> systems;

    // tag 7 — render-graph passes (meaningful only for render-phase plugins).
    std::pmr::vector<PassDecl> passes;

    // tag 8 — editor-UI panels (no-op for non-editor builds).
    std::pmr::vector<PanelDecl> panels;

    // tag 9 — plugin names that must already be registered before this
    //   plugin's register() runs.  Ordering only; cross-plugin communication
    //   goes through the type registry, not direct calls.
    std::pmr::vector<std::pmr::string> depends_on;

    [[nodiscard]] bool operator==(const PluginManifest&) const noexcept = default;

    // -----------------------------------------------------------------------
    // open — read a Fory-serialized manifest from disk
    //
    // Actual deserialization implementation is generated by glibre-foryc as
    // a per-plugin manifest.cpp (plan #225).  This declaration reserves the
    // stable public interface so consumers may forward-declare and call it.
    //
    // Errors:
    //   core::Error::PluginManifestNotFound  — path does not exist or is not
    //     a regular file.  This arm is new and not listed in the loader-sequence
    //     table of plugin-abi.md §"Failure Modes → core::Error" because that
    //     table covers post-dlopen loader steps (steps 1..11); PluginManifest::
    //     open() is a pre-loader utility used in tests and codegen tooling, so
    //     its failure arm is a schema-layer addition orthogonal to loader step 3
    //     (PluginManifestInvalid).
    //   core::Error::PluginManifestInvalid   — file exists but Fory
    //     deserialization fails (corrupt, wrong schema version, truncated).
    //
    // Takes std::string_view so callers holding string literals, std::string,
    // std::pmr::string, or char arrays do not need to materialise an extra copy.
    //
    // Note: implementation stub in core/src/plugin_manifest.cpp; full
    //   deserialization lands when glibre-foryc emits manifest.cpp (#225).
    // -----------------------------------------------------------------------
    [[nodiscard]] static Result<PluginManifest> open(std::string_view path);
};

// static_assert: PluginManifest is an aggregate (no user-provided ctor,
// no private/protected non-static data, no virtual functions, no base
// classes with private/protected members).  The codegen pipeline requires
// this property for POD-compatible layout synthesis.
static_assert(
    std::is_aggregate_v<PluginManifest>,
    "PluginManifest must remain an aggregate for codegen compatibility"
);
static_assert(
    std::is_aggregate_v<SemVer>, "SemVer must remain an aggregate for codegen compatibility"
);
static_assert(
    std::is_aggregate_v<ComponentDecl>,
    "ComponentDecl must remain an aggregate for codegen compatibility"
);
static_assert(
    std::is_aggregate_v<SystemDecl>, "SystemDecl must remain an aggregate for codegen compatibility"
);
static_assert(
    std::is_aggregate_v<PassDecl>, "PassDecl must remain an aggregate for codegen compatibility"
);
static_assert(
    std::is_aggregate_v<PanelDecl>, "PanelDecl must remain an aggregate for codegen compatibility"
);

}  // namespace glibre::core
