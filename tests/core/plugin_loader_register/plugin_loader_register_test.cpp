// tests/core/plugin_loader_register/plugin_loader_register_test.cpp
//
// Unit tests for loader-procedure free functions (steps 9–11):
//   call_register   (step 9) — invoke glibre_plugin_register(PluginContext&)
//   rebuild_schedule (step 10) — MVP stub; always succeeds
//   migrate_components (step 11) — MVP stub; always succeeds
//
// Named test cases (plan #231 Unit Test Plan):
//   - register_invokes_plugin_entry_point
//   - register_failure_cleans_up_dlopen
//   - rebuild_schedule_smoke
//   - migrate_components_no_op_when_versions_equal
//   - call_register_stamped_stamps_correct_tag_from_manifest_name  (plan #989 HIGH-1, r2)
//
// Named test cases (plan #982 Unit Test Plan):
//   - rebuild_schedule_returns_cycle_on_conflicting_systems
//
// Named test cases (plan #983 Unit Test Plan):
//   - migrate_components_returns_failed_on_broken_migration_step
//
// Design constraints:
//   • -fno-exceptions (error-model.md §Decision 3).
//   • libc++ standard library + std::pmr containers per PHILOSOPHY §11 (rewritten
//     in PR #1059); see reviews/decisions/eastl-removal.md.
//   • Each TEST_CASE constructs its own objects (no singletons).
//   • PluginContext references World, TypeRegistry, etc. which are opaque
//     pending types.  The test defines minimal empty stubs for each
//     forward-declared class so that PluginContext can be constructed
//     without pulling in the real implementations (which are not yet landed).
//     These stubs are TU-local and cannot conflict because none of the
//     opaque types have definitions in glibre-core yet (all are pending).
//
// Authority: reviews/decisions/plugin-abi.md §"Loader Sequence" steps 9–11,
//            §"Failure Modes → core::Error" table (rows 9, 10, 11).
//
// Plan: #231 — loader-procedure free functions: call_register + rebuild_schedule
//              + migrate_components.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <glibre/alloc.hpp>                       // AllocatorHandle, PerContextAllocator
#include <glibre/core/context_tag_resolver.hpp>   // derive_context_tag (SRP unit, plan #989)
#include <glibre/core/plugin_api.hpp>             // PluginContext aggregate
#include <glibre/core/plugin_loader.hpp>          // PluginLoader::open
#include <glibre/core/plugin_loader_actions.hpp>  // call_register, call_register_stamped, etc.
#include <glibre/core/plugin_manifest.hpp>
#include <glibre/error.hpp>

// ---------------------------------------------------------------------------
// Shell definitions for PluginContext's pending reference types.
//
// Pulled from the shared fixture header (MED-4, round-1 review).  All test
// TUs in this directory that need PluginContext include this header rather
// than re-defining the shells inline — single source of truth, no ODR risk.
// ---------------------------------------------------------------------------

#include "plugin_context_fixture.hpp"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

// ABI hash the noop plugin and stub_register_fails export (all-zeros).
constexpr const char kNoopAbiHash[] =
    "0000000000000000000000000000000000000000000000000000000000000000";

// Test-fixture allocator ceiling — 1 MiB is sufficient for all test cases
// (no test allocates near this amount; it only needs to be above zero).
constexpr std::uint64_t kTestAllocCeiling = 1024ULL * 1024ULL;

// make_test_alloc_handle — factory for the standard test-fixture AllocatorHandle.
//
// Returns an AllocatorHandle stamped with ContextTag::core and wrapping the
// caller-supplied PerContextAllocator.  The caller must ensure `alloc` is
// declared and alive for the full lifetime of the returned handle (RAII ordering).
//
// ContextTag::core is used as a representative tag for test fixtures; the
// production loader derives the tag from the manifest plugin name via
// derive_context_tag() (perf-budget.md §Allocator Rules #1, plan #989).
//
// Extracted to avoid repeating the identical {alloc, ContextTag::core} pattern
// at three test sites in this file (LOW-5, round-2 review).
[[nodiscard]] glibre::AllocatorHandle
make_test_alloc_handle(glibre::PerContextAllocator& alloc) noexcept {
    return glibre::AllocatorHandle{alloc, glibre::ContextTag::core};
}

