// SPDX-License-Identifier: Apache-2.0
// tools/foryc/src/emit_manifest.cpp
//
// Implementation of the manifest-emission pass for glibre-foryc (plan #225).
//
// See emit_manifest.hpp for the public API, design notes, and MVP blob format.
//
// The emitted TU is self-contained: it includes only <cstddef> and <cstdint>
// and has no engine dependencies.  This keeps the generated file compilable
// in any C++23 translation unit regardless of whether glibre-core or
// glibre-types is present — an important property for plugin builds that may
// not have the full engine headers on their include path.

#include "emit_manifest.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string_view>

#include <EASTL/string.h>
#include <EASTL/vector.h>

namespace glibre::tools::foryc {

namespace {

// ---------------------------------------------------------------------------
// Blob serialization helpers
//
// All multi-byte values are big-endian (deterministic across platforms).
// ---------------------------------------------------------------------------

static void push_u16be(eastl::vector<std::uint8_t>& buf, std::uint16_t v) noexcept {
    buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}

static void push_str(eastl::vector<std::uint8_t>& buf, const eastl::string& s) noexcept {
    // Length-prefixed string: uint16_t len (big-endian) followed by raw bytes.
    const auto len = static_cast<std::uint16_t>(s.size() <= 0xFFFFu ? s.size() : 0xFFFFu);
    push_u16be(buf, len);
    for (std::size_t i = 0; i < len; ++i)
        buf.push_back(static_cast<std::uint8_t>(s[i]));
}

// ---------------------------------------------------------------------------
// serialize_spec — build the MVP hand-serialized blob
//
// Format (see emit_manifest.hpp §"MVP blob format"):
//   [name-str][major u16be][minor u16be][patch u16be]
//   [abi_hash-str][num_deps u16be]([dep-str]...)
// ---------------------------------------------------------------------------

[[nodiscard]] static eastl::vector<std::uint8_t>
serialize_spec(const PluginManifestSpec& spec) noexcept {
    eastl::vector<std::uint8_t> buf;

    // Tag 1: name
    push_str(buf, spec.name);

    // Tag 2: version
    push_u16be(buf, spec.version.major);
    push_u16be(buf, spec.version.minor);
    push_u16be(buf, spec.version.patch);

    // Tag 3: abi_hash
    push_str(buf, spec.abi_hash);

    // Tag 4: min_engine_version
    push_u16be(buf, spec.min_engine_version.major);
    push_u16be(buf, spec.min_engine_version.minor);
    push_u16be(buf, spec.min_engine_version.patch);

    // Tag 9: depends_on list
    const auto num_deps = static_cast<std::uint16_t>(
        spec.depends_on.size() <= 0xFFFFu ? spec.depends_on.size() : 0xFFFFu
    );
    push_u16be(buf, num_deps);
    for (std::uint16_t i = 0; i < num_deps; ++i)
        push_str(buf, spec.depends_on[i]);

    return buf;
}

// ---------------------------------------------------------------------------
// bytes_to_hex_initializer — format a byte vector as a C++ hex byte
// initializer list.
//
// Output: "0x12, 0xAB, ..." (no surrounding braces; caller adds them).
// Empty vector → "0x00" (a zero sentinel so kManifestBytes is never empty,
// which avoids a zero-size array that would be a VLA in older standards).
// ---------------------------------------------------------------------------

[[nodiscard]] static eastl::string
bytes_to_hex_initializer(const eastl::vector<std::uint8_t>& bytes) noexcept {
    if (bytes.empty())
        return eastl::string{"0x00"};

    eastl::string out;
    // Each byte: "0xNN" = 4 chars; ", " between = 2 chars.
    out.reserve(bytes.size() * 6);

    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i > 0)
            out += ", ";
        // Format byte as 0xNN.
        const eastl::string hex_byte = eastl::string(std::format("0x{:02X}", bytes[i]).c_str());
        out += hex_byte;
    }
    return out;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// emit_manifest — public API
// ---------------------------------------------------------------------------

[[nodiscard]] glibre::Result<eastl::string> emit_manifest(const PluginManifestSpec& spec) noexcept {
    // Reject empty name or empty abi_hash — both are required for a valid manifest.
    // plugin-abi.md §"Plugin file shape" item 3: abi_hash must be a 64-char hex string.
    if (spec.name.empty() || spec.abi_hash.empty())
        return std::unexpected{glibre::Error{tools::Error::ForycSyntaxError}};

    // Serialize the manifest spec to the MVP blob.
    const auto blob = serialize_spec(spec);
    const eastl::string blob_init = bytes_to_hex_initializer(blob);

    // Sanitize the plugin name to a C string literal: escape backslashes and
    // double-quotes.
    eastl::string safe_name;
    safe_name.reserve(spec.name.size());
    for (const char c : spec.name) {
        if (c == '\\' || c == '"')
            safe_name += '\\';
        safe_name += c;
    }

    // The abi_hash is embedded as a string literal.
    // plugin-abi.md §"Plugin file shape" item 3: glibre_plugin_abi_hash is
    // `extern "C" const char*`, a 64-char blake3 hex string global captured
    // at plugin compile time from the middleman headers.
    // The full hex string is safe to embed directly (lowercase hex chars only).
    const eastl::string safe_abi_hash(spec.abi_hash.data(), spec.abi_hash.size());

    // Build the generated source text.
    eastl::string out;
    out.reserve(512 + blob_init.size());

    out += "// GENERATED FILE — do not edit by hand.\n";
    out += eastl::string(
        std::format("// Plugin: {}\n", std::string_view(spec.name.data(), spec.name.size())).c_str()
    );
    out += "// Generator: glibre-foryc (plan #225)\n";
    out += "//\n";
    out += "// Exports the four C-ABI symbols required by the glibre plugin loader\n";
    out += "// (reviews/decisions/plugin-abi.md §\"Plugin file shape\").\n";
    out += "//\n";
    out += "// plugin-abi.md §\"Plugin file shape\" item 3:\n";
    out += "//   glibre_plugin_abi_hash — extern \"C\" const char*, 64-char blake3 hex\n";
    out += "//   string global. The loader compares this via byte-string equality.\n";
    out += "//\n";
    out += "// MVP note: the manifest blob uses a hand-serialized length-prefixed\n";
    out += "// format (emit_manifest.hpp §\"MVP blob format\").  Real Apache Fory\n";
    out += "// serialization lands in plan #231.\n";
    out += "\n";
    out += "#include <cstddef>\n";
    out += "#include <cstdint>\n";
    out += "\n";
    out += "namespace {\n";
    out += "// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)\n";
    out += eastl::string(
        std::format(
            "constexpr uint8_t kManifestBytes[] = {{ {} }};\n",
            std::string_view(blob_init.data(), blob_init.size())
        )
            .c_str()
    );
    out += "}  // namespace\n";
    out += "\n";
    out += "extern \"C\" {\n";
    out += "\n";
    out += "// Pointer to the Fory-serialized PluginManifest blob in .rodata.\n";
    out += "// Type: const uint8_t* (non-const pointer; const-pointer has internal\n";
    out += "// linkage in C++ per §6.5[basic.link] and would be invisible to dlsym).\n";
    out += "// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)\n";
    out += "const uint8_t* glibre_plugin_manifest = kManifestBytes;\n";
    out += "\n";
    out += "\n";
    out += "// Byte length of the manifest blob.\n";
    out += "size_t glibre_plugin_manifest_size = sizeof(kManifestBytes);\n";
    out += "\n";
    out += "// 64-char blake3 hex ABI hash, captured at plugin compile time.\n";
    out += "// plugin-abi.md §\"Plugin file shape\" item 3: extern \"C\" const char*.\n";
    out += "// The loader performs byte-string equality against the host's\n";
    out += "// glibre_types_abi_hash() value (no numeric parsing required).\n";
    out += eastl::string(
        std::format(
            "const char* glibre_plugin_abi_hash = \"{}\";\n",
            std::string_view(safe_abi_hash.data(), safe_abi_hash.size())
        )
            .c_str()
    );
    out += "\n";
    out += "// Fully-qualified plugin name as a C-string literal.\n";
    out += eastl::string(
        std::format(
            "const char* glibre_plugin_name = \"{}\";\n",
            std::string_view(safe_name.data(), safe_name.size())
        )
            .c_str()
    );
    out += "\n";
    out += "}  // extern \"C\"\n";

    return out;
}

}  // namespace glibre::tools::foryc
