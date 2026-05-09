// SPDX-License-Identifier: Apache-2.0
// tools/foryc/src/emit_migration.cpp
//
// Implementation of the migration-dispatcher emission pass for glibre-foryc
// (plan #221).
//
// See emit_migration.hpp for the public API and design notes.

#include "emit_migration.hpp"

#include <format>
#include <string_view>

#include <EASTL/string.h>
#include <EASTL/vector.h>

namespace glibre::tools::foryc {

namespace {

// -----------------------------------------------------------------------
// emit_migration_for_type — emit the dispatcher fragment for one TypeDecl
//
// If the TypeDecl has no migrations, emits nothing (empty string).
// Otherwise emits:
//   // Forward declarations
//   extern void* <provider0>;
//   ...
//   // Static table
//   static const MigrationEntry k_migrations_<type>[] = { ... };
//
// The per-type tables are later stitched together by emit_migration into
// a single TU with the global exported symbols.
// -----------------------------------------------------------------------

struct TypeMigrationFragment {
    eastl::string forward_decls;  // extern declarations for provider symbols
    eastl::string table_entry;    // one MigrationEntry initialiser list entry per step
    std::size_t count{0};         // number of migration steps
};

// Build the forward-declaration block and table entries for one TypeDecl.
[[nodiscard]] static TypeMigrationFragment
build_type_fragment(const TypeDecl& td) noexcept {
    TypeMigrationFragment frag;
    if (td.migrations.empty())
        return frag;

    for (const auto& mig : td.migrations) {
        const std::string_view prov(mig.provider.data(), mig.provider.size());

        // Forward-declare the provider as an extern "C" function returning void*
        // (type-erased; the owning context supplies the real signature).
        // We use a void (*)(void) prototype as a minimal legal C function pointer
        // type that can be cast to/from other function pointer types in C++.
        frag.forward_decls += eastl::string(
            std::format("extern \"C\" void {}();\n", prov).c_str()
        );

        // One MigrationEntry initialiser:
        //   { from_version, to_version, reinterpret_cast<void*>(<provider>) }
        frag.table_entry += eastl::string(
            std::format(
                "    {{ {}, {}, reinterpret_cast<void*>(&{}) }},\n",
                mig.from_version,
                mig.to_version,
                prov
            )
                .c_str()
        );
        ++frag.count;
    }
    return frag;
}

}  // anonymous namespace

// -----------------------------------------------------------------------
// emit_migration — public API
// -----------------------------------------------------------------------

[[nodiscard]] glibre::Result<eastl::string>
emit_migration(const Schema& schema, std::string_view source_path) noexcept {
    if (schema.types.empty())
        return std::unexpected{glibre::Error{tools::Error::ForycEmptySchema}};

    // Validate: every TypeDecl must have a non-empty FQN.
    for (const auto& td : schema.types) {
        if (td.fqn.empty())
            return std::unexpected{glibre::Error{tools::Error::ForycSyntaxError}};
    }

    // Collect all migration steps across every TypeDecl.
    // The plugin exposes a single flat migration table; the dispatcher looks up
    // (from_version, to_version) pairs regardless of which TypeDecl they came from.
    eastl::string all_forward_decls;
    eastl::string all_entries;
    std::size_t total_count = 0;

    for (const auto& td : schema.types) {
        auto frag = build_type_fragment(td);
        all_forward_decls += frag.forward_decls;
        all_entries += frag.table_entry;
        total_count += frag.count;
    }

    // -----------------------------------------------------------------------
    // Assemble the generated TU.
    // -----------------------------------------------------------------------

    eastl::string out;

    // File-level header comment.
    out += "// GENERATED FILE — do not edit by hand.\n";
    out += "// Source: ";
    out += eastl::string(source_path.data(), source_path.size());
    out += "\n";
    out += "// Generator: glibre-foryc (plan #221)\n";
    out += "//\n";
    out += "// Migration dispatcher table for plugin-loader use.\n";
    out += "// The plugin loader reads glibre_plugin_migrations[0..size-1] at\n";
    out += "// deserialization time and chains (from_version -> to_version) steps.\n";
    out += "\n";

    // Mandatory includes.
    out += "#include <cstddef>\n";
    out += "#include <cstdint>\n";
    out += "\n";

    // MigrationEntry struct — must match glibre/types/migration_entry.hpp layout.
    // Defined here to keep the generated TU self-contained and independent of
    // the owning context's headers (no #include needed in the dispatcher TU).
    out += "// MigrationEntry — ABI-stable entry in the dispatcher table.\n";
    out += "// Layout MUST match glibre::types::MigrationEntry in the plugin loader.\n";
    out += "struct MigrationEntry {\n";
    out += "    std::uint32_t from_version;\n";
    out += "    std::uint32_t to_version;\n";
    out += "    void* provider;  // type-erased function pointer\n";
    out += "};\n";
    out += "\n";

    if (total_count == 0) {
        // -----------------------------------------------------------------------
        // No-migration path: export empty table.
        // -----------------------------------------------------------------------
        out += "// No migration steps declared for this schema.\n";
        out += "extern \"C\" const MigrationEntry* glibre_plugin_migrations = nullptr;\n";
        out += "extern \"C\" std::size_t glibre_plugin_migrations_size = 0;\n";
    } else {
        // -----------------------------------------------------------------------
        // One or more migration steps.
        // -----------------------------------------------------------------------

        // Forward-declare provider symbols.
        out += "// Provider forward declarations.\n";
        out += "// The owning context supplies the function bodies.\n";
        out += all_forward_decls;
        out += "\n";

        // Static migration table.
        out += "// clang-format off\n";
        out += "static const MigrationEntry k_migrations[] = {\n";
        out += all_entries;
        out += "};\n";
        out += "// clang-format on\n";
        out += "\n";

        // Exported symbols.
        out += "extern \"C\" const MigrationEntry* glibre_plugin_migrations = k_migrations;\n";
        out += eastl::string(
            std::format(
                "extern \"C\" std::size_t glibre_plugin_migrations_size = {};\n",
                total_count
            )
                .c_str()
        );
    }

    return out;
}

}  // namespace glibre::tools::foryc
