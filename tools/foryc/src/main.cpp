// SPDX-License-Identifier: Apache-2.0
// tools/foryc/src/main.cpp
//
// glibre-foryc — .fory schema compiler host tool (plan #219 skeleton,
//                plan #222 adds --emit=abi-hash + --emit=collection-abi-hash).
//
// Usage:
//   glibre-foryc --in <dir> --out <dir> --stamp <file>
//   glibre-foryc --in <dir> --emit=abi-hash
//   glibre-foryc --in <dir> --emit=collection-abi-hash
//
// --in <dir> --out <dir> --stamp <file>:
//   Walks <in>/**/*.fory, parses each file into the schema IR, validates
//   basic shape (unique tags, version >= 1, builtins-only types), emits a
//   one-line summary to stdout per file, touches <stamp> on success.
//
// --in <dir> --emit=abi-hash:
//   Walks <in>/**/*.fory, parses each file, emits one tab-separated row
//   per TypeDecl (one row per fqn, NOT one row per file):
//     <schema_path>\t<fqn>\t<source_hash_hex_64>\t<abi_hash_hex_64>
//   (source_hash = 64-char blake3 of raw file bytes; abi_hash = 64-char
//    single-type collection ABI hash, per plugin-abi.md §"ABI Hash Function".)
//   Designed for direct use by #225 (ComponentDecl.schema_hash) without
//   requiring re-parsing.
//
// --in <dir> --emit=collection-abi-hash:
//   Walks <in>/**/*.fory, computes the engine-wide ABI hash over ALL schemas
//   using the locked recipe from plugin-abi.md §"ABI Hash Function":
//     For each TypeDecl (fqn-sorted): fqn || ":" || version_le(4) || ":" || src_digest(32)
//     Separated by "\n", no trailing newline.
//   Prints a single line:
//     <COLLECTION>\t<global_abi_hash_hex_64>
//   This is the value embedded as glibre_types_abi_hash in _abi_hash.cpp.
//
// Exits non-zero on parse failure, with a diagnostic on stderr.
//
// Out of scope (sibling plans #220–#225):
//   - Full C++ header / source emission
//   - Migration dispatcher emit

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#include <EASTL/optional.h>
#include <EASTL/sort.h>
#include <EASTL/vector.h>

#include "abi_hash.hpp"
#include "parser.hpp"

namespace fs = std::filesystem;
using namespace glibre::tools::foryc;

// -----------------------------------------------------------------------
// Emit mode
// -----------------------------------------------------------------------

enum class EmitMode {
    Default,           // --out + --stamp (original mode)
    AbiHash,           // --emit=abi-hash (per-file source hashes)
    CollectionAbiHash, // --emit=collection-abi-hash (engine-wide hash)
};

// -----------------------------------------------------------------------
// Argument parsing
// -----------------------------------------------------------------------

struct Args {
    fs::path in_dir{};
    fs::path out_dir{};
    fs::path stamp_file{};
    EmitMode emit_mode{EmitMode::Default};
};

static void usage(std::string_view program) {
    std::cerr << "Usage: " << program
              << " --in <dir> --out <dir> --stamp <file>\n"
                 "   or: "
              << program << " --in <dir> --emit=abi-hash\n"
                 "   or: "
              << program << " --in <dir> --emit=collection-abi-hash\n";
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
        } else if (tok == "--emit=abi-hash") {
            args.emit_mode = EmitMode::AbiHash;
        } else if (tok == "--emit=collection-abi-hash") {
            args.emit_mode = EmitMode::CollectionAbiHash;
        } else {
            std::cerr << "foryc: unknown flag: " << tok << "\n";
            return eastl::nullopt;
        }
    }

    // Validate argument combinations.
    if (args.in_dir.empty())
        return eastl::nullopt;

    if (args.emit_mode == EmitMode::Default) {
        // Default mode requires --out and --stamp.
        if (args.out_dir.empty() || args.stamp_file.empty())
            return eastl::nullopt;
    }
    // AbiHash mode requires only --in (already validated above).

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
        default:
            __builtin_unreachable();
        }
    }
    return "unknown error";
}

// -----------------------------------------------------------------------
// Shared helper: collect and sort .fory files under in_dir.
// -----------------------------------------------------------------------

static eastl::optional<eastl::vector<fs::path>>
collect_schema_files(const fs::path& in_dir) {
    if (!fs::exists(in_dir) || !fs::is_directory(in_dir)) {
        std::cerr << "foryc: --in directory does not exist: " << in_dir << "\n";
        return eastl::nullopt;
    }
    eastl::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(in_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".fory")
            files.push_back(entry.path());
    }
    eastl::sort(files.begin(), files.end());
    return files;
}

// -----------------------------------------------------------------------
// abi-hash emit mode: per-TypeDecl rows.
//
// For each TypeDecl in every .fory file, emits one tab-separated line:
//   <schema_path>\t<fqn>\t<source_hash_hex_64>\t<abi_hash_hex_64>
//
// where:
//   source_hash_hex_64 — 64-char lowercase blake3 hex of the raw file
//                        bytes (plugin-abi.md §"ABI Hash Function" pt 1)
//   abi_hash_hex_64    — 64-char lowercase blake3 hex of the single-type
//                        collection ABI hash for this TypeDecl
//
// One row per fqn (not per file) so #225 ComponentDecl.schema_hash can
// be populated without re-parsing.  Multiple `schema <fqn>` blocks in
// a single .fory file yield multiple rows (one per TypeDecl).
// -----------------------------------------------------------------------

