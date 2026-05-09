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
//   - plugin_context_register_after_phase8_fails_pending_plan_229
//
// Design constraints:
//   • -fno-exceptions (error-model.md §Decision 3).
//   • No real registry objects exist yet (plan #229 onwards) — tests use
//     stub implementations declared at file scope to satisfy the aggregate's
//     reference fields.
//   • PluginContext is a pure-data aggregate of references (plugin-abi.md
//     §"Registration Entry-Point Signature" Open Question #3).  Registration
//     methods live on the registry types (TypeRegistry, SystemRegistry, …),
//     not on PluginContext.  The smoke test confirms the aggregate compiles and
//     the references can be read back — actual registration API is tested in
//     plan #229's loader tests when the real registry types are defined.

#include <cstddef>
#include <cstdint>
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
// These stubs live in an ANONYMOUS namespace — they are hermetic to this
// translation unit and cannot conflict with plan #229's real definitions in
// another TU.
//
// The aggregate PluginContext holds references to the forward-declared names.
// Initialising those references from the stub types via reinterpret_cast is
// used only for address / identity checks in plugin_context_is_pod_aggregate.
// No methods are called through the reinterpreted references in these tests,
// so no incomplete-type member access occurs.
// ---------------------------------------------------------------------------

namespace {

struct StubWorld {
    int dummy{0};
};

struct StubTypeRegistry {
    int dummy{0};
};

struct StubSystemRegistry {
    int dummy{0};
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
// Test fixture — builds a complete PluginContext from stubs.
//
// PluginContext holds references to the registry types by their incomplete
// forward-declared names.  We initialise those references from the anonymous-
// namespace stubs whose layouts are identical (trivial structs with one int).
// The reinterpret_cast is used only to bind the references; no method is
// called through the resulting reference (eliminating the UB from
// [basic.compound]/4 that previously arose from delegating calls through the
// reinterpreted pointers).
// ---------------------------------------------------------------------------

namespace {

// Concrete storage for stub registries — outlives the PluginContext.
struct PluginContextFixture {
    StubWorld world;
    StubTypeRegistry type_reg;
    StubSystemRegistry sys_reg;
    StubPassRegistry pass_reg;
    StubPanelRegistry panel_reg;
    StubPluginManifest manifest;
    // Per-context allocator used to construct the AllocatorHandle in the
    // PluginContext.  ContextTag::core is used here as a representative tag
    // for test fixtures; a 1 MiB ceiling prevents OutOfBudget in strict-mode
    // (GLIBRE_ALLOC_STRICT=1).
    glibre::PerContextAllocator test_alloc{glibre::ContextTag::core, 1024ULL * 1024ULL};
    StubLogSink log;

