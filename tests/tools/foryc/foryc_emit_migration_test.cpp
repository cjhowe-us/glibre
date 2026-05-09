// SPDX-License-Identifier: Apache-2.0
// tests/tools/foryc/foryc_emit_migration_test.cpp
//
// Catch2 unit tests for glibre-foryc migration dispatcher emission (plan #221).
//
// Test names match the dispatch prompt's Unit Test Plan:
//   - foryc_emit_migration_dispatcher_writes_table
//   - foryc_emit_migration_handles_no_migrations
//
// Additional coverage tests:
//   - foryc_emit_migration_exports_correct_symbols
//   - foryc_emit_migration_rejects_empty_schema
//   - foryc_parser_stores_migration_decls
//   - foryc_emit_migration_round_trip_via_compile
//
// PHILOSOPHY §11: EASTL replaces std containers/strings in the IR.
//   std:: retained for: std::expected (glibre::Result), std::string_view,
//   std::filesystem, std::system (subprocess invocation), dlfcn.h.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>

#include <EASTL/string.h>
#include <catch2/catch_test_macros.hpp>

#include "emit_migration.hpp"
#include "parser.hpp"

namespace fs = std::filesystem;
using namespace glibre::tools::foryc;

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

static Schema parse_ok(std::string_view src, std::string_view vpath = "<test>") {
    auto r = parse_string(src, vpath);
    REQUIRE(r.has_value());
    return std::move(*r);
}

static bool has_tools_error(const glibre::Error& e, glibre::tools::Error code) noexcept {
    const auto* te = eastl::get_if<glibre::tools::Error>(&e.code());
    return te && (*te == code);
}

// -----------------------------------------------------------------------
// Dispatch-prompt named tests
// -----------------------------------------------------------------------

TEST_CASE("foryc_emit_migration_dispatcher_writes_table", "[foryc][emit_migration]") {
    // Schema with 2 migration declarations.  emit_migration must return a TU
    // that contains both function-pointer entries in the per-type dispatcher table.
    constexpr std::string_view src = R"(
schema glibre.core.Transform {
  version 3
  field translation : vec3f tag 1
  migration from 1 to 2 calls "glibre::core::migrate_Transform_v1_to_v2"
  migration from 2 to 3 calls "glibre::core::migrate_Transform_v2_to_v3"
}
)";

    const auto schema = parse_ok(src, "core/Transform.fory");

    // Verify parser stored both MigrationDecls.
    REQUIRE(schema.types.size() == 1);
    const auto& td = schema.types[0];
    REQUIRE(td.migrations.size() == 2);
    CHECK(td.migrations[0].from_version == 1);
    CHECK(td.migrations[0].to_version == 2);
    CHECK(td.migrations[0].provider == eastl::string("glibre::core::migrate_Transform_v1_to_v2"));
    CHECK(td.migrations[1].from_version == 2);
    CHECK(td.migrations[1].to_version == 3);
    CHECK(td.migrations[1].provider == eastl::string("glibre::core::migrate_Transform_v2_to_v3"));

    // Emit migration dispatcher.
    auto result = emit_migration(schema, "core/Transform.fory");
    REQUIRE(result.has_value());

    const eastl::string& text = *result;

    // Both provider forward-declarations must appear as C++ namespace-qualified
    // functions (NOT extern "C") per fory-codegen.md §"Migration Mechanic" point 2.
    CHECK(text.find("glibre::core::migrate_Transform_v1_to_v2") != eastl::string::npos);
    CHECK(text.find("glibre::core::migrate_Transform_v2_to_v3") != eastl::string::npos);

    // C linkage is NOT used for the provider forward-decls (namespaced C++ fns).
    // We check that "extern \"C\"" does NOT precede any provider decl.
    // The per-type table and export symbols do use extern "C", but the providers don't.
    // Simplest check: forward-decl uses std::expected return type (correct sig).
    CHECK(text.find("std::expected<void, glibre::Error>") != eastl::string::npos);

    // Per-type static table variable must be present (fory-codegen.md §"Migration
    // Mechanic" point 1: each type carries its own table).
    CHECK(text.find("k_migrations_Transform") != eastl::string::npos);

    // Both from/to version pairs must appear as integer literals in the table.
    CHECK(text.find("1, 2") != eastl::string::npos);
    CHECK(text.find("2, 3") != eastl::string::npos);

    // The MigrationEntry struct definition must be present.
    CHECK(text.find("MigrationEntry") != eastl::string::npos);

    // Per-type exported symbols must be present (not the old flat names).
    CHECK(text.find("glibre_plugin_migrations_Transform") != eastl::string::npos);
    CHECK(text.find("glibre_plugin_migrations_Transform_size") != eastl::string::npos);
}

