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
//   glibre_test_context_write(char* buf, size_t buf_size)
//     Constructs a glibre::Error with a known ErrorContext (file/line/detail)
//     and serialises the three string fields into the caller-supplied buffer
//     as a flat GlibreTestContextTransfer POD.  Returns true on success, false
//     if buf is null or too small.
//
//   glibre_test_error_code()
//     Returns the uint16_t underlying value of core::Error::PluginInitFailed,
//     used by the host to verify the enumerator passes through unchanged.
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

// ---------------------------------------------------------------------------
// GlibreTestContextTransfer — flat POD layout for cross-ABI ErrorContext data
//
// ErrorContext holds eastl::string_view members (non-owning pointer+size
// references).  Transferring raw bytes of the struct across the ABI boundary
// would produce dangling string_view pointers in the host.  Instead this
// test-specific struct copies the string data into fixed-size char arrays,
// making it safe to memcpy through the caller-supplied buffer.
//
// kFileMax / kDetailMax are generous; the sentinel strings in this stub are
// well below these limits.
// ---------------------------------------------------------------------------

namespace {

inline constexpr std::size_t kFileMax   = 256;
inline constexpr std::size_t kDetailMax = 256;

// Keep in sync with the mirror definition in error_propagation_test.cpp.
struct GlibreTestContextTransfer {
    char   file[kFileMax];    // null-terminated
    int    line;
    char   detail[kDetailMax]; // null-terminated
};

// Sentinel values baked into the stub so the host can check them by value.
// They are string literals whose storage lives in the stub's .rodata — valid
// for the lifetime of the loaded dylib.
inline constexpr const char* kSentinelFile   = "stub_error_returns.cpp";
inline constexpr int         kSentinelLine   = 99;
inline constexpr const char* kSentinelDetail = "PluginInitFailed-context-sentinel";

}  // namespace

// ---------------------------------------------------------------------------
// glibre_test_error_discriminant
//
// Constructs a glibre::Result<void> holding core::Error::PluginInitFailed
// and returns the variant index of its error alternative as int32_t.
//
// The variant index of the first arm (core::Error) in glibre::Error::Variant
// is 0.  The host side checks for exactly 0 to verify that the C-ABI boundary
// sees a core::Error (not a render::Error or tools::Error).
//
// The function signature is a plain C int32_t (no C++ templates cross ABI)
// so this can be called via dlsym + function-pointer cast.
// ---------------------------------------------------------------------------

extern "C" [[gnu::visibility("default")]]
int32_t glibre_test_error_discriminant() noexcept {
    // Construct a failed Result<void> with a core::Error arm.
    glibre::Result<void> r = std::unexpected(
        glibre::Error{glibre::core::Error::PluginInitFailed}
    );

    // r is an error; extract the variant index of its error alternative.
    // eastl::variant_index is the 0-based index of the active alternative.
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
// Constructs a glibre::Error with a known ErrorContext (sentinel file/line/
// detail) and serialises the three string fields into a GlibreTestContextTransfer
// POD written to the caller-supplied buffer.
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

    // Construct a glibre::Error with the sentinel ErrorContext.
    // The eastl::string_view members reference .rodata strings in this TU.
    glibre::ErrorContext ctx;
    ctx.file   = eastl::string_view{kSentinelFile};
    ctx.line   = kSentinelLine;
    ctx.detail = eastl::string_view{kSentinelDetail};

    glibre::Error err{glibre::core::Error::PluginInitFailed, ctx};

    // Serialise into the flat transfer struct and memcpy into buf.
    GlibreTestContextTransfer transfer{};
    transfer.line = err.where().line;

    const eastl::string_view file_view   = err.where().file;
    const eastl::string_view detail_view = err.where().detail;

    const std::size_t file_copy   = std::min(file_view.size(),   kFileMax - 1u);
    const std::size_t detail_copy = std::min(detail_view.size(), kDetailMax - 1u);

    std::memcpy(transfer.file,   file_view.data(),   file_copy);
    std::memcpy(transfer.detail, detail_view.data(), detail_copy);
    // Arrays are zero-initialised above; null terminators are already in place.

    std::memcpy(buf, &transfer, sizeof(GlibreTestContextTransfer));
    return true;
}
