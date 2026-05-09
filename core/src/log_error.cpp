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
// noexcept contract: log_error() is declared noexcept.  glibre-core compiles
// with -fno-exceptions so std::format and std::string cannot throw from this
// TU.  The vcpkg spdlog port builds with SPDLOG_COMPILED_LIB only (not
// SPDLOG_NO_EXCEPTIONS), so spdlog's internal exception paths are compiled
// into the spdlog binary; however, because log_error() is noexcept, any
// exception that attempts to escape spdlog terminates the process via
// std::terminate (the standard C++ noexcept-on-throw rule).  For a log-emit
// failure, std::terminate is the correct outcome: the previous-good plugin
// state is preserved (per error-model.md §Logging rule 3), and the crash is
// detectable in CI.

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

    // Construct a std::string_view over the same character buffer that
    // ctx.file / ctx.detail point at.  This is a standard range constructor
    // (data + size) — no copy, no type-pun.  std::format then consumes the
    // view directly without further allocation.
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
    // format_error_message returns std::string (may allocate).
    // This TU compiles with -fno-exceptions so std::bad_alloc cannot be
    // thrown here; the noexcept annotation is valid for this call site.
    // If spdlog itself throws (its binary was compiled without -fno-exceptions),
    // the noexcept boundary converts it to std::terminate — see file-level
    // noexcept contract comment above.
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