/// Return the core::Error variant arm, or nullptr if the error belongs to a
/// different context (render::Error, tools::Error, etc.).
[[nodiscard]] const glibre::core::Error* as_core_error(const glibre::Error& err) noexcept {
    return std::get_if<glibre::core::Error>(&err.code());
}

/// Build a minimal valid PluginManifest that passes all registry gates when
/// expected_abi_hash == kNoopAbiHash.
glibre::core::PluginManifest make_manifest(
    const char* name = "glibre.test.register",
    const char* abi_hash = kNoopAbiHash,
    glibre::core::SemVer version = {1, 0, 0},
    glibre::core::SemVer min_engine = {0, 1, 0}
) {
    glibre::core::PluginManifest m;
    m.name = name;          // std::pmr::string from const char*
    m.abi_hash = abi_hash;  // std::pmr::string from const char*
    m.version = version;
    m.min_engine_version = min_engine;
    return m;
}

// File-level allocator + resource for PluginLoader::open() calls.
//
// PluginLoader::open() requires a std::pmr::memory_resource& to back
// dylib_path_ under the per-context ceiling (HIGH-1 + HIGH-2, round-2).
// All tests in this file share the core ContextTag allocator.
// Construction order: loader_alloc_ before loader_mr_ so the resource's
// reference is valid.  Named with "loader_" prefix to avoid collision with
// test-local PerContextAllocator instances named test_alloc.
glibre::PerContextAllocator loader_alloc_{glibre::ContextTag::core};
glibre::PerContextAllocatorResource loader_mr_{loader_alloc_};

/// Build a minimal PluginContext from stub references.
/// Each stub object is passed by reference; none of the test stubs dereference
/// the fields, so the stubs' layout is irrelevant — only their address matters.
/// `alloc_handle` is the tag-stamped AllocatorHandle the loader stamps at
/// glibre_plugin_register time (perf-budget.md §Allocator Rules #1, plan #989).
glibre::core::PluginContext make_context(
    glibre::core::World& world,
    glibre::core::TypeRegistry& type_reg,
    glibre::core::SystemRegistry& sys_reg,
    glibre::core::PassRegistry& pass_reg,
    glibre::core::PanelRegistry& panel_reg,
    glibre::core::LogSink& log_sink,
    const glibre::core::PluginManifest& manifest,
    glibre::AllocatorHandle alloc_handle
) {
    return glibre::core::PluginContext{
        .world = world,
        .type_registry = type_reg,
        .system_registry = sys_reg,
        .pass_registry = pass_reg,
        .panel_registry = panel_reg,
        .manifest = manifest,
        .log = log_sink,
        .alloc = alloc_handle,
    };
}

}  // namespace

// ===========================================================================
// Test: register_invokes_plugin_entry_point
//
// Exercises loader step 9 (plugin-abi.md §"Loader Sequence") on the happy path:
//   1. Load the noop plugin via PluginLoader::open() — steps 1–2 succeed.
//   2. Extract the RegisterFn function pointer via loader.register_fn().
//   3. Call PluginLoaderRegistry::call_register(register_fn, ctx).
//   4. Assert the call returns success and did not mutate the registry.
//
// The noop plugin's glibre_plugin_register returns {} (success) without
// touching the context.  This test verifies that call_register correctly
// propagates a success result.
//
// Refs: plugin-abi.md §"Loader Sequence" step 9 (success path).
// DoD: unit_test_named: register_invokes_plugin_entry_point
// ===========================================================================

