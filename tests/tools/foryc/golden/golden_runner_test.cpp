// SPDX-License-Identifier: Apache-2.0
// tests/tools/foryc/golden/golden_runner_test.cpp
//
// Golden-output test harness for glibre-foryc (plan #226).
//
// Each fixture in golden/fixtures/ is parsed with parse_file() and emitted
// via emit_header() (with source_path set to the fixture basename).  The
// resulting text is compared byte-for-byte against the checked-in golden in
// golden/expected/<basename>.hpp.golden.
//
// Regenerating goldens:
//   Set GLIBRE_UPDATE_GOLDEN=1 in the environment before running ctest:
//     GLIBRE_UPDATE_GOLDEN=1 ctest --preset macos-debug -R foryc_golden_
//   This overwrites golden/expected/*.hpp.golden with the actual output.
//   Review the diff before committing.
//
// Adding a new fixture:
//   1. Add <name>.fory to golden/fixtures/.
//   2. Add the fixture basename to the GENERATE() list in
//      foryc_golden_emit_header_matches_expected below.
//   3. Run with GLIBRE_UPDATE_GOLDEN=1 to generate the initial golden.
//   4. Review the generated golden and commit both files.

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <EASTL/string.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "emit_header.hpp"
#include "parser.hpp"

namespace fs = std::filesystem;
namespace foryc = glibre::tools::foryc;

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

// Read a file into an eastl::string.  Returns an empty string on failure.
// Uses std::filesystem::file_size for the pre-sized buffer read (PHILOSOPHY
// §11 permits std::filesystem; std::ifstream::read is a language I/O
// primitive, not a container — plain iostreams streaming is avoided).
static eastl::string slurp(const fs::path& p) {
    std::error_code ec;
    const auto sz = fs::file_size(p, ec);
    if (ec || sz == 0)
        return {};

    eastl::string buf;
    buf.resize(static_cast<eastl::string::size_type>(sz));

    std::ifstream ifs{p, std::ios::binary};
    if (!ifs)
        return {};

    ifs.read(buf.data(), static_cast<std::streamsize>(sz));
    if (!ifs)
        return {};

    return buf;
}

// Write an eastl::string to a file.  Returns true on success.
static bool splat(const fs::path& p, const eastl::string& content) {
    std::ofstream ofs{p, std::ios::binary | std::ios::trunc};
    if (!ofs)
        return false;
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    return ofs.good();
}

// GLIBRE_GOLDEN_DIR is the absolute path to tests/tools/foryc/golden/
// injected by CMakeLists.txt so the binary can locate fixtures/ and expected/.
#ifndef GLIBRE_GOLDEN_DIR
#error "GLIBRE_GOLDEN_DIR must be defined by CMakeLists.txt"
#endif

static fs::path golden_dir() {
    return fs::path{GLIBRE_GOLDEN_DIR};
}

// Produce a simple diagnostic string describing where `actual` and `expected`
// differ.  Not a true unified diff — used only in FAIL() messages.
static eastl::string simple_diff(const eastl::string& actual, const eastl::string& expected) {
    eastl::string msg;

    // Find the first differing character position.
    const std::size_t n = std::min(actual.size(), expected.size());
    std::size_t first_diff = eastl::string::npos;
    for (std::size_t i = 0; i < n; ++i) {
        if (actual[i] != expected[i]) {
            first_diff = i;
            break;
        }
    }
    if (first_diff == eastl::string::npos && actual.size() != expected.size())
        first_diff = n;

    if (first_diff == eastl::string::npos) {
        return eastl::string("(no difference found — sizes match)\n");
    }

    // Find the approximate line number of the first difference.
    std::size_t line = 1;
    for (std::size_t i = 0; i < first_diff && i < actual.size(); ++i) {
        if (actual[i] == '\n')
            ++line;
    }

    // Build diagnostic using std::format is acceptable (PHILOSOPHY §11
    // retains std::format), but simple concatenation avoids the include cost.
    msg += "First difference at byte offset ";
    msg += eastl::string(std::to_string(first_diff).c_str());
    msg += " (approx line ";
    msg += eastl::string(std::to_string(line).c_str());
    msg += ")\n";

    // Show a context window around the difference.
    const std::size_t ctx_start = (first_diff > 80) ? first_diff - 80 : 0;
    const std::size_t ctx_end_a = std::min(first_diff + 80, actual.size());
    const std::size_t ctx_end_e = std::min(first_diff + 80, expected.size());

    msg += "actual:   [";
    msg += actual.substr(ctx_start, ctx_end_a - ctx_start);
    msg += "]\n";
    msg += "expected: [";
    msg += expected.substr(ctx_start, ctx_end_e - ctx_start);
    msg += "]\n";

    msg += "actual size:   ";
    msg += eastl::string(std::to_string(actual.size()).c_str());
    msg += "\n";
    msg += "expected size: ";
    msg += eastl::string(std::to_string(expected.size()).c_str());
    msg += "\n";

    return msg;
}

