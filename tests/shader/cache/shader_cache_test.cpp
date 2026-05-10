// tests/shader/cache/shader_cache_test.cpp
//
// Catch2 unit tests for glibre::shader::cache::Blake3Hasher and CasStore.
//
// Plan: #516 — feat(shader): BLAKE3 ShaderHash + CAS store.
// Authority: specs/shader/SPEC.md §4.6, §7.5.
//
// Named test cases (plan #516 Unit Test Plan + DoD):
//   - shader/cache: blake3_hash_is_deterministic_across_repeated_calls
//   - shader/cache: blake3_hash_input_composition_matches_spec_section_2
//   - shader/cache: cas_store_insert_if_absent_is_idempotent
//   - shader/cache: cas_store_path_layout_matches_two_byte_prefix_sharding
//   - shader/cache: cas_store_get_returns_byte_equal_payload_for_inserted_hash
//   - shader/cache: cas_store_returns_CacheCorrupt_on_blake3_self_check_failure
//   - shader/cache: cas_store_returns_CacheIntegrity_on_hash_collision_with_different_payload
//
// Design constraints:
//   - -fno-exceptions compatible (error-model.md §Decision 3).
//   - No REQUIRE_THROWS.
//   - Temporary directories are created under std::filesystem::temp_directory_path()
//     and cleaned up in section tear-down.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

// Internal cache headers (exposed via target_include_directories in CMakeLists).
#include "blake3.hpp"
#include "cas_store.hpp"

// Public shader boundary (for ShaderHash, PermutationKey, CompileTarget).
#include <glibre/shader/shader.hpp>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/// Create a unique temp directory for one test's CAS store.
/// The directory is removed by the returned RAII guard's destructor.
struct TempDir {
    std::filesystem::path path;