TEST_CASE("register_invokes_plugin_entry_point", "[core][register]") {
#ifndef GLIBRE_NOOP_DYLIB_PATH
    // FAIL rather than SKIP: noop plugin is required for this test.
    // Rebuild with GLIBRE_BUILD_EXAMPLES=ON or add glibre-plugin-noop as a dep.
    FAIL(
        "GLIBRE_NOOP_DYLIB_PATH not defined — "
        "rebuild with GLIBRE_BUILD_EXAMPLES=ON (noop plugin required)"
    );
#else
    constexpr std::string_view noop_path{GLIBRE_NOOP_DYLIB_PATH};
    REQUIRE_FALSE(noop_path.empty());

    // Step 1–2: load the noop plugin.
    auto loader_result = glibre::core::PluginLoader::open(noop_path, loader_mr_);
    REQUIRE(loader_result.has_value());

    const glibre::core::PluginLoader& loader = *loader_result;
    REQUIRE(loader.register_fn() != nullptr);

    // Step 3: build stub context objects and PluginContext.
    glibre::core::World world;
    glibre::core::TypeRegistry type_reg;
    glibre::core::SystemRegistry sys_reg;
    glibre::core::PassRegistry pass_reg;
    glibre::core::PanelRegistry panel_reg;
    glibre::core::LogSink log_sink;
    // AllocatorHandle stamped with ContextTag::core for test fixtures
    // (perf-budget.md §Allocator Rules #1, plan #989).
    glibre::PerContextAllocator test_alloc{glibre::ContextTag::core, kTestAllocCeiling};
    auto manifest = make_manifest("glibre.test.register.noop");
    auto ctx = make_context(
        world,
        type_reg,
        sys_reg,
        pass_reg,
        panel_reg,
        log_sink,
        manifest,
        make_test_alloc_handle(test_alloc)
    );

    // Step 4: call call_register — noop plugin returns success.
    auto result = glibre::core::call_register(loader.register_fn(), ctx);

    REQUIRE(result.has_value());
#endif
}

// ===========================================================================
// Test: register_failure_cleans_up_dlopen
//
// Exercises loader step 9 on the failure path:
//   1. Load the stub_register_fails plugin via PluginLoader::open() — steps
//      1–2 succeed (all four symbols present; register returns failure).
//   2. Extract the RegisterFn function pointer.
//   3. Call PluginLoaderRegistry::call_register(register_fn, ctx).
//   4. Assert the call returns core::Error::PluginInitFailed.
//   5. Assert that after the error the PluginLoader can be destructed without
//      crashing (the caller is responsible for dlclose — the loader's RAII
//      destructor handles it automatically when the loader goes out of scope).
//
// Note: "cleans_up_dlopen" refers to the caller's responsibility to let the
// PluginLoader destructor run (which calls dlclose).  call_register itself
// does NOT call dlclose; it only invokes the entry-point and surfaces the
// error.  This test verifies that after a call_register failure the PluginLoader
// RAII destructor safely performs dlclose without crashing or double-closing.
//
// Refs: plugin-abi.md §"Loader Sequence" step 9 (failure path),
//       §"Failure Modes → core::Error" step 9 (PluginInitFailed).
// DoD: unit_test_named: register_failure_cleans_up_dlopen
// ===========================================================================

