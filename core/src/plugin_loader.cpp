// core/src/plugin_loader.cpp
//
// PluginLoader — dlopen + dlsym + manifest read implementation.
//
// Authority: reviews/decisions/plugin-abi.md §"Loader Sequence" steps 1–3.
// Plan: #229 (this plan).
//
// Loader sequence implemented here:
//   Step 1: dlopen(path, RTLD_NOW | RTLD_LOCAL) → PluginDlopenFailed on null.
//   Step 2: dlsym four required symbols        → PluginMissingEntryPoint on
//           any missing symbol; dlclose before returning error.
//   Step 3: PluginManifest::open(<path>.manifest)
//           — result stored regardless of success/failure; validation in #230.
//
// Steps 4–11 (ABI hash gate, version/name/deps gates, register call,
// schedule rebuild, migrate) land in plans #230 and #231.
//
// OS dependencies:
//   <dlfcn.h>   — dlopen, dlsym, dlclose, dlerror (POSIX)
//   <cstdio>    — fprintf (for dlerror() diagnostic output at refusal site)
//
// Per PHILOSOPHY §11 and error-model.md §Decision 3:
//   -fno-exceptions; no std:: containers; std::filesystem carve-out OK.
//
// dlerror() note (HIGH finding round-1, addressed):
//   POSIX specifies dlerror() returns a pointer to a thread-local static that
//   may be overwritten by the next dlerror() call.  Storing it in a non-owning
//   eastl::string_view (ErrorContext::detail) is therefore a dangling-pointer
//   hazard once the Error escapes this function.
//
//   Fix: dlerror() text is emitted at the refusal site via fprintf(stderr),
//   BEFORE the Error is constructed.  ErrorContext::detail receives a stable
//   string literal ("dlopen failed") that is safe to inspect at any later point.
//   Per error-model.md §"Logging / Telemetry" rule 1, structured error logging
//   happens at the handling boundary (plan #230 caller), not at the raise site.
//   The diagnostic output here is a low-level aide for development builds only.
//
// try_resolve_required() note (MED-A + MED-B, round-2, addressed):
//   POSIX dlerror() disambiguation: dlsym() can return nullptr for two distinct
//   reasons — (a) the symbol is absent from the dylib, or (b) the symbol is
//   present but its value is legitimately null (e.g. a weak null data pointer).
//   Clearing dlerror() before the call and then inspecting it after is the
//   canonical POSIX way to distinguish these cases:
//     dlerror() == non-null after call  → symbol absent
//     dlerror() == null   after call    → symbol present (value may be null)
//   For the four required symbols in the loader all four are non-optional so
//   a post-call null value (however obtained) is treated as PluginMissingEntryPoint.
//   This full disambiguation is encapsulated in try_resolve_required(), which also
//   owns the dlclose+error-construction path, eliminating four 12-line duplicates.

#include "glibre/core/plugin_loader.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <string_view>
#include <utility>

#include <EASTL/string.h>
#include <EASTL/string_view.h>

#include "glibre/alloc.hpp"
#include "glibre/core/context_tag_resolver.hpp"  // derive_context_tag (moved to SRP unit, MED-2)
#include "glibre/core/plugin_manifest.hpp"
#include "glibre/error.hpp"

namespace glibre::core {

namespace {

// ---------------------------------------------------------------------------
// Helper: convert eastl::string_view → std::string (NUL-terminated) for
// dlopen and std::filesystem.  std::string is an explicit std:: carve-out
// when bridging to POSIX/OS interfaces per PHILOSOPHY §11.
// ---------------------------------------------------------------------------
[[nodiscard]] std::string to_std_string(eastl::string_view sv) {
    return std::string{sv.data(), sv.size()};
}

// ---------------------------------------------------------------------------
// Helper: try_resolve_required
//
// Resolves a required dlsym symbol with full POSIX dlerror() disambiguation
// (MED-A, round-2):
//
//   1. dlerror() is cleared before dlsym() so any prior error state cannot
//      contaminate the post-call check.
//   2. After dlsym(), dlerror() is called exactly once.
//      - Non-null return → the symbol was absent; return PluginMissingEntryPoint.
//      - Null return with null sym → the symbol is present but its value is null;
//        for a required symbol this is also PluginMissingEntryPoint.
//   3. On any failure path dlclose(handle) is called before returning, so the
//      caller never holds a handle to a partially-loaded plugin.
//
// Callers use the returned Result directly; on success they receive the void*
// and may proceed; on failure they propagate the error (MED-B, round-2).
// ---------------------------------------------------------------------------
[[nodiscard]] glibre::Result<void*>
try_resolve_required(void* handle, const char* sym_name) noexcept {
    (void)dlerror();  // clear prior state
    void* const sym = dlsym(handle, sym_name);
    const char* const dl_err = dlerror();  // consume once — invalidates pointer

    // Two-branch POSIX disambiguation:
    //   dl_err != nullptr  → symbol absent (dlsym set error text)
    //   dl_err == nullptr and sym == nullptr → present but null value
    // Both cases are fatal for required symbols.
    if (dl_err != nullptr || sym == nullptr) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        (void)fprintf(
            stderr,
            "[glibre] dlsym(%s) failed: %s\n",
            sym_name,
            (dl_err != nullptr) ? dl_err : "symbol resolved to null"
        );
        dlclose(handle);
        // Build a stable detail string in a char array on the stack.
        // We cannot store dl_err — it is a thread-local pointer that may
        // be invalidated by the dlclose() above.
        // sym_name is a string literal at every call site so it outlives this
        // stack frame; embedding it directly in detail is safe.
        return std::unexpected(
            glibre::Error{
                core::Error::PluginMissingEntryPoint,
                ErrorContext{
                    .file = __FILE__,
                    .line = __LINE__,
                    .detail = sym_name,  // stable literal at all call sites
                },
            }
        );
    }
    return sym;
}

}  // namespace