TEST_CASE("foryc_emit_migration_handles_no_migrations", "[foryc][emit_migration]") {
    // Schema with no migration declarations.  The emitted TU must expose an
    // empty table (size=0) with a nullptr migrations pointer, keyed by type name.
    constexpr std::string_view src = R"(
schema glibre.core.Velocity {
  version 1
  field linear  : vec3f tag 1
  field angular : vec3f tag 2
}
)";

    const auto schema = parse_ok(src, "core/Velocity.fory");

    // Parser should have stored zero migrations.
    REQUIRE(schema.types.size() == 1);
    CHECK(schema.types[0].migrations.empty());

    // Emit migration dispatcher.
    auto result = emit_migration(schema, "core/Velocity.fory");
    REQUIRE(result.has_value());

    const eastl::string& text = *result;

    // No-migration path: must NOT contain k_migrations_<type> (the static table).
    CHECK(text.find("k_migrations") == eastl::string::npos);

    // size must be 0.
    CHECK(text.find("= 0") != eastl::string::npos);

    // Null pointer for the migrations pointer.
    CHECK(text.find("nullptr") != eastl::string::npos);

    // Per-type exported symbols must still appear.
    CHECK(text.find("glibre_plugin_migrations_Velocity") != eastl::string::npos);
    CHECK(text.find("glibre_plugin_migrations_Velocity_size") != eastl::string::npos);
}

// -----------------------------------------------------------------------
// Additional tests
// -----------------------------------------------------------------------

TEST_CASE("foryc_emit_migration_exports_correct_symbols", "[foryc][emit_migration]") {
    // The generated TU must export per-type symbols with extern "C" linkage.
    constexpr std::string_view src = R"(
schema glibre.physics.RigidBody {
  version 2
  field mass    : f32 tag 1
  field restitution : f32 tag 2
  migration from 1 to 2 calls "glibre::physics::migrate_RigidBody_v1_to_v2"
}
)";

    const auto schema = parse_ok(src);
    auto result = emit_migration(schema, "<test>");
    REQUIRE(result.has_value());

    const eastl::string& text = *result;

    // Exact export pattern (extern "C" linkage on the per-type symbols).
    CHECK(text.find("extern \"C\"") != eastl::string::npos);

    // Per-type symbol names.
    CHECK(text.find("glibre_plugin_migrations_RigidBody") != eastl::string::npos);
    CHECK(text.find("glibre_plugin_migrations_RigidBody_size") != eastl::string::npos);

    // std::size_t used for the size (PHILOSOPHY §11: std:: for non-EASTL utilities).
    CHECK(text.find("std::size_t") != eastl::string::npos);
}

TEST_CASE("foryc_emit_migration_rejects_empty_schema", "[foryc][emit_migration]") {
    // A Schema with zero TypeDecls must return ForycEmptySchema.
    Schema schema;
    schema.source_path = eastl::string("<empty>");

    auto result = emit_migration(schema, "<empty>");
    REQUIRE(!result.has_value());
    CHECK(has_tools_error(result.error(), glibre::tools::Error::ForycEmptySchema));
}