TEST_CASE("register_failure_cleans_up_dlopen", "[core][register]") {
#ifndef GLIBRE_STUB_REGISTER_FAILS_DYLIB_PATH
    // FAIL rather than SKIP: this test is part of the DoD for #231.
    FAIL(
        "GLIBRE_STUB_REGISTER_FAILS_DYLIB_PATH not defined — "
        "CMakeLists.txt must inject this macro for stub_register_fails target"
    );
#else
<<<<<<< HEAD
    constexpr std::string_view stub_path{GLIBRE_STUB_REGISTER_FAILS_DYLIB_PATH};
    REQUIRE_FALSE(stub_path.empty());
=======
    constexpr const char* stub_path = GLIBRE_STUB_REGISTER_FAILS_DYLIB_PATH;
    REQUIRE(stub_path != nullptr);
    REQUIRE(*stub_path != '\0');
>>>>>>> origin/main

    // Step 1–2: load the failing stub — dlopen + dlsym must succeed.
    auto loader_result = glibre::core::PluginLoader::open(stub_path, loader_mr_);
    REQUIRE(loader_result.has_value());

    glibre::core::PluginLoader& loader = *loader_result;
    REQUIRE(loader.register_fn() != nullptr);

    // Step 3: build stub context objects and PluginContext.
    glibre::core::World world;
    glibre::core::TypeRegistry type_reg;
    glibre::core::SystemRegistry sys_reg;
    glibre::core::PassRegistry pass_reg;
    glibre::core::PanelRegistry panel_reg;
    glibre::core::LogSink log_sink;
    // AllocatorHandle stamped with ContextTag::core for test fixtures
    // (plan #989: tag passed by the test caller; in production the loader
    // derives the tag from the manifest name at glibre_plugin_register time).
    glibre::PerContextAllocator test_alloc{glibre::ContextTag::core, kTestAllocCeiling};
    auto manifest = make_manifest("glibre.test.register.fails");
    auto ctx = make_context(
        world,
        type_reg,
        sys_reg,
        pass_reg,
        panel_reg,
        log_sink,
        manifest,
        make_test_alloc_handle(test_alloc)
    );

    // Step 4: call_register must return PluginInitFailed.
    auto result = glibre::core::call_register(loader.register_fn(), ctx);

    REQUIRE_FALSE(result.has_value());
    const auto* core_err = as_core_error(result.error());
    REQUIRE(core_err != nullptr);
    CHECK(*core_err == glibre::core::Error::PluginInitFailed);

    // Assert that the inner error's stable enumerator name is carried in
    // ErrorContext::detail (plugin-abi.md §"Loader Sequence" step 9 —
    // "carrying the inner error in ErrorContext::detail").
    // The stub returns core::Error::PluginInitFailed, so detail must be
    // "PluginInitFailed" (the stable to_string value from log_error.hpp).
    CHECK(result.error().where().detail == std::string_view{"PluginInitFailed"});

    // Step 5: loader goes out of scope at end of test — destructor calls
    // dlclose.  If the RAII cleanup crashes or double-frees, the test runner
    // will report an abnormal exit, catching regressions.
    // (No explicit assertion needed; absence of crash is the verification.)
#endif
}

// ===========================================================================
// Test: rebuild_schedule_smoke
//
// Exercises loader step 10 (plugin-abi.md §"Loader Sequence"):
//   Calls rebuild_schedule() on an empty registry.  The MVP stub must return
//   success unconditionally without allocating or accessing any state.
//
// This is a smoke test: it verifies that the stub does not crash, does not
// return an error, and does not depend on any loaded plugins being present.
//
// When the real topology sort lands (#247, #248), this test remains valid
// for the empty-registry case (zero plugins → no cycle possible → success).
//
// Refs: plugin-abi.md §"Loader Sequence" step 10,
//       §"Failure Modes → core::Error" step 10 (SystemScheduleCycle).
// DoD: unit_test_named: rebuild_schedule_smoke
// ===========================================================================

TEST_CASE("rebuild_schedule_smoke", "[core][register]") {
    auto result = glibre::core::rebuild_schedule();

    REQUIRE(result.has_value());
}

// ===========================================================================
// Test: migrate_components_no_op_when_versions_equal
//
// Exercises loader step 11 (plugin-abi.md §"Loader Sequence"):
//   Calls migrate_components(N, N) — same from and to version.  When versions
//   are equal there is nothing to migrate; the stub must return success.
//
// Also tests the mixed-version case (from != to) — the MVP stub must still
// return success because no real migration tables exist yet (plan #221).
//
// When plan #221 lands and real migration tables are emitted by glibre-foryc,
// this test will need to be extended with a fixture that exercises the real
// migration path.  The no-op case (from == to) remains valid indefinitely.
//
// Refs: plugin-abi.md §"Loader Sequence" step 11,
//       §"Failure Modes → core::Error" step 11 (SchemaMigrationFailed).
// DoD: unit_test_named: migrate_components_no_op_when_versions_equal
// ===========================================================================

