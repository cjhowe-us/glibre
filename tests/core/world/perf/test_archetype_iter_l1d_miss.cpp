// tests/core/world/perf/test_archetype_iter_l1d_miss.cpp
//
// Catch2 test: PMC L1-D miss-rate alarm on archetype iteration hot loop.
//
// Authority: plan #938, specs/core/world-design.md §5.3 + §9.5.
//
// Named test cases (plan #938 Unit Test Plan / DoD):
//   - world/perf: archetype_iter_l1d_miss_under_5pct
//
// Design:
//   Uses PmcSampler::measure() (plan #944) to bracket a synthetic archetype
//   iteration hot loop modelled on the S1 fixture (1 char + 200 props + 8
//   lights = 209 entities across 3 archetypes, loaded via load_s1()).
//
//   The hot loop simulates the §9.5 chunk-walk + column-read pattern:
//   for each archetype, iterate packed arrays of 64-byte-aligned rows.
//   This is the workload whose L1-D miss rate the §5.3 alarm guards.
//
// Zero-stub policy (plan #938 §Note on PMC zero-stub):
//   PmcSampler currently returns all-zero counters (KPC integration deferred).
//   When loads_retired == 0 the ratio 0/0 is undefined.  The test uses
//   strategy (a): assert ratio when loads_retired > 0; if loads_retired == 0
//   (zero-stub), assert the PmcCounters struct is well-formed and skip the
//   ratio check.  This is defensible — the test is still meaningful as a
//   compilation and linkage smoke-check, and the ratio assert fires on real
//   hardware once the KPC integration spike lands.
//
// Artifact:
//   Emits tests/core/world/perf/_artifacts/l1d_miss.json after each run.
//   Format:
//     {
//       "commit_sha":     "<GLIBRE_COMMIT_SHA env, or empty>",
//       "entity_count":   <uint32>,
//       "archetype_count":<uint32>,
//       "row_bytes":      <uint32>,
//       "chunk_rows":     <uint32>,
//       "l1d_misses":     <uint64>,
//       "loads_retired":  <uint64>,
//       "threshold_pct":  5,
//       "miss_pct":       <float or null>,   // null when loads_retired == 0
//       "verdict":        "pass" | "skip_zero_stub"
//     }
//   CI artifact-upload step (plan #944) picks up this path.
//
// Build flags:
//   - Exception handling: inherits CMake defaults (exception-neutral).
//     The glibre-core-world-perf-tests target does NOT link
//     glibre::compile_contract and does NOT call glibre_target_exceptions(),
//     so no -fno-exceptions flag is injected.  This is the standard pattern
//     for Catch2 test targets — Catch2 TEST_CASE requires exception support
//     and the CMake exception-neutral default provides it.
//   - Compiled as part of the glibre-core-world-perf-tests target
//     (tests/core/world/perf/CMakeLists.txt).
//   - GLIBRE_ENABLE_PMC compile definition forwarded from CMakeLists.txt.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <catch2/catch_test_macros.hpp>

// PmcSampler lives in the same directory as this file.
#include "pmc_sampler.hpp"

// S1 fixture — provides load_s1() + S1Scene (entity_count=209, archetypes={1,200,8}).
// Included from core/tests/ which is added to the include path in CMakeLists.txt.
#include "fixtures/s1.hpp"

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

// kL1dMissThreshold — L1-D miss rate alarm ceiling.
// Authority: specs/core/world-design.md §5.3:
//   "PMC sampling … verifies the hot loop's L1-D miss rate remains under 5%
//    on the S1 fixture; drift is the §9.5 alarm."
inline constexpr double kL1dMissThreshold = 0.05;  // 5 %

// kChunkRows — rows per archetype chunk.
// Aligned to the §5.3 invariant: Chunk holds 64-byte-aligned rows.
// 16 rows × 64 bytes = 1 KiB per chunk (representative of real chunked storage).
inline constexpr std::uint32_t kChunkRows = 16U;

