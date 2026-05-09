// SPDX-License-Identifier: Apache-2.0
// tools/foryc/src/main.cpp
//
// glibre-foryc — .fory schema compiler host tool.
//
// Usage:
//   glibre-foryc --in <dir> --out <dir> --stamp <file> [--emit=header]
//
// Walks <in>/**/*.fory, parses each file into the schema IR, validates
// basic shape (unique tags, version >= 1, builtins-only types).
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
// Output path derivation (fory-codegen.md §Pipeline):
//   source:  <in>/<rel>/<file>.fory
//   for each TypeDecl with FQN "glibre.<ns>.<Type>":
//     output: <out>/include/glibre/types/<rel>/<Type>.hpp
//
// Exits non-zero on parse or emit failure, with a diagnostic on stderr.

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include <EASTL/optional.h>
#include <EASTL/vector.h>

#include "emit_header.hpp"
#include "parser.hpp"

namespace fs = std::filesystem;
using namespace glibre::tools::foryc;

// -----------------------------------------------------------------------
// Emit mode enum
// -----------------------------------------------------------------------

enum class EmitMode {
    None,    // parse-only (default, plan #219 behaviour)
    Header,  // emit C++ header per TypeDecl (plan #220)
};

// -----------------------------------------------------------------------
// Argument parsing
// -----------------------------------------------------------------------

struct Args {
    fs::path in_dir{};
    fs::path out_dir{};
    fs::path stamp_file{};
    EmitMode emit{EmitMode::None};
};

static void usage(std::string_view program) {
    std::cerr << "Usage: " << program << " --in <dir> --out <dir> --stamp <file> [--emit=header]\n";
}

static eastl::optional<Args> parse_args(int argc, char** argv) {
    Args args;
    eastl::vector<std::string_view> tokens(argv + 1, argv + argc);
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const auto tok = tokens[i];
        auto next = [&]() -> eastl::optional<std::string_view> {
            if (i + 1 >= tokens.size())
                return eastl::nullopt;
            return tokens[++i];
        };
        if (tok == "--in") {
            auto v = next();
            if (!v)
                return eastl::nullopt;
            args.in_dir = *v;
        } else if (tok == "--out") {
            auto v = next();
            if (!v)
                return eastl::nullopt;
            args.out_dir = *v;
        } else if (tok == "--stamp") {
            auto v = next();
            if (!v)
                return eastl::nullopt;
            args.stamp_file = *v;
        } else if (tok == "--emit=header") {
            args.emit = EmitMode::Header;
        } else {
            std::cerr << "foryc: unknown flag: " << tok << "\n";
            return eastl::nullopt;
        }
    }
    if (args.in_dir.empty() || args.out_dir.empty() || args.stamp_file.empty())
        return eastl::nullopt;
    return args;
}

// -----------------------------------------------------------------------
// Error code to human-readable message
// -----------------------------------------------------------------------

static std::string_view error_name(const glibre::Error& e) noexcept {
    using glibre::tools::Error;
    if (const auto* te = eastl::get_if<Error>(&e.code())) {
        switch (*te) {
        case Error::ForycSyntaxError:
            return "syntax error";
        case Error::ForycDuplicateTag:
            return "duplicate tag";
        case Error::ForycNonMonotoneVersion:
            return "non-monotone version";
        case Error::ForycUnknownType:
            return "unknown type";
        case Error::ForycIOError:
            return "I/O error";
        case Error::ForycEmptySchema:
            return "schema has zero TypeDecl blocks";
        default:
            __builtin_unreachable();
        }
    }
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
        const eastl::string& fqn = td.fqn;
        const std::size_t dot = fqn.rfind('.');
        const eastl::string type_name = (dot == eastl::string::npos) ? fqn : fqn.substr(dot + 1);

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

        const eastl::string& text = *result;
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
// Entry point
// -----------------------------------------------------------------------

int main(int argc, char** argv) {
    const auto args_opt = parse_args(argc, argv);
    if (!args_opt) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const auto& args = *args_opt;

    // Collect all .fory files under args.in_dir recursively.
    eastl::vector<fs::path> schema_files;
    if (!fs::exists(args.in_dir) || !fs::is_directory(args.in_dir)) {
        std::cerr << "foryc: --in directory does not exist: " << args.in_dir << "\n";
        return EXIT_FAILURE;
    }

    for (const auto& entry : fs::recursive_directory_iterator(args.in_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".fory")
            schema_files.push_back(entry.path());
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
