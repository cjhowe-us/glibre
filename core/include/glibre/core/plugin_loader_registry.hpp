#pragma once
// core/include/glibre/core/plugin_loader_registry.hpp
//
// PluginLoaderRegistry — tracks loaded plugins and enforces the compatibility
// gates described in reviews/decisions/plugin-abi.md §"Loader Sequence"
// steps 4–7.
//
// This class is NOT a singleton; callers own the registry value.  In
// production the engine owns one instance per process.  In tests,
// each TEST_CASE constructs its own independent registry.
//
// Gates enforced (in order, steps 4–7):
//   1. ABI hash gate (step 4) — two sub-checks in plugin-abi.md §step 4 order:
//      a) validate_manifest_abi_hash: manifest.abi_hash == expected_abi_hash.
//      b) validate_symbol_abi_hash:   symbol value == expected_abi_hash.
//      Both must pass; either mismatch → core::Error::PluginAbiHashMismatch.
//      validate_all runs (a) then (b) per plugin-abi.md §step 4 text.
//      Individual granular callers may invoke each sub-check directly.
//   2. Engine version gate (step 5)
//      manifest.min_engine_version ≤ host engine version.  Violation →
//      core::Error::PluginEngineTooOld.
//   3. Name uniqueness gate (step 6)
//      manifest.name must not already be registered WITH A DIFFERENT file
//      path.  Same name + same path is idempotent (allows hot-reload
//      re-registration of the same dylib).  Different path →
//      core::Error::PluginNameCollision.
//   4. Dependency gate (step 7)
//      every entry in manifest.depends_on must already be registered.
//      Missing → core::Error::PluginDependencyMissing.
//
// Post-gate actions (steps 9–11) live in plugin_loader_actions.hpp as free
// functions.  They are free functions — not methods here — because they do
// not read or mutate this class's registry-of-records state (loaded_ map and
// host_engine_version_).  Mixing registry-of-records state with loader-
// procedure logic would introduce a second reason to change this class (SRP).
//
// Container + string migration (reviews/decisions/eastl-removal.md plan #1044):
//   eastl::hash_map<eastl::string, V>  →  std::pmr::unordered_map<
//       std::pmr::string, V,
//       glibre::TransparentStringHash,
//       std::equal_to<>                   // transparent equality
//   >  backed by PerContextAllocatorResource{ContextTag::core}.
//
//   Open Question 2 resolution: __cpp_lib_flat_map = 202511 on the locked
//   toolchain (Apple Silicon clang 22, macOS 26), so std::flat_map IS shipped.
//   However std::pmr::flat_map does NOT exist as a namespace alias in libc++ 22
//   (unlike std::pmr::unordered_map which is a C++17 alias).  Per the fallback
//   rule ("if std::pmr::flat_* unavailable, use std::pmr::unordered_*"), this
//   plan uses std::pmr::unordered_map.  A follow-up PLAN may add a
//   std::flat_map-backed variant once the PMR alias lands in libc++.
//
//   eastl::string_view parameters → std::string_view (row 2, ships C++17).
//   eastl::string members         → std::pmr::string (row 1, ships C++17).
//
//   Heterogeneous lookup (MED-3: no per-call key materialisation):
//     loaded_.find(std::string_view{...})  — no std::pmr::string allocation
//     per lookup; std::equal_to<> provides the transparent equality.
//
// PHILOSOPHY §9: "Plugin ABI gated by middleman dylib hash."

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <string>
#include <string_view>
#include <unordered_map>

#include <glibre/alloc.hpp>
#include <glibre/compat/transparent_string_hash.hpp>
#include <glibre/core/frame_phase.hpp>
#include <glibre/core/plugin_manifest.hpp>
#include <glibre/error.hpp>

namespace glibre::core {

// ---------------------------------------------------------------------------
// PluginRecord — immutable descriptor stored per registered plugin.
//
// Stored in the registry after a plugin passes all gates.  The record is
// used by subsequent loads for name-collision and dependency checks.
//
// Allocator-awareness: PluginRecord declares allocator_type and an
// allocator-extended constructor so that std::pmr::unordered_map's
// try_emplace/piecewise_construct path constructs the value directly under
// the map's allocator.  Without this, default-constructing a PluginRecord
// and then copy-assigning pmr::string members falls back to
// get_default_resource() for the destination (PMR's non-propagating
// move-assign semantics), silently bypassing the per-context ceiling
// enforced by PluginLoaderRegistry::mr_.
// ---------------------------------------------------------------------------

struct PluginRecord {
    using allocator_type = std::pmr::polymorphic_allocator<std::byte>;

    // Allocator-extended constructor: wires name and path to the supplied
    // allocator so both strings live under the caller's memory resource.
    explicit PluginRecord(
        std::string_view name_sv, SemVer ver, std::string_view path_sv, const allocator_type& alloc
    )
        : name{name_sv, alloc},
          version{ver},
          path{path_sv, alloc} {}

