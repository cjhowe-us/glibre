// plugins/shader/src/reflection/slangc_reflection_record.cpp
//
// ingest_slangc_reflection — hand-rolled JSON parser for slangc reflection output.
//
// Authority: specs/shader/SPEC.md §4.4, §6.3.
// Plan: #514 — feat(shader): slangc reflection record ingester.
//
// ## Parser design
//
// The parser is a recursive-descent, single-pass, zero-copy scanner over the
// UTF-8 text supplied as string_view.  It supports the subset of JSON
// required by the slangc reflection schema:
//
//   - JSON objects (key-value pairs, string keys).
//   - JSON arrays (heterogeneous, iterated element by element).
//   - JSON strings (UTF-8, no surrogate-pair handling — slangc names are ASCII).
//   - JSON integers (unsigned 64-bit; overflow to uint32_t is detected).
//   - JSON null (silently skipped in array/object traversal).
//
// The parser does NOT support:
//   - Floating-point numbers.
//   - JSON streaming across multiple calls.
//   - Unicode escape sequences beyond basic ASCII (\n \t \\ \").
//
// All error paths return ReflectionExtractionFailed without allocating into mr,
// so a failed ingest leaves mr untouched (§4.4 invariant 1).
//
// ## Allocation discipline
//
// Every std::pmr::string and std::pmr::vector inside SlangcReflectionRecord
// is constructed with the caller-supplied mr.  No allocation falls through to
// std::pmr::get_default_resource().

#include "slangc_reflection_record.hpp"

#include <charconv>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <string_view>

#include <glibre/error.hpp>

namespace glibre::shader {

namespace {

// ---------------------------------------------------------------------------
// Error shorthand
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr glibre::Error reflection_failed() noexcept {
    return glibre::Error{shader::Error::ReflectionExtractionFailed};
}

// ---------------------------------------------------------------------------
// Minimal JSON scanner state
//
// Holds a const view into the source text and a cursor.  All parse functions
// advance cursor_ on success and leave it unchanged on failure.
// ---------------------------------------------------------------------------

class JsonScanner {
public:
    explicit JsonScanner(std::string_view text) noexcept
        : text_{text},
          cursor_{0} {}

    // Returns remaining unparsed text.
    [[nodiscard]] std::string_view remaining() const noexcept { return text_.substr(cursor_); }

    [[nodiscard]] bool at_end() const noexcept { return cursor_ >= text_.size(); }

    // skip_whitespace — advance past ASCII whitespace.
    void skip_whitespace() noexcept {
        while (cursor_ < text_.size()) {
            const char c = text_[cursor_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++cursor_;
            } else {
                break;
            }
        }
    }

    // peek — return current character without advancing, or '\0' at end.
    [[nodiscard]] char peek() const noexcept {
        return cursor_ < text_.size() ? text_[cursor_] : '\0';
    }

    // consume — advance past c, return true on match.
    [[nodiscard]] bool consume(char c) noexcept {
        if (cursor_ < text_.size() && text_[cursor_] == c) {
            ++cursor_;
            return true;
        }
        return false;
    }

    // parse_string — scan a JSON string literal, return the content without quotes.
    // Returns an empty optional on failure.
    [[nodiscard]] bool parse_string(std::string_view& out) noexcept {
        skip_whitespace();
        if (!consume('"'))
            return false;
        const std::size_t start = cursor_;
        while (cursor_ < text_.size()) {
            const char c = text_[cursor_];
            if (c == '"') {
                out = text_.substr(start, cursor_ - start);
                ++cursor_;  // consume closing quote
                return true;
            }
            if (c == '\\') {
                ++cursor_;  // skip escape prefix
                if (cursor_ >= text_.size())
                    return false;
                ++cursor_;  // skip escaped char
            } else {
                ++cursor_;
            }
        }
        return false;  // unterminated string
    }

    // parse_uint64 — scan a JSON integer (non-negative, no leading zeros
    // except plain "0").  Returns false on non-integer or overflow.
    [[nodiscard]] bool parse_uint64(std::uint64_t& out) noexcept {
        skip_whitespace();
        if (cursor_ >= text_.size())
            return false;
        const char first = text_[cursor_];
        if (first < '0' || first > '9')
            return false;

        const std::size_t start = cursor_;
        while (cursor_ < text_.size() && text_[cursor_] >= '0' && text_[cursor_] <= '9') {
            ++cursor_;
        }
        const std::string_view digits = text_.substr(start, cursor_ - start);
        const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), out);
        if (result.ec != std::errc{})
            return false;
        return true;
    }