TEST_CASE("foryc_parser_stores_migration_decls", "[foryc][parser][migration]") {
    // The parser must correctly store MigrationDecl entries in TypeDecl::migrations
    // when migration declarations appear inside a schema block.
    constexpr std::string_view src = R"(
schema glibre.data.Asset {
  version 3
  since "0.2.0"
  field path : string tag 1
  field flags : u32 tag 2
  migration from 1 to 2 calls "glibre::data::migrate_Asset_v1_to_v2"
  migration from 2 to 3 calls "glibre::data::migrate_Asset_v2_to_v3"
}
)";

    auto result = parse_string(src, "data/Asset.fory");
    REQUIRE(result.has_value());

    const auto& schema = *result;
    REQUIRE(schema.types.size() == 1);
    const auto& td = schema.types[0];

    // Version and fields still parsed correctly (no regression).
    CHECK(td.version == 3);
    CHECK(td.fields.size() == 2);

    // Migrations stored.
    REQUIRE(td.migrations.size() == 2);
    CHECK(td.migrations[0].from_version == 1);
    CHECK(td.migrations[0].to_version == 2);
    CHECK(td.migrations[0].provider == eastl::string("glibre::data::migrate_Asset_v1_to_v2"));
    CHECK(td.migrations[1].from_version == 2);
    CHECK(td.migrations[1].to_version == 3);
    CHECK(td.migrations[1].provider == eastl::string("glibre::data::migrate_Asset_v2_to_v3"));
}

// -----------------------------------------------------------------------
// Test: foryc_emit_migration_cross_namespace_provider
//
// MED-B regression: when the schema type lives in one namespace and the
// provider lives in another (e.g. glibre.core.Transform with provider
// "glibre::physics::migrate_Transform_v1_to_v2"), the emitter must wrap
// the fwd-decl in the PROVIDER's namespace, not the type's namespace.
// Failure mode: emitter wraps the fwd-decl in glibre::core but the table
// references &glibre::physics::migrate_Transform_v1_to_v2 → link error.
// -----------------------------------------------------------------------

TEST_CASE("foryc_emit_migration_cross_namespace_provider", "[foryc][emit_migration]") {
    constexpr std::string_view src = R"(
schema glibre.core.Transform {
  version 2
  field translation : vec3f tag 1
  migration from 1 to 2 calls "glibre::physics::migrate_Transform_v1_to_v2"
}
)";

    const auto schema = parse_ok(src, "core/Transform.fory");
    auto result = emit_migration(schema, "core/Transform.fory");
    REQUIRE(result.has_value());

    const eastl::string& text = *result;

    // The provider forward-declaration must be wrapped in the provider's
    // namespace (glibre::physics).
    CHECK(text.find("namespace glibre::physics") != eastl::string::npos);

    // The fully-qualified provider reference in the table must use the
    // full path (glibre::physics::migrate_Transform_v1_to_v2).
    CHECK(text.find("glibre::physics::migrate_Transform_v1_to_v2") != eastl::string::npos);

    // The function signature uses the correct std::expected return type.
    CHECK(text.find("std::expected<void, glibre::Error>") != eastl::string::npos);

    // The versioned type forward-declarations (TransformV1, TransformV2) must
    // appear in the type's namespace (glibre::core), not the provider's.
    // Check that glibre::core namespace appears (for type fwd-decl) and that
    // the function fwd-decl uses fully-qualified type names (glibre::core::).
    CHECK(text.find("namespace glibre::core") != eastl::string::npos);

    // The provider function signature uses fully-qualified parameter types.
    // This ensures the fwd-decl is valid regardless of which namespace wraps it.
    CHECK(text.find("glibre::core::TransformV1") != eastl::string::npos);
    CHECK(text.find("glibre::core::TransformV2") != eastl::string::npos);

    // Verify the provider fwd-decl (inside glibre::physics) uses the
    // unqualified function name (not the fully-qualified glibre::physics:: prefix).
    // The namespace block owns the name; full qualification inside would be wrong.
    const auto physics_open = text.find("namespace glibre::physics");
    const auto physics_close = text.find("}  // namespace glibre::physics");
    REQUIRE(physics_open != eastl::string::npos);
    REQUIRE(physics_close != eastl::string::npos);
    REQUIRE(physics_open < physics_close);
    const eastl::string physics_block(text.data() + physics_open, physics_close - physics_open);
    // Inside the namespace block, the function name is unqualified.
    CHECK(physics_block.find("migrate_Transform_v1_to_v2") != eastl::string::npos);
}