static int run_abi_hash(const fs::path& in_dir) {
    auto files_opt = collect_schema_files(in_dir);
    if (!files_opt)
        return EXIT_FAILURE;
    const eastl::vector<fs::path>& schema_files = *files_opt;

    if (schema_files.empty()) {
        std::cout << "foryc: no .fory files found in " << in_dir.native() << "\n";
        return EXIT_SUCCESS;
    }

    for (const auto& path : schema_files) {
        // Read raw source.
        std::ifstream ifs{path};
        if (!ifs) {
            std::cerr << std::format("foryc: cannot open: {}\n", path.native());
            return EXIT_FAILURE;
        }
        std::ostringstream buf;
        buf << ifs.rdbuf();
        const std::string raw_source = buf.str();

        // Compute per-file source-hash (raw bytes, no canonicalization).
        auto src_hash_r = compute_source_hash(raw_source);
        if (!src_hash_r) {
            std::cerr << std::format(
                "foryc: source-hash failed for {}: {}\n", path.native(),
                error_name(src_hash_r.error())
            );
            return EXIT_FAILURE;
        }

        // Parse schema IR to enumerate TypeDecls.
        auto parse_r = parse_file(path);
        if (!parse_r) {
            std::cerr << std::format(
                "foryc: {}: {}\n", path.native(), error_name(parse_r.error())
            );
            return EXIT_FAILURE;
        }

        const eastl::string src_hex = format_as_full_hex(*src_hash_r);

        // Emit one row per TypeDecl: path \t fqn \t source_hash \t abi_hash
        for (const TypeDecl& td : parse_r->types) {
            // Compute single-type collection ABI hash for this TypeDecl.
            // Build a single-entry collection for this one TypeDecl.
            eastl::vector<SchemaWithDigest> single_entry;
            Schema single_schema;
            single_schema.types.push_back(td);
            single_entry.push_back({std::move(single_schema), *src_hash_r});
            auto abi_hash_r = compute_collection_abi_hash(single_entry);
            if (!abi_hash_r) {
                std::cerr << std::format(
                    "foryc: abi-hash failed for {}/{}: {}\n",
                    path.native(),
                    std::string_view(td.fqn.data(), td.fqn.size()),
                    error_name(abi_hash_r.error())
                );
                return EXIT_FAILURE;
            }
            const eastl::string abi_hex = format_as_full_hex(*abi_hash_r);

            std::cout << path.native() << "\t"
                      << std::string_view(td.fqn.data(), td.fqn.size()) << "\t"
                      << std::string_view(src_hex.data(), src_hex.size()) << "\t"
                      << std::string_view(abi_hex.data(), abi_hex.size()) << "\n";
        }
    }

    return EXIT_SUCCESS;
}

// -----------------------------------------------------------------------
// collection-abi-hash emit mode: single engine-wide ABI hash (64-char).
// This is the value embedded as glibre_types_abi_hash in _abi_hash.cpp.
// Recipe: plugin-abi.md §"ABI Hash Function" point 1.
// -----------------------------------------------------------------------

static int run_collection_abi_hash(const fs::path& in_dir) {
    auto files_opt = collect_schema_files(in_dir);
    if (!files_opt)
        return EXIT_FAILURE;
    const eastl::vector<fs::path>& schema_files = *files_opt;

    if (schema_files.empty()) {
        std::cout << "foryc: no .fory files found in " << in_dir.native() << "\n";
        return EXIT_SUCCESS;
    }

    eastl::vector<SchemaWithDigest> schema_entries;
    schema_entries.reserve(schema_files.size());

    for (const auto& path : schema_files) {
        // Read raw source.
        std::ifstream ifs{path};
        if (!ifs) {
            std::cerr << std::format("foryc: cannot open: {}\n", path.native());
            return EXIT_FAILURE;
        }
        std::ostringstream buf;
        buf << ifs.rdbuf();
        const std::string raw_source = buf.str();

        // Compute raw source-hash.
        auto src_hash_r = compute_source_hash(raw_source);
        if (!src_hash_r) {
            std::cerr << std::format(
                "foryc: source-hash failed for {}: {}\n", path.native(),
                error_name(src_hash_r.error())
            );
            return EXIT_FAILURE;
        }

        // Parse schema IR.
        auto parse_r = parse_file(path);
        if (!parse_r) {
            std::cerr << std::format(
                "foryc: {}: {}\n", path.native(), error_name(parse_r.error())
            );
            return EXIT_FAILURE;
        }

        schema_entries.push_back({std::move(*parse_r), *src_hash_r});
    }

    // Compute collection ABI hash over all schemas.
    auto coll_hash_r = compute_collection_abi_hash(schema_entries);
    if (!coll_hash_r) {
        std::cerr << "foryc: collection-abi-hash computation failed\n";
        return EXIT_FAILURE;
    }
    const eastl::string coll_hex = format_as_full_hex(*coll_hash_r);

    // Print: <COLLECTION>\t<64-char hex>
    std::cout << "<COLLECTION>\t"
              << std::string_view(coll_hex.data(), coll_hex.size()) << "\n";

    return EXIT_SUCCESS;
}

// -----------------------------------------------------------------------
// Default (stamp) mode
// -----------------------------------------------------------------------

static int run_default(const Args& args) {
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

    // Parse each file.
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

    if (args.emit_mode == EmitMode::AbiHash)
        return run_abi_hash(args.in_dir);

    if (args.emit_mode == EmitMode::CollectionAbiHash)
        return run_collection_abi_hash(args.in_dir);

    return run_default(args);
}