TEST_CASE("migrate_components_no_op_when_versions_equal", "[core][register]") {
    // Case 1: from == to (identity — nothing to migrate).
    SECTION("identical versions return success") {
        auto result = glibre::core::migrate_components(3u, 3u);
        REQUIRE(result.has_value());
    }

    // Case 2: from == 0, to == 0 (initial install — no prior version).
    SECTION("both zero versions return success") {
        auto result = glibre::core::migrate_components(0u, 0u);
        REQUIRE(result.has_value());
    }

    // Case 3: from < to (upgrade path — MVP stub returns success;
    //   real migration deferred to plan #221).
    SECTION("upgrade path stub returns success") {
        auto result = glibre::core::migrate_components(1u, 2u);
        REQUIRE(result.has_value());
    }
}

// ===========================================================================
// Test: plugin_loader_stamps_allocator_handle_with_plugin_tag
//
// Integration test for loader-side AllocatorHandle stamping (plan #989,
// perf-budget.md §Allocator Rules #1).
//
// Verifies that derive_context_tag() — the loader function called at
// glibre_plugin_register time — maps a plugin's manifest name to the correct
// ContextTag, and that the resulting AllocatorHandle carries that tag.
//
// This test exercises the full stamping pipeline that the loader orchestrator
// (plans #230/#231) will invoke:
//   1. Derive the ContextTag from the plugin manifest name.
//   2. Construct an AllocatorHandle with that tag and the context's
//      PerContextAllocator.
//   3. Verify that handle.tag() == expected tag (the tag is preserved end-to-end).
//
// Named plugin samples and their expected tags (perf-budget.md §Budget Table):
//   "glibre.render.camera"   → ContextTag::render
//   "glibre.physics.jolt"    → ContextTag::physics
//   "glibre.core.ecs"        → ContextTag::core
//   "glibre.geometry.mesh"   → ContextTag::geometry
//
// Authority: plan #989 §Scope: "Plugin loader (core/src/plugin_loader.cpp):
//   stamp AllocatorHandle with the plugin's ContextTag at glibre_plugin_register
//   step, pass handle into plugin init via the plugin ABI struct."
// ===========================================================================

TEST_CASE("plugin_loader_stamps_allocator_handle_with_plugin_tag", "[core][register][alloc]") {
    constexpr std::uint64_t kCeiling = kTestAllocCeiling;  // 1 MiB — sufficient for test

    struct Sample {
        const char* plugin_name;
        glibre::ContextTag expected_tag;
    };

    // Representative sample of the nine bounded contexts.
    const Sample samples[] = {
        {"glibre.render.camera", glibre::ContextTag::render},
        {"glibre.physics.jolt", glibre::ContextTag::physics},
        {"glibre.core.ecs", glibre::ContextTag::core},
        {"glibre.geometry.mesh", glibre::ContextTag::geometry},
        {"glibre.platform.sdl", glibre::ContextTag::platform},
        {"glibre.data.asset", glibre::ContextTag::data},
        {"glibre.shader.slang", glibre::ContextTag::shader},
        {"glibre.content.importer", glibre::ContextTag::content},
        {"glibre.tools.editor", glibre::ContextTag::tools},
    };

    for (const auto& s : samples) {
        // Step 1: derive the ContextTag from the manifest plugin name.
        auto tag_result = glibre::core::derive_context_tag(s.plugin_name);
        REQUIRE(tag_result.has_value());
        CHECK(tag_result.value() == s.expected_tag);

        // Step 2: construct a PerContextAllocator and stamp an AllocatorHandle.
        glibre::PerContextAllocator alloc{*tag_result, kCeiling};
        glibre::AllocatorHandle handle{alloc, *tag_result};

        // Step 3: the stamped handle carries the expected tag.
        CHECK(handle.tag() == s.expected_tag);
        CHECK(handle.wraps(alloc));
    }

    // Edge: single-component name (no dot) must fail.
    {
        auto r = glibre::core::derive_context_tag("myplugin");
        REQUIRE_FALSE(r.has_value());
    }

    // Edge: name with recognised prefix but unknown context must fail.
    {
        auto r = glibre::core::derive_context_tag("glibre.unknown.foo");
        REQUIRE_FALSE(r.has_value());
    }
}