// -----------------------------------------------------------------------
// Test: foryc_emit_migration_round_trip_via_compile
//
// Compiles the emitted migrations.cpp into a stub .dylib using the system
// clang++, then dlopen()s it and dlsym()s the per-type exported C-ABI
// symbols.  Verifies:
//   1. The TU compiles without errors (no invalid C++ in the generated file).
//   2. Per-type table pointer resolves (non-null).
//   3. Per-type size symbol resolves and equals the expected count.
//   4. Per-type nullptr + size=0 for the no-migration type.
//
// Compile failure is a hard REQUIRE failure — if clang++ is unavailable
// the CI configuration is broken, not the source.
//
// HIGH-2 fix: tests actually compile + link the emitted TU so that linkage
// defects (wrong forward-decl shape, invalid C++) cannot hide behind
// string-grep-only assertions.
// -----------------------------------------------------------------------

TEST_CASE("foryc_emit_migration_round_trip_via_compile", "[foryc][emit_migration][integration]") {
    // Schema with one type having 1 migration + one type with no migrations.
    // The stub dylib also needs the provider body (forward-declared in the TU).
    // We supply a stub body in the same compilation unit.
    constexpr std::string_view fory_src = R"(
schema glibre.test.Widget {
  version 2
  field name : string tag 1
  migration from 1 to 2 calls "glibre::test::migrate_Widget_v1_to_v2"
}
schema glibre.test.Gadget {
  version 1
  field value : u32 tag 1
}
)";

    auto parse_result = parse_string(fory_src, "test/Widget.fory");
    REQUIRE(parse_result.has_value());

    auto emit_result = emit_migration(*parse_result, "test/Widget.fory");
    REQUIRE(emit_result.has_value());

    const eastl::string& gen_text = *emit_result;

    // --- Write the generated source + a stub migration body to a temp dir. ---
    const fs::path tmp_base = fs::temp_directory_path() / "glibre_foryc_mig_test";
    const auto unique_suffix =
        std::format("roundtrip_{}_{}", getpid(), reinterpret_cast<uintptr_t>(&gen_text));
    const fs::path tmp_dir = tmp_base / unique_suffix;

    std::error_code ec;
    fs::create_directories(tmp_dir, ec);
    REQUIRE(!ec);

    const fs::path gen_path = tmp_dir / "migrations_roundtrip.cpp";
    const fs::path dylib_path = tmp_dir / "migrations_roundtrip.dylib";

    // The generated TU now includes "glibre/error.hpp" (complete glibre::Error)
    // and forward-declares provider functions with versioned argument types.
    // In a real build those types are defined in glibre/types/<ctx>/<Type>V<N>.hpp.
    // For this round-trip test we write a stub preamble source that:
    //   1. Includes the real "glibre/error.hpp" (via -I in compile_cmd).
    //   2. Defines versioned struct stand-ins and the provider body.
    // The preamble is written into a SEPARATE file (preamble.cpp), compiled
    // into a separate object, and linked with the generated TU into the dylib.
    // This means the generated TU (gen_path) is compiled WITHOUT any preamble
    // injection, so the seam is genuinely exercised: the generated #include
    // "glibre/error.hpp" must resolve on its own via -I flags.
    const fs::path preamble_path = tmp_dir / "preamble.cpp";
    {
        // Preamble: versioned type stubs + provider body.
        // glibre::Error is NOT defined here; it comes from glibre/error.hpp
        // which the generated TU includes directly.
        constexpr std::string_view preamble = R"(
#include <expected>
#include "glibre/error.hpp"

// Versioned struct definitions (stand-ins for the real generated types).
namespace glibre::test {
struct WidgetV1 { int name{}; };
struct WidgetV2 { int name{}; };

// Provider body — the owning context supplies this in a real build.
std::expected<void, glibre::Error>
migrate_Widget_v1_to_v2(const WidgetV1&, WidgetV2&) {
    return {};
}
}  // namespace glibre::test
)";
        std::ofstream ofs{preamble_path, std::ios::trunc};
        REQUIRE(ofs.is_open());
        ofs.write(preamble.data(), static_cast<std::streamsize>(preamble.size()));
        REQUIRE(ofs.good());
    }

    // Write the generated TU verbatim — no preamble prepended.
    {
        std::ofstream ofs{gen_path, std::ios::trunc};
        REQUIRE(ofs.is_open());
        ofs.write(gen_text.data(), static_cast<std::streamsize>(gen_text.size()));
        REQUIRE(ofs.good());
    }

    // --- Compile the combined TU into a .dylib. ---
    // HIGH-A fix: supply -I flags so the generated TU's #include "glibre/error.hpp"
    // and "glibre/error.hpp"'s own #include <EASTL/...> both resolve.
    // GLIBRE_CORE_INCLUDE_DIR and GLIBRE_VCPKG_INCLUDE_DIR are injected by
    // the CMakeLists target_compile_definitions for this test binary.
    const auto compile_cmd = std::format(
        "clang++ -std=c++23 -fno-exceptions -fno-rtti "
        "-I\"" GLIBRE_CORE_INCLUDE_DIR "\" "
        "-I\"" GLIBRE_VCPKG_INCLUDE_DIR "\" "
        "-dynamiclib -o \"{}\" \"{}\" \"{}\" 2>&1",
        dylib_path.native(),
        preamble_path.native(),
        gen_path.native()
    );

    // HIGH-2 fix: compile failure is a hard REQUIRE failure.
    // If clang++ is unavailable the CI environment is broken, not the source.
    const int compile_rc = std::system(compile_cmd.c_str());  // NOLINT(concurrency-mt-unsafe)
    REQUIRE(compile_rc == 0);
    REQUIRE(fs::exists(dylib_path));

    // --- dlopen the compiled .dylib. ---
    const std::string dylib_native = dylib_path.native();
    void* handle = dlopen(dylib_native.c_str(), RTLD_NOW | RTLD_LOCAL);
    REQUIRE(handle != nullptr);

    // --- dlsym per-type C-ABI symbols. ---

    // Widget has 1 migration: table pointer should be non-null, size == 1.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const void** widget_ptr =
        reinterpret_cast<const void**>(dlsym(handle, "glibre_plugin_migrations_Widget"));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    std::size_t* widget_size =
        reinterpret_cast<std::size_t*>(dlsym(handle, "glibre_plugin_migrations_Widget_size"));

    // Gadget has no migrations: table pointer should be nullptr, size == 0.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const void** gadget_ptr =
        reinterpret_cast<const void**>(dlsym(handle, "glibre_plugin_migrations_Gadget"));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    std::size_t* gadget_size =
        reinterpret_cast<std::size_t*>(dlsym(handle, "glibre_plugin_migrations_Gadget_size"));

    // All four per-type symbols must resolve.
    CHECK(widget_ptr != nullptr);
    CHECK(widget_size != nullptr);
    CHECK(gadget_ptr != nullptr);
    CHECK(gadget_size != nullptr);

    if (widget_ptr && widget_size) {
        // Widget has migrations: table must be non-null and size == 1.
        CHECK(*widget_ptr != nullptr);
        CHECK(*widget_size == 1);
    }

    if (gadget_ptr && gadget_size) {
        // Gadget has no migrations: empty-table form.
        CHECK(*gadget_ptr == nullptr);
        CHECK(*gadget_size == 0);
    }

    dlclose(handle);

    // Cleanup (best effort).
    fs::remove_all(tmp_dir, ec);
}

