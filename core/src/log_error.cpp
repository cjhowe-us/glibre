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
// Zero-allocation fast path: spdlog::logger::log() accepts a fmtlib format string
// and arguments directly through its internal fmtlib pipeline.  No std::string
// temporary is constructed; the format expansion happens inside spdlog's buffer.
//
// noexcept contract: log_error() is declared noexcept.  glibre-core compiles
// with -fno-exceptions so the format path cannot throw from this TU.  The vcpkg
// spdlog port builds with SPDLOG_COMPILED_LIB only (not SPDLOG_NO_EXCEPTIONS),
// so spdlog's internal exception paths are compiled into the spdlog binary;
// however, because log_error() is noexcept, any exception that attempts to
// escape spdlog terminates the process via std::terminate (the standard C++
// noexcept-on-throw rule).  For a log-emit failure, std::terminate is the
// correct outcome: the previous-good plugin state is preserved (per
// error-model.md §Logging rule 3), and the crash is detectable in CI.

#include "glibre/log_error.hpp"

#include <spdlog/spdlog.h>

namespace glibre {

// ---------------------------------------------------------------------------
// log_error
// ---------------------------------------------------------------------------

void log_error(
    spdlog::logger& sink, const glibre::Error& err, spdlog::level::level_enum level
) noexcept {
    const glibre::ErrorContext& ctx = err.where();

    // ErrorContext::file and ::detail are std::string_view (post-migration per
    // reviews/decisions/eastl-removal.md §4), so fmtlib can format them directly
    // without any conversion.
    const std::string_view file_sv = ctx.file;
    const std::string_view detail_sv = ctx.detail;

    // Pass format string and arguments directly to spdlog's logger::log().
    // spdlog forwards them into its internal fmtlib pipeline — zero heap
    // allocation on the hot path.
    sink.log(
        level,
        "[tag={} variant={} file={} line={} detail={}]",
        tag_string(err),
        variant_code_string(err),
        file_sv.empty() ? std::string_view{"<unknown>"} : file_sv,
        ctx.line,
        detail_sv.empty() ? std::string_view{"<none>"} : detail_sv
    );
}

// ---------------------------------------------------------------------------
// log_error_to_default
// ---------------------------------------------------------------------------

void log_error_to_default(const glibre::Error& err, spdlog::level::level_enum level) noexcept {
    auto logger = spdlog::default_logger();
    if (logger) {
        log_error(*logger, err, level);
    }
}

}  // namespace glibre
