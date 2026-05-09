// SPDX-License-Identifier: Apache-2.0
// tools/foryc/src/parser.cpp
//
// Minimal recursive-descent parser for the .fory schema language.
// See parser.hpp for the grammar subset description and design notes.

#include "parser.hpp"

#include <cctype>
#include <charconv>
#include <cstring>
#include <fstream>
#include <sstream>

#include <EASTL/unordered_set.h>

namespace glibre::tools::foryc {

namespace {

// -----------------------------------------------------------------------
// Lexer
// -----------------------------------------------------------------------

enum class TokenKind {
    Ident,      // bareword identifier or keyword
    StringLit,  // "..."
    IntLit,     // decimal integer
    LBrace,     // {
    RBrace,     // }
    Colon,      // :
    Lt,         // <
    Gt,         // >
    Comma,      // ,
    Dot,        // .
    Eof,
    Error,  // lexer-level error
};

struct Token {
    TokenKind kind{TokenKind::Eof};
    std::string_view text{};
    std::size_t line{1};
};

// A zero-allocation lexer that scans a string_view in place.
class Lexer {
public:
    explicit Lexer(std::string_view src)
        : src_{src} {}

    Token next() {
        skip_whitespace_and_comments();
        if (pos_ >= src_.size())
            return make(TokenKind::Eof, "");

        const std::size_t ln = line_;
        const char c = src_[pos_];

        // Single-char tokens
        if (c == '{') {
            ++pos_;
            return make(TokenKind::LBrace, "{", ln);
        }
        if (c == '}') {
            ++pos_;
            return make(TokenKind::RBrace, "}", ln);
        }
        if (c == ':') {
            ++pos_;
            return make(TokenKind::Colon, ":", ln);
        }
        if (c == '<') {
            ++pos_;
            return make(TokenKind::Lt, "<", ln);
        }
        if (c == '>') {
            ++pos_;
            return make(TokenKind::Gt, ">", ln);
        }
        if (c == ',') {
            ++pos_;
            return make(TokenKind::Comma, ",", ln);
        }
        if (c == '.') {
            ++pos_;
            return make(TokenKind::Dot, ".", ln);
        }

        // String literal
        if (c == '"') {
            const std::size_t start = pos_++;
            while (pos_ < src_.size() && src_[pos_] != '"') {
                if (src_[pos_] == '\n')
                    ++line_;
                ++pos_;
            }
            if (pos_ >= src_.size())
                return make(TokenKind::Error, "unterminated string", ln);
            ++pos_;  // consume closing "
            return make(TokenKind::StringLit, src_.substr(start, pos_ - start), ln);
        }

        // Integer literal (decimal only)
        if (std::isdigit(static_cast<unsigned char>(c))) {
            const std::size_t start = pos_;
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_])))
                ++pos_;
            return make(TokenKind::IntLit, src_.substr(start, pos_ - start), ln);
        }

        // Identifier / keyword (includes '_', alpha, alphanumeric after first)
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            const std::size_t start = pos_;
            while (pos_ < src_.size()) {
                const char ch = src_[pos_];
                if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_')
                    break;
                ++pos_;
            }
            return make(TokenKind::Ident, src_.substr(start, pos_ - start), ln);
        }

        // Unknown character
        ++pos_;
        return make(TokenKind::Error, src_.substr(pos_ - 1, 1), ln);
    }

    [[nodiscard]] std::size_t current_line() const noexcept { return line_; }

private:
    void skip_whitespace_and_comments() {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == '\n') {
                ++line_;
                ++pos_;
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(c))) {
                ++pos_;
                continue;
            }
            // Line comment
            if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                while (pos_ < src_.size() && src_[pos_] != '\n')
                    ++pos_;
                continue;
            }
            break;
        }
    }

    Token make(TokenKind k, std::string_view text, std::size_t ln = 0) {
        return Token{k, text, ln ? ln : line_};
    }

    std::string_view src_;
    std::size_t pos_{0};
    std::size_t line_{1};
};

// -----------------------------------------------------------------------
// Parser
// -----------------------------------------------------------------------