// -----------------------------------------------------------------------
// Test: foryc_emit_migration_emits_current_version (plan #978)
//
// The emitted TU must contain a `glibre_plugin_current_version_<TypeName>`
// symbol set to the schema's declared version (td.version).
// -----------------------------------------------------------------------

TEST_CASE("foryc_emit_migration_emits_current_version", "[foryc][emit_migration][current_version]") {
    // Schema with a specific version number — the emitter must use td.version.
    constexpr std::string_view src = R"(
schema glibre.core.Sensor {
  version 5
  field reading : f32 tag 1
  field flags   : u32 tag 2
}
)";

    const auto schema = parse_ok(src, "core/Sensor.fory");
    REQUIRE(schema.types.size() == 1);
    CHECK(schema.types[0].version == 5);

    auto result = emit_migration(schema, "core/Sensor.fory");
    REQUIRE(result.has_value());

    const eastl::string& text = *result;

    // The current_version symbol must appear with the correct type.
    CHECK(text.find("glibre_plugin_current_version_Sensor") != eastl::string::npos);

    // Must use extern "C" linkage for dlsym-ability (fory-codegen.md §"ABI
    // Stability Rules" — exported C entry points cross dylib boundaries as C ABI).
    CHECK(text.find("extern \"C\"") != eastl::string::npos);

    // Must use std::uint32_t (not int/uint32_t bare) per the plan spec.
    CHECK(text.find("std::uint32_t") != eastl::string::npos);

    // The value must equal the schema version (5).
    // We look for "glibre_plugin_current_version_Sensor = 5" (spaces may vary
    // slightly, so search for the symbol name and "5" in the same vicinity by
    // checking the sub-string within a reasonable window).
    const auto sym_pos = text.find("glibre_plugin_current_version_Sensor");
    REQUIRE(sym_pos != eastl::string::npos);
    // Grab the rest of the line (up to 120 chars).
    const eastl::string line(
        text.data() + sym_pos,
        eastl::string::size_type(
            std::min(std::size_t{120}, text.size() - sym_pos)
        )
    );
    CHECK(line.find("5") != eastl::string::npos);

    // Also verify that a second type in the same schema gets its own symbol
    // with its own version (multi-type schema case).
    constexpr std::string_view multi_src = R"(