    std::pmr::string name;  // manifest.name  — must be unique
    SemVer version;         // manifest.version
    std::pmr::string path;  // filesystem path to the .dylib (may be empty in tests)
};

// ---------------------------------------------------------------------------
// PluginLoaderRegistry — owns the loaded-plugin table and validates gates.
//
// Thread safety: None.  Mutations must happen during phase 8 (HotReload)
// when the world is already drained (frame-phases.md §8).  No external
// locking is provided; the engine's frame-phase barrier is the only guard.
//
// Allocator: PluginLoaderRegistry holds a PerContextAllocatorResource backed
// by the core context allocator (ContextTag::core).  All PMR containers in
// this class allocate under the core context ceiling (perf-budget.md §1).
// The allocator is injected at construction time; production callers pass the
// real PerContextAllocator; tests may pass any std::pmr::memory_resource*.
//
// Member declaration order (critical for RAII):
//   mr_ must be declared before loaded_ so the resource outlives the map.
// ---------------------------------------------------------------------------

class PluginLoaderRegistry {
public:
    // -----------------------------------------------------------------------
    // PluginLoaderRegistry(host_engine_version, mr) — resource-injected ctor.
    //
    // `mr` backs all PMR string and map storage in this registry.  The
    // parameter is REQUIRED: callers must explicitly choose a resource.
    // Production callers pass a PerContextAllocatorResource (perf-budget.md
    // §1 core ceiling).  Tests pass std::pmr::get_default_resource()
    // explicitly so per-context tracking is consciously opt-in and no
    // callsite silently falls through to the global default.
    //
    // The host engine version is injected at construction time so that it
    // participates in gate checks without coupling the registry to a global
    // constant.  Tests supply a synthetic version; production supplies the
    // real `glibre_core_version` value.
    // -----------------------------------------------------------------------

    explicit PluginLoaderRegistry(
        SemVer host_engine_version, std::pmr::memory_resource* mr
    ) noexcept;

    // -----------------------------------------------------------------------
    // validate_manifest_abi_hash — gate 1a (manifest field only).
    //
    // Compares manifest.abi_hash against expected_abi_hash.
    // Per plugin-abi.md §"Loader Sequence" step 4, the manifest field and
    // the exported symbol must both agree with the host's hash.  This
    // method checks the manifest field; validate_symbol_abi_hash checks
    // the compiled-in symbol value.
    //
    // On mismatch returns std::unexpected(core::Error::PluginAbiHashMismatch).
    // -----------------------------------------------------------------------

    [[nodiscard]] Result<void> validate_manifest_abi_hash(
        const PluginManifest& manifest, std::string_view expected_abi_hash
    ) const noexcept;

    // -----------------------------------------------------------------------
    // validate_symbol_abi_hash — gate 1b (exported symbol value only).
    //
    // Compares the value the loader read from the plugin's exported
    // `glibre_plugin_abi_hash` C symbol against expected_abi_hash.
    // The redundant check (step 4) catches a malformed manifest whose
    // abi_hash field disagrees with the compiled-in symbol.
    //
    // On mismatch returns std::unexpected(core::Error::PluginAbiHashMismatch).
    // -----------------------------------------------------------------------

    [[nodiscard]] Result<void> validate_symbol_abi_hash(
        std::string_view symbol_abi_hash, std::string_view expected_abi_hash
    ) const noexcept;

    // -----------------------------------------------------------------------
    // validate_engine_version — run gate 2 (engine version).
    //
    // Checks manifest.min_engine_version ≤ host engine version.
    // On violation returns core::Error::PluginEngineTooOld.
    // -----------------------------------------------------------------------

    [[nodiscard]] Result<void>
    validate_engine_version(const PluginManifest& manifest) const noexcept;

    // -----------------------------------------------------------------------
    // validate_name_unique — run gate 3 (name uniqueness).
    //
    // Per plugin-abi.md §"Loader Sequence" step 6:
    //   "refuse if a plugin with the same name is already registered with
    //    a different file path."
    //
    // Collision is fired only when name matches AND file_path differs.
    // Same name + same file_path is treated as an idempotent re-registration
    // (the hot-reload path in plan #231 loads the same .dylib again after a
    // swap; the registry must accept it without error).
    //
    // On collision returns core::Error::PluginNameCollision.
    // -----------------------------------------------------------------------

    [[nodiscard]] Result<void>
    validate_name_unique(const PluginManifest& manifest, std::string_view file_path) const noexcept;

    // -----------------------------------------------------------------------
    // validate_dependencies — run gate 4 (dependency resolution).
    //
    // Checks every entry in manifest.depends_on is already registered.
    // On the first missing dependency returns core::Error::PluginDependencyMissing.
    // -----------------------------------------------------------------------

    [[nodiscard]] Result<void> validate_dependencies(const PluginManifest& manifest) const noexcept;