// ---------------------------------------------------------------------------
// PluginLoader::open
// ---------------------------------------------------------------------------

glibre::Result<PluginLoader> PluginLoader::open(eastl::string_view dylib_path) {
    // Step 1: dlopen
    //
    // RTLD_NOW   — resolve all undefined symbols in the dylib immediately.
    //              Missing middleman symbols surface here, not at first call.
    // RTLD_LOCAL — plugin symbols do not pollute the global dynamic linker
    //              namespace; cross-plugin calls go through the type registry.
    const std::string path_c = to_std_string(dylib_path);

    (void)dlerror();  // clear any prior error state
    void* const handle = dlopen(path_c.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        // dlerror() returns a thread-local pointer invalidated by the next
        // dlerror() call.  Storing it in a non-owning eastl::string_view
        // inside ErrorContext::detail would create a dangling pointer once
        // the Error escapes this function (round-1 HIGH finding, addressed).
        //
        // Fix: emit the dlerror text immediately to stderr for development
        // diagnostics, then use a stable string literal for detail.
        // Structured logging (glibre::log_error) is the caller's
        // responsibility per error-model.md §"Logging / Telemetry" rule 1.
        const char* const dl_err = dlerror();
        if (dl_err != nullptr) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
            (void)fprintf(stderr, "[glibre] dlopen(%s) failed: %s\n", path_c.c_str(), dl_err);
        }

        return std::unexpected(
            glibre::Error{
                core::Error::PluginDlopenFailed,
                ErrorContext{
                    .file = __FILE__,
                    .line = __LINE__,
                    .detail = "dlopen failed",  // stable literal — dlerror logged above
                },
            }
        );
    }

    // Step 2: resolve the four required symbols via try_resolve_required().
    //
    // plugin-abi.md §"Plugin file shape" lists the canonical names:
    //   glibre_plugin_abi_hash          → const char*
    //   glibre_plugin_manifest          → const std::byte*
    //   glibre_plugin_manifest_size     → std::size_t
    //   glibre_plugin_register          → RegisterFn
    //
    // try_resolve_required() performs full POSIX dlerror() disambiguation
    // (MED-A, round-2) and calls dlclose(handle) before returning on failure
    // (MED-B, round-2).  Each call site is a single propagation line; the
    // 12-line Error+dlclose construction is not repeated.

    // glibre_plugin_abi_hash
    auto res_abi_hash = try_resolve_required(handle, "glibre_plugin_abi_hash");
    if (!res_abi_hash) {
        return std::unexpected(res_abi_hash.error());
    }
    void* const sym_abi_hash = *res_abi_hash;

    // glibre_plugin_manifest
    auto res_manifest = try_resolve_required(handle, "glibre_plugin_manifest");
    if (!res_manifest) {
        return std::unexpected(res_manifest.error());
    }
    void* const sym_manifest = *res_manifest;

    // glibre_plugin_manifest_size
    auto res_manifest_size = try_resolve_required(handle, "glibre_plugin_manifest_size");
    if (!res_manifest_size) {
        return std::unexpected(res_manifest_size.error());
    }
    void* const sym_manifest_size = *res_manifest_size;

    // glibre_plugin_register
    auto res_register = try_resolve_required(handle, "glibre_plugin_register");
    if (!res_register) {
        return std::unexpected(res_register.error());
    }
    void* const sym_register = *res_register;

    // Step 3: read sidecar manifest.
    //
    // CANONICAL DESIGN (plugin-abi.md §"Loader Sequence" step 3):
    //   glibre::types::deserialize<PluginManifest>(std::span{manifest_blob, size})
    //
    // CURRENT DEFERRAL (HIGH-2, round-1, DEFER — see reviews/decisions/plugin-abi.md
    // §"Loader Sequence" step 3 amendment):
    //   Plan #225 (glibre-foryc per-plugin manifest.cpp emission) has not landed.
    //   Until it does, the `glibre_plugin_manifest` blob is null/zero in MVP stubs.
    //   This fallback reads a sidecar `<dylib>.manifest` file from disk instead.
    //   Plan #230 will:
    //     a) require a valid manifest (hard error on missing/invalid), AND
    //     b) switch to the blob deserialisation path once #225 emits real blobs.
    //
    // When both #225 and #230 land, delete the sidecar path below and replace
    // with: glibre::types::deserialize<PluginManifest>({manifest_blob_, manifest_blob_size_})
    //
    // Both manifest_blob_ and manifest_blob_size_ are already captured from
    // the dlsym'd symbols above; no additional dlsym step is needed at that point.
    const std::string manifest_path = path_c + ".manifest";
    glibre::Result<PluginManifest> manifest_result =
        PluginManifest::open(std::string_view{manifest_path.data(), manifest_path.size()});

    // Construct a valid PluginLoader and transfer all ownership.
    //
    // The static_cast<> calls below dereference the void* dlsym results as the
    // correct pointer-to-pointer types, obtaining the actual typed values
    // stored in the plugin's .rodata.
    //
    // For the function pointer (sym_register): POSIX mandates dlsym() returns
    // a void* but the caller must convert it to a function pointer.  Casting
    // a data pointer to a function pointer is technically UB in ISO C++ but
    // POSIX-specified to work on conformant platforms.  We use std::memcpy to
    // copy the bits between the two pointer types — this is the most portable
    // approach (avoids the reinterpret_cast UB flagged by MED finding round-1).
    PluginLoader loader;
    loader.handle_ = handle;
    loader.abi_hash_ = *static_cast<const char* const*>(sym_abi_hash);
    loader.manifest_blob_ = *static_cast<const std::byte* const*>(sym_manifest);
    loader.manifest_blob_size_ = *static_cast<const std::size_t*>(sym_manifest_size);

    // memcpy bit-cast: void* → function pointer (POSIX ABI contract).
    // std::memcpy is defined for any trivially-copyable types of equal size;
    // both void* and RegisterFn are pointer-sized on LP64 (macOS/Apple Silicon).
    static_assert(
        sizeof(void*) == sizeof(RegisterFn), "void* and RegisterFn must be the same size (LP64 ABI)"
    );
    RegisterFn register_fn_tmp{nullptr};
    std::memcpy(&register_fn_tmp, &sym_register, sizeof(register_fn_tmp));
    loader.register_fn_ = register_fn_tmp;

    loader.dylib_path_ = eastl::string{dylib_path.data(), dylib_path.size()};
    loader.manifest_result_ = std::move(manifest_result);

    return loader;
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