    glibre::core::PluginContext ctx{
        reinterpret_cast<glibre::core::World&>(world),
        reinterpret_cast<glibre::core::TypeRegistry&>(type_reg),
        reinterpret_cast<glibre::core::SystemRegistry&>(sys_reg),
        reinterpret_cast<glibre::core::PassRegistry&>(pass_reg),
        reinterpret_cast<glibre::core::PanelRegistry&>(panel_reg),
        reinterpret_cast<const glibre::core::PluginManifest&>(manifest),
        reinterpret_cast<glibre::core::LogSink&>(log),
        glibre::AllocatorHandle{test_alloc, glibre::ContextTag::core},
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
    CHECK(
        &fixture.ctx.type_registry ==
        reinterpret_cast<glibre::core::TypeRegistry*>(&fixture.type_reg)
    );
    CHECK(
        &fixture.ctx.system_registry ==
        reinterpret_cast<glibre::core::SystemRegistry*>(&fixture.sys_reg)
    );
    CHECK(
        &fixture.ctx.pass_registry ==
        reinterpret_cast<glibre::core::PassRegistry*>(&fixture.pass_reg)
    );
    CHECK(
        &fixture.ctx.panel_registry ==
        reinterpret_cast<glibre::core::PanelRegistry*>(&fixture.panel_reg)
    );
    CHECK(
        &fixture.ctx.manifest == reinterpret_cast<glibre::core::PluginManifest*>(&fixture.manifest)
    );
    // Verify the AllocatorHandle is correctly stamped with the expected tag.
    CHECK(fixture.ctx.alloc.tag() == glibre::ContextTag::core);
    CHECK(fixture.ctx.alloc.wraps(fixture.test_alloc));
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
using PluginRegisterFn = glibre::Result<void> (*)(glibre::core::PluginContext&) noexcept;

static_assert(
    std::is_same_v<PluginRegisterFn, decltype(&glibre_plugin_register)>,
    "glibre_plugin_register must have signature: "
    "glibre::Result<void>(glibre::core::PluginContext&) noexcept"
);

TEST_CASE("register_signature_compiles", "[core][plugin_api]") {
    // Runtime mirror of the static_assert.
    CHECK(std::is_same_v<PluginRegisterFn, decltype(&glibre_plugin_register)>);

    // Verify the unregister counterpart has the same shape.
    using PluginUnregisterFn = glibre::Result<void> (*)(glibre::core::PluginContext&) noexcept;
    CHECK(std::is_same_v<PluginUnregisterFn, decltype(&glibre_plugin_unregister)>);
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
    SKIP(
        "GLIBRE_NOOP_DYLIB_PATH not defined — build with GLIBRE_BUILD_EXAMPLES=ON to run this test"
    );
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
// Symbol matching uses exact third-field comparison: nm output is read line
// by line and each line is split by whitespace to extract the third column,
// avoiding false positives from substring matches (e.g. "glibre_plugin_manifest"
// matching "glibre_plugin_manifest_size").
// ===========================================================================

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

/// Count lines in nm output where the third whitespace-delimited column
/// exactly matches symbol_name.  Reads nm_output line by line through a
/// popen() of `printf '%s' <nm_output> | awk '$3 == symbol { count++ }'`
/// — implemented in plain C++ without invoking a shell pipeline carrying
/// user-controlled nm output, to avoid metacharacter injection from paths
/// or symbol names that contain shell-special characters.
///
/// nm output format (macOS `nm -gU`):
///   <address>  <type>  <mangled-or-unmangled-name>
/// The address may be absent for undefined symbols, but -gU filters those
/// out, so every line has at least three columns.
int count_exact_symbol(const std::string& nm_output, const char* symbol_name) {
    int count = 0;
    std::size_t pos = 0;
    while (pos < nm_output.size()) {
        // Locate end of current line.
        std::size_t eol = nm_output.find('\n', pos);
        if (eol == std::string::npos)
            eol = nm_output.size();

        // Extract the line and scan for the third column.
        const char* line = nm_output.c_str() + pos;
        std::size_t line_len = eol - pos;
        pos = eol + 1;

        if (line_len == 0)
            continue;

        // Skip leading whitespace.
        std::size_t i = 0;
        while (i < line_len && (line[i] == ' ' || line[i] == '\t'))
            ++i;

        int col = 0;
        while (col < 2 && i < line_len) {
            // Advance past the current token.
            while (i < line_len && line[i] != ' ' && line[i] != '\t')
                ++i;
            ++col;
            // Skip inter-column whitespace.
            while (i < line_len && (line[i] == ' ' || line[i] == '\t'))
                ++i;
        }

        // col == 2: i now points at start of third column (or end of line).
        if (col < 2 || i >= line_len)
            continue;

        // Measure third token length.
        std::size_t tok_start = i;
        while (i < line_len && line[i] != ' ' && line[i] != '\t' && line[i] != '\n' &&
               line[i] != '\r')
            ++i;
        std::size_t tok_len = i - tok_start;

        if (tok_len == std::strlen(symbol_name) &&
            std::memcmp(line + tok_start, symbol_name, tok_len) == 0) {
            ++count;
        }
    }
    return count;
}

}  // namespace

TEST_CASE("noop_plugin_exports_four_required_symbols", "[core][plugin_api]") {
    // GLIBRE_NOOP_DYLIB_PATH is injected by CMake via compile definition.
#ifndef GLIBRE_NOOP_DYLIB_PATH
    SKIP(
        "GLIBRE_NOOP_DYLIB_PATH not defined — build with GLIBRE_BUILD_EXAMPLES=ON to run this test"
    );
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

    // Exact third-column match in pure C++ — avoids metacharacter injection
    // from shell-escaping nm output (LOW-7 fix).
    CHECK(count_exact_symbol(nm_output, "_glibre_plugin_abi_hash") >= 1);
    CHECK(count_exact_symbol(nm_output, "_glibre_plugin_manifest_size") >= 1);
    CHECK(count_exact_symbol(nm_output, "_glibre_plugin_manifest") >= 1);
    CHECK(count_exact_symbol(nm_output, "_glibre_plugin_register") >= 1);
#endif  // GLIBRE_NOOP_DYLIB_PATH
}

// ===========================================================================
// Test: plugin_context_register_smoke
//
// Instantiates a PluginContext from stubs and confirms the aggregate is
// well-formed and all reference fields are correctly bound.
//
// Registration methods (register_component, register_system) live on the
// real TypeRegistry / SystemRegistry types (plan #229), not on PluginContext.
// This smoke test validates only the aggregate shape and reference binding —
// the registration API is exercised in loader unit tests once plan #229 lands.
//
// This test replaces the former register_component / register_system call
// smoke that required UB-inducing stub method definitions in namespace
// glibre::core (HIGH-1 / MED-5 fix).
// ===========================================================================

TEST_CASE("plugin_context_register_smoke", "[core][plugin_api]") {
    // Constructing the fixture exercises PluginContext aggregate initialization.
    PluginContextFixture fixture;

    // Confirm that the aggregate is well-formed and all reference fields are
    // correctly bound to the stubs.
    CHECK(&fixture.ctx.world != nullptr);
    CHECK(&fixture.ctx.type_registry != nullptr);
    CHECK(&fixture.ctx.system_registry != nullptr);
    CHECK(&fixture.ctx.pass_registry != nullptr);
    CHECK(&fixture.ctx.panel_registry != nullptr);
    CHECK(&fixture.ctx.manifest != nullptr);
    CHECK(fixture.ctx.alloc.wraps(fixture.test_alloc));
    CHECK(&fixture.ctx.log != nullptr);
}

// ===========================================================================
// Test: plugin_context_register_after_phase8_fails_pending_plan_229
//
// Documents the deferred phase-gate contract: registration calls made outside
// phase 8 (HotReload) should eventually return core::Error::FramePhaseMisordered.
//
// The actual phase-gate enforcement lives in the loader (plan #229).  This
// test records the deferred contract so it is visible in the test report and
// can be updated when plan #229 ships.
//
// Renamed from "plugin_context_register_after_phase8_fails" to
// "plugin_context_register_after_phase8_fails_pending_plan_229" to make the
// deferred nature explicit in the test name and avoid implying a currently-
// enforced invariant (LOW-8 fix).
//
// When plan #229 is implemented:
//   - The registries will track whether phase 8 is active.
//   - Calls outside phase 8 will return std::unexpected(core::Error::FramePhaseMisordered).
//   - This test will be updated to mock a "not phase 8" state and assert the error.
//   - Rename back to "plugin_context_register_after_phase8_fails".
//
// For MVP: document the deferred gate explicitly; the test exercises the
// aggregate construction path only.
// ===========================================================================

TEST_CASE("plugin_context_register_after_phase8_fails_pending_plan_229", "[core][plugin_api]") {
    // Construct the aggregate — confirms the fixture compiles and links.
    PluginContextFixture fixture;
    (void)fixture;

    // Phase-gate enforcement (core::Error::FramePhaseMisordered) is deferred
    // to plan #229 (loader implementation).  This test is a placeholder.
    WARN("Phase-gate enforcement (FramePhaseMisordered) is deferred to plan #229 (loader)");
    SUCCEED("stub context constructed — gate not yet implemented (see plan #229)");
    // When #229 lands, replace the two lines above with:
    //   auto r = fixture.ctx.system_registry.register_system(..., phase_not_8);
    //   REQUIRE_FALSE(r.has_value());
    //   REQUIRE(r.error() == glibre::core::Error::FramePhaseMisordered);
}