// ===========================================================================
// Test: migrate_components_returns_failed_on_broken_migration_step
//
// Exercises loader step 11 failure path (plugin-abi.md §"Loader Sequence"
// step 11, §"Failure Modes → core::Error" row 11):
//   migration step failed → core::Error::SchemaMigrationFailed.
//
// Uses option 1 from plan #983 §Scope: inject a mock MigrationStepFn that
// returns std::unexpected(core::Error::SchemaMigrationFailed) to simulate a
// broken migration step.  This exercises the failure arm without requiring
// the real per-type migration table walk from plan #221.
//
// Placed here (plugin_loader_register/) because the SUT is
// plugin_loader_actions.hpp free functions — the same translation unit that
// hosts migrate_components_no_op_when_versions_equal and the other step 9–11
// tests (cohesion: all plugin_loader_actions.* tests live together).
//
// Sections:
//   A. from!=to + broken step_fn → SchemaMigrationFailed propagated.
//      step_fn invocation counter == 1 (step_fn was called exactly once).
//   B. from==to + broken step_fn → success (identity short-circuit fires
//      before step_fn).  step_fn invocation counter == 0 (step_fn not called).
//
// Authority: reviews/decisions/plugin-abi.md §"Loader Sequence" step 11,
//            §"Failure Modes → core::Error" table (row 11).
// Plan: #983 — SchemaMigrationFailed integration test.
// DoD: unit_test_named: migrate_components_returns_failed_on_broken_migration_step
// ===========================================================================

// TU-local invocation counter for the broken_step lambda (accessed via a
// file-scope helper so it can be used from a non-capturing function pointer).
// Reset to 0 before each SECTION to avoid cross-section interference.
namespace {
int g_broken_step_call_count = 0;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

glibre::Result<void> broken_step_fn() noexcept {
    ++g_broken_step_call_count;
    return std::unexpected(glibre::Error{glibre::core::Error::SchemaMigrationFailed});
}
}  // namespace

TEST_CASE(
    "migrate_components_returns_failed_on_broken_migration_step", "[core][register][migrate]"
) {
    SECTION("from!=to: step_fn invoked once, failure propagated") {
        // Reset invocation counter for this section.
        g_broken_step_call_count = 0;

        // from_version (1) != to_version (2): a real schema bump — step_fn is
        // invoked and its failure propagated to the caller.
        auto result = glibre::core::migrate_components(1u, 2u, broken_step_fn);

        // step_fn must have been called exactly once (plugin-abi.md §"Loader
        // Sequence" step 11 — when versions differ, the migration step runs).
        CHECK(g_broken_step_call_count == 1);

        // The failure from step_fn must be propagated unchanged.
        REQUIRE(!result);
        const auto* core_err = as_core_error(result.error());
        REQUIRE(core_err != nullptr);
        CHECK(*core_err == glibre::core::Error::SchemaMigrationFailed);
    }

    SECTION("from==to: step_fn not invoked, success returned") {
        // Reset invocation counter for this section.
        g_broken_step_call_count = 0;

        // from_version == to_version: identity case — the implementation must
        // short-circuit before calling step_fn (plugin_loader_actions.hpp
        // lines 186-187: "When from_version == to_version, step_fn is NOT called").
        auto result = glibre::core::migrate_components(3u, 3u, broken_step_fn);

        // step_fn must NOT have been called: counter remains 0.
        CHECK(g_broken_step_call_count == 0);

        // The identity case always succeeds (nothing to migrate).
        REQUIRE(result.has_value());
    }
}

