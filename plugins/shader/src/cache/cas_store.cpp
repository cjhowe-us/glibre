// plugins/shader/src/cache/cas_store.cpp
//
// Content-addressable filesystem store implementation.
//
// Authority: specs/shader/SPEC.md §4.6, §7.5.
//
// Path derivation (§7.5):
//   <root>/artifacts/<aa>/<bb>/<hash>
//   - <aa>   = lowercase hex of bytes[0]   (2 chars)
//   - <bb>   = lowercase hex of bytes[1]   (2 chars)
//   - <hash> = lowercase hex of all 32 bytes (64 chars)
//
// Integrity check on get():
//   Re-hash the file content with BLAKE3.  If the recomputed hash
//   does not match the requested key, return Error::CacheCorrupt.
//
// Idempotency on insert_if_absent():
//   If the file already exists, return immediately without re-writing.
//   Values are immutable once stored.

#include "cas_store.hpp"

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

#include <glibre/shader/shader.hpp>

#include "blake3.hpp"

namespace glibre::shader::cache {

// ---------------------------------------------------------------------------
// Hex helpers
// ---------------------------------------------------------------------------

[[nodiscard]] std::string shader_hash_to_hex(const ShaderHash& h) {
    std::string hex;
    hex.reserve(64);
    static constexpr char kHex[] = "0123456789abcdef";
    for (const auto byte : h.bytes) {
        const auto b = static_cast<unsigned char>(byte);
        hex.push_back(kHex[(b >> 4) & 0xFu]);
        hex.push_back(kHex[b & 0xFu]);
    }
    return hex;
}

[[nodiscard]] std::filesystem::path
cas_artifact_path(const std::filesystem::path& root, const ShaderHash& hash) {
    // Extract first two hex-encoded bytes for two-level sharding.
    const auto b0 = static_cast<unsigned char>(hash.bytes[0]);
    const auto b1 = static_cast<unsigned char>(hash.bytes[1]);
    static constexpr char kHex[] = "0123456789abcdef";

    char aa[3] = {kHex[(b0 >> 4) & 0xFu], kHex[b0 & 0xFu], '\0'};
    char bb[3] = {kHex[(b1 >> 4) & 0xFu], kHex[b1 & 0xFu], '\0'};

    return root / "artifacts" / aa / bb / shader_hash_to_hex(hash);
}

// ---------------------------------------------------------------------------
// CasStore implementation
// ---------------------------------------------------------------------------

CasStore::CasStore(std::filesystem::path cache_root)
    : root_{std::move(cache_root)} {}

const std::filesystem::path& CasStore::root() const noexcept { return root_; }

bool CasStore::has(const ShaderHash& hash) const {
    const auto path = cas_artifact_path(root_, hash);
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

glibre::Result<std::optional<std::vector<std::byte>>> CasStore::get(const ShaderHash& hash) const {
    const auto path = cas_artifact_path(root_, hash);

    // Miss: file does not exist.
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return std::optional<std::vector<std::byte>>{std::nullopt};
    }

    // Read the entire file.
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file.is_open()) {
        // File exists but cannot be opened — treat as corrupt.
        return std::unexpected(glibre::Error{shader::Error::CacheCorrupt});
    }

    const auto file_size = static_cast<std::size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    std::vector<std::byte> buf(file_size);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (!file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(file_size))) {
        return std::unexpected(glibre::Error{shader::Error::CacheCorrupt});
    }

    // Integrity self-check: re-hash the content.
    const ShaderHash actual = blake3_hash_buffer(std::span<const std::byte>{buf});
    if (actual != hash) {
        return std::unexpected(glibre::Error{shader::Error::CacheCorrupt});
    }

    return std::optional<std::vector<std::byte>>{std::move(buf)};
}

glibre::Result<void>
CasStore::insert_if_absent(const ShaderHash& hash, std::span<const std::byte> data) {
    // Idempotency: if the key already exists, succeed immediately.
    const auto path = cas_artifact_path(root_, hash);
    {
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            return {};
        }
    }

    // Create shard directories (artifacts/<aa>/<bb>/) if absent.
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            // Cannot create directory hierarchy.
            return std::unexpected(glibre::Error{shader::Error::CacheIntegrity});
        }
    }

    // Write atomically: write to a temp file, then rename.
    // Rename is atomic on POSIX when src/dst are on the same filesystem.
    const auto tmp_path = path.parent_path() / (shader_hash_to_hex(hash) + ".tmp");

    {
        std::ofstream out{tmp_path, std::ios::binary | std::ios::trunc};
        if (!out.is_open()) {
            return std::unexpected(glibre::Error{shader::Error::CacheIntegrity});
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        if (!out.write(
                reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size())
            )) {
            // Write failed — clean up temp file.
            std::error_code ec;
            std::filesystem::remove(tmp_path, ec);
            return std::unexpected(glibre::Error{shader::Error::CacheIntegrity});
        }
    }  // ofstream closed (flushed) here.

    // Rename temp → final.
    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        std::filesystem::remove(tmp_path, ec);  // best-effort cleanup
        return std::unexpected(glibre::Error{shader::Error::CacheIntegrity});
    }

    return {};
}

}  // namespace glibre::shader::cache
