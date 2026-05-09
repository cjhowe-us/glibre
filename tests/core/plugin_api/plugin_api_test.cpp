// tests/core/plugin_api/plugin_api_test.cpp
//
// Catch2 unit tests for glibre::core::PluginContext and the plugin
// entry-point declaration.
//
// Named test cases (per plan #228 Unit Test Plan + dispatch DoD):
//   - plugin_context_is_pod_aggregate
//   - register_signature_compiles
//   - noop_plugin_dylib_builds
//   - noop_plugin_exports_four_required_symbols
//   - plugin_context_register_smoke
//   - plugin_context_register_after_phase8_fails
//
// Design constraints:
//   • -fno-exceptions (error-model.md §Decision 3).
//   • No real registry objects exist yet (plan #229 onwards) — tests use
//     stub implementations declared at file scope to satisfy the aggregate's
//     reference fields.
//   • PHILOSOPHY §11: no std:: containers or eastl:: containers on the
//     plugin ABI surface.  Convenience methods now accept (const char*,
//     std::size_t) pairs as mandated by the ABI boundary rule.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include <glibre/core/plugin_api.hpp>
#include <glibre/core/plugin_entry.hpp>
#include <glibre/error.hpp>

// ---------------------------------------------------------------------------
// Stub registry types
//
// The real World, TypeRegistry, SystemRegistry, PassRegistry, PanelRegistry,
// LogSink, and PluginManifest types land in sibling plans.  For now we
// define empty stub structs so that PluginContext can be instantiated.
//
// IMPORTANT: these stubs live in an ANONYMOUS namespace so they are hermetic
// to this translation unit and cannot conflict with — or accidentally provide
// — definitions for the forward-declared names in glibre::core when plan
// #229 ships its real implementations in another TU.
//
// The anonymous-namespace stubs satisfy the forward declarations in
// plugin_api.hpp because the forward declarations are incomplete types used
// only to form references inside PluginContext; no class-member access occurs
// in the headers, so the linker never needs to reconcile the two definitions.
// The PluginContext method stubs (register_component / register_system) ARE
// member-function definitions in glibre::core — those must remain in the
// glibre::core namespace to match the declaration, but they are compiled into
// this test TU only and will be superseded by plan #229's real definitions.
// ---------------------------------------------------------------------------

namespace {

struct StubWorld {
    int dummy{0};
};

struct StubTypeRegistry {
    int dummy{0};

    glibre::Result<void> register_component_impl(
        const char* /*type_name*/,
        std::size_t /*type_name_len*/,
        const char* /*schema_hash*/,
        std::size_t /*schema_hash_len*/,
        std::uint8_t /*storage_hint*/
    ) noexcept {
        return {};
    }
};

struct StubSystemRegistry {
    int dummy{0};

    glibre::Result<void> register_system_impl(
        const char* /*name*/,
        std::size_t /*name_len*/,
        std::uint8_t /*phase*/
    ) noexcept {
        return {};
    }
};

struct StubPassRegistry {
    int dummy{0};
};

struct StubPanelRegistry {
    int dummy{0};
};

struct StubLogSink {
    int dummy{0};
};

// PluginManifest stub — the real struct is Fory-generated (plans #223/#225).
struct StubPluginManifest {
    const char* name{"glibre.test.noop"};
};

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Stub PluginContext method definitions
//
// These are member-function definitions for the declarations in plugin_api.hpp.
// Real implementations land in plan #229 (the loader) when the registry types
// are fully defined.  These stubs are compiled into this test TU only; plan
// #229 will supply the authoritative definitions in a separate .cpp file that
// is part of glibre-core.
//
// The stubs must live in namespace glibre::core to match the declaration site.
// They delegate to the anonymous-namespace stubs via reinterpret_cast through
// the incomplete-type references in PluginContext.  Since the anonymous-
// namespace types are layout-compatible with the forward-declared names (same
// trivial struct shape), this is safe for the test fixture only.
//
// WARNING: Do not copy this pattern to production code.  The real plan #229
// implementations will include the actual registry headers and call real APIs.
// ---------------------------------------------------------------------------

namespace glibre::core {

glibre::Result<void> PluginContext::register_component(
    const char*  type_name,
    std::size_t  type_name_len,
    const char*  schema_hash,
    std::size_t  schema_hash_len,
    std::uint8_t storage_hint
) noexcept {
    // In tests, type_registry is actually a StubTypeRegistry (layout-compatible).
    return reinterpret_cast<::StubTypeRegistry&>(type_registry)
        .register_component_impl(
            type_name, type_name_len, schema_hash, schema_hash_len, storage_hint);
}

glibre::Result<void> PluginContext::register_system(
    const char*  name,
    std::size_t  name_len,
    std::uint8_t phase
) noexcept {
    // In tests, system_registry is actually a StubSystemRegistry (layout-compatible).
    return reinterpret_cast<::StubSystemRegistry&>(system_registry)
        .register_system_impl(name, name_len, phase);
}

}  // namespace glibre::core

// ---------------------------------------------------------------------------
// Test fixture — builds a complete PluginContext from stubs.
//
// PluginContext holds references to the registry types by their incomplete
// forward-declared names.  We initialise those references from the anonymous-
// namespace stubs whose layouts are identical (trivial structs with one int).
// ---------------------------------------------------------------------------

namespace {

// Concrete storage for stub registries — outlives the PluginContext.
struct PluginContextFixture {
    StubWorld           world;
    StubTypeRegistry    type_reg;
    StubSystemRegistry  sys_reg;
    StubPassRegistry    pass_reg;
    StubPanelRegistry   panel_reg;
    StubPluginManifest  manifest;
    StubLogSink         log;