    // -----------------------------------------------------------------------
    // validate_drain_phase — phase-8 precondition guard (plan #981).
    //
    // Authority: reviews/decisions/plugin-abi.md §"Loader Sequence" step 8:
    //   "at this point the world is already drained (frame-phases §8 guarantee).
    //    The loader is free to mutate type registry, system schedule, pass
    //    registry, panel registry."
    //   reviews/decisions/frame-phases.md §8 — hot-reload is the ONLY phase
    //   during which registry mutations (call_register, rebuild_schedule,
    //   migrate_components) are permitted.
    //
    // SRP boundary: PluginLoaderRegistry does NOT own phase state.  The caller
    // (PluginLoader in the phase-8 frame loop) supplies the current frame phase
    // as a parameter.  Plans #247 and #248 will land the full FramePhaseTracker
    // API; this guard is the minimal injection point they can replace.
    //
    // Returns success when current_phase == Phase::HotReload (phase 8).
    // Returns std::unexpected(core::Error::FramePhaseMisordered) on any other
    // phase — registry mutations outside the hot-reload barrier are forbidden.
    //
    // Call this BEFORE any registry mutation (validate_all, register_plugin,
    // rebuild_schedule, migrate_components) to enforce the frame-phase invariant
    // in debug and release builds alike.
    // -----------------------------------------------------------------------

    [[nodiscard]] static Result<void> validate_drain_phase(Phase current_phase) noexcept;

    // -----------------------------------------------------------------------
    // validate_all — run all four gates in order (steps 4–7).
    //
    // symbol_abi_hash: value read from the plugin's exported
    //   `glibre_plugin_abi_hash` C symbol (a const char* that the loader
    //   dlsym()s).  The manifest.abi_hash field is also checked; both
    //   must equal expected_abi_hash per plugin-abi.md §step 4.
    //
    // file_path: the .dylib path being loaded, forwarded to
    //   validate_name_unique for the "different file path" qualifier
    //   (plugin-abi.md §step 6).
    //
    // Returns the first error encountered; gates are run in plugin-abi.md
    // ordering (hash → engine version → name → deps).
    // -----------------------------------------------------------------------

    [[nodiscard]] Result<void> validate_all(
        const PluginManifest& manifest,
        std::string_view expected_abi_hash,
        std::string_view symbol_abi_hash,
        std::string_view file_path
    ) const noexcept;

    // -----------------------------------------------------------------------
    // register_plugin — record a plugin as successfully loaded.
    //
    // Call this AFTER validate_all succeeds and AFTER glibre_plugin_register
    // (plan #231) completes without error.  The registry then makes the
    // plugin's name visible to subsequent dependency checks.
    //
    // path is the filesystem path to the .dylib; may be empty in tests.
    //
    // Returns an error (without modifying the registry) if the name is
    // already registered with a different path.  This is an unconditional
    // precondition check — not a debug-only assert — so release builds are
    // protected from silent overwrites.
    //
    // Same name + same path: idempotent no-op, returns success.
    // Same name + different path: returns core::Error::PluginNameCollision.
    // -----------------------------------------------------------------------

    [[nodiscard]] Result<void>
    register_plugin(const PluginManifest& manifest, std::string_view path) noexcept;

    // -----------------------------------------------------------------------
    // is_registered — query whether a plugin name is already registered.
    //
    // Heterogeneous lookup: std::string_view avoids materialising a temporary
    // std::pmr::string key per call (TransparentStringHash + std::equal_to<>).
    // -----------------------------------------------------------------------

    [[nodiscard]] bool is_registered(std::string_view name) const noexcept;

    // -----------------------------------------------------------------------
    // loaded_count — number of successfully registered plugins.
    // -----------------------------------------------------------------------

    [[nodiscard]] std::size_t loaded_count() const noexcept;

private:
    // -----------------------------------------------------------------------
    // is_collision — name/path collision predicate (single source of truth).
    //
    // Returns true when a plugin with the same name is already registered
    // with a DIFFERENT file path — the condition that fires PluginNameCollision.
    // Same name + same path (hot-reload idempotent path): returns false.
    // Name not present: returns false.
    //
    // Used by both validate_name_unique and register_plugin so the predicate
    // has one definition (SOLID SRP: one reason to change).
    // -----------------------------------------------------------------------

    [[nodiscard]] bool
    is_collision(std::string_view name, std::string_view file_path) const noexcept;

    SemVer host_engine_version_;

    // mr_ must be declared before loaded_ (RAII: resource outlives map).
    // Points to the backing resource injected at construction.  Production
    // callers pass a PerContextAllocatorResource; tests pass get_default_resource().
    std::pmr::memory_resource* mr_;

    // key = plugin name (std::pmr::string), value = PluginRecord.
    //
    // Container choice (plan #1044, Open Question 2):
    //   std::flat_map is shipped (__cpp_lib_flat_map = 202511) but
    //   std::pmr::flat_map does NOT exist as a namespace alias in libc++ 22.
    //   Fallback to std::pmr::unordered_map per eastl-removal.md OQ-2.
    //
    // TransparentStringHash + std::equal_to<> enable heterogeneous lookup:
    //   loaded_.find(std::string_view{...})  — no std::pmr::string allocation
    //   per lookup (MED-3: avoids per-call key materialisation).
    std::pmr::unordered_map<
        std::pmr::string,
        PluginRecord,
        glibre::TransparentStringHash,
        std::equal_to<>>
        loaded_;
};

}  // namespace glibre::core
