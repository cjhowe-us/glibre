// tests/shader/source/shader_source_test.cpp
//
// Catch2 unit tests for glibre::shader::ShaderSource (plan #508).
//
// Authority: specs/shader/SPEC.md §4.1.
//
// Named test cases (plan #508 Unit Test Plan):
//   - shader_source_open_validates_entry_points
//   - shader_source_resolves_includes
//   - shader_source_include_closure_rejects_escape_and_cycle
//   - shader_source_open_returns_SourceNotFound_for_missing_path
//   - shader_source_preprocessed_total_hash_is_byte_stable_across_calls
//   - shader_source_open_returns_EntryPointStageAmbiguous_on_dual_attributes
//   - include_resolver_rejects_absolute_path_with_IncludeEscape
//   - include_resolver_rejects_upward_traversal_with_IncludeEscape
//   - include_resolver_detects_cycle_with_IncludeCycle
//
// Design constraints:
//   - -fno-exceptions compatible (error-model.md §Decision 3).
//   - Fixtures live in tests/shader/source/fixtures/ and are referenced via
//     GLIBRE_SHADER_FIXTURE_DIR, defined by the CMakeLists.txt compile definition.
//   - No REQUIRE_THROWS.

#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <glibre/shader/shader_source.hpp>

// GLIBRE_SHADER_FIXTURE_DIR is defined by CMakeLists as a compile definition
// pointing to the absolute path of tests/shader/source/fixtures/.
#ifndef GLIBRE_SHADER_FIXTURE_DIR
#error "GLIBRE_SHADER_FIXTURE_DIR must be defined by CMakeLists.txt"
#endif

namespace {

const std::filesystem::path kFixtureDir{GLIBRE_SHADER_FIXTURE_DIR};

}  // namespace

// ===========================================================================
// Test: shader_source_open_validates_entry_points
//
// Opens a .slang file with two stage attributes (vertex + pixel) and asserts
// that the returned ShaderSource exposes exactly two EntryPoints.
//
// Fixture: fixtures/two_stage_shader.slang
// ===========================================================================

TEST_CASE("shader_source_open_validates_entry_points", "[shader][shader_source]") {
    // project_root is the fixture directory itself for this test.
    const auto project_root = kFixtureDir;
    const auto project_rel = std::filesystem::path{"two_stage_shader.slang"};

    auto result = glibre::shader::ShaderSource::open(project_root, project_rel);
    REQUIRE(result.has_value());

    const auto& src = *result;
    CHECK(src.id().project_relative_path == eastl::string{"two_stage_shader.slang"});

    auto eps = src.entry_points();
    REQUIRE(eps.size() == 2u);

    // HIGH-1 regression: entry_points() must be in source-encounter order.
    // two_stage_shader.slang declares [shader("vertex")] vs_main first,
    // then [shader("pixel")] ps_main.  The scanner preserves source order.
    CHECK(eps[0].name == eastl::string{"vs_main"});
    CHECK(eps[0].stage == glibre::shader::Stage::Vertex);
    CHECK(eps[1].name == eastl::string{"ps_main"});
    CHECK(eps[1].stage == glibre::shader::Stage::Pixel);
}

// ===========================================================================
// Test: shader_source_resolves_includes
//
// Opens a root file (root_a.slang) that transitively includes two other files
// (middle_b.slang → leaf_c.slang) and asserts that the preprocessed expanded
// source contains content from all three files, and the include_closure
// records at least 2 include nodes.
//
// Fixture: fixtures/root_a.slang, middle_b.slang, leaf_c.slang
// ===========================================================================

TEST_CASE("shader_source_resolves_includes", "[shader][shader_source]") {
    const auto project_root = kFixtureDir;
    const auto project_rel = std::filesystem::path{"root_a.slang"};

    auto result = glibre::shader::ShaderSource::open(project_root, project_rel);
    REQUIRE(result.has_value());

    const auto& src = *result;
    const auto& pp = src.preprocessed();

    // Include closure must record both middle_b.slang and leaf_c.slang.
    CHECK(pp.include_closure.size() >= 2u);

    // Expanded source must contain text from all three files.
    // Convert bytes to a string for substring search.
    const std::string expanded{reinterpret_cast<const char*>(pp.bytes.data()), pp.bytes.size()};

    // Content from root_a.slang (a_pixel).
    CHECK(expanded.find("a_pixel") != std::string::npos);
    // Content from middle_b.slang (b_vertex).
    CHECK(expanded.find("b_vertex") != std::string::npos);
    // Content from leaf_c.slang (cs_main).
    CHECK(expanded.find("cs_main") != std::string::npos);
}

