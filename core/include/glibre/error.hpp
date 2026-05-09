#pragma once
// core/include/glibre/error.hpp
//
// Engine-wide error type.  Per reviews/decisions/error-model.md:
//   - Every public boundary returns std::expected<T, glibre::Error>.
//   - glibre::Error is a tagged union over per-context error enums.
//   - Engine code compiles with -fno-exceptions.
//
// New contexts append their own enum to the Variant; old enums are
// never edited by other contexts.

#include <concepts>
#include <cstdint>
#include <expected>
#include <string_view>
#include <type_traits>
#include <variant>

namespace glibre {

// -----------------------------------------------------------------------
// Per-context error enumerations
// -----------------------------------------------------------------------

namespace core {
enum class Error : std::uint16_t {
    PluginAbiHashMismatch,
    PluginInitFailed,
    SchemaMigrationFailed,
    HotReloadRefused,
    FramePhaseMisordered,  // debug-build: a phase ran out-of-sequence
    OutOfBudget,
    SystemScheduleCycle,
    ScheduleAccessConflict,
};
}  // namespace core

namespace render {
enum class Error : std::uint16_t {
    DeviceLost,
    PipelineCompileFailed,
    ResourceResidencyExceeded,
    RenderGraphCycle,
    UnsupportedBackend,
};
}  // namespace render

// -----------------------------------------------------------------------
// ErrorContext — source-location attachment (optional human hint)
// -----------------------------------------------------------------------

struct ErrorContext {
    std::string_view file{};    // __FILE__
    int line{0};                // __LINE__
    std::string_view detail{};  // optional human hint, never load-bearing
};

// -----------------------------------------------------------------------
// glibre::Error — tagged union over per-context enumerations
// -----------------------------------------------------------------------

class Error {
public:
    using Variant = std::variant<
        core::Error,
        render::Error
        // physics::Error, data::Error, shader::Error, ...
        // append as each context lands
        >;

    // Constructible from any per-context error enum that is a member of
    // Variant.  The constraint prevents the generic constructor from
    // participating in overload resolution for unrelated types (e.g.
    // std::expected<…>), which would cause circular constraint evaluation
    // in libc++ clang-22.
    template<class E>
        requires std::constructible_from<Variant, E>
    constexpr Error(E e, ErrorContext ctx = {}) noexcept
        : variant_{e},
          ctx_{ctx} {}

    [[nodiscard]] constexpr const Variant& code() const noexcept { return variant_; }

    [[nodiscard]] constexpr const ErrorContext& where() const noexcept { return ctx_; }

private:
    Variant variant_;
    ErrorContext ctx_;
};

// -----------------------------------------------------------------------
// Convenience alias used throughout the engine
// -----------------------------------------------------------------------

template<class T>
using Result = std::expected<T, Error>;

}  // namespace glibre
