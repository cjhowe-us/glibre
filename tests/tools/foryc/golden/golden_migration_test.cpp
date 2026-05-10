// SPDX-License-Identifier: Apache-2.0
// tests/tools/foryc/golden/golden_migration_test.cpp
//
// Golden-output test harness for glibre-foryc migration dispatcher emission
// (plan #227).
//
// Each fixture in golden/fixtures_migration/ is parsed with parse_file() and
// emitted via emit_migration() with source_path set to the fixture basename.
// The resulting text is compared byte-for-byte against the checked-in golden
// in golden/expected_migration/<basename>.migrations.cpp.golden.
//
// Regenerating goldens:
//   Set GLIBRE_UPDATE_GOLDEN=1 in the environment before running ctest:
//     GLIBRE_UPDATE_GOLDEN=1 ctest --preset macos-debug -R foryc_migration_golden_
//   This overwrites golden/expected_migration/*.migrations.cpp.golden with the
//   actual output.  Review the diff before committing.
//
// Dependency:
//   emit_migration (plan #221 / PR #975) must be merged before this binary
//   compiles.  The CMakeLists.txt guarding this target uses if(EXISTS ...) to
//   skip the add_executable() call when emit_migration.{hpp,cpp} are absent
//   from the source tree.  This test will NOT appear in ctest output until
//   #221 lands.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>


#include <catch2/catch_test_macros.hpp>

#include "emit_migration.hpp"
#include "parser.hpp"

namespace fs = std::filesystem;
namespace foryc = glibre::tools::foryc;

// ---------------------------------------------------------------------------
// Helpers — mirror golden_runner_test.cpp helpers for portability.
// Both test files compile independently; sharing via a static library is
// deferred (no second user yet — PHILOSOPHY §2).
// ---------------------------------------------------------------------------

static std::string slurp(const fs::path& p) {
    std::error_code ec;
    const auto sz = fs::file_size(p, ec);
    if (ec || sz == 0)
        return {};

    std::string buf;
    buf.resize(static_cast<std::string::size_type>(sz));

    std::ifstream ifs{p, std::ios::binary};
    if (!ifs)
        return {};

    ifs.read(buf.data(), static_cast<std::streamsize>(sz));
    if (!ifs)
        return {};

    return buf;
}

static bool splat(const fs::path& p, const std::string& content) {
    std::ofstream ofs{p, std::ios::binary | std::ios::trunc};
    if (!ofs)
        return false;
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    return ofs.good();
}

// GLIBRE_GOLDEN_DIR is the absolute path to tests/tools/foryc/golden/
// injected by CMakeLists.txt so the binary can locate fixture/ and expected/
// subdirectories regardless of CWD.
#ifndef GLIBRE_GOLDEN_DIR
#error "GLIBRE_GOLDEN_DIR must be defined by CMakeLists.txt"
#endif

static fs::path golden_dir() { return fs::path{GLIBRE_GOLDEN_DIR}; }

static std::string simple_diff(const std::string& actual, const std::string& expected) {
    std::string msg;

    const std::size_t n = std::min(actual.size(), expected.size());
    std::size_t first_diff = std::string::npos;
    for (std::size_t i = 0; i < n; ++i) {
        if (actual[i] != expected[i]) {
            first_diff = i;
            break;
        }
    }
    if (first_diff == std::string::npos && actual.size() != expected.size())
        first_diff = n;

    if (first_diff == std::string::npos)
        return std::string("(no difference found — sizes match)\n");

    std::size_t line = 1;
    for (std::size_t i = 0; i < first_diff && i < actual.size(); ++i) {
        if (actual[i] == '\n')
            ++line;
    }

    // std::format retained per PHILOSOPHY §11 (not an EASTL-owned utility).
    msg += std::string(
        std::format("First difference at byte offset {} (approx line {})\n", first_diff, line)
            .c_str()
    );

    const std::size_t ctx_start = (first_diff > 80) ? first_diff - 80 : 0;
    const std::size_t ctx_end_a = std::min(first_diff + 80, actual.size());
    const std::size_t ctx_end_e = std::min(first_diff + 80, expected.size());

    msg += "actual:   [";
    msg += actual.substr(ctx_start, ctx_end_a - ctx_start);
    msg += "]\n";
    msg += "expected: [";
    msg += expected.substr(ctx_start, ctx_end_e - ctx_start);
    msg += "]\n";

    msg += std::string(
        std::format("actual size:   {}\nexpected size: {}\n", actual.size(), expected.size())
            .c_str()
    );

    return msg;
}

// ---------------------------------------------------------------------------
// foryc_migration_golden_v1_to_v2_matches_expected
//
// Parses fixtures_migration/v1_to_v2.fory, calls emit_migration() with a
// stable (basename-only) virtual source path, then compares the output
// byte-for-byte against expected_migration/v1_to_v2.migrations.cpp.golden.
//
// Regeneration:
//   GLIBRE_UPDATE_GOLDEN=1 ctest --preset macos-debug \
//     -R foryc_migration_golden_v1_to_v2_matches_expected
//
// This test is the canonical check that the migration dispatcher TU shape
// does not drift silently.  Cited as a DoD assertion on issue #227.
// ---------------------------------------------------------------------------

TEST_CASE("foryc_migration_golden_v1_to_v2_matches_expected", "[foryc][golden][migration]") {
    const fs::path fixture_path = golden_dir() / "fixtures_migration" / "v1_to_v2.fory";
    const fs::path golden_path =
        golden_dir() / "expected_migration" / "v1_to_v2.migrations.cpp.golden";

    INFO("fixture: " << fixture_path.native());
    INFO("golden:  " << golden_path.native());

    // Step 1: parse the fixture.
    auto parse_result = foryc::parse_file(fixture_path);
    INFO("parse_file failed for: " << fixture_path.native());
    CHECK(parse_result.has_value());
    if (!parse_result.has_value())
        return;

    // Step 2: emit migration dispatcher with a stable (basename-only) virtual
    // source path so goldens are portable across machines.
    foryc::Schema& schema = *parse_result;
    auto emit_result = foryc::emit_migration(schema, "v1_to_v2.fory");
    INFO("emit_migration failed for v1_to_v2.fory");
    CHECK(emit_result.has_value());
    if (!emit_result.has_value())
        return;

    const std::string& actual = *emit_result;

    // Step 3: UPDATE_GOLDEN mode — overwrite the golden and return.
    const char* update_env = std::getenv("GLIBRE_UPDATE_GOLDEN");
    if (update_env && std::string_view{update_env} == "1") {
        INFO("Updating golden: " << golden_path.native());
        CHECK(splat(golden_path, actual));
        return;
    }

    // Step 4: read existing golden.
    const std::string expected = slurp(golden_path);
    {
        INFO(
            "golden file missing or empty: " << golden_path.native()
                                             << "\nRun with GLIBRE_UPDATE_GOLDEN=1 to generate it."
        );
        CHECK(!expected.empty());
        if (expected.empty())
            return;
    }

    // Step 5: byte-equality check with diagnostic diff on failure.
    if (actual != expected) {
        const std::string diff = simple_diff(actual, expected);
        INFO(
            "Golden mismatch for 'v1_to_v2':\n"
            << diff.c_str() << "\nRun with GLIBRE_UPDATE_GOLDEN=1 to regenerate the golden."
        );
        CHECK(actual == expected);
    }
}
