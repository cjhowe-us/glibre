#pragma once
// plugins/shader/src/cache/blake3.hpp
//
// BLAKE3 incremental hasher — sole source of glibre::shader::ShaderHash.
//
// SRP: "Stream bytes into a BLAKE3 digest and produce a ShaderHash."
//
// Authority: specs/shader/SPEC.md §4.6, §6.4.
//
// The SPEC mandates one hash family across the shader context (SPEC §6.4:
// "one hash family across the context, no SHA-/MD-/xxhash drift").  All
// ShaderHash values — artifact hash, include-graph content hashes — must
// originate from this type.
//
// Hash composition per SPEC §2 / §6.4:
//   artifact_hash := BLAKE3(source_hash || key.to_bytes() || flags_bytes || u8(target))
//
// Usage:
//   Blake3Hasher h;
//   h.update(preprocessed.bytes);
//   h.update(key.to_bytes());
//   h.update(flags_bytes);
//   h.update_byte(static_cast<std::byte>(target));
//   ShaderHash hash = h.finalize();
//
// The hasher is NOT copyable; finalize() must be called at most once.
// For single-buffer hashes (include-graph nodes) prefer blake3_hash_buffer().

#include <cstddef>
#include <cstdint>
#include <span>

// BLAKE3 C reference implementation (vcpkg).
#include <blake3.h>

#include <glibre/shader/shader.hpp>

namespace glibre::shader::cache {

// ---------------------------------------------------------------------------
// Blake3Hasher — incremental streaming hasher
// ---------------------------------------------------------------------------

class Blake3Hasher {
public:
    Blake3Hasher() noexcept {
        blake3_hasher_init(&state_);
    }

    // Non-copyable, non-movable.  Finalize once and discard.
    Blake3Hasher(const Blake3Hasher&) = delete;
    Blake3Hasher& operator=(const Blake3Hasher&) = delete;

    /// Feed arbitrary bytes into the hasher.
    void update(std::span<const std::byte> data) noexcept {
        blake3_hasher_update(&state_, data.data(), data.size());
    }

    /// Feed a std::array directly (e.g. PermutationKey::PackedBytes).
    template <std::size_t N>
    void update(const std::array<std::byte, N>& arr) noexcept {
        blake3_hasher_update(&state_, arr.data(), N);
    }

    /// Feed a single byte (e.g. CompileTarget discriminant).
    void update_byte(std::byte b) noexcept {
        blake3_hasher_update(&state_, &b, 1);
    }

    /// Produce the 32-byte BLAKE3 output.
    /// Must be called exactly once per hasher instance.
    [[nodiscard]] ShaderHash finalize() noexcept {
        ShaderHash result{};
        blake3_hasher_finalize(
            &state_,
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            reinterpret_cast<std::uint8_t*>(result.bytes.data()),
            result.bytes.size()
        );
        return result;
    }

private:
    blake3_hasher state_{};
};

// ---------------------------------------------------------------------------
// Single-buffer convenience: BLAKE3(data) → ShaderHash.
// Used for include-graph node hashes (§4.1).
// ---------------------------------------------------------------------------

[[nodiscard]] inline ShaderHash blake3_hash_buffer(
    std::span<const std::byte> data
) noexcept {
    Blake3Hasher h;
    h.update(data);
    return h.finalize();
}

// ---------------------------------------------------------------------------
// Artifact-hash composition per SPEC §2 / §6.4:
//   BLAKE3(preprocessed_bytes || key.to_bytes() || flags_bytes || u8(target))
// ---------------------------------------------------------------------------

[[nodiscard]] inline ShaderHash compute_artifact_hash(
    std::span<const std::byte>              preprocessed_bytes,
    const PermutationKey::PackedBytes&      key_bytes,
    std::span<const std::byte>              flags_bytes,
    CompileTarget                           target
) noexcept {
    Blake3Hasher h;
    h.update(preprocessed_bytes);
    h.update(key_bytes);
    h.update(flags_bytes);
    h.update_byte(static_cast<std::byte>(target));
    return h.finalize();
}

}  // namespace glibre::shader::cache
