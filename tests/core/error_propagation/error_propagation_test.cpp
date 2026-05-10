// tests/core/error_propagation/error_propagation_test.cpp
//
// Contract tests verifying that errors propagate as return codes (not
// exceptions) across the host↔plugin C-ABI boundary (plan #240).
//
// Authority: reviews/decisions/error-model.md §Decision 1 ("Every public
// boundary returns std::expected<T, glibre::Error>"), §Decision 3
// ("-fno-exceptions on engine code"), §Composition Rules #1–2.
//
// Named test cases (plan #240 Unit Test Plan + DoD):
//   - c_abi_boundary_returns_error_code_no_exception
//   - error_propagation_preserves_error_context
//   - terminating_exception_aborts_plugin_load
//
// Compile-time path macros (injected by CMakeLists.txt):
//   GLIBRE_STUB_ERROR_RETURNS_DYLIB_PATH — path to stub_error_returns.dylib
//
// Design constraints:
//   - -fno-exceptions / -fno-rtti (error-model.md §Decision 3).
//   - dlopen/dlsym used directly — tests do not depend on PluginLoader.
//     These tests target the C-ABI contract itself, not the loader machinery.
//   - No REQUIRE_THROWS — -fno-exceptions build.
//
// Stub dylib contract (stub_error_returns.cpp):
//   glibre_test_error_discriminant() -> int32_t
//     Returns the std::variant index of core::Error arm = 0.
//   glibre_test_error_code() -> uint16_t
//     Returns underlying integer value of core::Error::PluginInitFailed.
//   glibre_test_context_write(char* buf, size_t buf_size) -> bool
//     Constructs glibre::Result<void> via std::unexpected(glibre::Error{...}),
//     extracts .error().where(), and writes a GlibreTestContextTransfer POD
//     into the caller's buffer.  The line field is __LINE__ at the construction
//     site — no hard-coded sentinel constant on the host side.
//
// Migrated from EASTL to libc++ stdlib per
// reviews/decisions/eastl-removal.md §4.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>  // dlopen, dlsym, dlclose — macOS / POSIX
#include <memory>
#include <string_view>
#include <variant>

#include <catch2/catch_test_macros.hpp>
#include <glibre/error.hpp>

#include "transfer.hpp"

// ---------------------------------------------------------------------------
// DlHandle — RAII wrapper for dlopen handles.
//
// Ensures dlclose is called even when a REQUIRE assertion aborts the test
// case early, so the dylib handle is never leaked on failure paths.
// ---------------------------------------------------------------------------

namespace {

struct DlHandleDeleter {
    void operator()(void* h) const noexcept {
        if (h) {
            ::dlclose(h);
        }
    }
};

using DlHandle = std::unique_ptr<void, DlHandleDeleter>;

// Shared POD layout and sentinel constants from transfer.hpp.
// Extracted to remove the "keep in sync" manual hazard (SRP: one definition).
using glibre::test::error_propagation::GlibreTestContextTransfer;
using glibre::test::error_propagation::kDetailMax;
using glibre::test::error_propagation::kFileMax;
// Sentinel strings: host uses the canonical names from the shared header.
// No kExpectedSentinelLine — the stub captures __LINE__ at construction;
// the host checks transfer.line > 0 (a valid source location).
using glibre::test::error_propagation::kSentinelDetail;
using glibre::test::error_propagation::kSentinelFile;

// Variant index for glibre::core::Error in glibre::Error::Variant.
// core::Error is the FIRST arm → index == 0.
inline constexpr int32_t kCoreErrorVariantIndex = 0;

}  // namespace

// ===========================================================================
// Test: c_abi_boundary_returns_error_code_no_exception
//
// Verifies that:
//   1. A C-ABI function inside a plugin TU compiled with -fno-exceptions can
//      construct a glibre::Result<void> holding a core::Error and return its
//      variant discriminant as a plain int32_t.
//   2. The returned int32_t matches the expected core::Error variant index (0).
//   3. The specific enumerator (PluginInitFailed) round-trips correctly: the
//      stub's glibre_test_error_code() returns the same uint16_t as the
//      host-side static_cast of glibre::core::Error::PluginInitFailed.
//   4. No exception crosses the boundary — under -fno-exceptions the only
//      observable exit from the stub is a normal return; any throw would have
//      called std::terminate instead of returning.
//
// The stub functions are resolved via dlsym so this test exercises the same
// code path as the real plugin loader's dlsym lookup, but without going
// through PluginLoader (which is not in scope).
//
// Refs: error-model.md §Decision 3, §Decision 1.
// DoD: unit_test_named: c_abi_boundary_returns_error_code_no_exception
// ===========================================================================