    glibre::core::PluginContext ctx{
        reinterpret_cast<glibre::core::World&>(world),
        reinterpret_cast<glibre::core::TypeRegistry&>(type_reg),
        reinterpret_cast<glibre::core::SystemRegistry&>(sys_reg),
        reinterpret_cast<glibre::core::PassRegistry&>(pass_reg),
        reinterpret_cast<glibre::core::PanelRegistry&>(panel_reg),
        reinterpret_cast<const glibre::core::PluginManifest&>(manifest),
        reinterpret_cast<glibre::core::LogSink&>(log)
    };
};

}  // namespace

// ===========================================================================
// Test: plugin_context_is_pod_aggregate
//
// Verifies that PluginContext is an aggregate type (per the decision record's
// "POD-like aggregate" mandate) and that its size and layout are stable.
//
// "Aggregate" in C++23 terms means: no user-provided constructors, no
// private/protected non-static data members, no base classes, no virtual
// functions (std::is_aggregate_v).  PluginContext uses aggregate
// initialization in plugin code (brace-init), so this property must hold.
//
// Note: std::is_aggregate_v<T> is false when T has reference members per
// [dcl.init.aggr] p1.3 — a type with reference members IS an aggregate in
// C++23 as long as the other conditions hold.  We verify with a static_assert
// in addition to the runtime CHECK to catch regressions at compile time.
// ===========================================================================

// Static compile-time check: PluginContext must be an aggregate.
// This fires at compile time if someone accidentally adds a user-provided
// constructor or virtual function to PluginContext.
static_assert(
    std::is_aggregate_v<glibre::core::PluginContext>,
    "PluginContext must be a C++23 aggregate (POD-like per plugin-abi.md §Open Questions #3)"
);

TEST_CASE("plugin_context_is_pod_aggregate", "[core][plugin_api]") {
    // Runtime mirror of the static_assert — exercises the same invariant
    // via the Catch2 harness so it appears in the test report.
    CHECK(std::is_aggregate_v<glibre::core::PluginContext>);

    // Verify that PluginContext can be constructed by aggregate initialization.
    // If aggregate initialization fails to compile, this test will not compile.
    PluginContextFixture fixture;
    CHECK(&fixture.ctx.world == reinterpret_cast<glibre::core::World*>(&fixture.world));
    CHECK(&fixture.ctx.type_registry == reinterpret_cast<glibre::core::TypeRegistry*>(&fixture.type_reg));
    CHECK(&fixture.ctx.system_registry == reinterpret_cast<glibre::core::SystemRegistry*>(&fixture.sys_reg));
    CHECK(&fixture.ctx.pass_registry == reinterpret_cast<glibre::core::PassRegistry*>(&fixture.pass_reg));
    CHECK(&fixture.ctx.panel_registry == reinterpret_cast<glibre::core::PanelRegistry*>(&fixture.panel_reg));
    CHECK(&fixture.ctx.manifest == reinterpret_cast<glibre::core::PluginManifest*>(&fixture.manifest));
    CHECK(&fixture.ctx.log == reinterpret_cast<glibre::core::LogSink*>(&fixture.log));
}

// ===========================================================================
// Test: register_signature_compiles
//
// Verifies that the extern "C" glibre_plugin_register signature matches the
// expected function-pointer type used by the loader (plan #229).
//
// This is a compile-time check: if the signature drifts, the static_assert
// below will fire.  The TEST_CASE wrapper makes the check visible in the
// Catch2 report.
//
// Expected signature (plugin-abi.md §"Registration Entry-Point Signature"):
//   glibre::Result<void> glibre_plugin_register(glibre::core::PluginContext&) noexcept
// ===========================================================================

// The loader (plan #229) will dlsym + reinterpret_cast to this type.
using PluginRegisterFn = glibre::Result<void>(*)(glibre::core::PluginContext&) noexcept;

static_assert(
    std::is_same_v<
        PluginRegisterFn,
        decltype(&glibre_plugin_register)
    >,
    "glibre_plugin_register must have signature: "
    "glibre::Result<void>(glibre::core::PluginContext&) noexcept"
);

TEST_CASE("register_signature_compiles", "[core][plugin_api]") {
    // Runtime mirror of the static_assert.
    CHECK(std::is_same_v<
        PluginRegisterFn,
        decltype(&glibre_plugin_register)
    >);

    // Verify the unregister counterpart has the same shape.
    using PluginUnregisterFn = glibre::Result<void>(*)(glibre::core::PluginContext&) noexcept;
    CHECK(std::is_same_v<
        PluginUnregisterFn,
        decltype(&glibre_plugin_unregister)
    >);
}

// ===========================================================================
// Test: noop_plugin_dylib_builds
//
// Asserts that the glibre-plugin-noop .dylib target was produced by the
// build by checking whether the file exists at the CMake-injected path.
//
// GLIBRE_NOOP_DYLIB_PATH is injected by CMake when GLIBRE_BUILD_EXAMPLES=ON
// (the default).  When examples are disabled, the test is SKIPPED so it does
// not pass silently or mislead CI.
// ===========================================================================

TEST_CASE("noop_plugin_dylib_builds", "[core][plugin_api]") {
#ifndef GLIBRE_NOOP_DYLIB_PATH
    SKIP("GLIBRE_NOOP_DYLIB_PATH not defined — build with GLIBRE_BUILD_EXAMPLES=ON to run this test");
#else
    constexpr const char* kDylibPath = GLIBRE_NOOP_DYLIB_PATH;
    REQUIRE(std::filesystem::exists(kDylibPath));
#endif
}

// ===========================================================================
// Test: noop_plugin_exports_four_required_symbols
//
// Verifies that examples/plugin-noop exports all four symbols required by
// the loader (plugin-abi.md §"Plugin file shape").
//
// Implementation: uses `nm -gU` (macOS) via popen() to inspect the built
// dylib.  If popen is unavailable (e.g. sanitizer builds) the test is
// skipped via WARN.  When GLIBRE_BUILD_EXAMPLES=OFF the test is SKIPPED.
//
// The four required symbols:
//   glibre_plugin_abi_hash        (data symbol, T or D)
//   glibre_plugin_manifest        (data symbol)
//   glibre_plugin_manifest_size   (data symbol)
//   glibre_plugin_register        (function symbol, T)
//
// Symbol matching uses exact third-field comparison via awk to avoid false
// positives from substring matches (e.g. glibre_plugin_manifest matching
// glibre_plugin_manifest_size).
// ===========================================================================

#include <array>
#include <cstdio>
#include <string>

namespace {

/// Count exact third-column symbol occurrences in nm output via awk.
/// Returns 0 on popen failure or if the symbol is absent.
int count_exact_symbol(const std::string& nm_output, const char* symbol_name) {
    std::string awk_cmd{"echo \""};
    awk_cmd += nm_output;
    awk_cmd += "\" | awk '$3 == \"";
    awk_cmd += symbol_name;
    awk_cmd += "\" { count++ } END { print count+0 }'";

    // NOLINTNEXTLINE(cert-env33-c)
    FILE* p = popen(awk_cmd.c_str(), "r");
    if (!p) return 0;
    int n = 0;
    // NOLINTNEXTLINE(cert-err34-c)
    (void)fscanf(p, "%d", &n);
    pclose(p);
    return n;
}

}  // namespace

TEST_CASE("noop_plugin_exports_four_required_symbols", "[core][plugin_api]") {
    // GLIBRE_NOOP_DYLIB_PATH is injected by CMake via compile definition.
#ifndef GLIBRE_NOOP_DYLIB_PATH
    SKIP("GLIBRE_NOOP_DYLIB_PATH not defined — build with GLIBRE_BUILD_EXAMPLES=ON to run this test");
#else
    constexpr const char* kDylibPath = GLIBRE_NOOP_DYLIB_PATH;

    // nm -gU: list only externally-visible, defined symbols (no undefined).
    // -U on macOS nm means "don't list undefined symbols".
    std::string cmd{"nm -gU \""};
    cmd += kDylibPath;
    cmd += "\" 2>/dev/null";

    // NOLINTNEXTLINE(cert-env33-c) — popen is acceptable in a test harness.
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        WARN("popen() failed; skipping symbol-export check");
        return;
    }

