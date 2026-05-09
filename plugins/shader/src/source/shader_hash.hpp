#pragma once
// plugins/shader/src/source/shader_hash.hpp
//
// Shared BLAKE3 hashing helper for the shader plugin.
//
// SRP: "Compute a ShaderHash from a raw byte buffer."
//
// Authority: specs/shader/SPEC.md §4.1 (content hashing invariant).
//
// Both shader_source.cpp (total-hash) and preprocessor.cpp (per-include hash)
// call this helper.  Having a single definition eliminates the byte-identical
// duplication that previously existed in two anonymous namespaces (R2 HIGH-1).
//
// NOTE: vendored BLAKE3 is exception-free (no longjmp, no C++ exceptions);
// marking the helper noexcept is safe.

#include <cstddef>

// BLAKE3 vendored header (vcpkg).
#include <blake3.h>

#include <glibre/shader/shader.hpp>

namespace glibre::shader::detail {

/// Compute a BLAKE3 hash over [data, data+len) and return a ShaderHash.
///
/// Callers must ensure data is non-null when len > 0.
/// vendored BLAKE3 is exception-free; safe to mark noexcept.
[[nodiscard]] inline ShaderHash blake3_hash(const void* data, std::size_t len) noexcept {
    ShaderHash result{};
    blake3_hasher hasher{};
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data, len);
    blake3_hasher_finalize(&hasher, reinterpret_cast<uint8_t*>(result.bytes.data()), 32);
    return result;
}

}  // namespace glibre::shader::detail