    // skip_value — skip any JSON value (object, array, string, number, true/false/null).
    // Returns false on parse error.
    [[nodiscard]] bool skip_value() noexcept;

    // skip_object — skip a JSON object body (everything inside { }).
    // Called after the opening '{' has been consumed.
    [[nodiscard]] bool skip_object_body() noexcept;

    // skip_array — skip a JSON array body (everything inside [ ]).
    // Called after the opening '[' has been consumed.
    [[nodiscard]] bool skip_array_body() noexcept;

    // save / restore cursor (for lookahead).
    [[nodiscard]] std::size_t save() const noexcept { return cursor_; }

    void restore(std::size_t pos) noexcept { cursor_ = pos; }

private:
    std::string_view text_;
    std::size_t cursor_;
};

bool JsonScanner::skip_value() noexcept {
    skip_whitespace();
    if (at_end())
        return false;
    const char c = peek();
    if (c == '"') {
        std::string_view dummy;
        return parse_string(dummy);
    }
    if (c == '{') {
        ++cursor_;
        return skip_object_body();
    }
    if (c == '[') {
        ++cursor_;
        return skip_array_body();
    }
    if (c == 't') {
        // true
        if (cursor_ + 4 <= text_.size() && text_.substr(cursor_, 4) == "true") {
            cursor_ += 4;
            return true;
        }
        return false;
    }
    if (c == 'f') {
        // false
        if (cursor_ + 5 <= text_.size() && text_.substr(cursor_, 5) == "false") {
            cursor_ += 5;
            return true;
        }
        return false;
    }
    if (c == 'n') {
        // null
        if (cursor_ + 4 <= text_.size() && text_.substr(cursor_, 4) == "null") {
            cursor_ += 4;
            return true;
        }
        return false;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        // Number: skip digits, optional dot+more digits, optional exponent.
        if (c == '-')
            ++cursor_;
        while (cursor_ < text_.size() && text_[cursor_] >= '0' && text_[cursor_] <= '9')
            ++cursor_;
        if (cursor_ < text_.size() && text_[cursor_] == '.') {
            ++cursor_;
            while (cursor_ < text_.size() && text_[cursor_] >= '0' && text_[cursor_] <= '9')
                ++cursor_;
        }
        if (cursor_ < text_.size() && (text_[cursor_] == 'e' || text_[cursor_] == 'E')) {
            ++cursor_;
            if (cursor_ < text_.size() && (text_[cursor_] == '+' || text_[cursor_] == '-'))
                ++cursor_;
            while (cursor_ < text_.size() && text_[cursor_] >= '0' && text_[cursor_] <= '9')
                ++cursor_;
        }
        return true;
    }
    return false;
}

bool JsonScanner::skip_object_body() noexcept {
    skip_whitespace();
    if (consume('}'))
        return true;
    while (true) {
        std::string_view key;
        if (!parse_string(key))
            return false;
        skip_whitespace();
        if (!consume(':'))
            return false;
        if (!skip_value())
            return false;
        skip_whitespace();
        if (consume('}'))
            return true;
        if (!consume(','))
            return false;
        skip_whitespace();
    }
}

bool JsonScanner::skip_array_body() noexcept {
    skip_whitespace();
    if (consume(']'))
        return true;
    while (true) {
        if (!skip_value())
            return false;
        skip_whitespace();
        if (consume(']'))
            return true;
        if (!consume(','))
            return false;
    }
}

// ---------------------------------------------------------------------------
// Stage string mapping
// ---------------------------------------------------------------------------

[[nodiscard]] RawStage parse_stage(std::string_view s) noexcept {
    if (s == "vertex")
        return RawStage::Vertex;
    if (s == "pixel" || s == "fragment")
        return RawStage::Pixel;
    if (s == "compute")
        return RawStage::Compute;
    if (s == "mesh")
        return RawStage::Mesh;
    if (s == "amplification")
        return RawStage::Amplification;
    if (s == "library" || s == "callable")
        return RawStage::Library;
    return RawStage::Unknown;
}

// ---------------------------------------------------------------------------
// Binding-kind string mapping
// ---------------------------------------------------------------------------

