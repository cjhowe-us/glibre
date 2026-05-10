// SPDX-License-Identifier: Apache-2.0
// tools/foryc/src/main.cpp
//
// glibre-foryc — .fory schema compiler host tool.
//
// Usage:
//   glibre-foryc --in <dir> --out <dir> --stamp <file> [--emit=header|--emit=migration]
//   glibre-foryc --file <path> --out <dir> --stamp <file> --emit=manifest
//
// In directory mode (--in <dir>):
//   Walks <in>/**/*.fory, parses each file into the schema IR, validates
//   basic shape (unique tags, version >= 1, builtins-only types).
//
// In single-file mode (--file <path>):
//   Processes exactly one .fory file.  Required for --emit=manifest to avoid
//   races when multiple plugins invoke the helper concurrently (MED-7 fix,
//   reviews/decisions/plugin-abi.md §"Manifest source-of-truth").
//
// Without --emit: parse-only mode (plan #219 behaviour).  Emits a
//   one-line summary to stdout per file and touches <stamp> on success.
//
// With --emit=header: emits one .hpp file per TypeDecl to
//   <out>/include/glibre/types/<ctx>/<Type>.hpp
// where <ctx> is derived from the source-relative subdirectory path of
// the .fory file under <in> (e.g. data/schemas/core/Transform.fory with
// --in data/schemas → <ctx> = "core") and <Type> is the last component
// of the schema FQN.  Multi-type .fory files produce multiple .hpp files.
//
// With --emit=migration: emits one migrations.cpp per .fory file to
//   <out>/src/<ctx>/<stem>_migrations.cpp
// The generated TU exports:
//   extern "C" const MigrationEntry* glibre_plugin_migrations;
//   extern "C" std::size_t           glibre_plugin_migrations_size;
// For schemas with no migration declarations the table is empty (size=0).
// See reviews/decisions/fory-codegen.md §"Migration Mechanic".
//
// With --emit=manifest: emits <out>/manifest.cpp with the four C-ABI symbols
//   required by the glibre plugin loader (plugin-abi.md §"Plugin file shape"):
//   - glibre_plugin_manifest      — extern "C" const uint8_t*, serialized blob
//   - glibre_plugin_manifest_size — extern "C" size_t, blob byte length
//   - glibre_plugin_abi_hash      — extern "C" const char*, 64-char blake3 hex
//   - glibre_plugin_name          — extern "C" const char*, fully-qualified name
//   Use --file <path> (single file) rather than --in <dir> to avoid races.
//
// Output path derivation (fory-codegen.md §Pipeline):
//   source:  <in>/<rel>/<file>.fory
//   for each TypeDecl with FQN "glibre.<ns>.<Type>":
//     output: <out>/include/glibre/types/<rel>/<Type>.hpp   (--emit=header)
//     output: <out>/src/<rel>/<stem>_migrations.cpp         (--emit=migration)
//     output: <out>/manifest.cpp                            (--emit=manifest)
//
// Exits non-zero on parse or emit failure, with a diagnostic on stderr.

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "emit_header.hpp"
#include "emit_manifest.hpp"
#include "emit_migration.hpp"
#include "glibre/log_error.hpp"
#include "parser.hpp"

namespace fs = std::filesystem;
using namespace glibre::tools::foryc;

// -----------------------------------------------------------------------
// Emit mode enum
// -----------------------------------------------------------------------

enum class EmitMode {
    None,       // parse-only (default, plan #219 behaviour)
    Header,     // emit C++ header per TypeDecl (plan #220)
    Migration,  // emit C++ migration dispatcher per .fory file (plan #221)
    Manifest,   // emit manifest.cpp per plugin.fory (plan #225)
};

// -----------------------------------------------------------------------
// Argument parsing
// -----------------------------------------------------------------------

struct Args {
    fs::path in_dir{};       // directory to scan recursively (--in)
    fs::path single_file{};  // single .fory file to process (--file)
    fs::path out_dir{};
    fs::path stamp_file{};
    EmitMode emit{EmitMode::None};
};

static void usage(std::string_view program) {
    std::cerr << "Usage: " << program
              << " --in <dir>|--file <path> --out <dir> --stamp <file>"
                 " [--emit=header|--emit=migration|--emit=manifest]\n";
}