// ===========================================================================
// Test: shader_source_include_closure_rejects_escape_and_cycle
//
// Verifies that the cycle-detection logic rejects A→B→A include cycles
// with Error::IncludeCycle.
//
// Fixture: fixtures/cycle_a.slang, cycle_b.slang
// ===========================================================================

TEST_CASE("shader_source_include_closure_rejects_escape_and_cycle", "[shader][shader_source]") {
    const auto project_root = kFixtureDir;
    const auto project_rel = std::filesystem::path{"cycle_a.slang"};

    auto result = glibre::shader::ShaderSource::open(project_root, project_rel);
    REQUIRE_FALSE(result.has_value());

    // eastl::variant holds glibre::shader::Error.
    const auto& err = result.error();
    const bool is_cycle =
        std::holds_alternative<glibre::shader::Error>(err.code()) &&
        std::get<glibre::shader::Error>(err.code()) == glibre::shader::Error::IncludeCycle;
    CHECK(is_cycle);
}

// ===========================================================================
// Test: shader_source_open_returns_SourceNotFound_for_missing_path
//
// Verifies that opening a non-existent file returns Error::SourceNotFound.
// ===========================================================================

TEST_CASE("shader_source_open_returns_SourceNotFound_for_missing_path", "[shader][shader_source]") {
    const auto project_root = kFixtureDir;
    const auto project_rel = std::filesystem::path{"does_not_exist.slang"};

    auto result = glibre::shader::ShaderSource::open(project_root, project_rel);
    REQUIRE_FALSE(result.has_value());

    const auto& err = result.error();
    const bool is_not_found =
        std::holds_alternative<glibre::shader::Error>(err.code()) &&
        std::get<glibre::shader::Error>(err.code()) == glibre::shader::Error::SourceNotFound;
    CHECK(is_not_found);
}

// ===========================================================================
// Test: shader_source_preprocessed_total_hash_is_byte_stable_across_calls
//
// Opens the same file twice and asserts the total_hash is identical on both
// calls (deterministic hashing — §4.1 invariant 2).
// ===========================================================================

TEST_CASE(
    "shader_source_preprocessed_total_hash_is_byte_stable_across_calls", "[shader][shader_source]"
) {
    const auto project_root = kFixtureDir;
    const auto project_rel = std::filesystem::path{"two_stage_shader.slang"};

    auto r1 = glibre::shader::ShaderSource::open(project_root, project_rel);
    auto r2 = glibre::shader::ShaderSource::open(project_root, project_rel);

    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());

    const auto& h1 = r1->preprocessed().total_hash;
    const auto& h2 = r2->preprocessed().total_hash;

    CHECK(h1 == h2);
}

// ===========================================================================
// Test: shader_source_open_returns_EntryPointStageAmbiguous_on_dual_attributes
//
// Verifies that a function bearing two [shader("...")] attributes produces
// Error::EntryPointStageAmbiguous (§4.1 invariant 1).
//
// Fixture: fixtures/dual_attr.slang
// ===========================================================================

TEST_CASE(
    "shader_source_open_returns_EntryPointStageAmbiguous_on_dual_attributes",
    "[shader][shader_source]"
) {
    const auto project_root = kFixtureDir;
    const auto project_rel = std::filesystem::path{"dual_attr.slang"};

    auto result = glibre::shader::ShaderSource::open(project_root, project_rel);
    REQUIRE_FALSE(result.has_value());

    const auto& err = result.error();
    const bool is_ambiguous = std::holds_alternative<glibre::shader::Error>(err.code()) &&
                              std::get<glibre::shader::Error>(err.code()) ==
                                  glibre::shader::Error::EntryPointStageAmbiguous;
    CHECK(is_ambiguous);
}

// ===========================================================================
// Test: include_resolver_rejects_absolute_path_with_IncludeEscape
//
// Verifies that an #include with an absolute path produces IncludeEscape.
//
// Fixture: fixtures/escape_abs.slang
// ===========================================================================