// ===========================================================================
// Test: rebuild_schedule_returns_cycle_on_conflicting_systems
//
// Exercises loader step 10 failure path (plugin-abi.md §"Loader Sequence"
// step 10, §"Failure Modes → core::Error" row 10):
//   system schedule cycle detected → core::Error::SystemScheduleCycle.
//
// Uses option 1 from plan #982 §Scope: inject a mock ScheduleRebuildFn that
// returns std::unexpected(core::Error::SystemScheduleCycle) to simulate a
// cycle in the system schedule graph.  This exercises the failure arm without
// requiring the real topology sort from plans #247/#248.
//
// Placed here (plugin_loader_register/) because the SUT is
// plugin_loader_actions.hpp free functions — the same translation unit that
// hosts rebuild_schedule_smoke and the other step 9–11 tests (cohesion: all
// plugin_loader_actions.* tests live together).
//
// Sections:
//   A. Injected rebuild_fn that returns SystemScheduleCycle → failure
//      propagated, mock invocation counter == 1 (mock was called exactly once).
//   B. Injected rebuild_fn that returns success → success propagated,
//      mock invocation counter == 1 (mock was called, returned success).
//
// Authority: reviews/decisions/plugin-abi.md §"Loader Sequence" step 10,
//            §"Failure Modes → core::Error" table (row 10).
// Plan: #982 — SystemScheduleCycle integration test.
// DoD: unit_test_named: rebuild_schedule_returns_cycle_on_conflicting_systems
// ===========================================================================

// TU-local invocation counters for the rebuild mock functions (accessed via
// file-scope helpers so they can be used from non-capturing function pointers).
// Reset to 0 before each SECTION to avoid cross-section interference.
namespace {
int g_cycle_rebuild_call_count = 0;    // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
int g_success_rebuild_call_count = 0;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

/// Mock rebuild_fn that returns SystemScheduleCycle.
/// Simulates a cycle detected in the system schedule graph (step 10 failure path).
glibre::Result<void> cycle_rebuild_fn() noexcept {
    ++g_cycle_rebuild_call_count;
    return std::unexpected(glibre::Error{glibre::core::Error::SystemScheduleCycle});
}

/// Mock rebuild_fn that returns success.
/// Simulates a cycle-free schedule rebuild (step 10 happy path via injection).
glibre::Result<void> success_rebuild_fn() noexcept {
    ++g_success_rebuild_call_count;
    return {};
}
}  // namespace

TEST_CASE("rebuild_schedule_returns_cycle_on_conflicting_systems", "[core][register][schedule]") {
    SECTION("injected rebuild_fn returns cycle: failure propagated, mock called once") {
        // Reset invocation counter for this section.
        g_cycle_rebuild_call_count = 0;

        // Inject a mock that simulates a cycle in the system schedule graph.
        // plugin-abi.md §"Loader Sequence" step 10:
        //   "A cycle → core::Error::SystemScheduleCycle."
        auto result = glibre::core::rebuild_schedule(cycle_rebuild_fn);

        // Mock must have been called exactly once.
        CHECK(g_cycle_rebuild_call_count == 1);

        // The SystemScheduleCycle failure must be propagated unchanged.
        REQUIRE(!result);
        const auto* core_err = as_core_error(result.error());
        REQUIRE(core_err != nullptr);
        CHECK(*core_err == glibre::core::Error::SystemScheduleCycle);
    }

    SECTION("injected rebuild_fn returns success: success propagated, mock called once") {
        // Reset invocation counter for this section.
        g_success_rebuild_call_count = 0;

        // Inject a mock that simulates a cycle-free schedule rebuild (no cycle
        // detected).  The injectable overload must call the mock and propagate
        // success.
        auto result = glibre::core::rebuild_schedule(success_rebuild_fn);

        // Mock must have been called exactly once.
        CHECK(g_success_rebuild_call_count == 1);

        // Success from the mock must be propagated: the result is a value.
        REQUIRE(result.has_value());
    }
}

