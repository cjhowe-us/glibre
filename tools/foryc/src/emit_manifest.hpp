// SPDX-License-Identifier: Apache-2.0
// tools/foryc/src/emit_manifest.hpp
//
// Manifest-emission pass for glibre-foryc (plan #225).
//
// Produces a generated C++ translation unit (manifest.cpp) that embeds a
// hand-serialized PluginManifest blob into the plugin .rodata section and
// exports the four C-ABI symbols required by the loader (plugin-abi.md
// §"Plugin file shape"):
//
//   glibre_plugin_manifest      — extern "C" const uint8_t*, Fory-serialized blob
//   glibre_plugin_manifest_size — extern "C" size_t, byte length of the blob
//   glibre_plugin_abi_hash      — extern "C" const char*, 64-char blake3 hex global
//   glibre_plugin_name          — extern "C" const char*, fully-qualified name global
//
// plugin-abi.md §"Plugin file shape" item 3 mandates:
//   glibre_plugin_abi_hash — `extern "C" const char*`, 32-byte blake3 hex string
//   (64 lowercase hex chars) compiled in from the middleman headers at build time.
//   The plugin obtains this value by #include <glibre/types/abi_hash.hpp> and
//   re-exports glibre_types_abi_hash()'s value as a string literal.
//
// MVP blob format:
//   Real Apache Fory C++ serialization is deferred (plugin-abi.md §"Step-3
//   deferral", plan #225 dispatch §"Trickiest part: option (a)"). The emitted
//   blob uses a minimal hand-written length-prefixed format sufficient for the
//   loader's MVP fallback path.  The format is intentionally simple and
//   documented here so it can be swapped for real Fory once #231 lands.
//
//   Each string is encoded as:
//     uint16_t len   (big-endian for determinism)
//     uint8_t  data[len]
//   The manifest blob is:
//     [name-str][version-major u16be][version-minor u16be][version-patch u16be]
//     [abi_hash-str][num_deps u16be]([dep-str]...)
//   where u16be means two bytes, big-endian.
//
// PHILOSOPHY §11: libc++ stdlib is canonical. Tools are one-shot CLIs —
//   plain std::string / std::vector (no PMR).

#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "glibre/error.hpp"

namespace glibre::tools::foryc {

// ---------------------------------------------------------------------------
// SemVer — semantic version triple used in the manifest spec
// ---------------------------------------------------------------------------

struct ManifestSemVer {
    std::uint16_t major{0};
    std::uint16_t minor{0};
    std::uint16_t patch{0};
};

// ---------------------------------------------------------------------------
// PluginManifestSpec — input data for the manifest emitter
//
// This is the codegen-side view of the manifest; it is populated by the
// foryc pipeline from plugin.fory (via parse_file) and does not carry Fory
// deserialization logic.  The loader's in-memory PluginManifest
// (core/include/glibre/core/plugin_manifest.hpp) is populated by the
// deserializer at load time.
//
// Fields match plugin-abi.md §"Plugin Manifest Schema" (subset used by
// the emitter for the MVP blob):
//   name               — fully-qualified plugin id (e.g. "glibre.render")
//   version            — plugin SemVer
//   abi_hash           — 64-char blake3 hex captured from the middleman
//   min_engine_version — minimum glibre-core SemVer
//   depends_on         — plugin names that must already be registered
//
// Richer fields (components, systems, passes, panels) are part of the full
// PluginManifest schema and are included in the blob in future iterations
// once the Fory codegen pipeline (#231) produces them.  The emitter
// currently serializes only the fields above; the loader's MVP blob parser
// reads exactly those fields.
// ---------------------------------------------------------------------------

struct PluginManifestSpec {
    std::string name;                     // tag 1 — e.g. "glibre.render"
    ManifestSemVer version{};             // tag 2
    std::string abi_hash;                 // tag 3 — 64-char blake3 hex
    ManifestSemVer min_engine_version{};  // tag 4
    std::vector<std::string> depends_on;  // tag 9
};

// ---------------------------------------------------------------------------
// emit_manifest — generate a manifest.cpp translation unit
//
// Given a PluginManifestSpec, returns the complete text of a C++ source file
// that:
//   1. Declares a `static constexpr uint8_t kManifestBytes[]` array in an
//      anonymous namespace, populated with the hand-serialized blob (see
//      §"MVP blob format" above).
//   2. Exports four C-ABI symbols in an `extern "C"` block:
//      - glibre_plugin_manifest      → const uint8_t* const pointer to blob
//      - glibre_plugin_manifest_size → sizeof(kManifestBytes)
//      - glibre_plugin_abi_hash      → const char* 64-char hex string literal
//                                      (plugin-abi.md §"Plugin file shape" item 3)
//      - glibre_plugin_name          → const char* string literal of spec.name
//
// The generated source compiles cleanly with:
//   clang++ -std=c++23 -fno-exceptions -fno-rtti -c manifest.cpp
//
// Error codes:
//   tools::Error::ForycSyntaxError — spec.name or spec.abi_hash is empty
// ---------------------------------------------------------------------------

[[nodiscard]] glibre::Result<std::string> emit_manifest(const PluginManifestSpec& spec) noexcept;

}  // namespace glibre::tools::foryc
