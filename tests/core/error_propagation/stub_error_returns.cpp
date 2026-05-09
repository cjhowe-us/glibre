// tests/core/error_propagation/stub_error_returns.cpp
//
// C-ABI stub dylib for error-propagation contract tests (plan #240).
//
// Exports three plain C functions used by error_propagation_test.cpp:
//
//   glibre_test_error_discriminant()
//     Returns the variant index of the error arm produced by constructing a
//     glibre::Result<void> with core::Error::PluginInitFailed, cast to int32_t.
//     Under -fno-exceptions the only observable exit from this function is a
//     normal return; any throw would std::terminate instead.
//
//   glibre_test_error_code()
//     Returns the uint16_t underlying value of core::Error::PluginInitFailed,
//     used by the host to verify the enumerator passes through unchanged.
//
//   glibre_test_context_write(char* buf, size_t buf_size)
//     Constructs a glibre::Result<void> via std::unexpected(glibre::Error{...})
//     — an actual std::expected / std::unexpected round-trip — then extracts
//     .error().where() and serialises the three string fields into the
//     caller-supplied buffer as a flat GlibreTestContextTransfer POD.
//     The line field is populated with __LINE__ at the construction site so
//     the host can verify a positive, non-zero source location without coupling
//     to a hand-mirrored magic constant.
//     Returns true on success, false if buf is null or too small.
//
// Compilation requirements:
//   -fno-exceptions (error-model.md §Decision 3)
//   -fno-rtti
//   C++23
//
// This stub is a test-only artifact; it is not a plugin and does not export
// the four canonical plugin ABI symbols (glibre_plugin_abi_hash, etc.).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>

#include <EASTL/variant.h>
#include <glibre/error.hpp>

#include "transfer.hpp"

// Pull shared types and constants into the anonymous namespace so the rest of
// this file uses them unqualified, matching the original usage.
namespace {

using glibre::test::error_propagation::GlibreTestContextTransfer;
using glibre::test::error_propagation::kDetailMax;
using glibre::test::error_propagation::kFileMax;
using glibre::test::error_propagation::kSentinelDetail;
using glibre::test::error_propagation::kSentinelFile;

}  // namespace

// ---------------------------------------------------------------------------
// glibre_test_error_discriminant
//
// Constructs a glibre::Result<void> holding core::Error::PluginInitFailed
// and returns the variant index of its error alternative as int32_t.
//
// The variant index of the first arm (core::Error) in glibre::Error::Variant
// is 0.  The host side checks for exactly 0 to verify that the C-ABI boundary
// sees a core::Error (not a render::Error or other context error).
//
// The function signature is a plain C int32_t (no C++ templates cross ABI)
// so this can be called via dlsym + function-pointer cast.
// ---------------------------------------------------------------------------

extern "C" [[gnu::visibility("default")]]
int32_t glibre_test_error_discriminant() noexcept {
    // Construct a failed Result<void> with a core::Error arm.
    glibre::Result<void> r = std::unexpected(glibre::Error{glibre::core::Error::PluginInitFailed});

    // r is an error; extract the variant index of its error alternative.
    // eastl::variant::index() returns the zero-based position of the active
    // alternative.
    const std::size_t idx = r.error().code().index();

    // Return as int32_t (plain C integer) — safe for C-ABI crossing.
    return static_cast<int32_t>(idx);
}

// ---------------------------------------------------------------------------
// glibre_test_error_code
//
// Returns the underlying integer value of core::Error::PluginInitFailed.
// The host uses this to verify the enumerator's integer identity without
// needing to include glibre/error.hpp in the cross-ABI call path.
// ---------------------------------------------------------------------------

extern "C" [[gnu::visibility("default")]]
uint16_t glibre_test_error_code() noexcept {
    return static_cast<uint16_t>(glibre::core::Error::PluginInitFailed);
}

// ---------------------------------------------------------------------------
// glibre_test_context_write
//
// Constructs a glibre::Result<void> via std::unexpected(glibre::Error{...}),
// then reads .error().where() to verify the ErrorContext survives the
// std::expected / std::unexpected round-trip (error-model.md §Decision 1).
// Serialises the three string fields into a GlibreTestContextTransfer POD
// written to the caller-supplied buffer.
//
// The line field is set to __LINE__ at the construction site so the host can
// check a positive, non-zero source location without relying on a hand-
// mirrored magic constant.  The host verifies line > 0; it does NOT compare
// to a specific integer.
//
// Parameters:
//   buf      — host-allocated buffer; must point to at least
//              sizeof(GlibreTestContextTransfer) bytes.
//   buf_size — byte length of buf.
//
// Returns:
//   true  — transfer struct written to buf.
//   false — buf is null or buf_size < sizeof(GlibreTestContextTransfer).
// ---------------------------------------------------------------------------

extern "C" [[gnu::visibility("default")]]
bool glibre_test_context_write(char* buf, std::size_t buf_size) noexcept {
    if (buf == nullptr || buf_size < sizeof(GlibreTestContextTransfer)) {
        return false;
    }

    // Populate ErrorContext with sentinel values.
    // Line is __LINE__ at this exact call site — no hand-mirrored constant.
    glibre::ErrorContext ctx;
    ctx.file = eastl::string_view{kSentinelFile};
    ctx.line = __LINE__;  // captures the source line of this assignment
    ctx.detail = eastl::string_view{kSentinelDetail};

    // Round-trip through std::unexpected so the test verifies that
    // glibre::Result<void> carries ErrorContext through .error().where().
    // This is the actual contract under test (error-model.md §Decision 1).
    glibre::Result<void> r =
        std::unexpected(glibre::Error{glibre::core::Error::PluginInitFailed, ctx});

    // Extract fields via .error().where() — the actual propagation path.
    GlibreTestContextTransfer transfer{};
    transfer.line = r.error().where().line;

    const eastl::string_view file_view = r.error().where().file;
    const eastl::string_view detail_view = r.error().where().detail;

    const std::size_t file_copy = std::min(file_view.size(), kFileMax - 1u);
    const std::size_t detail_copy = std::min(detail_view.size(), kDetailMax - 1u);

    std::memcpy(transfer.file, file_view.data(), file_copy);
    std::memcpy(transfer.detail, detail_view.data(), detail_copy);
    // Arrays are zero-initialised above; null terminators are already in place.

    std::memcpy(buf, &transfer, sizeof(GlibreTestContextTransfer));
    return true;
}