// FORYC_ERR(code, detail_sv) — build a glibre::Error from a tools::Error
// enumerator, capturing __FILE__ and __LINE__ at the *call site* (not here).
// `detail_sv` must be a string literal (or similarly immortal storage)
// because ErrorContext.detail is eastl::string_view — it does not own the bytes.
// Every call site in this file passes a string literal, satisfying the
// lifetime requirement.
//
// This is intentionally a macro (not a static member function) so that
// __FILE__ / __LINE__ expand to the caller's location rather than to the
// definition site — a static function would always report parser.cpp:NNN.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define FORYC_ERR(code, detail_literal)                                                            \
    std::unexpected<glibre::Error> {                                                               \
        glibre::Error {                                                                            \
            (code), glibre::ErrorContext {                                                         \
                __FILE__, __LINE__, eastl::string_view {                                           \
                    std::string_view{detail_literal}.data(),                                       \
                        std::string_view{detail_literal}.size()                                    \
                }                                                                                  \
            }                                                                                      \
        }                                                                                          \
    }

// Returns true if type_name is in the builtins set (prefix match for
// generic types like list<T>, map<K,V>, option<T>).
// Uses k_builtin_scalar_map and k_generic_prefixes from builtin_map.hpp —
// the single source of truth shared with emit_header.cpp.
[[nodiscard]] static bool is_builtin(std::string_view type_name) noexcept {
    // Strip generic parameter if present: "list<T>" → "list"
    const std::size_t lt_pos = type_name.find('<');
    const std::string_view base =
        (lt_pos == std::string_view::npos) ? type_name : type_name.substr(0, lt_pos);

    // Check scalar builtins.
    for (const auto& entry : k_builtin_scalar_map) {
        if (entry.fory == base)
            return true;
    }
    // Check generic prefixes.
    for (const std::string_view g : k_generic_prefixes) {
        if (g == base)
            return true;
    }
    return false;
}

// Sentinel error type for parse_uint32 (not glibre::Error — purely internal).
struct ParseIntError {};

// Parse a uint32 from a string_view.
[[nodiscard]] static std::expected<std::uint32_t, ParseIntError>
parse_uint32(std::string_view sv) noexcept {
    std::uint32_t val{};
    const auto* first = sv.data();
    const auto* last = sv.data() + sv.size();
    const auto [ptr, ec] = std::from_chars(first, last, val);
    if (ec != std::errc{} || ptr != last)
        return std::unexpected{ParseIntError{}};
    return val;
}

class Parser {
public:
    Parser(std::string_view src, std::string_view virtual_path)
        : lexer_{src},
          virtual_path_{virtual_path} {
        advance();  // prime current_
    }

    [[nodiscard]] ParseResult parse() {
        Schema schema;
        schema.source_path = eastl::string(virtual_path_.data(), virtual_path_.size());

        while (current_.kind != TokenKind::Eof) {
            if (current_.kind == TokenKind::Error)
                return FORYC_ERR(tools::Error::ForycSyntaxError, "unexpected character");

            if (current_.kind != TokenKind::Ident)
                return FORYC_ERR(tools::Error::ForycSyntaxError, "expected keyword");

            if (current_.text == "schema") {
                advance();
                auto r = parse_schema_block();
                if (!r)
                    return std::unexpected{r.error()};
                schema.types.push_back(std::move(*r));
            } else if (current_.text == "migration") {
                // Top-level migration blocks (fory-codegen.md style: migration v2_to_v3 { ... })
                // are skipped.  Inline migration declarations INSIDE schema blocks are stored
                // (plan #221, parse_inline_migration).  The top-level form is kept for
                // backward compatibility with existing .fory files.
                advance();
                auto r = skip_migration_block();
                if (!r)
                    return std::unexpected{r.error()};
            } else {
                return FORYC_ERR(
                    tools::Error::ForycSyntaxError, "expected 'schema' or 'migration' at top level"
                );
            }
        }

        return schema;
    }

private:
    // Consume the current token and advance to the next.
    void advance() { current_ = lexer_.next(); }

    // Consume and return true if the current token matches kind+text.
    bool consume(TokenKind k, std::string_view text = {}) {
        if (current_.kind != k)
            return false;
        if (!text.empty() && current_.text != text)
            return false;
        advance();
        return true;
    }

