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

#include <catch2/catch_test_macros.hpp>

#include "emit_migration.hpp"
#include "parser.hpp"

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
    // that contains both function-pointer entries in the dispatcher table.
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

    // Both provider forward-declarations must appear.
    CHECK(text.find("glibre::core::migrate_Transform_v1_to_v2") != eastl::string::npos);
    CHECK(text.find("glibre::core::migrate_Transform_v2_to_v3") != eastl::string::npos);

    // The static table variable must be present.
    CHECK(text.find("k_migrations") != eastl::string::npos);

    // Both from/to version pairs must appear as integer literals.
    CHECK(text.find("1, 2") != eastl::string::npos);
    CHECK(text.find("2, 3") != eastl::string::npos);

    // The MigrationEntry struct definition must be present.
    CHECK(text.find("MigrationEntry") != eastl::string::npos);

    // Exported symbols must be present.
    CHECK(text.find("glibre_plugin_migrations") != eastl::string::npos);
    CHECK(text.find("glibre_plugin_migrations_size") != eastl::string::npos);
}

TEST_CASE("foryc_emit_migration_handles_no_migrations", "[foryc][emit_migration]") {
    // Schema with no migration declarations.  The emitted TU must expose an
    // empty table (size=0) with a nullptr migrations pointer.
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

    // No-migration path: must NOT contain k_migrations (the static table).
    CHECK(text.find("k_migrations") == eastl::string::npos);

    // size must be 0.
    CHECK(text.find("= 0") != eastl::string::npos);

    // Null pointer for the migrations pointer.
    CHECK(text.find("nullptr") != eastl::string::npos);

    // Both exported symbols must still appear.
    CHECK(text.find("glibre_plugin_migrations") != eastl::string::npos);
    CHECK(text.find("glibre_plugin_migrations_size") != eastl::string::npos);
}

// -----------------------------------------------------------------------
// Additional tests
// -----------------------------------------------------------------------

TEST_CASE("foryc_emit_migration_exports_correct_symbols", "[foryc][emit_migration]") {
    // The generated TU must export exactly:
    //   extern "C" const MigrationEntry* glibre_plugin_migrations
    //   extern "C" std::size_t           glibre_plugin_migrations_size
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

    // Exact export pattern (extern "C" linkage).
    CHECK(text.find("extern \"C\"") != eastl::string::npos);

    // Both symbol names present.
    CHECK(text.find("glibre_plugin_migrations") != eastl::string::npos);
    CHECK(text.find("glibre_plugin_migrations_size") != eastl::string::npos);

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
