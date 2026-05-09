// core/src/log_error.cpp
//
// Implementation of glibre::log_error() and glibre::log_error_to_default().
//
// Authority: reviews/decisions/error-model.md §"Logging / Telemetry" rules 1–3.
// Plan: #236.
//
// Format: each call emits one spdlog log line of the form:
//   [tag=core::Error variant=PluginDlopenFailed file=core/src/plugin_loader.cpp
//    line=87 detail=dlopen failed]
//
// std::format is used to build the log message string (PHILOSOPHY §11 permits
// std::format — it is a language/runtime utility that EASTL does not own;
// std::string is used only as the transient buffer passed to spdlog, which is
// immediately discarded after the log call).
//
// -fno-exceptions: spdlog 1.x supports SPDLOG_NO_EXCEPTIONS.
// The log_error functions are declared noexcept; any spdlog internal
// allocation failure is swallowed (spdlog's no-exceptions behaviour).

#include "glibre/log_error.hpp"

#include <format>
#include <string>

#include <EASTL/string_view.h>

namespace glibre {

namespace {

// Build the structured log message string.
// Uses std::format (PHILOSOPHY §11 carve-out — std::format is permitted).
[[nodiscard]] std::string format_error_message(const glibre::Error& err) {
    const glibre::Error::Variant& v = err.code();
    const glibre::ErrorContext& ctx = err.where();

    // Convert eastl::string_view to std::string_view for std::format interop.
    // eastl::string_view and std::string_view have the same data/size semantics;
    // the reinterpret is safe for UTF-8 string literals from __FILE__ and detail.
    const std::string_view file_sv{ctx.file.data(), ctx.file.size()};
    const std::string_view detail_sv{ctx.detail.data(), ctx.detail.size()};

    return std::format(
        "[tag={} variant={} file={} line={} detail={}]",
        tag_string(v),
        variant_code_string(v),
        file_sv.empty() ? "<unknown>" : file_sv,
        ctx.line,
        detail_sv.empty() ? "<none>" : detail_sv
    );
}

}  // namespace

// ---------------------------------------------------------------------------
// log_error
// ---------------------------------------------------------------------------

void log_error(
    spdlog::logger& sink,
    const glibre::Error& err,
    spdlog::level::level_enum level
) noexcept {
    // format_error_message may allocate (std::format, std::string).
    // The noexcept boundary is maintained by the OS-level assumption:
    // std::string allocation failure on macOS terminates via new-handler.
    // For -fno-exceptions builds, exceptions from std::format/string are
    // compile-time impossible; this noexcept annotation is valid.
    sink.log(level, "{}", format_error_message(err));
}

// ---------------------------------------------------------------------------
// log_error_to_default
// ---------------------------------------------------------------------------

void log_error_to_default(
    const glibre::Error& err,
    spdlog::level::level_enum level
) noexcept {
    auto logger = spdlog::default_logger();
    if (logger) {
        log_error(*logger, err, level);
    }
}

}  // namespace glibre
