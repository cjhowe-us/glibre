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

    // The generated TU forward-declares provider functions with versioned
    // argument types (e.g. `const WidgetV1&, WidgetV2&`).  In a real build
    // those types are defined in `glibre/types/<ctx>/<Type>V<N>.hpp`.  For
    // this round-trip test we inject the type definitions via a preamble
    // written before the generated TU content into a single combined source
    // file.  This mirrors how the real build assembles the generated TU with
    // the owning context's type headers visible.
    {
        // Preamble: minimal type definitions + glibre::Error stub so the
        // generated forward-decls and the provider body all compile together.
        constexpr std::string_view preamble = R"(
#include <expected>

// Minimal definition of glibre::Error (stand-in for the real header).
namespace glibre { struct Error {}; }

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
        std::ofstream ofs{gen_path, std::ios::trunc};
        REQUIRE(ofs.is_open());
        // Write the preamble, then the generated content.
        ofs.write(preamble.data(), static_cast<std::streamsize>(preamble.size()));
        ofs.write(gen_text.data(), static_cast<std::streamsize>(gen_text.size()));
        REQUIRE(ofs.good());
    }

    // --- Compile the combined TU into a .dylib. ---
    const auto compile_cmd = std::format(
        "clang++ -std=c++23 -fno-exceptions -fno-rtti "
        "-dynamiclib -o \"{}\" \"{}\" 2>&1",
        dylib_path.native(),
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