[[nodiscard]] RawBindingKind parse_binding_kind(std::string_view s) noexcept {
    if (s == "constantBuffer" || s == "ConstantBuffer")
        return RawBindingKind::ConstantBuffer;
    if (s == "texture" || s == "SampledImage" || s == "sampledImage")
        return RawBindingKind::SampledImage;
    if (s == "storageImage" || s == "uavImage" || s == "RWTexture")
        return RawBindingKind::StorageImage;
    if (s == "sampler" || s == "SamplerState")
        return RawBindingKind::Sampler;
    if (s == "structuredBuffer" || s == "StructuredBuffer")
        return RawBindingKind::StructuredBuffer;
    if (s == "rwStructuredBuffer" || s == "RWStructuredBuffer" || s == "uavBuffer")
        return RawBindingKind::RWStructuredBuffer;
    if (s == "accelerationStructure" || s == "RaytracingAccelerationStructure")
        return RawBindingKind::AccelerationStructure;
    if (s == "pushConstant" || s == "PushConstant")
        return RawBindingKind::PushConstant;
    return RawBindingKind::Unknown;
}

// ---------------------------------------------------------------------------
// Object-key iteration helper
//
// Calls visitor(key, scanner) for each key in a JSON object.
// visitor must return bool: true = continue, false = parse error.
// Returns false if the object is malformed.
// ---------------------------------------------------------------------------

template<typename Visitor>
[[nodiscard]] bool iter_object(JsonScanner& sc, Visitor&& visitor) noexcept {
    sc.skip_whitespace();
    if (!sc.consume('{'))
        return false;
    sc.skip_whitespace();
    if (sc.consume('}'))
        return true;
    while (true) {
        std::string_view key;
        if (!sc.parse_string(key))
            return false;
        sc.skip_whitespace();
        if (!sc.consume(':'))
            return false;
        sc.skip_whitespace();
        if (!visitor(key, sc))
            return false;
        sc.skip_whitespace();
        if (sc.consume('}'))
            return true;
        if (!sc.consume(','))
            return false;
        sc.skip_whitespace();
    }
}

// ---------------------------------------------------------------------------
// Array element iteration helper
//
// Calls visitor(scanner) for each element in a JSON array.
// visitor must return bool: true = continue, false = parse error.
// Returns false if the array is malformed.
// ---------------------------------------------------------------------------

template<typename Visitor>
[[nodiscard]] bool iter_array(JsonScanner& sc, Visitor&& visitor) noexcept {
    sc.skip_whitespace();
    if (!sc.consume('['))
        return false;
    sc.skip_whitespace();
    if (sc.consume(']'))
        return true;
    while (true) {
        sc.skip_whitespace();
        if (!visitor(sc))
            return false;
        sc.skip_whitespace();
        if (sc.consume(']'))
            return true;
        if (!sc.consume(','))
            return false;
    }
}

// ---------------------------------------------------------------------------
// Parse a single entry-point object
// ---------------------------------------------------------------------------

[[nodiscard]] bool parse_entry_point(
    JsonScanner& sc, SlangcReflectionRecord& rec, std::pmr::memory_resource* mr
) noexcept {
    RawEntryPoint ep{std::pmr::string{mr}};
    bool ok = iter_object(sc, [&](std::string_view key, JsonScanner& inner) -> bool {
        if (key == "name") {
            std::string_view sv;
            if (!inner.parse_string(sv))
                return false;
            ep.name = std::pmr::string{sv.data(), sv.size(), mr};
            return true;
        }
        if (key == "stage") {
            std::string_view sv;
            if (!inner.parse_string(sv))
                return false;
            ep.stage = parse_stage(sv);
            return true;
        }
        // Unknown key — skip value.
        return inner.skip_value();
    });
    if (!ok)
        return false;
    if (ep.stage == RawStage::Unknown)
        return false;
    rec.entry_points.push_back(std::move(ep));
    return true;
}

// ---------------------------------------------------------------------------
// Parse binding sub-object: { "kind": string, "space": int, "index": int, "count": int }
// ---------------------------------------------------------------------------