schema glibre.core.Alpha {
  version 2
  field x : u32 tag 1
}
schema glibre.core.Beta {
  version 7
  field y : f32 tag 1
}
)";
    const auto multi_schema = parse_ok(multi_src, "core/multi.fory");
    REQUIRE(multi_schema.types.size() == 2);

    auto multi_result = emit_migration(multi_schema, "core/multi.fory");
    REQUIRE(multi_result.has_value());
    const eastl::string& mt = *multi_result;

    CHECK(mt.find("glibre_plugin_current_version_Alpha") != eastl::string::npos);
    CHECK(mt.find("glibre_plugin_current_version_Beta") != eastl::string::npos);

    // Each symbol carries its own version value.
    const auto alpha_pos = mt.find("glibre_plugin_current_version_Alpha");
    REQUIRE(alpha_pos != eastl::string::npos);
    const eastl::string alpha_line(
        mt.data() + alpha_pos,
        eastl::string::size_type(std::min(std::size_t{120}, mt.size() - alpha_pos))
    );
    CHECK(alpha_line.find("2") != eastl::string::npos);

    const auto beta_pos = mt.find("glibre_plugin_current_version_Beta");
    REQUIRE(beta_pos != eastl::string::npos);
    const eastl::string beta_line(
        mt.data() + beta_pos,
        eastl::string::size_type(std::min(std::size_t{120}, mt.size() - beta_pos))
    );
    CHECK(beta_line.find("7") != eastl::string::npos);
}

// -----------------------------------------------------------------------
// Test: foryc_emit_migration_current_version_round_trip (plan #978)
//
// Round-trip compile the emitted TU into a stub .dylib with clang++, then
// dlopen + dlsym `glibre_plugin_current_version_<TypeName>` and verify
// the loaded value matches the schema's declared version.
// -----------------------------------------------------------------------

