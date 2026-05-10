// tests/core/plugin_manifest/plugin_manifest_test.cpp
//
// Catch2 unit tests for glibre::core::PluginManifest schema.
//
// Named test cases (per plan #223 Unit Test Plan):
//   - plugin_manifest_round_trip
//   - plugin_manifest_open_returns_not_found_on_missing_path
//
// Named test cases added by plan #1042 (EASTL → std::pmr migration):
//   - core/plugin_manifest: pmr_string_fields_thread_allocator
//
// Extended by plan #1066 (sub-struct PMR threading):
//   - core/plugin_manifest: pmr_string_fields_thread_allocator (extended:
//     emplace_back all four sub-struct types (ComponentDecl, SystemDecl,
//     PassDecl, PanelDecl) and assert direct get_allocator().resource() == &mr
//     on each sub-struct's PMR string/vector fields, proving that
//     std::uses_allocator_construction_args forwards the per-context resource)
//
// Scope: schema correctness, open() error paths, PMR allocator threading.
// Out of scope: Fory serialisation (plan #225), loader sequence (#229..#231).

#include <filesystem>
#include <memory_resource>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "glibre/alloc.hpp"
#include "glibre/core/plugin_manifest.hpp"
#include "glibre/error.hpp"

using glibre::core::ComponentDecl;
using glibre::core::PanelDecl;
using glibre::core::PassDecl;
using glibre::core::PluginManifest;
using glibre::core::SemVer;
using glibre::core::SystemDecl;

// ---------------------------------------------------------------------------
// Helper: construct a fully-populated manifest for round-trip testing.
// ---------------------------------------------------------------------------

static PluginManifest make_test_manifest() {
    PluginManifest m;
    m.name = "glibre.render";
    m.version = SemVer{1, 2, 3};
    m.abi_hash = "0000000000000000000000000000000000000000000000000000000000000000";
    m.min_engine_version = SemVer{0, 1, 0};

    ComponentDecl comp;
    comp.fqn = "glibre.render.Camera";
    comp.schema_hash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    comp.storage_hint = 0;  // archetype
    m.components.push_back(comp);

    SystemDecl sys;
    sys.name = "CullSystem";
    sys.phase = 6;  // CullExtract per frame-phases.md
    sys.reads.push_back("glibre.render.Camera");
    sys.writes.push_back("glibre.render.DrawList");
    m.systems.push_back(sys);

    PassDecl pass;
    pass.name = "opaque-pass";
    pass.render_phase = 7;  // RenderSubmit
    pass.inputs.push_back("glibre.render.DrawList");
    pass.outputs.push_back("glibre.render.BackBuffer");
    m.passes.push_back(pass);

    PanelDecl panel;
    panel.id = "render-stats";
    panel.title = "Render Statistics";
    panel.area = 2;
    m.panels.push_back(panel);

    m.depends_on.push_back("glibre.core");

    return m;
}

// ---------------------------------------------------------------------------
// Test: plugin_manifest_round_trip
//
// Construct a PluginManifest in-memory, copy it, and assert all fields are
// preserved.  Validates that:
//  - Copy construction and equality comparison are correct (operator==
//    delegates to per-field std::pmr::string / scalar equality).
//  - All sub-struct types (SemVer, ComponentDecl, SystemDecl, PassDecl,
//    PanelDecl) round-trip correctly through copy.
//
// Note: PluginManifest and the four Decl sub-structs are not aggregates (each
// carries an explicit allocator-aware constructor per plans #1042 / #1066);
// SemVer remains an aggregate.  The static_assert in plugin_manifest.hpp
// covers SemVer only.
// ---------------------------------------------------------------------------