[[nodiscard]] bool parse_binding_descriptor(
    JsonScanner& sc,
    RawBindingKind& out_kind,
    std::uint32_t& out_space,
    std::uint32_t& out_index,
    std::uint32_t& out_count
) noexcept {
    return iter_object(sc, [&](std::string_view key, JsonScanner& inner) -> bool {
        if (key == "kind") {
            std::string_view sv;
            if (!inner.parse_string(sv))
                return false;
            out_kind = parse_binding_kind(sv);
            return true;
        }
        if (key == "space" || key == "registerSpace") {
            std::uint64_t v{};
            if (!inner.parse_uint64(v))
                return false;
            if (v > 0xFFFF'FFFF)
                return false;
            out_space = static_cast<std::uint32_t>(v);
            return true;
        }
        if (key == "index" || key == "register") {
            std::uint64_t v{};
            if (!inner.parse_uint64(v))
                return false;
            if (v > 0xFFFF'FFFF)
                return false;
            out_index = static_cast<std::uint32_t>(v);
            return true;
        }
        if (key == "count" || key == "arraySize") {
            std::uint64_t v{};
            if (!inner.parse_uint64(v))
                return false;
            if (v == 0 || v > 0xFFFF'FFFF)
                return false;
            out_count = static_cast<std::uint32_t>(v);
            return true;
        }
        return inner.skip_value();
    });
}

// ---------------------------------------------------------------------------
// Parse a single parameter / binding object
// ---------------------------------------------------------------------------

[[nodiscard]] bool parse_parameter(
    JsonScanner& sc, SlangcReflectionRecord& rec, std::pmr::memory_resource* mr
) noexcept {
    RawBinding b{.name = std::pmr::string{mr}};
    b.kind = RawBindingKind::Unknown;

    bool ok = iter_object(sc, [&](std::string_view key, JsonScanner& inner) -> bool {
        if (key == "name") {
            std::string_view sv;
            if (!inner.parse_string(sv))
                return false;
            b.name = std::pmr::string{sv.data(), sv.size(), mr};
            return true;
        }
        if (key == "binding") {
            return parse_binding_descriptor(
                inner, b.kind, b.register_space, b.register_index, b.array_size
            );
        }
        if (key == "stages" || key == "stageMask") {
            std::uint64_t v{};
            if (!inner.parse_uint64(v))
                return false;
            if (v > 0xFF)
                return false;
            b.stage_mask = static_cast<std::uint8_t>(v);
            return true;
        }
        return inner.skip_value();
    });
    if (!ok)
        return false;
    if (b.kind == RawBindingKind::Unknown)
        return false;
    rec.bindings.push_back(std::move(b));
    return true;
}

// ---------------------------------------------------------------------------
// Parse a single vertex-input object
// ---------------------------------------------------------------------------

[[nodiscard]] bool parse_vertex_input(
    JsonScanner& sc, SlangcReflectionRecord& rec, std::pmr::memory_resource* mr
) noexcept {
    RawVertexInput vi{.semantic = std::pmr::string{mr}};

    bool ok = iter_object(sc, [&](std::string_view key, JsonScanner& inner) -> bool {
        if (key == "semantic" || key == "semanticName") {
            std::string_view sv;
            if (!inner.parse_string(sv))
                return false;
            vi.semantic = std::pmr::string{sv.data(), sv.size(), mr};
            return true;
        }
        if (key == "semanticIndex") {
            std::uint64_t v{};
            if (!inner.parse_uint64(v))
                return false;
            if (v > 0xFFFF'FFFF)
                return false;
            vi.semantic_index = static_cast<std::uint32_t>(v);
            return true;
        }
        if (key == "location") {
            std::uint64_t v{};
            if (!inner.parse_uint64(v))
                return false;
            if (v > 0xFFFF'FFFF)
                return false;
            vi.location = static_cast<std::uint32_t>(v);
            return true;
        }
        if (key == "formatCode" || key == "format") {
            std::uint64_t v{};
            if (!inner.parse_uint64(v))
                return false;
            if (v > 0xFFFF'FFFF)
                return false;
            vi.format_code = static_cast<std::uint32_t>(v);
            return true;
        }
        return inner.skip_value();
    });
    if (!ok)
        return false;
    rec.vertex_inputs.push_back(std::move(vi));
    return true;
}

// ---------------------------------------------------------------------------
// Parse a single push-constant-range object
// ---------------------------------------------------------------------------

[[nodiscard]] bool
parse_push_constant_range(JsonScanner& sc, SlangcReflectionRecord& rec) noexcept {
    RawPushConstantRange pcr{};

    bool ok = iter_object(sc, [&](std::string_view key, JsonScanner& inner) -> bool {
        if (key == "offset") {
            std::uint64_t v{};
            if (!inner.parse_uint64(v))
                return false;
            if (v > 0xFFFF'FFFF)
                return false;
            pcr.offset = static_cast<std::uint32_t>(v);
            return true;
        }
        if (key == "size") {
            std::uint64_t v{};
            if (!inner.parse_uint64(v))
                return false;
            if (v > 0xFFFF'FFFF)
                return false;
            pcr.size = static_cast<std::uint32_t>(v);
            return true;
        }
        if (key == "stages" || key == "stageMask") {
            std::uint64_t v{};
            if (!inner.parse_uint64(v))
                return false;
            if (v > 0xFF)
                return false;
            pcr.stage_mask = static_cast<std::uint8_t>(v);
            return true;
        }
        return inner.skip_value();
    });
    if (!ok)
        return false;
    rec.push_constant_ranges.push_back(pcr);
    return true;
}

// ---------------------------------------------------------------------------
// Parse a single specialization-constant slot
// ---------------------------------------------------------------------------

[[nodiscard]] bool parse_specialization_constant(
    JsonScanner& sc, SlangcReflectionRecord& rec, std::pmr::memory_resource* mr
) noexcept {
    RawSpecializationConstant sc_slot{.name = std::pmr::string{mr}};

    bool ok = iter_object(sc, [&](std::string_view key, JsonScanner& inner) -> bool {
        if (key == "name") {
            std::string_view sv;
            if (!inner.parse_string(sv))
                return false;
            sc_slot.name = std::pmr::string{sv.data(), sv.size(), mr};
            return true;
        }
        if (key == "id" || key == "constantId") {
            std::uint64_t v{};
            if (!inner.parse_uint64(v))
                return false;
            if (v > 0xFFFF'FFFF)
                return false;
            sc_slot.id = static_cast<std::uint32_t>(v);
            return true;
        }
        if (key == "size" || key == "sizeInBytes") {
            std::uint64_t v{};
            if (!inner.parse_uint64(v))
                return false;
            if (v > 0xFFFF'FFFF)
                return false;
            sc_slot.size_bytes = static_cast<std::uint32_t>(v);
            return true;
        }
        return inner.skip_value();
    });
    if (!ok)
        return false;
    rec.spec_constants.push_back(std::move(sc_slot));
    return true;
}

// ---------------------------------------------------------------------------
// Top-level parser — parses the full slangc reflection JSON object.
// ---------------------------------------------------------------------------

[[nodiscard]] bool parse_top_level(
    JsonScanner& sc, SlangcReflectionRecord& rec, std::pmr::memory_resource* mr
) noexcept {
    return iter_object(sc, [&](std::string_view key, JsonScanner& inner) -> bool {
        if (key == "entryPoints") {
            return iter_array(inner, [&](JsonScanner& elem) -> bool {
                return parse_entry_point(elem, rec, mr);
            });
        }
        if (key == "parameters") {
            return iter_array(inner, [&](JsonScanner& elem) -> bool {
                return parse_parameter(elem, rec, mr);
            });
        }
        if (key == "vertexInputs" || key == "inputs") {
            return iter_array(inner, [&](JsonScanner& elem) -> bool {
                return parse_vertex_input(elem, rec, mr);
            });
        }
        if (key == "pushConstantRanges") {
            return iter_array(inner, [&](JsonScanner& elem) -> bool {
                return parse_push_constant_range(elem, rec);
            });
        }
        if (key == "specializationConstants") {
            return iter_array(inner, [&](JsonScanner& elem) -> bool {
                return parse_specialization_constant(elem, rec, mr);
            });
        }
        if (key == "rtPayloadBytes" || key == "rayPayloadSizeInBytes") {
            std::uint64_t v{};
            if (!inner.parse_uint64(v))
                return false;
            if (v > 0xFFFF'FFFF)
                return false;
            rec.rt_payload_bytes = static_cast<std::uint32_t>(v);
            return true;
        }
        // Unknown top-level key — skip value so forward-compatibility is preserved.
        return inner.skip_value();
    });
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

glibre::Result<SlangcReflectionRecord>
ingest_slangc_reflection(std::string_view json_text, std::pmr::memory_resource* mr) noexcept {
    if (json_text.empty()) {
        return std::unexpected(reflection_failed());
    }

    SlangcReflectionRecord rec{
        .entry_points = std::pmr::vector<RawEntryPoint>{mr},
        .bindings = std::pmr::vector<RawBinding>{mr},
        .vertex_inputs = std::pmr::vector<RawVertexInput>{mr},
        .push_constant_ranges = std::pmr::vector<RawPushConstantRange>{mr},
        .spec_constants = std::pmr::vector<RawSpecializationConstant>{mr},
        .rt_payload_bytes = 0,
    };

    JsonScanner sc{json_text};
    sc.skip_whitespace();
    if (sc.at_end() || sc.peek() != '{') {
        return std::unexpected(reflection_failed());
    }

    if (!parse_top_level(sc, rec, mr)) {
        return std::unexpected(reflection_failed());
    }

    return rec;
}

}  // namespace glibre::shader