TEST_CASE(
    "include_resolver_rejects_absolute_path_with_IncludeEscape", "[shader][include_resolver]"
) {
    const auto project_root = kFixtureDir;
    const auto project_rel = std::filesystem::path{"escape_abs.slang"};

    auto result = glibre::shader::ShaderSource::open(project_root, project_rel);
    REQUIRE_FALSE(result.has_value());

    const auto& err = result.error();
    const bool is_escape =
        std::holds_alternative<glibre::shader::Error>(err.code()) &&
        std::get<glibre::shader::Error>(err.code()) == glibre::shader::Error::IncludeEscape;
    CHECK(is_escape);
}

// ===========================================================================
// Test: include_resolver_rejects_upward_traversal_with_IncludeEscape
//
// Verifies that an #include with a ../ traversal produces IncludeEscape.
//
// Fixture: fixtures/escape_dotdot.slang
// ===========================================================================

TEST_CASE(
    "include_resolver_rejects_upward_traversal_with_IncludeEscape", "[shader][include_resolver]"
) {
    const auto project_root = kFixtureDir;
    const auto project_rel = std::filesystem::path{"escape_dotdot.slang"};

    auto result = glibre::shader::ShaderSource::open(project_root, project_rel);
    REQUIRE_FALSE(result.has_value());

    const auto& err = result.error();
    const bool is_escape =
        std::holds_alternative<glibre::shader::Error>(err.code()) &&
        std::get<glibre::shader::Error>(err.code()) == glibre::shader::Error::IncludeEscape;
    CHECK(is_escape);
}

// ===========================================================================
// Test: include_resolver_detects_cycle_with_IncludeCycle
//
// Alias for shader_source_include_closure_rejects_escape_and_cycle — ensures
// the DoD test name is also explicitly present.
//
// Fixture: fixtures/cycle_a.slang, cycle_b.slang
// ===========================================================================

TEST_CASE("include_resolver_detects_cycle_with_IncludeCycle", "[shader][include_resolver]") {
    const auto project_root = kFixtureDir;
    const auto project_rel = std::filesystem::path{"cycle_a.slang"};

    auto result = glibre::shader::ShaderSource::open(project_root, project_rel);
    REQUIRE_FALSE(result.has_value());

    const auto& err = result.error();
    const bool is_cycle =
        std::holds_alternative<glibre::shader::Error>(err.code()) &&
        std::get<glibre::shader::Error>(err.code()) == glibre::shader::Error::IncludeCycle;
    CHECK(is_cycle);
}

// ===========================================================================
// Test: shader_source_open_returns_EncodingInvalid_for_non_utf8_content
//
// HIGH-4: read_and_normalize_file must reject files with non-UTF-8 bytes.
//
// Fixture: fixtures/binary_content.slang (contains \x80\xFF invalid bytes).
// ===========================================================================

TEST_CASE(
    "shader_source_open_returns_EncodingInvalid_for_non_utf8_content", "[shader][shader_source]"
) {
    const auto project_root = kFixtureDir;
    const auto project_rel = std::filesystem::path{"binary_content.slang"};

    auto result = glibre::shader::ShaderSource::open(project_root, project_rel);
    REQUIRE_FALSE(result.has_value());

    const auto& err = result.error();
    const bool is_encoding_invalid =
        std::holds_alternative<glibre::shader::Error>(err.code()) &&
        std::get<glibre::shader::Error>(err.code()) == glibre::shader::Error::EncodingInvalid;
    CHECK(is_encoding_invalid);
}

// ===========================================================================
// Test: shader_source_open_returns_EncodingInvalid_for_empty_file
//
// HIGH-4: read_and_normalize_file must reject empty files.
//
// Fixture: fixtures/empty_file.slang (zero bytes).
// ===========================================================================

TEST_CASE("shader_source_open_returns_EncodingInvalid_for_empty_file", "[shader][shader_source]") {
    const auto project_root = kFixtureDir;
    const auto project_rel = std::filesystem::path{"empty_file.slang"};

    auto result = glibre::shader::ShaderSource::open(project_root, project_rel);
    REQUIRE_FALSE(result.has_value());

    const auto& err = result.error();
    const bool is_encoding_invalid =
        std::holds_alternative<glibre::shader::Error>(err.code()) &&
        std::get<glibre::shader::Error>(err.code()) == glibre::shader::Error::EncodingInvalid;
    CHECK(is_encoding_invalid);
}