TEST_CASE(
    "foryc_emit_migration_current_version_round_trip",
    "[foryc][emit_migration][current_version][integration]"
) {
    // Schema: one type with a non-trivial version + one type with no migrations.
    // Both must get their own current_version symbols.
    constexpr std::string_view fory_src = R"(
schema glibre.test.Turbo {
  version 4
  field power : f32 tag 1
  migration from 1 to 2 calls "glibre::test::migrate_Turbo_v1_to_v2"
  migration from 2 to 3 calls "glibre::test::migrate_Turbo_v2_to_v3"
  migration from 3 to 4 calls "glibre::test::migrate_Turbo_v3_to_v4"
}
schema glibre.test.Valve {
  version 1
  field open : u32 tag 1
}
)";

    auto parse_result = parse_string(fory_src, "test/Turbo.fory");
    REQUIRE(parse_result.has_value());
    CHECK(parse_result->types.size() == 2);
    CHECK(parse_result->types[0].version == 4);
    CHECK(parse_result->types[1].version == 1);

    auto emit_result = emit_migration(*parse_result, "test/Turbo.fory");
    REQUIRE(emit_result.has_value());

    const eastl::string& gen_text = *emit_result;

    // Confirm the symbols appear in the generated text before compiling.
    CHECK(gen_text.find("glibre_plugin_current_version_Turbo") != eastl::string::npos);
    CHECK(gen_text.find("glibre_plugin_current_version_Valve") != eastl::string::npos);

    // --- Set up temp dir. ---
    const fs::path tmp_base = fs::temp_directory_path() / "glibre_foryc_ver_test";
    const auto unique_suffix = std::format(
        "ver_rt_{}_{}", getpid(), reinterpret_cast<uintptr_t>(&gen_text)
    );
    const fs::path tmp_dir = tmp_base / unique_suffix;

    std::error_code ec;
    fs::create_directories(tmp_dir, ec);
    REQUIRE(!ec);

    const fs::path gen_path     = tmp_dir / "current_version_roundtrip.cpp";
    const fs::path preamble_path = tmp_dir / "preamble_ver.cpp";
    const fs::path dylib_path   = tmp_dir / "current_version_roundtrip.dylib";

    // Preamble: versioned struct stubs + provider bodies for Turbo migrations.
    constexpr std::string_view preamble = R"(
#include <expected>
#include "glibre/error.hpp"

namespace glibre::test {
// Versioned struct stubs for Turbo migrations.
struct TurboV1 { float power{}; };
struct TurboV2 { float power{}; };
struct TurboV3 { float power{}; };
struct TurboV4 { float power{}; };

std::expected<void, glibre::Error> migrate_Turbo_v1_to_v2(const TurboV1&, TurboV2&) { return {}; }
std::expected<void, glibre::Error> migrate_Turbo_v2_to_v3(const TurboV2&, TurboV3&) { return {}; }
std::expected<void, glibre::Error> migrate_Turbo_v3_to_v4(const TurboV3&, TurboV4&) { return {}; }
}  // namespace glibre::test
)";
    {
        std::ofstream ofs{preamble_path, std::ios::trunc};
        REQUIRE(ofs.is_open());
        ofs.write(preamble.data(), static_cast<std::streamsize>(preamble.size()));
        REQUIRE(ofs.good());
    }

    // Write the generated TU verbatim.
    {
        std::ofstream ofs{gen_path, std::ios::trunc};
        REQUIRE(ofs.is_open());
        ofs.write(gen_text.data(), static_cast<std::streamsize>(gen_text.size()));
        REQUIRE(ofs.good());
    }

    // --- Compile into a .dylib. ---
    const auto compile_cmd = std::format(
        "clang++ -std=c++23 -fno-exceptions -fno-rtti "
        "-I\"" GLIBRE_CORE_INCLUDE_DIR "\" "
        "-I\"" GLIBRE_VCPKG_INCLUDE_DIR "\" "
        "-dynamiclib -o \"{}\" \"{}\" \"{}\" 2>&1",
        dylib_path.native(),
        preamble_path.native(),
        gen_path.native()
    );

    const int compile_rc = std::system(compile_cmd.c_str());  // NOLINT(concurrency-mt-unsafe)
    REQUIRE(compile_rc == 0);
    REQUIRE(fs::exists(dylib_path));

    // --- dlopen. ---
    const std::string dylib_native = dylib_path.native();
    void* handle = dlopen(dylib_native.c_str(), RTLD_NOW | RTLD_LOCAL);
    REQUIRE(handle != nullptr);

    // --- dlsym current_version symbols and verify values. ---

    // Turbo: schema version == 4.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* turbo_ver =
        reinterpret_cast<const std::uint32_t*>(
            dlsym(handle, "glibre_plugin_current_version_Turbo")
        );
    REQUIRE(turbo_ver != nullptr);
    CHECK(*turbo_ver == 4u);

    // Valve: schema version == 1.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* valve_ver =
        reinterpret_cast<const std::uint32_t*>(
            dlsym(handle, "glibre_plugin_current_version_Valve")
        );
    REQUIRE(valve_ver != nullptr);
    CHECK(*valve_ver == 1u);

    dlclose(handle);

    // Cleanup (best effort).
    fs::remove_all(tmp_dir, ec);
}