    // Parse a dotted FQN: word ('.' word)* — may also consume as Ident+Dot tokens
    // Returns the concatenated string.
    [[nodiscard]] std::expected<eastl::string, glibre::Error> parse_fqn() {
        if (current_.kind != TokenKind::Ident)
            return FORYC_ERR(tools::Error::ForycSyntaxError, "expected schema FQN");

        eastl::string fqn(current_.text.data(), current_.text.size());
        advance();

        while (current_.kind == TokenKind::Dot) {
            advance();  // consume '.'
            if (current_.kind != TokenKind::Ident)
                return FORYC_ERR(tools::Error::ForycSyntaxError, "expected identifier after '.'");
            fqn += '.';
            fqn += eastl::string(current_.text.data(), current_.text.size());
            advance();
        }
        return fqn;
    }

    // Parse a type name, which may include generic parameters: foo<bar,baz>
    [[nodiscard]] std::expected<eastl::string, glibre::Error> parse_type_name() {
        if (current_.kind != TokenKind::Ident)
            return FORYC_ERR(tools::Error::ForycSyntaxError, "expected type name");

        eastl::string name(current_.text.data(), current_.text.size());
        advance();

        if (current_.kind == TokenKind::Lt) {
            // Generic parameter list: <T> or <K,V>
            name += '<';
            advance();  // consume '<'
            while (current_.kind != TokenKind::Gt && current_.kind != TokenKind::Eof) {
                if (current_.kind == TokenKind::Ident) {
                    name += eastl::string(current_.text.data(), current_.text.size());
                    advance();
                } else if (current_.kind == TokenKind::Comma) {
                    name += ',';
                    advance();
                } else {
                    return FORYC_ERR(tools::Error::ForycSyntaxError, "malformed generic type");
                }
            }
            if (current_.kind != TokenKind::Gt)
                return FORYC_ERR(tools::Error::ForycSyntaxError, "expected '>' to close generic");
            name += '>';
            advance();
        }
        return name;
    }

    // Parse:  schema <fqn> { version N [since "x.y.z"] [field ...] * }
    [[nodiscard]] std::expected<TypeDecl, glibre::Error> parse_schema_block() {
        // Parse FQN
        auto fqn_r = parse_fqn();
        if (!fqn_r)
            return std::unexpected{fqn_r.error()};

        if (!consume(TokenKind::LBrace))
            return FORYC_ERR(tools::Error::ForycSyntaxError, "expected '{' after schema FQN");

        TypeDecl decl;
        decl.fqn = std::move(*fqn_r);

        // Track seen tags for duplicate detection.
        eastl::unordered_set<std::uint32_t> seen_tags;
        bool version_seen = false;
        bool since_seen = false;

        while (current_.kind != TokenKind::RBrace && current_.kind != TokenKind::Eof) {
            if (current_.kind != TokenKind::Ident)
                return FORYC_ERR(tools::Error::ForycSyntaxError, "expected keyword inside schema");

            if (current_.text == "version") {
                if (version_seen)
                    return FORYC_ERR(
                        tools::Error::ForycSyntaxError, "duplicate 'version' key in schema block"
                    );
                advance();
                if (current_.kind != TokenKind::IntLit)
                    return FORYC_ERR(
                        tools::Error::ForycSyntaxError, "expected integer after 'version'"
                    );
                auto vr = parse_uint32(current_.text);
                if (!vr)
                    return FORYC_ERR(tools::Error::ForycSyntaxError, "invalid version integer");
                if (*vr == 0)
                    return FORYC_ERR(tools::Error::ForycNonMonotoneVersion, "version must be >= 1");
                decl.version = *vr;
                version_seen = true;
                advance();

            } else if (current_.text == "since") {
                if (since_seen)
                    return FORYC_ERR(
                        tools::Error::ForycSyntaxError, "duplicate 'since' key in schema block"
                    );
                advance();
                if (current_.kind != TokenKind::StringLit)
                    return FORYC_ERR(
                        tools::Error::ForycSyntaxError, "expected string after 'since'"
                    );
                // Strip surrounding quotes from the string literal.
                const std::string_view raw = current_.text;
                decl.since_version = eastl::string(raw.data() + 1, raw.size() - 2);
                since_seen = true;
                advance();

            } else if (current_.text == "field") {
                advance();
                auto fr = parse_field_decl();
                if (!fr)
                    return std::unexpected{fr.error()};
                // Validate: tag uniqueness
                if (!seen_tags.insert(fr->tag).second)
                    return FORYC_ERR(
                        tools::Error::ForycDuplicateTag, "duplicate tag number in schema"
                    );
                // Validate: type is a builtin (or a declared schema type).
                // For this plan, only builtins are accepted.
                const std::string_view tn(fr->type_name.data(), fr->type_name.size());
                if (!is_builtin(tn))
                    return FORYC_ERR(
                        tools::Error::ForycUnknownType, "field type is not in the builtins set"
                    );
                decl.fields.push_back(std::move(*fr));

            } else if (current_.text == "migration") {
                // Inline migration declaration inside a schema block (plan #221).
                // Syntax: migration from <N> to <M> calls "<provider>"
                advance();
                auto mr = parse_inline_migration();
                if (!mr)
                    return std::unexpected{mr.error()};
                decl.migrations.push_back(std::move(*mr));

            } else if (current_.text == "reserved") {
                // reserved <tag-number> — legal but no IR representation needed yet.
                advance();
                if (current_.kind != TokenKind::IntLit)
                    return FORYC_ERR(
                        tools::Error::ForycSyntaxError, "expected integer after 'reserved'"
                    );
                advance();

            } else {
                return FORYC_ERR(
                    tools::Error::ForycSyntaxError, "unknown keyword inside schema block"
                );
            }
        }

        if (!consume(TokenKind::RBrace))
            return FORYC_ERR(tools::Error::ForycSyntaxError, "expected '}' to close schema block");

        if (!version_seen)
            return FORYC_ERR(tools::Error::ForycSyntaxError, "schema block is missing 'version'");

        return decl;
    }