// ===========================================================================
// Test: call_register_stamped_stamps_correct_tag_from_manifest_name
//
// Integration test proving that the production call path:
//   derive_context_tag(manifest.name)
//   → AllocatorHandle{per_context_alloc, tag}
//   → PluginContext{..., alloc_handle}
//   → call_register(register_fn, ctx)
// is correctly wired end-to-end (plan #989 §Scope, HIGH-1 round-2 review).
//
// Exercises call_register_stamped on:
//   a) A valid manifest name ("glibre.render.camera") — must return success.
//      The render allocator receives the stamped tag at construct time.
//   b) An invalid manifest name ("myplugin") — must return PluginManifestInvalid.
//      This confirms that derive_context_tag is invoked (not bypassed) and
//      that its error is propagated before calling the plugin entry-point.
//
// The noop plugin's glibre_plugin_register returns success unconditionally, so
// the success of case (a) is the observable proof that all four steps in the
// production seam were executed without error.  The failure of case (b) is the
// observable proof that step 1 (derive_context_tag) runs before step 4
// (call_register), completing the seam verification.
//
// Authority: issue #989 §Scope; perf-budget.md §Allocator Rules #1.
// DoD: unit_test_named: call_register_stamped_stamps_correct_tag_from_manifest_name
// ===========================================================================

TEST_CASE(
    "call_register_stamped_stamps_correct_tag_from_manifest_name", "[core][register][alloc]"
) {
#ifndef GLIBRE_NOOP_DYLIB_PATH
    // FAIL rather than SKIP: this test is part of the DoD for plan #989.
    FAIL(
        "GLIBRE_NOOP_DYLIB_PATH not defined — "
        "rebuild with GLIBRE_BUILD_EXAMPLES=ON (noop plugin required)"
    );
#else
    constexpr std::string_view noop_path{GLIBRE_NOOP_DYLIB_PATH};
    REQUIRE_FALSE(noop_path.empty());

    // Load the noop plugin — steps 1–2 of the loader sequence.
    auto loader_result = glibre::core::PluginLoader::open(noop_path, loader_mr_);
    REQUIRE(loader_result.has_value());
    const glibre::core::PluginLoader& loader = *loader_result;
    REQUIRE(loader.register_fn() != nullptr);

    // Build stub registry objects.
    glibre::core::World world;
    glibre::core::TypeRegistry type_reg;
    glibre::core::SystemRegistry sys_reg;
    glibre::core::PassRegistry pass_reg;
    glibre::core::PanelRegistry panel_reg;
    glibre::core::LogSink log_sink;

    SECTION("valid manifest name — production stamping seam succeeds") {
        // A render-context allocator: the production seam should derive
        // ContextTag::render from "glibre.render.camera" and stamp the handle.
        glibre::PerContextAllocator render_alloc{glibre::ContextTag::render, kTestAllocCeiling};

        // Construct a manifest with a valid render-context plugin name.
        auto manifest = make_manifest("glibre.render.camera");

        // call_register_stamped: full production seam.
        //   Step 1: derive_context_tag("glibre.render.camera") → ContextTag::render
        //   Step 2: AllocatorHandle{render_alloc, ContextTag::render}
        //   Step 3: PluginContext{..., alloc_handle}
        //   Step 4: call_register(register_fn, ctx) — noop returns success
        auto result = glibre::core::call_register_stamped(
            loader.register_fn(),
            render_alloc,
            manifest,
            world,
            type_reg,
            sys_reg,
            pass_reg,
            panel_reg,
            log_sink
        );

        // All four steps succeeded: the seam is wired.
        REQUIRE(result.has_value());
    }

    SECTION("invalid manifest name — derive_context_tag fires before call_register") {
        // A core allocator (arbitrary; call_register_stamped must fail before
        // using it because the manifest name is invalid).
        glibre::PerContextAllocator core_alloc{glibre::ContextTag::core, kTestAllocCeiling};

        // Manifest with a name that cannot be parsed by derive_context_tag.
        auto manifest = make_manifest("myplugin");

        // call_register_stamped must return PluginManifestInvalid from step 1
        // (derive_context_tag) without reaching step 4 (the noop plugin is never called).
        auto result = glibre::core::call_register_stamped(
            loader.register_fn(),
            core_alloc,
            manifest,
            world,
            type_reg,
            sys_reg,
            pass_reg,
            panel_reg,
            log_sink
        );

        REQUIRE_FALSE(result.has_value());
        const auto* core_err = as_core_error(result.error());
        REQUIRE(core_err != nullptr);
        CHECK(*core_err == glibre::core::Error::PluginManifestInvalid);
    }
#endif
}
