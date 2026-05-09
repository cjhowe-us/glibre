// SPDX-License-Identifier: Apache-2.0
// tools/foryc/src/emit_header.cpp
//
// Implementation of the header-emission pass for glibre-foryc (plan #220).
//
// See emit_header.hpp for the public API and design notes.

#include "emit_header.hpp"

#include <algorithm>
#include <format>
#include <string_view>

#include <EASTL/string.h>
#include <EASTL/vector.h>

#include "builtin_map.hpp"

namespace glibre::tools::foryc {

namespace {

// -----------------------------------------------------------------------
// Builtin type lookup — delegates to builtin_map.hpp (single source of
// truth shared with parser.cpp).  Generic types (list<T>, map<K,V>,
// option<T>) are handled separately in map_generic_to_cpp().
// -----------------------------------------------------------------------

// Returns the BuiltinEntry* for a scalar type, or nullptr if not found.
[[nodiscard]] static const BuiltinEntry* lookup_scalar(std::string_view base) noexcept {
    for (const auto& e : k_builtin_scalar_map) {
        if (e.fory == base)
            return &e;
    }
    return nullptr;
}

// -----------------------------------------------------------------------
// map_generic_to_cpp — handle list<T>, map<K,V>, option<T>
//
// The inner type(s) are recursively translated via map_builtin_to_cpp_impl.
// The needs_builtins flag is propagated upward via the out-parameter.
// -----------------------------------------------------------------------

// Forward declaration.
[[nodiscard]] glibre::Result<eastl::string>
map_builtin_to_cpp_impl(std::string_view fory_type, bool& needs_builtins) noexcept;

// Split "A,B" → {"A", "B"}.  Returns false if the split is ambiguous
// (nested generics with commas — not supported in v1).
[[nodiscard]] static bool split_two(
    std::string_view params,
    std::string_view& out_a,
    std::string_view& out_b) noexcept
{
    // Find the top-level comma (depth 0 in angle brackets).
    int depth = 0;
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (params[i] == '<') ++depth;
        else if (params[i] == '>') --depth;
        else if (params[i] == ',' && depth == 0) {
            out_a = params.substr(0, i);
            out_b = params.substr(i + 1);
            return true;
        }
    }
    return false;
}

// Trim leading/trailing whitespace from a string_view.
[[nodiscard]] static std::string_view trim(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t')) sv.remove_suffix(1);
    return sv;
}

[[nodiscard]] static glibre::Result<eastl::string>
map_generic_to_cpp(
    std::string_view base,
    std::string_view params,
    bool& needs_builtins) noexcept
{
    if (base == "list") {
        // list<T> → eastl::vector<T_cpp>
        auto inner = map_builtin_to_cpp_impl(trim(params), needs_builtins);
        if (!inner)
            return std::unexpected{inner.error()};
        eastl::string out{"eastl::vector<"};
        out += *inner;
        out += '>';
        return out;
    }
    if (base == "option") {
        // option<T> → eastl::optional<T_cpp>
        auto inner = map_builtin_to_cpp_impl(trim(params), needs_builtins);
        if (!inner)
            return std::unexpected{inner.error()};
        eastl::string out{"eastl::optional<"};
        out += *inner;
        out += '>';
        return out;
    }
    if (base == "map") {
        // map<K,V> → eastl::unordered_map<K_cpp, V_cpp>
        std::string_view ka, vb;
        if (!split_two(params, ka, vb))
            return std::unexpected{glibre::Error{tools::Error::ForycSyntaxError}};
        auto key = map_builtin_to_cpp_impl(trim(ka), needs_builtins);
        if (!key)
            return std::unexpected{key.error()};
        auto val = map_builtin_to_cpp_impl(trim(vb), needs_builtins);
        if (!val)
            return std::unexpected{val.error()};
        eastl::string out{"eastl::unordered_map<"};
        out += *key;
        out += ", ";
        out += *val;
        out += '>';
        return out;
    }
    // Unknown generic base
    return std::unexpected{glibre::Error{tools::Error::ForycUnknownType}};
}

// -----------------------------------------------------------------------
// map_builtin_to_cpp_impl — core translation (called recursively)
//
// Sets needs_builtins to true if the resolved type requires
// #include <glibre/types/_builtins.hpp>.
// -----------------------------------------------------------------------

[[nodiscard]] glibre::Result<eastl::string>
map_builtin_to_cpp_impl(std::string_view fory_type, bool& needs_builtins) noexcept {
    // Strip surrounding whitespace (robustness for recursive calls).
    fory_type = trim(fory_type);

    // Generic type: find the '<' delimiter.
    const std::size_t lt = fory_type.find('<');
    if (lt != std::string_view::npos) {
        const std::string_view base = fory_type.substr(0, lt);
        // params is everything between < and the matching >
        if (fory_type.back() != '>')
            return std::unexpected{glibre::Error{tools::Error::ForycSyntaxError}};
        const std::string_view params = fory_type.substr(lt + 1, fory_type.size() - lt - 2);
        return map_generic_to_cpp(base, params, needs_builtins);
    }

    // Scalar lookup
    const BuiltinEntry* entry = lookup_scalar(fory_type);
    if (!entry)
        return std::unexpected{glibre::Error{tools::Error::ForycUnknownType}};
    if (entry->needs_builtins_include)
        needs_builtins = true;
    return eastl::string{entry->cpp.data(), entry->cpp.size()};
}

// -----------------------------------------------------------------------
// FQN decomposition helpers
//
// "glibre.core.Transform" →
//   namespace_parts = ["glibre", "core"]
//   type_name       = "Transform"
// -----------------------------------------------------------------------

struct FqnParts {
    eastl::vector<eastl::string> namespaces;  // all but the last component
    eastl::string type_name;                  // last component
};

[[nodiscard]] static FqnParts decompose_fqn(const eastl::string& fqn) noexcept {
    FqnParts parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = fqn.find('.', start);
        if (dot == eastl::string::npos) {
            parts.type_name = fqn.substr(start);
            break;
        }
        parts.namespaces.push_back(fqn.substr(start, dot - start));
        start = dot + 1;
    }
    return parts;
}

