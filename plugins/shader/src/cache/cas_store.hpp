#pragma once
// plugins/shader/src/cache/cas_store.hpp
//
// Content-addressable filesystem store for ShaderArtifact blobs.
//
// SRP: "Persist and retrieve opaque byte blobs keyed by ShaderHash using the
//       two-byte-prefix sharding layout: artifacts/<aa>/<bb>/<hash>."
//
// Authority: specs/shader/SPEC.md §4.6, §7.5.
//
// Directory layout (SPEC §7.5):
//   <root>/artifacts/<aa>/<bb>/<hash>
//   where <aa>   = hex bytes[0]    (2 chars, first hex pair)
//         <bb>   = hex bytes[1]    (2 chars, second hex pair)
//         <hash> = full 64-hex-char lowercase ShaderHash string
//
// Insert behaviour:
//   - Idempotent: inserting an existing key is a no-op (§4.6 invariant 1).
//   - Values are immutable once stored; the store never overwrites a key.
//
// Read behaviour:
//   - get() reads the blob and verifies its BLAKE3 hash matches the key
//     (integrity self-check — SPEC §4.6).
//   - Returns Error::CacheCorrupt if the content hash does not match.
//   - Returns std::nullopt (miss) when the file is absent.
//
// Thread-safety: NOT thread-safe.  External synchronization required.

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glibre/shader/shader.hpp>

namespace glibre::shader::cache {

// ---------------------------------------------------------------------------
// Hex encoding helpers for ShaderHash → directory path components
// ---------------------------------------------------------------------------

/// Convert one byte to two lowercase hex characters.
[[nodiscard]] std::string shader_hash_to_hex(const ShaderHash& h);

/// Derive the CAS path for a given hash under root:
///   <root>/artifacts/<aa>/<bb>/<hex-hash>
[[nodiscard]] std::filesystem::path
cas_artifact_path(const std::filesystem::path& root, const ShaderHash& hash);

// ---------------------------------------------------------------------------
// CasStore
// ---------------------------------------------------------------------------

class CasStore {
public:
    /// Open (or create) a CAS store rooted at cache_root.
    /// The artifacts/ subdirectory is created on first insert.
    explicit CasStore(std::filesystem::path cache_root);

    // Non-copyable; movable.
    CasStore(const CasStore&) = delete;
    CasStore& operator=(const CasStore&) = delete;
    CasStore(CasStore&&) noexcept = default;
    CasStore& operator=(CasStore&&) noexcept = default;

    // -----------------------------------------------------------------------
    // Read path (shipping + tooling)
    // -----------------------------------------------------------------------

    /// Check existence without reading the blob.
    [[nodiscard]] bool has(const ShaderHash& hash) const;

    /// Read and integrity-check the blob stored under hash.
    ///
    /// Returns nullopt when the key is absent (cache miss).
    /// Returns Error::CacheCorrupt when the file exists but BLAKE3(content)
    ///   does not match hash — the stored bytes are tampered or truncated
    ///   (SPEC §10.2: CacheCorrupt = CAS file fails its BLAKE3 self-check).
    [[nodiscard]] glibre::Result<std::optional<std::vector<std::byte>>>
    get(const ShaderHash& hash) const;

    // -----------------------------------------------------------------------
    // Write path (tooling / cooker only — excluded from shipping link)
    // -----------------------------------------------------------------------

    /// Idempotent insert.  If hash already exists, returns success without
    /// writing.  On success the blob is immutable and durable (sync'd).
    ///
    /// Error arms (SPEC §10.2):
    ///   CacheIntegrity — BLAKE3(data) != hash; caller-supplied hash is wrong.
    ///   CacheCorrupt   — an existing on-disk entry's bytes don't hash to its
    ///                    filename key (detected during the existence check).
    /// Empty data is not a distinct error: the span is accepted and stored.
    [[nodiscard]] glibre::Result<void>
    insert_if_absent(const ShaderHash& hash, std::span<const std::byte> data);

    /// Root path (for external integrity walkers).
    [[nodiscard]] const std::filesystem::path& root() const noexcept;

private:
    std::filesystem::path root_;
};

}  // namespace glibre::shader::cache