PluginLoader::~PluginLoader() {
    if (handle_ != nullptr) {
        dlclose(handle_);
        handle_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Move constructor / move assignment
// ---------------------------------------------------------------------------

PluginLoader::PluginLoader(PluginLoader&& other) noexcept
    : handle_{other.handle_},
      abi_hash_{other.abi_hash_},
      manifest_blob_{other.manifest_blob_},
      manifest_blob_size_{other.manifest_blob_size_},
      register_fn_{other.register_fn_},
      dylib_path_{std::move(other.dylib_path_)},
      manifest_result_{std::move(other.manifest_result_)} {
    // Null out the source so its destructor is a no-op.
    other.handle_ = nullptr;
    other.abi_hash_ = nullptr;
    other.manifest_blob_ = nullptr;
    other.manifest_blob_size_ = 0;
    other.register_fn_ = nullptr;
}

PluginLoader& PluginLoader::operator=(PluginLoader&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    // Close the current handle before stealing the other one.
    if (handle_ != nullptr) {
        dlclose(handle_);
    }
    handle_ = other.handle_;
    abi_hash_ = other.abi_hash_;
    manifest_blob_ = other.manifest_blob_;
    manifest_blob_size_ = other.manifest_blob_size_;
    register_fn_ = other.register_fn_;
    dylib_path_ = std::move(other.dylib_path_);
    manifest_result_ = std::move(other.manifest_result_);

    other.handle_ = nullptr;
    other.abi_hash_ = nullptr;
    other.manifest_blob_ = nullptr;
    other.manifest_blob_size_ = 0;
    other.register_fn_ = nullptr;
    return *this;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

const glibre::Result<PluginManifest>& PluginLoader::manifest_result() const noexcept {
    return manifest_result_;
}

const char* PluginLoader::abi_hash() const noexcept { return abi_hash_; }

const std::byte* PluginLoader::manifest_blob() const noexcept { return manifest_blob_; }

std::size_t PluginLoader::manifest_blob_size() const noexcept { return manifest_blob_size_; }

RegisterFn PluginLoader::register_fn() const noexcept { return register_fn_; }

const eastl::string& PluginLoader::dylib_path() const noexcept { return dylib_path_; }

}  // namespace glibre::core