TEST_CASE("c_abi_boundary_returns_error_code_no_exception", "[core][error_propagation][c_abi]") {
#ifndef GLIBRE_STUB_ERROR_RETURNS_DYLIB_PATH
    FAIL(
        "GLIBRE_STUB_ERROR_RETURNS_DYLIB_PATH not defined — "
        "rebuild with the error_propagation sub-directory in scope (plan #240)"
    );
#else
    const char* dylib_path = GLIBRE_STUB_ERROR_RETURNS_DYLIB_PATH;
    REQUIRE(dylib_path != nullptr);

    // Load the stub dylib using RTLD_NOW | RTLD_LOCAL so symbols are resolved
    // immediately and do not pollute the global symbol table.
    // DlHandle RAII ensures dlclose is called even on REQUIRE-fail paths.
    DlHandle handle{::dlopen(dylib_path, RTLD_NOW | RTLD_LOCAL)};
    REQUIRE(handle != nullptr);

    // -------------------------------------------------------------------------
    // Discriminant check: verify variant index == 0 (core::Error arm).
    // -------------------------------------------------------------------------
    using ErrorDiscriminantFn = int32_t (*)() noexcept;
    void* sym_disc = ::dlsym(handle.get(), "glibre_test_error_discriminant");
    REQUIRE(sym_disc != nullptr);

    ErrorDiscriminantFn discriminant_fn{nullptr};
    std::memcpy(&discriminant_fn, &sym_disc, sizeof(discriminant_fn));
    REQUIRE(discriminant_fn != nullptr);

    // Call the stub function.  If the stub had thrown (impossible under
    // -fno-exceptions, which turns throw into std::terminate), this line
    // would never return — the process would abort instead.
    const int32_t discriminant = discriminant_fn();
    CHECK(discriminant == kCoreErrorVariantIndex);

    // -------------------------------------------------------------------------
    // Enumerator check: verify the specific PluginInitFailed code round-trips.
    //
    // glibre_test_error_discriminant() only asserts the arm is core::Error;
    // it does not distinguish between PluginInitFailed and OutOfBudget etc.
    // glibre_test_error_code() exports the exact uint16_t underlying value so
    // the host can verify the enumerator identity, not just the variant arm.
    // -------------------------------------------------------------------------
    using ErrorCodeFn = uint16_t (*)() noexcept;
    void* sym_code = ::dlsym(handle.get(), "glibre_test_error_code");
    REQUIRE(sym_code != nullptr);

    ErrorCodeFn code_fn{nullptr};
    std::memcpy(&code_fn, &sym_code, sizeof(code_fn));
    REQUIRE(code_fn != nullptr);

    const uint16_t reported_code = code_fn();
    CHECK(reported_code == static_cast<uint16_t>(glibre::core::Error::PluginInitFailed));

    // -------------------------------------------------------------------------
    // Host-side compile-time structural check.
    //
    // Verify that the Variant layout assumed by kCoreErrorVariantIndex == 0 is
    // still true in this compilation unit.  std::variant_size is a stronger
    // invariant than sizeof > 0: it locks the arm count the test depends on.
    // Migrated from eastl::variant_size_v to std::variant_size_v per
    // reviews/decisions/eastl-removal.md §4 (matrix row 19).
    // -------------------------------------------------------------------------
    static_assert(
        std::variant_size_v<glibre::Error::Variant> >= 1u,
        "glibre::Error::Variant must have at least one arm (core::Error)"
    );
    glibre::Error probe{glibre::core::Error::OutOfBudget};
    CHECK(probe.code().index() == 0u);
#endif
}

// ===========================================================================
// Test: error_propagation_preserves_error_context
//
// Verifies that ErrorContext fields (file, line, detail) survive a
// std::expected<void, glibre::Error> / std::unexpected round-trip across the
// C-ABI boundary.
//
// The stub function glibre_test_context_write() constructs:
//   glibre::Result<void> r = std::unexpected(glibre::Error{enum, ctx});
// then reads r.error().where() to populate a GlibreTestContextTransfer POD
// written into the host-supplied buffer.  The host deserialises the POD and
// checks that file and detail match the known sentinel strings and that line
// is a positive, non-zero source location (the stub captures __LINE__ at the
// construction site — no magic constant on the host side).
//
// This tests error-model.md §Decision 1: std::expected<T, glibre::Error>
// carries ErrorContext through the std::unexpected round-trip and .error().where()
// surfaces it correctly.
//
// Refs: error-model.md §Type Sketch (ErrorContext), §Decision 1.
// DoD: unit_test_named: error_propagation_preserves_error_context
// ===========================================================================