// kRowBytes — bytes per row in the synthetic chunk.
// A row represents one entity's hot components (LocalTransform stub + 2 others).
// 64 bytes matches the §5.3 requirement that hot chunk header fits in one
// cache line (specs/core/world-design.md §5.3).
inline constexpr std::uint32_t kRowBytes = 64U;

// ---------------------------------------------------------------------------
// SyntheticChunk — a kChunkRows × kRowBytes array representing one archetype
// chunk.  Aligned to 64 bytes (§5.3 invariant 3: alignof(Chunk) >= 64).
//
// Each row encodes a 4×4 float matrix stub (16 floats × 4 bytes = 64 bytes),
// matching the LocalTransform storage pattern: the hot loop reads transform
// components sequentially, which is exactly the §9.5 BENCHMARK_CELL workload.
//
// Using std::array<float, kRowBytes/sizeof(float)> per row so the workload
// produces load operations the PMC can count.  Plain data; no EASTL needed
// for test-only infrastructure (PHILOSOPHY §11 — EASTL for runtime data
// structures; std:: is permitted for test scaffolding).
// ---------------------------------------------------------------------------

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
// SyntheticChunk and ArchetypeStorage are plain data aggregates used only
// inside the anonymous namespace of this test file.  All members are
// intentionally public.  Clang-tidy's misc-non-private-member-variables-in-
// classes check is suppressed for the two struct definitions below.

struct alignas(64) SyntheticChunk {
    using Row = std::array<float, kRowBytes / sizeof(float)>;  // 16 floats
    std::array<Row, kChunkRows> rows{};
    std::uint32_t row_count{kChunkRows};
};

// ---------------------------------------------------------------------------
// ArchetypeStorage — one archetype's contiguous chunk array.
//
// Holds ceil(entity_count / kChunkRows) SyntheticChunks for a given
// archetype.  The storage is heap-allocated once before sampling begins so
// the PMC captures iteration reads, not allocation overhead.
// ---------------------------------------------------------------------------

struct ArchetypeStorage {
    std::uint32_t entity_count;
    std::uint32_t chunk_count;
    // Raw chunk array.  new[] is fine for test-only scaffolding.
    // PHILOSOPHY §11 EASTL mandate applies to the runtime data path,
    // not test infrastructure.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    SyntheticChunk* chunks;

    explicit ArchetypeStorage(std::uint32_t n_entities)
        : entity_count{n_entities},
          chunk_count{(n_entities + kChunkRows - 1U) / kChunkRows},
          // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
          chunks{new SyntheticChunk[chunk_count]} {
        // Seed the rows with a deterministic pattern so values are not
        // trivially constant-foldable by the compiler.  Each float =
        // (chunk * kChunkRows + row + component_idx) as a float —
        // representative of a real transform column populated from a
        // placement_seed expansion.
        for (std::uint32_t c = 0U; c < chunk_count; ++c) {
            for (std::uint32_t r = 0U; r < kChunkRows; ++r) {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
                auto& row = chunks[c].rows.at(r);
                for (std::uint32_t k = 0U; k < static_cast<std::uint32_t>(row.size()); ++k) {
                    // Parentheses added for readability-math-missing-parentheses.
                    row.at(k) = static_cast<float>((c * kChunkRows) + r + k) + 0.1F;
                }
            }
            // Set actual row_count for the last chunk (may be partial).
            if (c == chunk_count - 1U) {
                const std::uint32_t full_chunks = entity_count / kChunkRows;
                const std::uint32_t remainder = entity_count % kChunkRows;
                chunks[c].row_count =
                    ((c == full_chunks) && (remainder != 0U)) ? remainder : kChunkRows;
            }
        }
    }

    ~ArchetypeStorage() {
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        delete[] chunks;
    }

    // Non-copyable to prevent accidental double-free.
    ArchetypeStorage(const ArchetypeStorage&) = delete;
    ArchetypeStorage& operator=(const ArchetypeStorage&) = delete;
    ArchetypeStorage(ArchetypeStorage&&) = delete;
    ArchetypeStorage& operator=(ArchetypeStorage&&) = delete;
};