    // Parse an inline migration declaration (plan #221):
    //   from <N> to <M> calls "<provider>"
    // Called after the "migration" keyword has been consumed.
    [[nodiscard]] std::expected<MigrationDecl, glibre::Error> parse_inline_migration() {
        // "from"
        if (current_.kind != TokenKind::Ident || current_.text != "from")
            return FORYC_ERR(tools::Error::ForycSyntaxError, "expected 'from' in migration decl");
        advance();

        // <from_version>
        if (current_.kind != TokenKind::IntLit)
            return FORYC_ERR(
                tools::Error::ForycSyntaxError, "expected integer after 'from' in migration decl"
            );
        auto from_r = parse_uint32(current_.text);
        if (!from_r)
            return FORYC_ERR(
                tools::Error::ForycSyntaxError, "invalid from-version in migration decl"
            );
        const std::uint32_t from_ver = *from_r;
        advance();

        // "to"
        if (current_.kind != TokenKind::Ident || current_.text != "to")
            return FORYC_ERR(tools::Error::ForycSyntaxError, "expected 'to' in migration decl");
        advance();

        // <to_version>
        if (current_.kind != TokenKind::IntLit)
            return FORYC_ERR(
                tools::Error::ForycSyntaxError, "expected integer after 'to' in migration decl"
            );
        auto to_r = parse_uint32(current_.text);
        if (!to_r)
            return FORYC_ERR(
                tools::Error::ForycSyntaxError, "invalid to-version in migration decl"
            );
        const std::uint32_t to_ver = *to_r;
        advance();

        // "calls"
        if (current_.kind != TokenKind::Ident || current_.text != "calls")
            return FORYC_ERR(tools::Error::ForycSyntaxError, "expected 'calls' in migration decl");
        advance();

        // "<provider>" — a string literal; strip surrounding quotes.
        if (current_.kind != TokenKind::StringLit)
            return FORYC_ERR(
                tools::Error::ForycSyntaxError,
                "expected quoted provider symbol after 'calls' in migration decl"
            );
        const std::string_view raw = current_.text;
        // StringLit includes the surrounding quotes: raw[0] == '"', raw[last] == '"'.
        // Minimum valid StringLit is '""' (2 chars). A shorter token or an
        // empty provider string ("") is a syntax error.
        if (raw.size() < 3)
            return FORYC_ERR(
                tools::Error::ForycSyntaxError, "provider symbol must be a non-empty quoted string"
            );
        eastl::string provider(raw.data() + 1, raw.size() - 2);
        advance();

        MigrationDecl md;
        md.from_version = from_ver;
        md.to_version = to_ver;
        md.provider = std::move(provider);
        return md;
    }