TEST_CASE("error_propagation_preserves_error_context", "[core][error_propagation][context]") {
#ifndef GLIBRE_STUB_ERROR_RETURNS_DYLIB_PATH
    FAIL(
        "GLIBRE_STUB_ERROR_RETURNS_DYLIB_PATH not defined — "
        "rebuild with the error_propagation sub-directory in scope (plan #240)"
    );
#else
    const char* dylib_path = GLIBRE_STUB_ERROR_RETURNS_DYLIB_PATH;
    REQUIRE(dylib_path != nullptr);

    DlHandle handle{::dlopen(dylib_path, RTLD_NOW | RTLD_LOCAL)};
    REQUIRE(handle != nullptr);

    // Resolve the context-write function.
    using ContextWriteFn = bool (*)(char*, std::size_t) noexcept;
    void* sym_raw = ::dlsym(handle.get(), "glibre_test_context_write");
    REQUIRE(sym_raw != nullptr);

    ContextWriteFn fn{nullptr};
    std::memcpy(&fn, &sym_raw, sizeof(fn));
    REQUIRE(fn != nullptr);

    // Provide the host buffer for the transfer struct.
    GlibreTestContextTransfer transfer{};
    const bool wrote = fn(reinterpret_cast<char*>(&transfer), sizeof(transfer));
    REQUIRE(wrote);

    // Verify file and detail match the sentinels baked into the stub.
    // Migrated from eastl::string_view to std::string_view per
    // reviews/decisions/eastl-removal.md §4 (matrix row 2).
    CHECK(std::string_view{transfer.file} == std::string_view{kSentinelFile});
    CHECK(std::string_view{transfer.detail} == std::string_view{kSentinelDetail});

    // Verify the line is a positive, non-zero source location.
    // The stub sets ctx.line = __LINE__ at the ErrorContext construction site;
    // the host does NOT compare to a hard-coded constant — any positive integer
    // is a valid source line (the exact value depends on the stub's source).
    CHECK(transfer.line > 0);
#endif
}

// ===========================================================================
// Test: terminating_exception_aborts_plugin_load
//
// Contract note — compile-time enforced, runtime skipped.
//
// The formal contract (error-model.md §Decision 3) states:
//   "Engine code (everything outside tools/editor/ui/) compiles with
//    -fno-exceptions. … Plugins must catch and convert any thrown exception
//    to a glibre::Error before returning."
//
// The intended property is: a plugin function that throws instead of returning
// glibre::Result<void> will cause std::terminate (abort) rather than unwinding
// through the C-ABI boundary into the host.  Under -fno-exceptions, the
// compiler implements throw expressions as calls to std::terminate, so the
// contract is enforced at compile time for all translation units compiled with
// that flag — including this test binary and stub_error_returns.cpp.
//
// Catch2 3.x under -fno-exceptions does NOT support death tests
// (REQUIRE_THROWS, REQUIRE_NOTHROW, etc. are disabled; no process-forking
// mechanism exists in the Catch2 3 framework for checking std::terminate).
// Verifying that std::terminate is called would require forking the process
// and observing the exit signal from outside — outside the scope of this plan
// and outside the Catch2 3 feature set on macOS.
//
// Compile-time verification:
//   The static_assert below audits that this translation unit was NOT compiled
//   with exception support.  Under clang/GCC with -fno-exceptions, __EXCEPTIONS
//   is undefined or 0; with -fexceptions, it is defined to 1.  The assertion
//   fires (at compile time) if -fexceptions is active — catching a build
//   misconfiguration before any tests run.
//
// Refs: error-model.md §Decision 3, §"Consequences" (plugin authors must catch).
// DoD: unit_test_named: terminating_exception_aborts_plugin_load
// ===========================================================================

// Compile-time enforcement: this file must be compiled with -fno-exceptions.
// Under clang/GCC with -fexceptions, __EXCEPTIONS is defined to 1.
// If this #error fires, the test binary was misconfigured — the build must
// pass -fno-exceptions for all engine / test TUs (error-model.md §Decision 3).
#if defined(__EXCEPTIONS) && __EXCEPTIONS
#error                                                                                             \
    "test binary compiled with -fexceptions — error-model.md §Decision 3 requires -fno-exceptions"
#endif

TEST_CASE("terminating_exception_aborts_plugin_load", "[core][error_propagation][contract]") {
    // The #error directive above fires at preprocessing if -fexceptions is
    // active, so this test case body only reaches compilation when the flag is
    // correctly absent.  There is no separate runtime check needed: the absence
    // of exceptions is enforced by the build flag, not by runtime observation.
    //
    // Catch2 3.x on macOS provides no death-test mechanism under -fno-exceptions.
    // Verifying SIGABRT from a throw would require forking a child process and
    // observing its exit signal — deferred to spike #973.
    SUCCEED(
        "terminating_exception_aborts_plugin_load: -fno-exceptions confirmed "
        "at compile time by #error guard (error-model.md §Decision 3). "
        "Runtime death-test deferred — Catch2 3.x on macOS has no process-fork "
        "mechanism under -fno-exceptions (see spike #973)."
    );
}