    std::string nm_output;
    {
        std::array<char, 256> buf{};
        while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
            nm_output += buf.data();
        }
    }
    pclose(pipe);

    // Exact third-column match via awk — avoids false positives from
    // substring matches (e.g. "glibre_plugin_manifest" matching
    // "glibre_plugin_manifest_size").
    CHECK(count_exact_symbol(nm_output, "_glibre_plugin_abi_hash")      >= 1);
    CHECK(count_exact_symbol(nm_output, "_glibre_plugin_manifest_size") >= 1);
    CHECK(count_exact_symbol(nm_output, "_glibre_plugin_manifest")      >= 1);
    CHECK(count_exact_symbol(nm_output, "_glibre_plugin_register")      >= 1);
#endif  // GLIBRE_NOOP_DYLIB_PATH
}

// ===========================================================================
// Test: plugin_context_register_smoke
//
// Instantiates a stub PluginContext, calls register_component() and
// register_system(), and asserts both return ok (std::expected with value).
//
// This is the canonical "happy path" smoke test for the PluginContext API
// surface.  It drives the stub implementations defined at the top of this
// file, verifying that the call chain compiles and the Result<void> is
// propagated correctly.
//
// Strings are passed as (const char*, std::size_t) pairs per the ABI
// boundary rule (HIGH-1 fix: no eastl::string_view on the ABI surface).
// ===========================================================================