static std::optional<Args> parse_args(int argc, char** argv) {
    Args args;
    std::vector<std::string_view> tokens(argv + 1, argv + argc);
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const auto tok = tokens[i];
        auto next = [&]() -> std::optional<std::string_view> {
            if (i + 1 >= tokens.size())
                return std::nullopt;
            return tokens[++i];
        };
        if (tok == "--in") {
            auto v = next();
            if (!v)
                return std::nullopt;
            args.in_dir = *v;
        } else if (tok == "--file") {
            auto v = next();
            if (!v)
                return std::nullopt;
            args.single_file = *v;
        } else if (tok == "--out") {
            auto v = next();
            if (!v)
                return std::nullopt;
            args.out_dir = *v;
        } else if (tok == "--stamp") {
            auto v = next();
            if (!v)
                return std::nullopt;
            args.stamp_file = *v;
        } else if (tok == "--emit=header") {
            args.emit = EmitMode::Header;
        } else if (tok == "--emit=migration") {
            args.emit = EmitMode::Migration;
        } else if (tok == "--emit=manifest") {
            args.emit = EmitMode::Manifest;
        } else {
            std::cerr << "foryc: unknown flag: " << tok << "\n";
            return std::nullopt;
        }
    }
    // Either --in (directory) or --file (single file) must be provided, not both.
    if (args.in_dir.empty() == args.single_file.empty()) {
        std::cerr << "foryc: exactly one of --in or --file must be specified\n";
        return std::nullopt;
    }
    if (args.out_dir.empty() || args.stamp_file.empty())
        return std::nullopt;
    return args;
}

// -----------------------------------------------------------------------
// Error code to human-readable message
//
// Routes through glibre::tools::to_string(Error) (log_error.hpp:126)
// which is exhaustive by construction (no default clause; -Werror=switch
// enforces coverage).  This avoids the dual-maintenance risk of keeping a
// parallel switch that can silently miss new arms (deferred defect from
// PR #1060 R2: ForycInvalidIdentifier was absent from the previous switch;
// fixes the UB that default:__builtin_unreachable() was masking).
// -----------------------------------------------------------------------

static std::string_view error_name(const glibre::Error& e) noexcept {
    using glibre::tools::Error;
    if (const auto* te = std::get_if<Error>(&e.code()))
        return glibre::tools::to_string(*te);
    return "unknown error";
}

// -----------------------------------------------------------------------
// Header emission helper
//
// Emits one .hpp file per TypeDecl in the schema, calling
// emit_header_for_type directly (canonical per-type entry, MED-3 r2).
//
// Output path per fory-codegen.md §Pipeline:
//   <out_dir>/include/glibre/types/<rel_subdir>/<Type>.hpp
// where:
//   <rel_subdir> is the source path relative to in_dir (directory part).
//   <Type>       is the last component of the TypeDecl FQN.
//
// Multi-type .fory files produce one file per TypeDecl.
// Schemas with no TypeDecls are rejected (ForycEmptySchema).
//
// Returns true on success, false on error (diagnostic already printed).
// -----------------------------------------------------------------------