// NOLINTEND(misc-non-private-member-variables-in-classes)

// ---------------------------------------------------------------------------
// run_archetype_iter_hot_loop — the §9.5 hot loop workload measured by PMC.
//
// Simulates the archetype iteration query pattern:
//   for each archetype (3 archetypes for S1: character, prop, dynamic_light):
//     for each chunk in archetype:
//       for each row in chunk:
//         read the 16-float transform row (64 bytes = one cache line)
//         accumulate into a running sum (prevents DCE)
//
// The memory access pattern mirrors the real Query::iter() hot path:
//   - Sequential within a chunk (cache-friendly, should stay in L1-D).
//   - Potential L1-D pressure arises from jumping between archetypes.
//   - On a well-laid-out ECS, this should produce < 5% L1-D misses.
//
// Returns an accumulated float to prevent dead-code elimination.
// [[gnu::noinline]] forces the call to not be inlined at the PMC call site,
// ensuring the measured region is the inner hot loop only.
//
// SYNTHETIC WALK — FUTURE SWAP-IN (#562 / #572):
//   This function walks SyntheticChunk / ArchetypeStorage arrays rather than
//   the real glibre Archetype, Chunk, and Query::iter() pipeline.  The
//   synthetic walk is intentionally accepted by plan #938 because the real
//   archetype storage (#562) and query iteration (#572) are still open issues.
//
//   The synthetic model is valid as a proxy because it preserves the invariants
//   that make the access pattern meaningful for PMC measurement:
//     - 64-byte alignment: SyntheticChunk is alignas(64), matching the §5.3
//       requirement that alignof(Chunk) >= 64 (each chunk header fits one cache
//       line).
//     - 16-float row: Row = array<float,16> = 64 bytes, matching the §5.3
//       "hot chunk header" width and the LocalTransform 4×4 matrix column
//       layout used in the §9.5 BENCHMARK_CELL workload.
//     - change-tick scan pattern: kChunkRows-per-chunk + partial last-chunk via
//       row_count mirrors the real Chunk iteration contract (live row range,
//       partial tail chunk, sequential reads within a chunk).
//
//   Once #562 (Archetype storage) and #572 (Query::iter) land, replace
//   SyntheticChunk/ArchetypeStorage with real glibre::Archetype and
//   glibre::Query, and repoint this function at Query::iter()'s chunk-walk.
//   The PMC bracketing and verdict logic in the TEST_CASE requires no change.
// ---------------------------------------------------------------------------
[[nodiscard]] [[gnu::noinline]] float run_archetype_iter_hot_loop(
    const ArchetypeStorage* archetypes, std::uint32_t archetype_count
) noexcept {
    float acc = 0.0F;

    for (std::uint32_t a = 0U; a < archetype_count; ++a) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        const ArchetypeStorage& storage = archetypes[a];
        for (std::uint32_t c = 0U; c < storage.chunk_count; ++c) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
            const SyntheticChunk& chunk = storage.chunks[c];
            // Iterate only the live rows (last chunk may be partial).
            for (std::uint32_t r = 0U; r < chunk.row_count; ++r) {
                // Sequential read of all 16 floats in the row — one cache line.
                // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                for (const float f : chunk.rows[r]) {
                    acc += f;
                    // Memory-clobber barrier: prevents clang/LTO from reducing
                    // the inner accumulator to a compile-time constant.  The
                    // "+r" constraint forces `acc` to be held in a register
                    // across the barrier; "memory" tells the compiler that
                    // arbitrary memory may have been read or written, so it
                    // cannot hoist or merge the surrounding loads.  This is the
                    // standard defensive pattern for benchmark hot loops and is
                    // required because [[gnu::noinline]] alone does not defeat
                    // LTO's cross-TU constant folding once #562/#572 wire in
                    // real archetype storage.
                    // NOLINTNEXTLINE(hicpp-no-assembler)
                    __asm__ __volatile__("" : "+r"(acc) : : "memory");
                }
                // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
        }
    }

    return acc;
}

