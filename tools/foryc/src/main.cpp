// SPDX-License-Identifier: Apache-2.0
// tools/foryc/src/main.cpp
//
// glibre-foryc — .fory schema compiler host tool (plan #219 skeleton,
//                plan #222 adds --emit=abi-hash).
//
// Usage:
//   glibre-foryc --in <dir> --out <dir> --stamp <file>
//   glibre-foryc --in <dir> --emit=abi-hash
//
// --in <dir> --out <dir> --stamp <file>:
//   Walks <in>/**/*.fory, parses each file into the schema IR, validates
//   basic shape (unique tags, version >= 1, builtins-only types), emits a
//   one-line summary to stdout per file, touches <stamp> on success.
//
// --in <dir> --emit=abi-hash:
//   Walks <in>/**/*.fory, computes the per-file source-hash and the
//   per-schema collection ABI hash, prints one tab-separated line per
//   schema file:
//     <schema_path>\t<source_hash_hex>\t<abi_hash_hex>
//   (hex is 16-char lowercase representing the first 8 bytes of the 32-byte
//   blake3 digest, suitable for embedding as uint64_t literals.)
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
    Default,  // --out + --stamp (original mode)
    AbiHash,  // --emit=abi-hash
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
              << program << " --in <dir> --emit=abi-hash\n";
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
// abi-hash emit mode
// -----------------------------------------------------------------------

static int run_abi_hash(const fs::path& in_dir) {
    eastl::vector<fs::path> schema_files;
    if (!fs::exists(in_dir) || !fs::is_directory(in_dir)) {
        std::cerr << "foryc: --in directory does not exist: " << in_dir << "\n";
        return EXIT_FAILURE;
    }

    for (const auto& entry : fs::recursive_directory_iterator(in_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".fory")
            schema_files.push_back(entry.path());
    }

    // Sort for deterministic output order.
    eastl::sort(schema_files.begin(), schema_files.end());

    for (const auto& path : schema_files) {
        // Read raw source for source-hash.
        std::ifstream ifs{path};
        if (!ifs) {
            std::cerr << std::format("foryc: cannot open: {}\n", path.native());
            return EXIT_FAILURE;
        }
        std::ostringstream buf;
        buf << ifs.rdbuf();
        const std::string raw_source = buf.str();

        // Compute source-hash.
        auto src_hash_r = compute_source_hash(raw_source);
        if (!src_hash_r) {
            std::cerr << std::format(
                "foryc: source-hash failed for {}: {}\n", path.native(),
                error_name(src_hash_r.error())
            );
            return EXIT_FAILURE;
        }
        const eastl::string src_hex = format_as_uint64_hex(*src_hash_r);

        // Parse schema IR.
        auto parse_r = parse_file(path);
        if (!parse_r) {
            std::cerr << std::format(
                "foryc: {}: {}\n", path.native(), error_name(parse_r.error())
            );
            return EXIT_FAILURE;
        }

        // Compute ABI hash.
        auto abi_hash_r = compute_abi_hash(*parse_r);
        if (!abi_hash_r) {
            std::cerr << std::format(
                "foryc: abi-hash failed for {}: {}\n", path.native(),
                error_name(abi_hash_r.error())
            );
            return EXIT_FAILURE;
        }
        const eastl::string abi_hex = format_as_uint64_hex(*abi_hash_r);

        // Print tab-separated: path, source_hash, abi_hash.
        std::cout << path.native() << "\t"
                  << std::string_view(src_hex.data(), src_hex.size()) << "\t"
                  << std::string_view(abi_hex.data(), abi_hex.size()) << "\n";
    }

    if (schema_files.empty()) {
        std::cout << "foryc: no .fory files found in " << in_dir.native() << "\n";
    }

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

    return run_default(args);
}