static bool emit_headers_for_schema(
    const Schema& schema,
    const fs::path& source_path,
    const fs::path& in_dir,
    const fs::path& out_dir
) noexcept {
    if (schema.types.empty()) {
        std::cerr << std::format(
            "foryc: {}: schema has zero TypeDecl blocks\n", source_path.native()
        );
        return false;
    }

    // Compute the subdirectory relative to in_dir.
    // e.g. source_path = data/schemas/core/Transform.fory, in_dir = data/schemas
    //   → rel_subdir = "core"
    fs::path rel_subdir;
    {
        std::error_code ec;
        const fs::path rel = fs::relative(source_path.parent_path(), in_dir, ec);
        if (ec) {
            std::cerr << std::format(
                "foryc: cannot compute relative path for {}: {}\n",
                source_path.native(),
                ec.message()
            );
            return false;
        }
        // rel may be "." if the file is directly under in_dir; use empty subdir.
        rel_subdir = (rel == fs::path(".")) ? fs::path{} : rel;
    }

    const fs::path include_base = out_dir / "include" / "glibre" / "types";
    const std::string src_native = source_path.native();

    for (const auto& td : schema.types) {
        // Extract the type name (last FQN component).
        // FQN format: "glibre.core.Transform" — type name is "Transform".
        const std::string& fqn = td.fqn;
        const std::size_t dot = fqn.rfind('.');
        const std::string type_name = (dot == std::string::npos) ? fqn : fqn.substr(dot + 1);

        // Build the per-type output directory.
        fs::path type_dir = include_base;
        if (!rel_subdir.empty())
            type_dir /= rel_subdir;

        std::error_code ec;
        fs::create_directories(type_dir, ec);
        if (ec) {
            std::cerr << std::format(
                "foryc: cannot create directory {}: {}\n", type_dir.native(), ec.message()
            );
            return false;
        }

        // Emit header for this TypeDecl via the canonical per-type entry point.
        auto result = emit_header_for_type(td, src_native);
        if (!result) {
            std::cerr << std::format(
                "foryc: {}: header emit failed for type {}: {}\n",
                source_path.native(),
                std::string_view(type_name.data(), type_name.size()),
                error_name(result.error())
            );
            return false;
        }

        const fs::path out_path =
            type_dir / (std::string(type_name.data(), type_name.size()) + ".hpp");
        std::ofstream ofs{out_path, std::ios::trunc};
        if (!ofs) {
            std::cerr << std::format("foryc: cannot write header: {}\n", out_path.native());
            return false;
        }

        const std::string& text = *result;
        ofs.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!ofs) {
            std::cerr << std::format("foryc: write error: {}\n", out_path.native());
            return false;
        }

        std::cout << std::format("foryc: emitted header {}\n", out_path.native());
    }

    return true;
}

// -----------------------------------------------------------------------
// Migration dispatcher emission helper (plan #221)
//
// Emits one migrations.cpp per .fory file to:
//   <out_dir>/src/<rel_subdir>/<stem>_migrations.cpp
//
// Returns true on success, false on error (diagnostic already printed).
// -----------------------------------------------------------------------

static bool emit_migration_for_schema(
    const Schema& schema,
    const fs::path& source_path,
    const fs::path& in_dir,
    const fs::path& out_dir
) noexcept {
    if (schema.types.empty()) {
        std::cerr << std::format(
            "foryc: {}: schema has zero TypeDecl blocks\n", source_path.native()
        );
        return false;
    }

    // Compute the subdirectory relative to in_dir.
    fs::path rel_subdir;
    {
        std::error_code ec;
        const fs::path rel = fs::relative(source_path.parent_path(), in_dir, ec);
        if (ec) {
            std::cerr << std::format(
                "foryc: cannot compute relative path for {}: {}\n",
                source_path.native(),
                ec.message()
            );
            return false;
        }
        rel_subdir = (rel == fs::path(".")) ? fs::path{} : rel;
    }

    // Output path: <out>/src/<rel>/<stem>_migrations.cpp
    fs::path src_dir = out_dir / "src";
    if (!rel_subdir.empty())
        src_dir /= rel_subdir;

    std::error_code ec;
    fs::create_directories(src_dir, ec);
    if (ec) {
        std::cerr << std::format(
            "foryc: cannot create directory {}: {}\n", src_dir.native(), ec.message()
        );
        return false;
    }

    // Stem: the .fory filename without extension.
    const std::string stem = source_path.stem().native();
    const fs::path out_path = src_dir / (stem + "_migrations.cpp");
    const std::string src_native = source_path.native();

    // Emit the migration dispatcher TU.
    auto result = emit_migration(schema, src_native);
    if (!result) {
        std::cerr << std::format(
            "foryc: {}: migration emit failed: {}\n",
            source_path.native(),
            error_name(result.error())
        );
        return false;
    }

    std::ofstream ofs{out_path, std::ios::trunc};
    if (!ofs) {
        std::cerr << std::format(
            "foryc: cannot write migration dispatcher: {}\n", out_path.native()
        );
        return false;
    }

    const std::string& text = *result;
    ofs.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!ofs) {
        std::cerr << std::format("foryc: write error: {}\n", out_path.native());
        return false;
    }

    std::cout << std::format("foryc: emitted migration dispatcher {}\n", out_path.native());
    return true;
}