// ---------------------------------------------------------------------------
// artifact_path — locate tests/core/world/perf/_artifacts/l1d_miss.json.
//
// Resolves from __FILE__ (absolute path at compile time) upward to find the
// tests/core/world/perf/ directory.  Returns the full path to l1d_miss.json.
//
// Creates the _artifacts/ directory if absent.
// ---------------------------------------------------------------------------
[[nodiscard]] std::filesystem::path artifact_path() {
    // This file lives at tests/core/world/perf/test_archetype_iter_l1d_miss.cpp.
    // __FILE__ is the absolute path at compile time.
    const std::filesystem::path this_file = std::filesystem::path(__FILE__);
    const std::filesystem::path perf_dir = this_file.parent_path();
    const std::filesystem::path artifacts_dir = perf_dir / "_artifacts";

    std::error_code ec;
    std::filesystem::create_directories(artifacts_dir, ec);
    // Ignore ec: if the directory already exists that is not an error.

    return artifacts_dir / "l1d_miss.json";
}

// ---------------------------------------------------------------------------
// emit_artifact — write the JSON artifact to the known path.
//
// Format (one JSON object, no trailing newline on last field):
//   {
//     "commit_sha":     "<GLIBRE_COMMIT_SHA env value, or empty string>",
//     "entity_count":   <uint32>,
//     "archetype_count":<uint32>,
//     "row_bytes":      <uint32>,
//     "chunk_rows":     <uint32>,
//     "l1d_misses":     <uint64>,
//     "loads_retired":  <uint64>,
//     "threshold_pct":  5,
//     "miss_pct":       <float with 4 decimal places> | null,
//     "verdict":        "pass" | "skip_zero_stub"
//   }
//
// Correlation fields rationale:
//   commit_sha      — links a drift-past-5% alarm to the introducing commit.
//                     Read from the GLIBRE_COMMIT_SHA environment variable
//                     (set by CI) or left empty when running locally.
//   entity_count    — workload size; confirms S1 fixture was used.
//   archetype_count — number of archetypes walked; stable at 3 for S1.
//   row_bytes       — bytes per row (64); confirms §5.3 alignment invariant.
//   chunk_rows      — rows per chunk (16); confirms §5.3 chunk granularity.
// ---------------------------------------------------------------------------
void emit_artifact(
    std::uint64_t l1d_misses,
    std::uint64_t loads_retired,
    const std::string& verdict,
    std::uint32_t entity_count,
    std::uint32_t archetype_count
) {
    const std::filesystem::path out = artifact_path();
    std::ofstream ofs(out);
    if (!ofs.is_open()) {
        // Best-effort: if we cannot write the artifact, do not fail the test.
        // WARN() logs the path so CI "artifact not found" errors can be
        // correlated with the test log rather than appearing with no breadcrumb.
        // plan #944's CI artifact-upload step depends on this file existing;
        // a missing file with no test-side message would be confusing to debug.
        WARN("emit_artifact: cannot open " + out.string());
        return;
    }

    // Correlation: read commit SHA from GLIBRE_COMMIT_SHA env (set by CI).
    // Empty string when running locally — the field is always present for
    // schema stability; tools can distinguish "" from a real SHA.
    const char* const commit_sha_env = std::getenv("GLIBRE_COMMIT_SHA");
    const std::string commit_sha = (commit_sha_env != nullptr) ? commit_sha_env : "";

    ofs << "{\n";
    ofs << R"(  "commit_sha": ")" << commit_sha << "\",\n";
    ofs << "  \"entity_count\": " << entity_count << ",\n";
    ofs << "  \"archetype_count\": " << archetype_count << ",\n";
    ofs << "  \"row_bytes\": " << kRowBytes << ",\n";
    ofs << "  \"chunk_rows\": " << kChunkRows << ",\n";
    ofs << "  \"l1d_misses\": " << l1d_misses << ",\n";
    ofs << "  \"loads_retired\": " << loads_retired << ",\n";
    ofs << "  \"threshold_pct\": 5,\n";

    if (loads_retired > 0U) {
        // Compute miss rate as a percentage with 4 decimal places.
        const double miss_pct =
            (static_cast<double>(l1d_misses) / static_cast<double>(loads_retired)) * 100.0;
        // Write with fixed precision (manual formatting avoids <iomanip> noise).
        // Four decimal places is sufficient for a 5% threshold comparison.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
        char buf[64];
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,cppcoreguidelines-pro-bounds-array-to-pointer-decay,cert-err33-c)
        std::snprintf(buf, sizeof(buf), "%.4f", miss_pct);
        ofs << R"(  "miss_pct": )" << buf << ",\n";
    } else {
        ofs << R"(  "miss_pct": null,)" << "\n";
    }

    ofs << R"(  "verdict": ")" << verdict << "\"\n";
    ofs << "}\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// world/perf: archetype_iter_l1d_miss_under_5pct
