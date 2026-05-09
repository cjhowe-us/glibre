// SPDX-License-Identifier: Apache-2.0
// tools/foryc/src/main.cpp
//
// glibre-foryc — .fory schema compiler host tool (plan #219 skeleton).
//
// Usage:
//   glibre-foryc --in <dir> --out <dir> --stamp <file>
//
// Walks <in>/**/*.fory, parses each file into the schema IR, validates
// basic shape (unique tags, version >= 1, builtins-only types), emits a
// one-line summary to stdout per file, touches <stamp> on success.
//
// Exits non-zero on parse failure, with a diagnostic on stderr.
//
// Out of scope (sibling plans #220–#225):
//   - C++ header / source emission
//   - Migration dispatcher emit
//   - ABI hash export

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "parser.hpp"

namespace fs = std::filesystem;
using namespace glibre::tools::foryc;

// -----------------------------------------------------------------------
// Argument parsing
// -----------------------------------------------------------------------

struct Args {
    fs::path in_dir{};
    fs::path out_dir{};
    fs::path stamp_file{};
};

static void usage(std::string_view program) {
    std::cerr << "Usage: " << program << " --in <dir> --out <dir> --stamp <file>\n";
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
        } else {
            std::cerr << "foryc: unknown flag: " << tok << "\n";
            return std::nullopt;
        }
    }
    if (args.in_dir.empty() || args.out_dir.empty() || args.stamp_file.empty())
        return std::nullopt;
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
        }
    }
    return "unknown error";
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
    std::vector<fs::path> schema_files;
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