// -----------------------------------------------------------------------
// emit_type_decl — emit one struct for a TypeDecl
//
// Sets needs_builtins to true if any field type requires _builtins.hpp.
// -----------------------------------------------------------------------

[[nodiscard]] static glibre::Result<eastl::string>
emit_type_decl(const TypeDecl& td, bool& needs_builtins) noexcept {
    // Sort fields by tag (ascending) — ABI rule 1.
    eastl::vector<const FieldDecl*> sorted;
    sorted.reserve(td.fields.size());
    for (const auto& f : td.fields)
        sorted.push_back(&f);
    std::sort(sorted.begin(), sorted.end(), [](const FieldDecl* a, const FieldDecl* b) {
        return a->tag < b->tag;
    });

    const FqnParts fqn = decompose_fqn(td.fqn);

    eastl::string out;

    // Open namespaces.
    for (const auto& ns : fqn.namespaces) {
        out += "namespace ";
        out += ns;
        out += " {\n";
    }
    if (!fqn.namespaces.empty())
        out += "\n";

    // Struct comment with version via std::format (MED-6).
    out += eastl::string(
        std::format("// Generated by glibre-foryc from schema {} v{}\n",
            std::string_view(td.fqn.data(), td.fqn.size()),
            td.version).c_str()
    );
    out += "// TODO(#222): insert ABI hash here once plan #222 lands.\n";

    // Struct declaration — ABI rule 2: final, no virtuals, = default ctor.
    out += "struct ";
    out += fqn.type_name;
    out += " final {\n";

    // Fields (tag-sorted).
    for (const FieldDecl* fd : sorted) {
        const std::string_view tn(fd->type_name.data(), fd->type_name.size());
        auto cpp_type = map_builtin_to_cpp_impl(tn, needs_builtins);
        if (!cpp_type)
            return std::unexpected{cpp_type.error()};

        out += "    ";
        out += *cpp_type;
        out += " ";
        out += fd->name;
        out += "{};\n";
    }

    // Default ctor — ABI rule 2.
    out += "\n";
    out += "    ";
    out += fqn.type_name;
    out += "() = default;\n";
    out += "};\n";

    // Close namespaces (reverse order).
    if (!fqn.namespaces.empty()) {
        out += "\n";
        for (auto it = fqn.namespaces.rbegin(); it != fqn.namespaces.rend(); ++it) {
            out += "}  // namespace ";
            out += *it;
            out += "\n";
        }
    }

    return out;
}

}  // anonymous namespace

// -----------------------------------------------------------------------
// Public API — map_builtin_to_cpp
// -----------------------------------------------------------------------

[[nodiscard]] glibre::Result<eastl::string>
map_builtin_to_cpp(std::string_view fory_type) noexcept {
    bool ignored = false;
    return map_builtin_to_cpp_impl(fory_type, ignored);
}

// -----------------------------------------------------------------------
// Public API — emit_header
//
// Returns ForycEmptySchema if schema.types is empty — a schema file with
// zero TypeDecl blocks produces no useful output and likely indicates a
// malformed or migration-only .fory file.
// -----------------------------------------------------------------------

[[nodiscard]] glibre::Result<eastl::string>
emit_header(const Schema& schema) noexcept {
    // LOW-8: reject empty schema.
    if (schema.types.empty())
        return std::unexpected{glibre::Error{tools::Error::ForycEmptySchema}};

    // Two-pass: first emit all types so we know whether _builtins.hpp is
    // needed; then prepend the file-level preamble with the correct includes.
    bool needs_builtins = false;
    eastl::string types_text;

    for (const auto& td : schema.types) {
        if (td.fqn.empty())
            return std::unexpected{glibre::Error{tools::Error::ForycSyntaxError}};

        auto type_out = emit_type_decl(td, needs_builtins);
        if (!type_out)
            return std::unexpected{type_out.error()};

        types_text += *type_out;
        types_text += "\n";
    }

    // Build the preamble now that we know needs_builtins.
    eastl::string out;

    // File-level header comment.
    out += "// GENERATED FILE — do not edit by hand.\n";
    out += "// Source: ";
    out += schema.source_path;
    out += "\n";
    out += "// Generator: glibre-foryc (plan #220)\n";
    out += "// TODO(#222): ABI hash export not yet wired (plan #222).\n";
    out += "\n";
    out += "#pragma once\n";
    out += "\n";

    // Standard includes needed by the generated struct types.
    out += "#include <cstdint>\n";
    out += "#include <cstddef>\n";  // std::byte

    // Conditionally include _builtins.hpp (HIGH-2).
    if (needs_builtins) {
        out += "\n";
        out += "#include <glibre/types/_builtins.hpp>\n";
    }

    out += "\n";
    out += "#include <EASTL/optional.h>\n";
    out += "#include <EASTL/string.h>\n";
    out += "#include <EASTL/unordered_map.h>\n";
    out += "#include <EASTL/vector.h>\n";
    out += "\n";

    out += types_text;

    return out;
}

}  // namespace glibre::tools::foryc