    // Parse:  <name> : <type>   tag <integer>  [since <integer>]  [default {...}]
    [[nodiscard]] std::expected<FieldDecl, glibre::Error> parse_field_decl() {
        if (current_.kind != TokenKind::Ident)
            return FORYC_ERR(tools::Error::ForycSyntaxError, "expected field name");

        FieldDecl fd;
        fd.name = eastl::string(current_.text.data(), current_.text.size());
        advance();

        if (!consume(TokenKind::Colon))
            return FORYC_ERR(tools::Error::ForycSyntaxError, "expected ':' after field name");

        auto tn_r = parse_type_name();
        if (!tn_r)
            return std::unexpected{tn_r.error()};
        fd.type_name = std::move(*tn_r);

        // Expect "tag <integer>"
        if (current_.kind != TokenKind::Ident || current_.text != "tag")
            return FORYC_ERR(tools::Error::ForycSyntaxError, "expected 'tag' after field type");
        advance();

        if (current_.kind != TokenKind::IntLit)
            return FORYC_ERR(tools::Error::ForycSyntaxError, "expected integer after 'tag'");
        auto tag_r = parse_uint32(current_.text);
        if (!tag_r)
            return FORYC_ERR(tools::Error::ForycSyntaxError, "invalid tag integer");
        fd.tag = *tag_r;
        advance();

        // Optional "since <integer>"
        if (current_.kind == TokenKind::Ident && current_.text == "since") {
            advance();
            if (current_.kind != TokenKind::IntLit)
                return FORYC_ERR(
                    tools::Error::ForycSyntaxError, "expected integer after field 'since'"
                );
            auto sr = parse_uint32(current_.text);
            if (!sr)
                return FORYC_ERR(tools::Error::ForycSyntaxError, "invalid since integer");
            fd.since = *sr;
            advance();
        }

        // Optional "default { ... }" — consumed as a balanced brace block, ignored.
        if (current_.kind == TokenKind::Ident && current_.text == "default") {
            advance();
            if (!consume(TokenKind::LBrace))
                return FORYC_ERR(tools::Error::ForycSyntaxError, "expected '{' after 'default'");
            int depth = 1;
            while (depth > 0 && current_.kind != TokenKind::Eof) {
                if (current_.kind == TokenKind::LBrace)
                    ++depth;
                if (current_.kind == TokenKind::RBrace)
                    --depth;
                advance();
            }
        }

        return fd;
    }

    // Skip a migration block: migration v<N>_to_v<M> { ... }
    // Returns true on success, an error on bad syntax.
    [[nodiscard]] std::expected<bool, glibre::Error> skip_migration_block() {
        // Consume migration name (e.g. "v2_to_v3")
        if (current_.kind != TokenKind::Ident)
            return FORYC_ERR(tools::Error::ForycSyntaxError, "expected migration name");
        advance();

        if (!consume(TokenKind::LBrace))
            return FORYC_ERR(tools::Error::ForycSyntaxError, "expected '{' after migration name");

        // Consume entire body with balanced braces.
        int depth = 1;
        while (depth > 0 && current_.kind != TokenKind::Eof) {
            if (current_.kind == TokenKind::LBrace)
                ++depth;
            if (current_.kind == TokenKind::RBrace)
                --depth;
            advance();
        }
        return true;
    }

    Lexer lexer_;
    std::string_view virtual_path_;
    Token current_{};
};

}  // anonymous namespace

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

ParseResult parse_string(std::string_view source, std::string_view virtual_path) noexcept {
    Parser p{source, virtual_path};
    return p.parse();
}

ParseResult parse_file(const std::filesystem::path& path) noexcept {
    // Read entire file into a std::string (PHILOSOPHY §11 permits std::
    // for I/O utilities not covered by EASTL).
    std::ifstream ifs{path};
    if (!ifs) {
        return std::unexpected<glibre::Error>{glibre::Error{tools::Error::ForycIOError}};
    }
    std::ostringstream buf;
    buf << ifs.rdbuf();
    const std::string content = buf.str();

    // Hold source in a local string; parse_string gets a view into it.
    // Pass path.native() as virtual_path so Parser::parse() sets source_path
    // exactly once (at line 208); no second assignment needed here.
    return parse_string(content, path.native());
}

}  // namespace glibre::tools::foryc

// FORYC_ERR is an implementation-only macro; undef to prevent leakage into
// unity builds or precompiled-header contexts.
#undef FORYC_ERR
