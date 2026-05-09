// SPDX-License-Identifier: Apache-2.0
// tests/tools/foryc/golden/golden_runner_test.cpp
//
// Golden-output test harness for glibre-foryc (plan #226).
//
// Each fixture in golden/fixtures/ is parsed with parse_file() and emitted
// via emit_header_for_type() (or emit_header() for multi-type files).  The
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
#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "emit_header.hpp"
#include "parser.hpp"

namespace fs = std::filesystem;
namespace foryc = glibre::tools::foryc;

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

// Read a file to string.  Returns empty string on failure.
static std::string slurp(const fs::path& p) {
    std::ifstream ifs{p, std::ios::binary};
    if (!ifs)
        return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

// Write a string to a file.  Returns true on success.
static bool splat(const fs::path& p, std::string_view content) {
    std::ofstream ofs{p, std::ios::binary | std::ios::trunc};
    if (!ofs)
        return false;
    ofs << content;
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

// Emit all types in a schema into a single string, using only the fixture
// basename as the virtual source path (so goldens are portable across machines).
// Matches the concatenation logic of emit_header(schema) but with a controlled
// virtual path that does not embed the machine-specific absolute fixture path.
static glibre::Result<eastl::string>
emit_with_stable_path(const foryc::Schema& schema, std::string_view virtual_path) noexcept {
    if (schema.types.empty())
        return std::unexpected{glibre::Error{glibre::tools::Error::ForycEmptySchema}};

    eastl::string out;
    for (const auto& td : schema.types) {
        auto result = foryc::emit_header_for_type(td, virtual_path);
        if (!result)
            return std::unexpected{result.error()};
        out += *result;
        out += "\n";
    }
    return out;
}

// Produce a simple diagnostic string describing where `actual` and `expected`
// differ.  Not a true unified diff — used only in FAIL() messages.
static std::string simple_diff(std::string_view actual, std::string_view expected) {
    std::string msg;

    // Find the first differing character position.
    const std::size_t n = std::min(actual.size(), expected.size());
    std::size_t first_diff = std::string_view::npos;
    for (std::size_t i = 0; i < n; ++i) {
        if (actual[i] != expected[i]) {
            first_diff = i;
            break;
        }
    }
    if (first_diff == std::string_view::npos && actual.size() != expected.size())
        first_diff = n;

    if (first_diff == std::string_view::npos) {
        return "(no difference found — sizes match)\n";
    }

    // Find the approximate line number of the first difference.
    std::size_t line = 1;
    for (std::size_t i = 0; i < first_diff && i < actual.size(); ++i) {
        if (actual[i] == '\n')
            ++line;
    }

    msg += "First difference at byte offset ";
    msg += std::to_string(first_diff);
    msg += " (approx line ";
    msg += std::to_string(line);
    msg += ")\n";

    // Show a context window around the difference.
    const std::size_t ctx_start = (first_diff > 80) ? first_diff - 80 : 0;
    const std::size_t ctx_end_a = std::min(first_diff + 80, actual.size());
    const std::size_t ctx_end_e = std::min(first_diff + 80, expected.size());

    msg += "actual:   [";
    msg += std::string(actual.substr(ctx_start, ctx_end_a - ctx_start));
    msg += "]\n";
    msg += "expected: [";
    msg += std::string(expected.substr(ctx_start, ctx_end_e - ctx_start));
    msg += "]\n";

    msg += "actual size:   " + std::to_string(actual.size()) + "\n";
    msg += "expected size: " + std::to_string(expected.size()) + "\n";

    return msg;
}

// -----------------------------------------------------------------------
// Primary golden test
//
// Parameterised over four fixture basenames via GENERATE().  For each:
//   1. Parse golden/fixtures/<basename>.fory.
//   2. Emit header via emit_with_stable_path(<basename>.fory as virtual path).
//   3. Read golden/expected/<basename>.hpp.golden.
//   4. If GLIBRE_UPDATE_GOLDEN=1, overwrite the golden and pass.
//   5. Otherwise assert byte-equality and emit a diagnostic diff on failure.
// -----------------------------------------------------------------------

TEST_CASE("foryc_golden_emit_header_matches_expected", "[foryc][golden]") {
    const std::string basename{GENERATE(
        std::string{"minimal_struct"},
        std::string{"nested_namespace"},
        std::string{"all_scalars"},
        std::string{"with_generics"}
    )};

    const fs::path fixture_path = golden_dir() / "fixtures" / (basename + ".fory");
    const fs::path golden_path = golden_dir() / "expected" / (basename + ".hpp.golden");

    INFO("fixture: " << fixture_path.native());
    INFO("golden:  " << golden_path.native());

    // Step 1: parse the fixture.
    auto parse_result = foryc::parse_file(fixture_path);
    INFO("parse_file failed for: " << fixture_path.native());
    REQUIRE(parse_result.has_value());

    // Step 2: emit header with a stable (basename-only) virtual path.
    const std::string virtual_path = basename + ".fory";
    auto emit_result = emit_with_stable_path(*parse_result, virtual_path);
    INFO("emit_header_for_type failed for: " << virtual_path);
    REQUIRE(emit_result.has_value());

    // Convert eastl::string to std::string for comparison and I/O.
    const std::string actual(emit_result->c_str(), emit_result->size());

    // Step 3: UPDATE_GOLDEN mode — overwrite the golden and return.
    const char* update_env = std::getenv("GLIBRE_UPDATE_GOLDEN");
    if (update_env && std::string_view{update_env} == "1") {
        INFO("Updating golden: " << golden_path.native());
        REQUIRE(splat(golden_path, actual));
        return;
    }

    // Step 4: read existing golden.
    const std::string expected = slurp(golden_path);
    {
        INFO("golden file missing or empty: " << golden_path.native()
             << "\nRun with GLIBRE_UPDATE_GOLDEN=1 to generate it.");
        REQUIRE(!expected.empty());
    }

    // Step 5: byte-equality check with a diagnostic diff on failure.
    // Use INFO() + CHECK() rather than FAIL() so that -fno-exceptions builds
    // (which abort on TestFailureException) continue running remaining GENERATE()
    // iterations after a mismatch.
    if (actual != expected) {
        const std::string diff = simple_diff(actual, expected);
        INFO("Golden mismatch for '" << basename << "':\n" << diff
             << "\nRun with GLIBRE_UPDATE_GOLDEN=1 to regenerate the golden.");
        CHECK(actual == expected);
    }
}