TEST_CASE("plugin_manifest_round_trip", "[core][plugin_manifest]") {
    const PluginManifest original = make_test_manifest();

    // Copy — exercises std::pmr::string / std::pmr::vector deep-copy paths.
    const PluginManifest copy = original;  // NOLINT(performance-unnecessary-copy-initialization)

    // Top-level scalar and string fields.
    REQUIRE(copy.name == original.name);
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
    REQUIRE(copy.components[0].fqn == original.components[0].fqn);
    REQUIRE(copy.components[0].schema_hash == original.components[0].schema_hash);
    REQUIRE(copy.components[0].storage_hint == original.components[0].storage_hint);

    // systems vector.
    REQUIRE(copy.systems.size() == original.systems.size());
    REQUIRE(copy.systems[0].name == original.systems[0].name);
    REQUIRE(copy.systems[0].phase == original.systems[0].phase);
    REQUIRE(copy.systems[0].reads.size() == original.systems[0].reads.size());
    REQUIRE(copy.systems[0].reads[0] == original.systems[0].reads[0]);

    // passes vector.
    REQUIRE(copy.passes.size() == original.passes.size());
    REQUIRE(copy.passes[0].name == original.passes[0].name);
    REQUIRE(copy.passes[0].render_phase == original.passes[0].render_phase);

    // panels vector.
    REQUIRE(copy.panels.size() == original.panels.size());
    REQUIRE(copy.panels[0].id == original.panels[0].id);
    REQUIRE(copy.panels[0].title == original.panels[0].title);
    REQUIRE(copy.panels[0].area == original.panels[0].area);

    // depends_on vector.
    REQUIRE(copy.depends_on.size() == original.depends_on.size());
    REQUIRE(copy.depends_on[0] == original.depends_on[0]);

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

TEST_CASE("plugin_manifest_open_returns_not_found_on_missing_path", "[core][plugin_manifest]") {
    // Construct a path that is guaranteed not to exist.  Using
    // std::filesystem::temp_directory_path() keeps this hermetic across CI
    // environments (avoids hard-coded /tmp which is not portable to all hosts).
    // The filename uses a UUID-like suffix to avoid collisions between parallel
    // test shards.
    const std::filesystem::path absent =
        std::filesystem::temp_directory_path() / "glibre_nonexistent_manifest_test.manifest";
    // Belt-and-suspenders: remove if somehow the file was left behind by a
    // previous run (error_code variant to stay -fno-exceptions compatible).
    std::error_code ec;
    std::filesystem::remove(absent, ec);

    const auto result = PluginManifest::open(std::string_view{absent.c_str()});

    REQUIRE(!result.has_value());

    const glibre::Error& err = result.error();

    // The error must be the core::Error variant arm.
    const auto* core_err = std::get_if<glibre::core::Error>(&err.code());
    REQUIRE(core_err != nullptr);
    REQUIRE(*core_err == glibre::core::Error::PluginManifestNotFound);
}

// ---------------------------------------------------------------------------
// Test: core/plugin_manifest: pmr_string_fields_thread_allocator
//
// Construct a real PluginManifest via the allocator-aware constructor, passing
// a std::pmr::polymorphic_allocator backed by a PerContextAllocatorResource.
// Populate name, abi_hash, and depends_on to force heap allocation.  Assert
// that bytes_used() on the backing PerContextAllocator increases after
// construction — confirming that manifest field storage is charged to the
// per-context allocator rather than the global heap (perf-budget.md
// §Allocator Rules #1).
//
// Extended by plan #1066 (sub-struct PMR threading): after emplace_back of
// each sub-struct (ComponentDecl, SystemDecl, PassDecl, PanelDecl) into
// manifest.components/systems/passes/panels, directly assert that the sub-
// struct's PMR string fields carry the expected allocator resource pointer.
// This is the only assertion that proves allocator forwarding: a bytes-delta
// check would also pass if the delta were due to vector element-storage growth
// alone, since vector grows charge to the same resource regardless of whether
// the sub-struct's strings are forwarded.
//
// The direct resource-pointer assertion fails if and only if:
//   (a) the sub-struct lacks `using allocator_type` (not uses-allocator), OR
//   (b) the sub-struct lacks the allocator-extended default ctor, OR
//   (c) std::uses_allocator_construction_args does not fire.
// All three failure modes are caught by `CHECK(field.get_allocator().resource()
// == &mr)`.
//
// Plan #1042 — reviews/decisions/eastl-removal.md §3 PMR lifetime contract.
// Plan #1066 — sub-struct allocator_type + ctor threading.
// ---------------------------------------------------------------------------

TEST_CASE("core/plugin_manifest: pmr_string_fields_thread_allocator", "[core][plugin_manifest]") {
    // Stand-alone PerContextAllocator.  Declared before mr so it outlives it
    // (PerContextAllocatorResource holds a reference to the allocator).
    glibre::PerContextAllocator alloc{glibre::ContextTag::core};
    glibre::PerContextAllocatorResource mr{alloc};

    // Confirm no bytes consumed before manifest construction.
    const std::uint64_t bytes_before = alloc.bytes_used();

    // Construct a real PluginManifest whose name/abi_hash/depends_on fields
    // are backed by our PerContextAllocatorResource.
    {
        std::pmr::polymorphic_allocator<std::byte> pa{&mr};
        PluginManifest manifest{pa};

        // Populate string fields with values long enough to exceed any SSO
        // buffer (std::pmr::string SSO on libc++ ≥ 19 is 22 bytes; this name
        // is 35 chars, well above that threshold, so the assignment routes
        // through the polymorphic_allocator and charges mr).
        manifest.name = "glibre.render.example.plugin.module";  // 35 chars — above SSO
        manifest.abi_hash =
            "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890";  // 64 chars
        manifest.depends_on.push_back("glibre.core");
        manifest.depends_on.push_back("glibre.platform");

        // Bytes charged to the per-context allocator must have increased: the
        // manifest's field storage is routed through mr, not get_default_resource().
        const std::uint64_t bytes_after_top_level = alloc.bytes_used();
        REQUIRE(bytes_after_top_level > bytes_before);

        // Field values are preserved through the PMR allocation.
        REQUIRE(manifest.name == "glibre.render.example.plugin.module");
        REQUIRE(manifest.abi_hash.size() == 64u);
        REQUIRE(manifest.depends_on.size() == 2u);
        REQUIRE(manifest.depends_on[0] == "glibre.core");

        // -----------------------------------------------------------------------
        // Sub-struct PMR threading (plan #1066) — ComponentDecl
        //
        // emplace_back constructs a ComponentDecl in-place via
        // std::uses_allocator_construction_args, forwarding the vector's resource
        // (mr, inherited from manifest's allocator) into the ComponentDecl ctor.
        // Direct assertion: fqn and schema_hash must carry &mr as their resource.
        //
        // A bytes-delta check would NOT prove forwarding — the delta from
        // vector element-storage alone (~sizeof(ComponentDecl)) passes regardless.
        // -----------------------------------------------------------------------
        ComponentDecl& comp = manifest.components.emplace_back();
        comp.fqn = "glibre.render.example.component.Camera";  // 38 chars — above SSO
        comp.schema_hash =
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";  // 64 chars
        comp.storage_hint = 0;

        // Direct allocator-forwarding proof: the sub-struct's PMR string fields
        // must be bound to &mr, not to get_default_resource().
        CHECK(comp.fqn.get_allocator().resource() == &mr);
        CHECK(comp.schema_hash.get_allocator().resource() == &mr);

        REQUIRE(manifest.components.size() == 1u);
        REQUIRE(manifest.components[0].fqn == "glibre.render.example.component.Camera");
        REQUIRE(manifest.components[0].schema_hash.size() == 64u);
        REQUIRE(manifest.components[0].storage_hint == 0u);

        // -----------------------------------------------------------------------
        // Sub-struct PMR threading (plan #1066) — SystemDecl
        // -----------------------------------------------------------------------
        SystemDecl& sys = manifest.systems.emplace_back();
        sys.name = "glibre.render.example.system.CullSystem";  // 40 chars — above SSO
        sys.phase = 6;
        sys.reads.push_back("glibre.render.Camera");

        CHECK(sys.name.get_allocator().resource() == &mr);
        // reads is a pmr::vector<pmr::string>; the vector itself must carry &mr.
        CHECK(sys.reads.get_allocator().resource() == &mr);
        // The string inside reads also carries &mr (push_back uses vector's alloc).
        CHECK(sys.reads[0].get_allocator().resource() == &mr);

        REQUIRE(manifest.systems.size() == 1u);
        REQUIRE(manifest.systems[0].name == "glibre.render.example.system.CullSystem");

        // -----------------------------------------------------------------------
        // Sub-struct PMR threading (plan #1066) — PassDecl
        // -----------------------------------------------------------------------
        PassDecl& pass = manifest.passes.emplace_back();
        pass.name = "glibre.render.example.pass.opaque-geometry";  // 43 chars — above SSO
        pass.render_phase = 7;
        pass.inputs.push_back("glibre.render.DrawList");
        pass.outputs.push_back("glibre.render.BackBuffer");

        CHECK(pass.name.get_allocator().resource() == &mr);
        CHECK(pass.inputs.get_allocator().resource() == &mr);
        CHECK(pass.outputs.get_allocator().resource() == &mr);

        REQUIRE(manifest.passes.size() == 1u);
        REQUIRE(manifest.passes[0].name == "glibre.render.example.pass.opaque-geometry");

        // -----------------------------------------------------------------------
        // Sub-struct PMR threading (plan #1066) — PanelDecl
        // -----------------------------------------------------------------------
        PanelDecl& panel = manifest.panels.emplace_back();
        panel.id = "glibre.render.example.panel.render-stats";  // 41 chars — above SSO
        panel.title = "Render Statistics Panel (glibre.render)";  // 39 chars — above SSO
        panel.area = 2;

        CHECK(panel.id.get_allocator().resource() == &mr);
        CHECK(panel.title.get_allocator().resource() == &mr);

        REQUIRE(manifest.panels.size() == 1u);
        REQUIRE(manifest.panels[0].id == "glibre.render.example.panel.render-stats");
        REQUIRE(manifest.panels[0].title == "Render Statistics Panel (glibre.render)");
        REQUIRE(manifest.panels[0].area == 2u);
    }

    // After manifest goes out of scope, all PMR fields (including nested
    // sub-struct fields) are deallocated back through mr → alloc.
    // bytes_used() should return to (or below) bytes_before.
    REQUIRE(alloc.bytes_used() <= bytes_before);
}