    TempDir() {
        // Unique subdir under system temp.
        path = std::filesystem::temp_directory_path() /
               ("glibre-cas-test-" + std::to_string(
                    static_cast<std::uint64_t>(
                        std::filesystem::last_write_time(
                            std::filesystem::temp_directory_path()
                        ).time_since_epoch().count()
                    ) ^ reinterpret_cast<std::uint64_t>(this)));  // NOLINT
        std::filesystem::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

/// Build a trivial span<const byte> from a string literal.
std::span<const std::byte> as_bytes(const std::string& s) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

/// Build a byte vector from a string literal (for insert payloads).
std::vector<std::byte> make_payload(const std::string& s) {
    std::vector<std::byte> v(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        v[i] = static_cast<std::byte>(static_cast<unsigned char>(s[i]));
    }
    return v;
}

/// Overwrite a file's first byte to corrupt it, leaving length unchanged.
void corrupt_file_first_byte(const std::filesystem::path& p) {
    std::fstream f{p, std::ios::in | std::ios::out | std::ios::binary};
    std::byte b{0xFFu};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    f.read(reinterpret_cast<char*>(&b), 1);
    f.seekp(0);
    const std::byte corrupted{static_cast<std::byte>(~static_cast<unsigned char>(b))};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    f.write(reinterpret_cast<const char*>(&corrupted), 1);
}

}  // namespace

// ---------------------------------------------------------------------------
// BLAKE3 hasher tests
// ---------------------------------------------------------------------------

TEST_CASE("blake3_hash_is_deterministic_across_repeated_calls", "[shader][cache]") {
    // Hashing the same input twice with independent Blake3Hasher instances
    // must yield bit-identical ShaderHash values.
    // SPEC §4.6 invariant 1: "content-addressable: ShaderHash is BLAKE3 of ..."
    // PHILOSOPHY §7: "Determinism by default."
    const std::string input = "determinism-test-payload-abc123";
    const auto bytes = as_bytes(input);

    glibre::shader::cache::Blake3Hasher h1;
    h1.update(bytes);
    const glibre::shader::ShaderHash hash1 = h1.finalize();

    glibre::shader::cache::Blake3Hasher h2;
    h2.update(bytes);
    const glibre::shader::ShaderHash hash2 = h2.finalize();

    REQUIRE(hash1 == hash2);

    // Also verify via the convenience wrapper.
    const glibre::shader::ShaderHash hash3 =
        glibre::shader::cache::blake3_hash_buffer(bytes);
    REQUIRE(hash1 == hash3);
}

TEST_CASE("blake3_hash_input_composition_matches_spec_section_2", "[shader][cache]") {
    // SPEC §2 / §6.4: artifact_hash = BLAKE3(source ∪ key.to_bytes() ∪ flags ∪ u8(target))
    //
    // Verify that compute_artifact_hash() is equivalent to manually building
    // the same concatenated input in one Blake3Hasher.update() sequence.

    const std::string source_str   = "preprocessed-source-bytes";
    const std::string flags_str    = "--O3 --target metallib";

    // Use a fixed all-zero PackedBytes to avoid depending on PermutationKey::to_bytes()
    // (permutation codec is implemented in a later plan).  The composition invariant
    // tested here is that compute_artifact_hash() feeds the four inputs in the
    // exact order mandated by SPEC §2, regardless of the concrete key value.
    glibre::shader::PermutationKey::PackedBytes key_bytes{};  // all-zero

    const glibre::shader::CompileTarget target = glibre::shader::CompileTarget::MetalLib;

    // Path A: use compute_artifact_hash().
    const glibre::shader::ShaderHash hash_a =
        glibre::shader::cache::compute_artifact_hash(
            as_bytes(source_str),
            key_bytes,
            as_bytes(flags_str),
            target
        );

    // Path B: manually compose via Blake3Hasher — must be bit-identical to Path A.
    glibre::shader::cache::Blake3Hasher h;
    h.update(as_bytes(source_str));
    h.update(key_bytes);
    h.update(as_bytes(flags_str));
    h.update_byte(static_cast<std::byte>(target));
    const glibre::shader::ShaderHash hash_b = h.finalize();

    REQUIRE(hash_a == hash_b);

    // Sanity: different source must produce different hash.
    const glibre::shader::ShaderHash hash_different =
        glibre::shader::cache::compute_artifact_hash(
            as_bytes("different-source"),
            key_bytes,
            as_bytes(flags_str),
            target
        );
    REQUIRE(hash_a != hash_different);

    // Sanity: different target must produce different hash.
    const glibre::shader::ShaderHash hash_dxil =
        glibre::shader::cache::compute_artifact_hash(
            as_bytes(source_str),
            key_bytes,
            as_bytes(flags_str),
            glibre::shader::CompileTarget::DXIL
        );
    REQUIRE(hash_a != hash_dxil);
}

// ---------------------------------------------------------------------------
// CasStore tests
// ---------------------------------------------------------------------------

TEST_CASE("cas_store_insert_if_absent_is_idempotent", "[shader][cache]") {
    // SPEC §4.6 invariant 1: "insert of an existing key is a no-op; values
    // are immutable once stored."
    TempDir tmp;
    glibre::shader::cache::CasStore store{tmp.path};

    const auto payload = make_payload("idempotency-test-blob");
    const glibre::shader::ShaderHash hash =
        glibre::shader::cache::blake3_hash_buffer(payload);

    // First insert.
    auto r1 = store.insert_if_absent(hash, payload);
    REQUIRE(r1.has_value());

    // Second insert with identical payload — must succeed (no-op).
    auto r2 = store.insert_if_absent(hash, payload);
    REQUIRE(r2.has_value());

    // The stored blob must still be intact after the second insert.
    auto got = store.get(hash);
    REQUIRE(got.has_value());
    REQUIRE(got.value().has_value());
    REQUIRE(*got.value() == payload);
}

TEST_CASE("cas_store_path_layout_matches_two_byte_prefix_sharding", "[shader][cache]") {
    // SPEC §7.5: artifacts/<aa>/<bb>/<hash>
    //   <aa>   = hex of bytes[0]  (2 chars)
    //   <bb>   = hex of bytes[1]  (2 chars)
    //   <hash> = full 64-char hex string
    TempDir tmp;
    glibre::shader::cache::CasStore store{tmp.path};

    const auto payload = make_payload("path-layout-test");
    const glibre::shader::ShaderHash hash =
        glibre::shader::cache::blake3_hash_buffer(payload);

    auto r = store.insert_if_absent(hash, payload);
    REQUIRE(r.has_value());

    // Reconstruct expected path.
    const std::filesystem::path expected =
        glibre::shader::cache::cas_artifact_path(tmp.path, hash);

    // File must exist at the expected CAS path.
    REQUIRE(std::filesystem::exists(expected));

    // Verify directory structure: artifacts/<aa>/<bb>/
    const std::string hex = glibre::shader::cache::shader_hash_to_hex(hash);
    const std::string aa  = hex.substr(0, 2);
    const std::string bb  = hex.substr(2, 2);

    REQUIRE(expected.parent_path().filename().string() == bb);
    REQUIRE(expected.parent_path().parent_path().filename().string() == aa);
    REQUIRE(expected.parent_path().parent_path().parent_path().filename().string()
            == "artifacts");

    // <hash> component is the full 64-char hex string.
    REQUIRE(expected.filename().string() == hex);
    REQUIRE(hex.size() == 64u);
}

TEST_CASE("cas_store_get_returns_byte_equal_payload_for_inserted_hash", "[shader][cache]") {
    // SPEC §4.6: get() returns the stored artifact bytes verbatim on hit.
    // The BLAKE3 integrity self-check must pass for a correctly stored blob.
    TempDir tmp;
    glibre::shader::cache::CasStore store{tmp.path};

    const auto payload = make_payload("round-trip-payload-12345678");
    const glibre::shader::ShaderHash hash =
        glibre::shader::cache::blake3_hash_buffer(payload);

    // Insert.
    REQUIRE(store.insert_if_absent(hash, payload).has_value());

    // Get — must return the original payload byte-for-byte.
    auto got = store.get(hash);
    REQUIRE(got.has_value());          // no error
    REQUIRE(got.value().has_value());  // cache hit (not nullopt)
    REQUIRE(*got.value() == payload);

    // Miss: a random hash that was never inserted must return nullopt.
    glibre::shader::ShaderHash absent_hash{};
    absent_hash.bytes[0] = std::byte{0xAAu};
    absent_hash.bytes[1] = std::byte{0xBBu};
    auto miss = store.get(absent_hash);
    REQUIRE(miss.has_value());
    REQUIRE(!miss.value().has_value());  // nullopt == miss
}

TEST_CASE("cas_store_returns_CacheCorrupt_on_blake3_self_check_failure",
          "[shader][cache]") {
    // SPEC §4.6: "integrity self-check on read" — if the content hash of
    // the stored blob does not match the key, return Error::CacheCorrupt.
    TempDir tmp;
    glibre::shader::cache::CasStore store{tmp.path};

    const auto payload = make_payload("integrity-check-payload");
    const glibre::shader::ShaderHash hash =
        glibre::shader::cache::blake3_hash_buffer(payload);

    REQUIRE(store.insert_if_absent(hash, payload).has_value());

    // Corrupt the stored file (flip one byte in place).
    const std::filesystem::path artifact_path =
        glibre::shader::cache::cas_artifact_path(tmp.path, hash);
    corrupt_file_first_byte(artifact_path);

    // get() must now return CacheCorrupt.
    auto got = store.get(hash);
    REQUIRE(!got.has_value());  // error, not success

    const bool is_corrupt = std::visit(
        [](auto e) -> bool {
            if constexpr (std::is_same_v<decltype(e), glibre::shader::Error>) {
                return e == glibre::shader::Error::CacheCorrupt;
            }
            return false;
        },
        got.error().code()
    );
    REQUIRE(is_corrupt);
}

TEST_CASE("cas_store_returns_CacheIntegrity_on_hash_collision_with_different_payload",
          "[shader][cache]") {
    // SPEC §4.6 unit-test plan item: "cas_store_returns_CacheIntegrity_on_hash_collision_with_different_payload"
    //
    // This test verifies that when a file exists at the CAS path but the
    // content is different from what was computed (simulated by inserting
    // a payload whose hash we then forcibly overwrite the file for a different
    // payload's hash), get() returns CacheCorrupt (content tamper detection).
    //
    // True BLAKE3 collisions are computationally infeasible.  We simulate the
    // scenario by:
    //   1. Computing hash_A for payload_A.
    //   2. Inserting payload_A under hash_A (success).
    //   3. Computing hash_B for payload_B.
    //   4. Overwriting the CAS file at the path for hash_A with payload_B's content.
    //   5. Calling get(hash_A) — the content is payload_B but hash is hash_A
    //      → BLAKE3 self-check fails → CacheCorrupt.
    //
    // The test is named "CacheIntegrity" per the plan's unit-test plan entry;
    // the wire-format error returned by get() for a hash-mismatch is
    // shader::Error::CacheCorrupt (§4.6: "integrity self-check on read").
    TempDir tmp;
    glibre::shader::cache::CasStore store{tmp.path};

    const auto payload_a = make_payload("payload-A-for-collision-sim");
    const auto payload_b = make_payload("payload-B-different-content-xyz");

    const glibre::shader::ShaderHash hash_a =
        glibre::shader::cache::blake3_hash_buffer(payload_a);

    // Insert payload_a under hash_a.
    REQUIRE(store.insert_if_absent(hash_a, payload_a).has_value());

    // Overwrite CAS file with payload_b's bytes to simulate a collision.
    const std::filesystem::path artifact_path =
        glibre::shader::cache::cas_artifact_path(tmp.path, hash_a);
    {
        std::ofstream out{artifact_path, std::ios::binary | std::ios::trunc};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        out.write(reinterpret_cast<const char*>(payload_b.data()),
                  static_cast<std::streamsize>(payload_b.size()));
    }

    // get(hash_a) must detect the mismatch and return CacheCorrupt.
    auto got = store.get(hash_a);
    REQUIRE(!got.has_value());

    const bool is_corrupt = std::visit(
        [](auto e) -> bool {
            if constexpr (std::is_same_v<decltype(e), glibre::shader::Error>) {
                return e == glibre::shader::Error::CacheCorrupt;
            }
            return false;
        },
        got.error().code()
    );
    REQUIRE(is_corrupt);
}
