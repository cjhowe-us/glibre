// SPDX-License-Identifier: Apache-2.0
// tools/foryc/src/emit_migration.cpp
//
// Implementation of the migration-dispatcher emission pass for glibre-foryc
// (plan #221).
//
// See emit_migration.hpp for the public API and design notes.

#include "emit_migration.hpp"

#include <format>
#include <string>
#include <string_view>

#include "fqn_mangle.hpp"

namespace glibre::tools::foryc {

namespace {

// -----------------------------------------------------------------------
// fmt_e — format a string and return it as a std::string.
//
// Thin alias kept so call sites remain readable without changes.
// -----------------------------------------------------------------------

template<class... Args>
[[nodiscard]] static std::string fmt_e(std::format_string<Args...> fmt, Args&&... args) noexcept {
    return std::format(fmt, std::forward<Args>(args)...);
}

// -----------------------------------------------------------------------
// fqn_to_ns_and_type — split a dotted FQN into C++ namespace + type name
//
// "glibre.core.Transform" -> ns="glibre::core"  type_name="Transform"
// "glibre.Transform"      -> ns="glibre"         type_name="Transform"
// "Transform"             -> ns=""               type_name="Transform"
// -----------------------------------------------------------------------

struct FqnParts {
    std::string ns;         // C++ namespace ("::" separator), may be empty
    std::string type_name;  // unqualified C++ class name
};

[[nodiscard]] static FqnParts split_fqn(const std::string& fqn) noexcept {
    FqnParts parts;
    // Find the last '.' separator.
    const auto last_dot = fqn.rfind('.');
    if (last_dot == std::string::npos) {
        parts.type_name = fqn;
        return parts;
    }
    // Everything before the last dot, with '.' replaced by '::'.
    const std::string prefix(fqn.data(), last_dot);
    std::string ns_str;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (prefix[i] == '.') {
            ns_str += "::";
        } else {
            ns_str += prefix[i];
        }
    }
    parts.ns = std::move(ns_str);
    parts.type_name = std::string(fqn.data() + last_dot + 1, fqn.size() - last_dot - 1);
    return parts;
}

// -----------------------------------------------------------------------
// emit_type_block — emit the per-TypeDecl section of the generated TU.
//
// Symbol names use the mangled FQN (<MangledFQN> = FQN with '.' → '__').
// Example: "glibre.core.Transform" → "glibre__core__Transform".
//
// For a TypeDecl with migrations, emits:
//   1. C++ namespace + forward-declarations of each provider with the correct
//      signature per fory-codegen.md §"Migration Mechanic" point 2:
//        std::expected<void, glibre::Error> migrate_<Type>_v<N>_to_v<N+1>(
//            const <Type>V<N>&, <Type>V<N+1>&)
//   2. A static per-type MigrationEntry table: k_migrations_<MangledFQN>[].
//   3. Two extern "C" exported symbols:
//        glibre_plugin_migrations_<MangledFQN>  (pointer to the table)
//        glibre_plugin_migrations_<MangledFQN>_size  (count)
//
// For a TypeDecl with no migrations, emits the empty-table form:
//   extern "C" const MigrationEntry* glibre_plugin_migrations_<MangledFQN> = nullptr;
//   extern "C" std::size_t glibre_plugin_migrations_<MangledFQN>_size = 0;
//
// fory-codegen.md §"Migration Mechanic" point 1:
//   "Each generated type carries a static migrations table populated at
//   static-init time inside the glibre-types dylib via the
//   codegen-emitted dispatcher."
// -----------------------------------------------------------------------

[[nodiscard]] static std::string emit_type_block(const TypeDecl& td) noexcept {
    const FqnParts parts = split_fqn(td.fqn);
    const std::string& type_name = parts.type_name;

    // Mangle the full FQN into the C symbol suffix (plan #1010).
    // "glibre.core.Transform" → "glibre__core__Transform"
    // This prevents symbol collisions when two schema types share the same
    // unqualified name but live in different namespaces (e.g. glibre.core.Particle
    // and glibre.fx.Particle would collide without mangling).
    // fory-codegen.md §"ABI Stability Rules" point 4.
    // Shared helper from fqn_mangle.hpp — also reused by parser.cpp.
    const std::string mangled = fqn_mangle::fqn_to_mangled(td.fqn);

    std::string out;
    out += "// ---- ";
    out += td.fqn;
    out += " ----\n";

    // Emit the current_version symbol first (fory-codegen.md §"Migration Mechanic"
    // point 1: "each generated type carries current_version").
    // The plugin loader dlsym()s this to short-circuit the no-migration-needed path
    // and to detect payloads from future schema versions (plan #978).
    out += fmt_e(
        "extern \"C\" const std::uint32_t glibre_plugin_current_version_{} = {};\n",
        std::string_view(mangled.data(), mangled.size()),
        td.version
    );
    out += "\n";

    if (td.migrations.empty()) {
        // Empty-table form.
        out += "extern \"C\" const MigrationEntry* glibre_plugin_migrations_";
        out += mangled;
        out += " = nullptr;\n";
        out += "extern \"C\" std::size_t glibre_plugin_migrations_";
        out += mangled;
        out += "_size = 0;\n";
        out += "\n";
        return out;
    }

    // -----------------------------------------------------------------------
    // Forward-declare each provider as a C++ function with the correct
    // signature per fory-codegen.md §"Migration Mechanic" point 2.
    //
    // Signature: std::expected<void, glibre::Error>
    //                migrate_<Type>_v<N>_to_v<M>(const <Type>V<N>&, <Type>V<M>&)
    //
    // These are C++ namespace-qualified declarations (NOT extern "C") because
    // C linkage forbids namespace-qualified names (C++ [dcl.link] / ISO C++23
    // [dcl.link]/6: "A name with C language linkage shall not be in a
    // namespace").
    // -----------------------------------------------------------------------

    for (const auto& mig : td.migrations) {
        // Versioned argument type names (always fully-qualified to be
        // unambiguous regardless of which namespace wraps the provider
        // fwd-decl): <ns>::<TypeName>V<N>, <ns>::<TypeName>V<M>.
        // These structs live in the type's own namespace (parts.ns).
        // Forward-declare them (in that namespace) so the migration
        // function signature compiles without including the generated
        // type headers.  In the real glibre-types.dylib build the
        // generated headers are on the include path, but forward-
        // declarations make the TU independently well-formed.
        const std::string from_unq = type_name + fmt_e("V{}", mig.from_version);
        const std::string to_unq = type_name + fmt_e("V{}", mig.to_version);

        // Emit struct forward-declarations in the type's namespace.
        if (!parts.ns.empty()) {
            out += "namespace ";
            out += parts.ns;
            out += " { struct ";
            out += from_unq;
            out += "; struct ";
            out += to_unq;
            out += "; }  // namespace ";
            out += parts.ns;
            out += "\n";
        } else {
            out += "struct ";
            out += from_unq;
            out += ";\n";
            out += "struct ";
            out += to_unq;
            out += ";\n";
        }

        // Fully-qualified parameter type names for the provider fwd-decl.
        // Always qualify with the type's namespace so the declaration is
        // unambiguous even when the provider lives in a different namespace.
        const std::string ns_prefix =
            parts.ns.empty() ? std::string("") : (parts.ns + std::string("::"));
        const std::string from_fq = ns_prefix + from_unq;
        const std::string to_fq = ns_prefix + to_unq;

        // Derive the provider's own namespace and unqualified function name by
        // splitting mig.provider on the last "::".  This is independent of the
        // type's namespace: a schema may declare a provider in a different
        // namespace (e.g. type "glibre.core.Transform" with provider
        // "glibre::physics::migrate_Transform_v1_to_v2").  Using the type's
        // namespace for the fwd-decl would silently forward-declare the wrong
        // symbol and cause a link error.
        std::string provider_ns;
        std::string fn_name = mig.provider;
        const auto last_sep = fn_name.rfind("::");
        if (last_sep != std::string::npos) {
            provider_ns = std::string(mig.provider.data(), last_sep);
            fn_name = std::string(
                mig.provider.data() + last_sep + 2, mig.provider.size() - last_sep - 2
            );
        }

        if (!provider_ns.empty()) {
            out += "namespace ";
            out += provider_ns;
            out += " {\n";
        }
        out += fmt_e(
            "std::expected<void, glibre::Error> {}(const {}&, {}&);\n",
            std::string_view(fn_name.data(), fn_name.size()),
            std::string_view(from_fq.data(), from_fq.size()),
            std::string_view(to_fq.data(), to_fq.size())
        );
        if (!provider_ns.empty()) {
            out += "}  // namespace ";
            out += provider_ns;
            out += "\n";
        }
    }
    out += "\n";

    // -----------------------------------------------------------------------
    // Build the per-type migration function pointer type.
    //
    // Because each migration step has different concrete argument types
    // (e.g. TransformV1 / TransformV2) the MigrationEntry stores the
    // function pointer as void* (type-erased) identical to the layout
    // declared in the MigrationEntry struct above.  The plugin loader
    // recovers the real type via the registered deserialize path.
    // -----------------------------------------------------------------------

    // Static per-type migration table.
    // Fully-qualified provider is used here so the reference links even if
    // the forward decl above is in a different namespace.
    // Use mangled FQN for the C++ static variable name so that two types with
    // the same unqualified name but different FQNs don't clash in the same TU.
    const std::size_t count = td.migrations.size();
    out += "static const MigrationEntry k_migrations_";
    out += mangled;
    out += "[] = {\n";
    for (const auto& mig : td.migrations) {
        out += fmt_e(
            "    {{ {}, {}, reinterpret_cast<void*>(&{}) }},\n",
            mig.from_version,
            mig.to_version,
            std::string_view(mig.provider.data(), mig.provider.size())
        );
    }
    out += "};\n";
    out += "\n";

    // Exported per-type symbols — use mangled FQN as the suffix (plan #1010).
    // fory-codegen.md §"ABI Stability Rules" point 4.
    out += "extern \"C\" const MigrationEntry* glibre_plugin_migrations_";
    out += mangled;
    out += " = k_migrations_";
    out += mangled;
    out += ";\n";
    out += fmt_e(
        "extern \"C\" std::size_t glibre_plugin_migrations_{}_size = {};\n",
        std::string_view(mangled.data(), mangled.size()),
        count
    );
    out += "\n";
    return out;
}

}  // anonymous namespace

