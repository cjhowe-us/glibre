// tests/core/plugin_manifest/plugin_manifest_test.cpp
//
// Catch2 unit tests for glibre::core::PluginManifest schema.
//
// Named test cases (per plan #223 Unit Test Plan):
//   - plugin_manifest_round_trip
//   - plugin_manifest_open_returns_not_found_on_missing_path
//
// Scope: schema correctness and open() error paths.
// Out of scope: Fory serialisation (plan #225), loader sequence (#229..#231).

#include <catch2/catch_test_macros.hpp>

#include "glibre/core/plugin_manifest.hpp"
#include "glibre/error.hpp"

using namespace glibre::core;

// ---------------------------------------------------------------------------
// Helper: construct a fully-populated manifest for round-trip testing.
// ---------------------------------------------------------------------------

static PluginManifest make_test_manifest() {
    PluginManifest m;
    m.name     = "glibre.render";
    m.version  = SemVer{1, 2, 3};
    m.abi_hash = "0000000000000000000000000000000000000000000000000000000000000000";
    m.min_engine_version = SemVer{0, 1, 0};

    ComponentDecl comp;
    comp.fqn          = "glibre.render.Camera";
    comp.schema_hash  = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    comp.storage_hint = 0;  // archetype
    m.components.push_back(comp);

    SystemDecl sys;
    sys.name  = "CullSystem";
    sys.phase = 6;  // CullExtract per frame-phases.md
    sys.reads.push_back("glibre.render.Camera");
    sys.writes.push_back("glibre.render.DrawList");
    m.systems.push_back(sys);

    PassDecl pass;
    pass.name         = "opaque-pass";
    pass.render_phase = 7;  // RenderSubmit
    pass.inputs.push_back("glibre.render.DrawList");
    pass.outputs.push_back("glibre.render.BackBuffer");
    m.passes.push_back(pass);

    PanelDecl panel;
    panel.id    = "render-stats";
    panel.title = "Render Statistics";
    panel.area  = 2;
    m.panels.push_back(panel);

    m.depends_on.push_back("glibre.core");

    return m;
}

// ---------------------------------------------------------------------------
// Test: plugin_manifest_round_trip
//
// Construct a PluginManifest in-memory, copy it, and assert all fields are
// preserved.  Validates that:
//  - The struct is an aggregate (static_assert in header guarantees this at
//    compile time; this test exercises the runtime value path).
//  - Copy construction and equality comparison are correct (operator==
//    delegates to per-field EASTL/scalar equality).
//  - All sub-struct types (SemVer, ComponentDecl, SystemDecl, PassDecl,
//    PanelDecl) round-trip correctly through copy.
// ---------------------------------------------------------------------------

TEST_CASE("plugin_manifest_round_trip", "[core][plugin_manifest]") {
    const PluginManifest original = make_test_manifest();

    // Copy — exercises eastl::string / eastl::vector deep-copy paths.
    const PluginManifest copy = original;  // NOLINT(performance-unnecessary-copy-initialization)

    // Top-level scalar and string fields.
    REQUIRE(copy.name     == original.name);
    REQUIRE(copy.abi_hash == original.abi_hash);

    // SemVer fields.
    REQUIRE(copy.version.major == original.version.major);
    REQUIRE(copy.version.minor == original.version.minor);
    REQUIRE(copy.version.patch == original.version.patch);

    REQUIRE(copy.min_engine_version.major == original.min_engine_version.major);
    REQUIRE(copy.min_engine_version.minor == original.min_engine_version.minor);
    REQUIRE(copy.min_engine_version.patch == original.min_engine_version.patch);

    // components vector.
    REQUIRE(copy.components.size() == original.components.size());
    REQUIRE(copy.components[0].fqn          == original.components[0].fqn);
    REQUIRE(copy.components[0].schema_hash  == original.components[0].schema_hash);
    REQUIRE(copy.components[0].storage_hint == original.components[0].storage_hint);

    // systems vector.
    REQUIRE(copy.systems.size() == original.systems.size());
    REQUIRE(copy.systems[0].name         == original.systems[0].name);
    REQUIRE(copy.systems[0].phase        == original.systems[0].phase);
    REQUIRE(copy.systems[0].reads.size() == original.systems[0].reads.size());
    REQUIRE(copy.systems[0].reads[0]     == original.systems[0].reads[0]);

    // passes vector.
    REQUIRE(copy.passes.size() == original.passes.size());
    REQUIRE(copy.passes[0].name         == original.passes[0].name);
    REQUIRE(copy.passes[0].render_phase == original.passes[0].render_phase);

    // panels vector.
    REQUIRE(copy.panels.size() == original.panels.size());
    REQUIRE(copy.panels[0].id    == original.panels[0].id);
    REQUIRE(copy.panels[0].title == original.panels[0].title);
    REQUIRE(copy.panels[0].area  == original.panels[0].area);

    // depends_on vector.
    REQUIRE(copy.depends_on.size() == original.depends_on.size());
    REQUIRE(copy.depends_on[0]     == original.depends_on[0]);

    // Aggregate equality (operator== synthesised from field-by-field comparison).
    REQUIRE(copy == original);
}

// ---------------------------------------------------------------------------
// Test: plugin_manifest_open_returns_not_found_on_missing_path
//
// Call PluginManifest::open() with a path that is guaranteed not to exist.
// Assert that the returned Result carries core::Error::PluginManifestNotFound.
//
// This exercises the error-path through PluginManifest::open() without
// requiring the Fory deserialisation pipeline (plan #225) to be present.
// ---------------------------------------------------------------------------

TEST_CASE("plugin_manifest_open_returns_not_found_on_missing_path",
          "[core][plugin_manifest]") {
    // /tmp is always writable but this particular path is intentionally absent.
    const auto result = PluginManifest::open("/tmp/glibre_nonexistent_manifest_42.manifest");

    REQUIRE(!result.has_value());

    const glibre::Error& err = result.error();

    // The error must be the core::Error variant arm.
    const auto* core_err = eastl::get_if<glibre::core::Error>(&err.code());
    REQUIRE(core_err != nullptr);
    REQUIRE(*core_err == glibre::core::Error::PluginManifestNotFound);
}