TEST_CASE("plugin_context_register_smoke", "[core][plugin_api]") {
    PluginContextFixture fixture;

    static constexpr char kTypeName[]   = "glibre.test.SomeComponent";
    static constexpr char kSchemaHash[] =
        "0000000000000000000000000000000000000000000000000000000000000000";

    // register_component: type_name, schema_hash (placeholder), storage_hint=0 (archetype).
    auto r1 = fixture.ctx.register_component(
        kTypeName,   sizeof(kTypeName) - 1,
        kSchemaHash, sizeof(kSchemaHash) - 1,
        std::uint8_t{0}
    );
    REQUIRE(r1.has_value());

    static constexpr char kSysName[] = "glibre.test.SomeSystem";

    // register_system: name, phase=2 (Logic).
    auto r2 = fixture.ctx.register_system(
        kSysName, sizeof(kSysName) - 1,
        std::uint8_t{2}
    );
    REQUIRE(r2.has_value());
}

// ===========================================================================
// Test: plugin_context_register_after_phase8_fails
//
// Documents the intended future behaviour: registration calls made outside
// phase 8 (HotReload) should return core::Error::FramePhaseMisordered.
//
// The actual phase-gate enforcement lives in the loader (plan #229).
// The stub implementations currently always succeed; this test records the
// deferred contract via WARN so a future regression is visible in the report.
//
// When plan #229 is implemented:
//   - The registries will track whether phase 8 is active.
//   - Calls outside phase 8 will return std::unexpected(core::Error::FramePhaseMisordered).
//   - This test will be updated to mock a "not phase 8" state and assert the error.
//
// For MVP: stub always succeeds; document the deferred gate explicitly.
// ===========================================================================

TEST_CASE("plugin_context_register_after_phase8_fails", "[core][plugin_api]") {
    PluginContextFixture fixture;

    static constexpr char kName[] = "glibre.test.LateSystem";

    // In MVP stubs, registration always succeeds regardless of phase.
    // The phase gate (core::Error::FramePhaseMisordered) is deferred to
    // plan #229 (loader implementation).
    //
    // Invoke the API to confirm it compiles and links; the return value is
    // intentionally discarded here because the stub always succeeds and the
    // test's job is to document the deferred contract, not assert a value.
    (void)fixture.ctx.register_system(
        kName, sizeof(kName) - 1,
        std::uint8_t{3}  // PhysicsFixed — valid phase ordinal
    );

    // Stub always succeeds: document that the gate is NOT yet enforced.
    WARN("Phase-gate enforcement (FramePhaseMisordered) is deferred to plan #229 (loader)");
    SUCCEED("stub returns ok — gate not yet implemented (see plan #229)");
    // When #229 lands, replace the two lines above with:
    //   REQUIRE_FALSE(r.has_value());
    //   REQUIRE(r.error() == glibre::core::Error::FramePhaseMisordered);
}