// -----------------------------------------------------------------------
// emit_migration — public API
// -----------------------------------------------------------------------

[[nodiscard]] glibre::Result<std::string>
emit_migration(const Schema& schema, std::string_view source_path) noexcept {
    if (schema.types.empty())
        return std::unexpected{glibre::Error{tools::Error::ForycEmptySchema}};

    // Validate: every TypeDecl must have a non-empty FQN, and no segment of that
    // FQN may contain "__" (double-underscore).  This defensive check mirrors the
    // parse-time guard in parser.cpp::parse_fqn() so that callers who build IR
    // directly (without going through the parser) cannot silently produce
    // non-injective mangles (plan #1010, fory-codegen.md §"ABI Stability Rules"
    // point 4).  Shared helper from fqn_mangle.hpp.
    for (const auto& td : schema.types) {
        if (td.fqn.empty())
            return std::unexpected{glibre::Error{tools::Error::ForycSyntaxError}};

        // Walk segments (split on '.') and check each for "__".
        std::string_view fqn_sv(td.fqn.data(), td.fqn.size());
        while (!fqn_sv.empty()) {
            const auto dot = fqn_sv.find('.');
            const std::string_view seg =
                (dot == std::string_view::npos) ? fqn_sv : fqn_sv.substr(0, dot);
            if (fqn_mangle::segment_contains_double_underscore(seg))
                return std::unexpected{glibre::Error{tools::Error::ForycInvalidIdentifier}};
            fqn_sv = (dot == std::string_view::npos) ? std::string_view{} : fqn_sv.substr(dot + 1);
        }
    }

    // -----------------------------------------------------------------------
    // Assemble the generated TU.
    // -----------------------------------------------------------------------

    std::string out;

    // File-level header comment.
    out += "// GENERATED FILE — do not edit by hand.\n";
    out += "// Source: ";
    out += std::string(source_path.data(), source_path.size());
    out += "\n";
    out += "// Generator: glibre-foryc (plan #221)\n";
    out += "//\n";
    out += "// Per-type migration dispatcher tables for plugin-loader use.\n";
    out += "// The plugin loader reads glibre_plugin_migrations_<Type>[0..size-1] at\n";
    out += "// deserialization time and chains (from_version -> to_version) steps.\n";
    out += "// fory-codegen.md §\"Migration Mechanic\" points 1-2.\n";
    out += "\n";

    // Mandatory includes.
    out += "#include <cstddef>\n";
    out += "#include <cstdint>\n";
    out += "#include <expected>\n";
    out += "\n";

    // glibre/error.hpp provides glibre::Error (complete definition).
    // The generated TU is compiled into glibre-types.dylib which has
    // core/include on its include path; this include is therefore valid.
    // A forward-declaration is insufficient because std::expected<void, E>
    // requires E complete on instantiation (libc++ stores E in a union).
    out += "#include \"glibre/error.hpp\"\n";
    out += "\n";

    // MigrationEntry struct — must match glibre/types/migration_entry.hpp layout.
    // Defined here to keep the generated TU self-contained.
    out += "// MigrationEntry — ABI-stable entry in the per-type dispatcher table.\n";
    out += "// Layout MUST match glibre::types::MigrationEntry in the plugin loader.\n";
    out += "struct MigrationEntry {\n";
    out += "    std::uint32_t from_version;\n";
    out += "    std::uint32_t to_version;\n";
    out += "    void* provider;  // type-erased function pointer\n";
    out += "};\n";
    out += "\n";

    // Per-type blocks.
    for (const auto& td : schema.types) {
        out += emit_type_block(td);
    }

    return out;
}

}  // namespace glibre::tools::foryc