// -----------------------------------------------------------------------
// Primary golden test
//
// Parameterised over four fixture basenames via GENERATE().  For each:
//   1. Parse golden/fixtures/<basename>.fory.
//   2. Mutate schema.source_path to <basename>.fory (stable virtual path,
//      no machine-specific prefix) and call emit_header(schema).
//   3. Read golden/expected/<basename>.hpp.golden.
//   4. If GLIBRE_UPDATE_GOLDEN=1, overwrite the golden and pass.
//   5. Otherwise assert byte-equality and emit a diagnostic diff on failure.
// -----------------------------------------------------------------------

TEST_CASE("foryc_golden_emit_header_matches_expected", "[foryc][golden]") {
    const eastl::string basename{GENERATE(
        eastl::string{"minimal_struct"},
        eastl::string{"nested_namespace"},
        eastl::string{"all_scalars"},
        eastl::string{"with_generics"}
    )};

    // Build std::filesystem paths from the eastl::string basename.
    const std::string basename_std{basename.c_str(), basename.size()};
    const fs::path fixture_path = golden_dir() / "fixtures" / (basename_std + ".fory");
    const fs::path golden_path  = golden_dir() / "expected"  / (basename_std + ".hpp.golden");

    INFO("fixture: " << fixture_path.native());
    INFO("golden:  " << golden_path.native());

    // Step 1: parse the fixture.
    // Use CHECK rather than REQUIRE so -fno-exceptions builds do not abort on
    // a parse failure; the early return prevents dereferencing a bad Result.
    auto parse_result = foryc::parse_file(fixture_path);
    INFO("parse_file failed for: " << fixture_path.native());
    CHECK(parse_result.has_value());
    if (!parse_result.has_value())
        return;

    // Step 2: emit header with a stable (basename-only) virtual source path
    // so goldens are portable across machines.  Mutate schema.source_path
    // instead of duplicating emit_header's logic in a wrapper.
    foryc::Schema& schema = *parse_result;
    schema.source_path = basename + eastl::string(".fory");
    auto emit_result = foryc::emit_header(schema);
    INFO("emit_header failed for: " << basename.c_str() << ".fory");
    CHECK(emit_result.has_value());
    if (!emit_result.has_value())
        return;

    const eastl::string& actual = *emit_result;

    // Step 3: UPDATE_GOLDEN mode — overwrite the golden and return.
    const char* update_env = std::getenv("GLIBRE_UPDATE_GOLDEN");
    if (update_env && std::string_view{update_env} == "1") {
        INFO("Updating golden: " << golden_path.native());
        CHECK(splat(golden_path, actual));
        return;
    }

    // Step 4: read existing golden.
    const eastl::string expected = slurp(golden_path);
    {
        INFO("golden file missing or empty: " << golden_path.native()
             << "\nRun with GLIBRE_UPDATE_GOLDEN=1 to generate it.");
        CHECK(!expected.empty());
        if (expected.empty())
            return;
    }

    // Step 5: byte-equality check with a diagnostic diff on failure.
    // Use INFO() + CHECK() rather than REQUIRE() so that -fno-exceptions builds
    // (which abort on TestFailureException) continue running remaining GENERATE()
    // iterations after a mismatch.
    if (actual != expected) {
        const eastl::string diff = simple_diff(actual, expected);
        INFO("Golden mismatch for '" << basename.c_str() << "':\n" << diff.c_str()
             << "\nRun with GLIBRE_UPDATE_GOLDEN=1 to regenerate the golden.");
        CHECK(actual == expected);
    }
}