//
// CI gate test: archetype iteration hot loop must produce <= 5% L1-D misses.
//
// Procedure:
//   1. Load the S1 fixture (entity_count=209, 3 archetypes: 1 char + 200
//      props + 8 lights) to get the canonical entity composition.
//   2. Build an ArchetypeStorage array matching the S1 archetype breakdown.
//   3. Warm-up: one un-measured iteration pass to populate the L1-D cache
//      with the working set (ensures first-run cold-cache effect does not
//      inflate the measured miss rate).
//   4. Measure: PmcSampler::measure([&]{ run_archetype_iter_hot_loop(...) })
//      captures l1d_misses and loads_retired as hardware deltas.
//   5. Verdict:
//      - If loads_retired > 0: REQUIRE(l1d_misses / loads_retired <= 0.05).
//      - If loads_retired == 0 (zero-stub path): the struct is well-formed;
//        log the skip and pass.  (Zero-stub is current state: pmc_sampler.cpp
//        §Integration Notes documents that KPC integration is a follow-up spike.)
//   6. Emit tests/core/world/perf/_artifacts/l1d_miss.json.
// ---------------------------------------------------------------------------

// NOLINTBEGIN(cppcoreguidelines-avoid-do-while,misc-use-anonymous-namespace,
//             bugprone-throwing-static-initialization)
// The following TEST_CASE macro expands to Catch2 boilerplate that uses
// do-while, static initialization, and namespace structure that differ
// from the project style.  These suppressions apply to the expanded form
// of the macro only and do not suppress checks on our own code bodies.