// -----------------------------------------------------------------------
// Manifest emission helper (plan #225)
//
// For a given parsed Schema, builds a PluginManifestSpec from the schema
// metadata and emits a manifest.cpp to <out_dir>/manifest.cpp.
//
// Spec derivation from parsed Schema (HIGH-3 fix):
//   - name         ← FQN of the first TypeDecl, minus its last component
//                    (e.g. schema "glibre.render.PluginManifest" → "glibre.render").
//                    This is the canonical plugin id per plugin-abi.md §"name" field.
//   - version      ← {0, 1, 0} — not encoded in the MVP .fory IR; deferred to #231.
//   - abi_hash     ← 64 hex zeros placeholder; the real value is the blake3 over
//                    all schema sources (plan #222, not yet landed).  The loader
//                    falls back to the sidecar path per plugin-abi.md §"Step-3
//                    deferral" until #222 lands and this is wired up.
//   - min_engine_version ← {0, 0, 0} — not encoded in MVP .fory IR; deferred to #231.
//   - depends_on   ← empty — not encoded in MVP .fory IR; deferred to #231.
//
// Returns true on success, false on error (diagnostic already printed).
// -----------------------------------------------------------------------

// Derive a plugin name from a TypeDecl FQN by stripping the last dot-separated
// component.
//
// Convention (enforced here): the first TypeDecl in a PluginManifest schema MUST
// have an FQN whose last component is exactly "PluginManifest", e.g.
// "glibre.render.PluginManifest" → plugin name "glibre.render".
// plugin-abi.md §"name" field: "fully-qualified plugin id (e.g. glibre.render)".
//
// Returns an empty string and sets *error_msg if the FQN does not end with
// ".PluginManifest" (including the case of a single-component FQN with no dot,
// which would otherwise silently return the FQN as-is and produce a non-namespaced
// plugin name).
static std::string
plugin_name_from_fqn(const std::string& fqn, std::string* error_msg) noexcept {
    static constexpr std::string_view kSuffix = ".PluginManifest";
    const std::string_view fqn_sv{fqn.data(), fqn.size()};

    if (!fqn_sv.ends_with(kSuffix)) {
        if (error_msg) {
            *error_msg = std::format(
                "first TypeDecl FQN \"{}\" does not end with \".PluginManifest\"; "
                "rename the type or update the schema convention",
                fqn_sv
            );
        }
        return {};
    }

    // Strip the ".PluginManifest" suffix to get the plugin namespace id.
    return fqn.substr(0, fqn.size() - kSuffix.size());
}

static bool emit_manifest_for_file(
    const Schema& schema, const fs::path& source_path, const fs::path& out_dir
) noexcept {
    if (schema.types.empty()) {
        std::cerr << std::format(
            "foryc: {}: schema has zero TypeDecl blocks\n", source_path.native()
        );
        return false;
    }

    // Derive plugin name from the first TypeDecl's FQN (not from the path stem).
    // Convention: FQN must end with ".PluginManifest" (enforced by plugin_name_from_fqn).
    // e.g. "glibre.render.PluginManifest" → plugin name "glibre.render".
    // plugin-abi.md §"name" field: "fully-qualified plugin id (e.g. glibre.render)".
    std::string fqn_error;
    const std::string plugin_fqn = plugin_name_from_fqn(schema.types[0].fqn, &fqn_error);
    if (plugin_fqn.empty()) {
        std::cerr << std::format(
            "foryc: {}: {}\n",
            source_path.native(),
            std::string_view{fqn_error.data(), fqn_error.size()}
        );
        return false;
    }

    // Parse `since` version from the first TypeDecl if present.
    // Format: "MAJOR.MINOR.PATCH" (e.g. "0.1.0").  Defaults to {0,1,0} if absent
    // or unparseable — version/min_engine_version are not yet in the .fory IR
    // (deferred to plan #231).
    ManifestSemVer version{0, 1, 0};

    PluginManifestSpec spec;
    spec.name = plugin_fqn;
    spec.version = version;
    // abi_hash: plan #222 (schema source-hash + ABI hash export) has not landed.
    // Emit a 64-zero hex placeholder so the loader can distinguish a zero hash
    // (pre-#222) from a missing field.  The loader falls back to the sidecar path
    // per plugin-abi.md §"Step-3 deferral".  When #222 lands, wire
    // compute_collection_abi_hash() + format_as_full_hex() here.
    spec.abi_hash = std::string(64, '0');
    spec.min_engine_version = ManifestSemVer{0, 0, 0};
    // depends_on: not yet encoded in the .fory IR; deferred to plan #231.

    auto result = emit_manifest(spec);
    if (!result) {
        std::cerr << std::format(
            "foryc: {}: manifest emit failed: {}\n",
            source_path.native(),
            error_name(result.error())
        );
        return false;
    }

    std::error_code ec;
    fs::create_directories(out_dir, ec);
    if (ec) {
        std::cerr << std::format(
            "foryc: cannot create output directory {}: {}\n", out_dir.native(), ec.message()
        );
        return false;
    }

    const fs::path out_path = out_dir / "manifest.cpp";
    std::ofstream ofs{out_path, std::ios::trunc};
    if (!ofs) {
        std::cerr << std::format("foryc: cannot write manifest: {}\n", out_path.native());
        return false;
    }

    const std::string& text = *result;
    ofs.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!ofs) {
        std::cerr << std::format("foryc: write error: {}\n", out_path.native());
        return false;
    }

    std::cout << std::format("foryc: emitted manifest {}\n", out_path.native());
    return true;
}