// ===========================================================================
// Test: shader_source_cycle_detection_is_case_insensitive
//
// MED-2 regression: a file that includes itself with a different-case path
// must be detected as a cycle on macOS case-insensitive filesystems.
//
// The fixture cycle_self_caseinsensitive.slang includes
// "CYCLE_SELF_CASEINSENSITIVE.slang" (uppercase) which is the same physical
// file.  The preprocessor ASCII-lowercases paths before comparing against the
// visit stack, so this produces IncludeCycle rather than looping forever.
//
// Fixture: fixtures/cycle_self_caseinsensitive.slang
// ===========================================================================

TEST_CASE("shader_source_cycle_detection_is_case_insensitive", "[shader][include_resolver]") {
    const auto project_root = kFixtureDir;
    const auto project_rel = std::filesystem::path{"cycle_self_caseinsensitive.slang"};

    auto result = glibre::shader::ShaderSource::open(project_root, project_rel);
    REQUIRE_FALSE(result.has_value());

    const auto& err = result.error();
    const bool is_cycle =
        std::holds_alternative<glibre::shader::Error>(err.code()) &&
        std::get<glibre::shader::Error>(err.code()) == glibre::shader::Error::IncludeCycle;
    CHECK(is_cycle);
}

// ===========================================================================
// Test: shader_source_same_stage_dup_is_collapsed_not_ambiguous
//
// R2 MED-3: a function with the same name AND the same stage appearing twice
// (e.g. via double-inclusion) must NOT produce EntryPointStageAmbiguous.
// Instead the duplicate is silently collapsed and exactly one EntryPoint is
// emitted for the function.
//
// Fixture: fixtures/same_stage_dup.slang
//   Contains [shader("vertex")] void vs_main() {} declared twice.
// ===========================================================================

TEST_CASE("shader_source_same_stage_dup_is_collapsed_not_ambiguous", "[shader][shader_source]") {
    const auto project_root = kFixtureDir;
    const auto project_rel = std::filesystem::path{"same_stage_dup.slang"};

    auto result = glibre::shader::ShaderSource::open(project_root, project_rel);
    REQUIRE(result.has_value());

    const auto& src = *result;
    auto eps = src.entry_points();

    // Must collapse to exactly one entry point, not error.
    REQUIRE(eps.size() == 1u);
    CHECK(eps[0].name == eastl::string{"vs_main"});
    CHECK(eps[0].stage == glibre::shader::Stage::Vertex);
}

// ===========================================================================
// Test: shader_source_open_returns_EncodingInvalid_for_utf8_isolated_continuation
//
// R2 LOW-6: a file with an isolated UTF-8 continuation byte (0x80, not
// preceded by a valid multi-byte lead) must produce EncodingInvalid.
//
// Fixture: fixtures/utf8_isolated_continuation.slang
//   Bytes: "// ok\n" + 0x41 0x80 0x42
// ===========================================================================

TEST_CASE(
    "shader_source_open_returns_EncodingInvalid_for_utf8_isolated_continuation",
    "[shader][shader_source]"
) {
    const auto project_root = kFixtureDir;
    const auto project_rel = std::filesystem::path{"utf8_isolated_continuation.slang"};

    auto result = glibre::shader::ShaderSource::open(project_root, project_rel);
    REQUIRE_FALSE(result.has_value());

    const auto& err = result.error();
    const bool is_encoding_invalid =
        std::holds_alternative<glibre::shader::Error>(err.code()) &&
        std::get<glibre::shader::Error>(err.code()) == glibre::shader::Error::EncodingInvalid;
    CHECK(is_encoding_invalid);
}

// ===========================================================================
// Test: shader_source_open_returns_EncodingInvalid_for_utf8_overlong_nul
//
// R2 LOW-6: a file with an overlong NUL encoding (0xC0 0x80, forbidden by
// RFC 3629 §3) must produce EncodingInvalid.
//
// Fixture: fixtures/utf8_overlong_nul.slang
//   Bytes: "// ok\n" + 0xC0 0x80
// ===========================================================================

TEST_CASE(
    "shader_source_open_returns_EncodingInvalid_for_utf8_overlong_nul", "[shader][shader_source]"
) {
    const auto project_root = kFixtureDir;
    const auto project_rel = std::filesystem::path{"utf8_overlong_nul.slang"};

    auto result = glibre::shader::ShaderSource::open(project_root, project_rel);
    REQUIRE_FALSE(result.has_value());

    const auto& err = result.error();
    const bool is_encoding_invalid =
        std::holds_alternative<glibre::shader::Error>(err.code()) &&
        std::get<glibre::shader::Error>(err.code()) == glibre::shader::Error::EncodingInvalid;
    CHECK(is_encoding_invalid);
}