TEST_CASE("world/perf: archetype_iter_l1d_miss_under_5pct", "[world][perf][pmc][l1d][s1]") {
    // ------------------------------------------------------------------
    // Step 1: Load S1 canonical fixture.
    // ------------------------------------------------------------------
    const glibre::testing::S1Scene scene = glibre::testing::load_s1();

    REQUIRE(scene.entity_count == 209U);
    REQUIRE(scene.archetype_count == 3U);

    // ------------------------------------------------------------------
    // Step 2: Build ArchetypeStorage for each S1 archetype.
    //
    // S1 breakdown: 1 character + 200 props + 8 dynamic lights.
    // Array order matches S1ArchetypeCounts field order; each storage is
    // allocated on the heap so the PMC measures iteration reads, not
    // object construction.
    // ------------------------------------------------------------------
    const std::array<std::uint32_t, 3U> archetype_entity_counts = {
        scene.archetypes.character,      // 1
        scene.archetypes.prop,           // 200
        scene.archetypes.dynamic_light,  // 8
    };

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)
    ArchetypeStorage storages[3U] = {
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index)
        ArchetypeStorage{archetype_entity_counts[0U]},
        ArchetypeStorage{archetype_entity_counts[1U]},
        ArchetypeStorage{archetype_entity_counts[2U]},
        // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)
    };

    // ------------------------------------------------------------------
    // Step 3: Warm-up pass.
    //
    // One unmeasured iteration to ensure the working set is resident in
    // L1-D before sampling begins.  Without warm-up, the first-run
    // cold-cache misses from page-faults / LLC fills would inflate the
    // measured miss rate, giving a false alarm even on a well-laid-out
    // archetype.
    //
    // volatile sink prevents DCE of the warm-up.
    // ------------------------------------------------------------------
    {
        volatile float warmup_sink =
            run_archetype_iter_hot_loop(static_cast<const ArchetypeStorage*>(storages), 3U);
        (void)warmup_sink;
    }

    // ------------------------------------------------------------------
    // Step 4: PMC-measured hot loop.
    // ------------------------------------------------------------------
    const glibre::testing::PmcCounters counters = glibre::testing::PmcSampler::measure([&] {
        volatile float measured_sink =
            run_archetype_iter_hot_loop(static_cast<const ArchetypeStorage*>(storages), 3U);
        (void)measured_sink;
    });

    // ------------------------------------------------------------------
    // Step 5: Verify counter struct is well-formed.
    //
    // These CHECKs hold on both the zero-stub and a real-KPC path:
    //   - On the zero-stub: all fields are 0, which passes the
    //     sanity-cap check (0 < 1 trillion).
    //   - On real KPC: fields are small positive integers reflecting
    //     actual hardware events, well below the sanity cap.
    // The sanity cap (1 << 40 ≈ 1 trillion) rejects reads of
    // uninitialised memory or hardware counter wrap-around artefacts.
    // ------------------------------------------------------------------
    const std::uint64_t kSanityCap = 1ULL << 40U;

    CHECK(counters.l1d_misses < kSanityCap);
    CHECK(counters.loads_retired < kSanityCap);

    // ------------------------------------------------------------------
    // Step 5 (continued): L1-D miss-rate assertion.
    //
    // Zero-stub branch (loads_retired == 0):
    //   KPC integration is pending (pmc_sampler.cpp §Integration Notes).
    //   Ratio assertion is skipped; test passes as a well-formed-struct
    //   smoke check.  This matches plan #938 §Note on PMC zero-stub
    //   strategy (a): "Assert ratio when loads_retired > 0; if
    //   loads_retired == 0 (zero-stub), assert structure is well-formed
    //   + skip the ratio check."
    //
    // Real-KPC branch (loads_retired > 0):
    //   Assert the miss rate is under the 5% threshold from
    //   specs/core/world-design.md §5.3.
    //   The REQUIRE fires as a CI gate; drifting past 5% is the §9.5
    //   alarm and requires a perf-budget amendment spike.
    // ------------------------------------------------------------------

    std::string verdict;

    if (counters.loads_retired == 0U) {
        // Zero-stub path — KPC integration not yet landed.
        // Test passes as a compilation / linkage smoke check.
        verdict = "skip_zero_stub";

        // CHECK(true) is a visible no-op that confirms the zero-stub
        // branch was taken rather than the ratio assertion.  It appears
        // in the Catch2 assertion count so the test is not zero-assertions.
        CHECK(true);  // zero-stub: loads_retired == 0; ratio check skipped

    } else {
        // Real-KPC path — counters are live hardware values.
        const double miss_rate =
            static_cast<double>(counters.l1d_misses) / static_cast<double>(counters.loads_retired);

        // Core assertion: §5.3 alarm fires when miss rate >= 5%.
        REQUIRE(miss_rate <= kL1dMissThreshold);

        verdict = "pass";
    }

    // ------------------------------------------------------------------
    // Step 6: Emit JSON artifact.
    // ------------------------------------------------------------------
    emit_artifact(
        counters.l1d_misses,
        counters.loads_retired,
        verdict,
        scene.entity_count,
        scene.archetype_count
    );
}

// NOLINTEND(cppcoreguidelines-avoid-do-while,misc-use-anonymous-namespace,
//            bugprone-throwing-static-initialization)