// -----------------------------------------------------------------------
// Entry point
// -----------------------------------------------------------------------

int main(int argc, char** argv) {
    const auto args_opt = parse_args(argc, argv);
    if (!args_opt) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const auto& args = *args_opt;

    // Collect .fory files to process.
    // In single-file mode (--file), process exactly the one specified path.
    // In directory mode (--in), recursively scan for all .fory files.
    std::vector<fs::path> schema_files;
    if (!args.single_file.empty()) {
        // Single-file mode (MED-7 fix): used for --emit=manifest to avoid races
        // when multiple plugins invoke the codegen helper concurrently.
        if (!fs::exists(args.single_file) || !fs::is_regular_file(args.single_file)) {
            std::cerr << "foryc: --file path does not exist or is not a file: " << args.single_file
                      << "\n";
            return EXIT_FAILURE;
        }
        schema_files.push_back(args.single_file);
    } else {
        // Directory mode: scan recursively.
        if (!fs::exists(args.in_dir) || !fs::is_directory(args.in_dir)) {
            std::cerr << "foryc: --in directory does not exist: " << args.in_dir << "\n";
            return EXIT_FAILURE;
        }
        for (const auto& entry : fs::recursive_directory_iterator(args.in_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".fory")
                schema_files.push_back(entry.path());
        }
    }

    // Parse (and optionally emit) each file.
    std::size_t total_types = 0;
    for (const auto& path : schema_files) {
        auto result = parse_file(path);
        if (!result) {
            std::cerr << std::format("foryc: {}: {}\n", path.native(), error_name(result.error()));
            return EXIT_FAILURE;
        }
        const auto& schema = *result;
        total_types += schema.types.size();
        std::cout << std::format(
            "foryc: parsed {} type(s) from {}\n", schema.types.size(), path.native()
        );

        // Emit headers if requested.
        if (args.emit == EmitMode::Header) {
            if (!emit_headers_for_schema(schema, path, args.in_dir, args.out_dir))
                return EXIT_FAILURE;
        }

        // Emit migration dispatcher if requested (plan #221).
        if (args.emit == EmitMode::Migration) {
            if (!emit_migration_for_schema(schema, path, args.in_dir, args.out_dir))
                return EXIT_FAILURE;
        }

        // Emit manifest.cpp if requested (plan #225).
        if (args.emit == EmitMode::Manifest) {
            if (!emit_manifest_for_file(schema, path, args.out_dir))
                return EXIT_FAILURE;
        }
    }

    // Emit overall summary.
    if (!schema_files.empty()) {
        std::cout << std::format(
            "foryc: total: {} type(s) from {} file(s)\n", total_types, schema_files.size()
        );
    } else {
        std::cout << "foryc: no .fory files found in " << args.in_dir.native() << "\n";
    }

    // Touch the stamp file to signal success to the CMake custom target.
    {
        fs::create_directories(args.stamp_file.parent_path());
        std::ofstream stamp_out{args.stamp_file, std::ios::trunc};
        if (!stamp_out) {
            std::cerr << "foryc: cannot write stamp file: " << args.stamp_file << "\n";
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}
