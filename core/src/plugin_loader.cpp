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
//   <cstring>   — strlen (for attaching dlerror() text to ErrorContext)
//
// Per PHILOSOPHY §11 and error-model.md §Decision 3:
//   -fno-exceptions; no std:: containers; std::filesystem carve-out OK.

#include "glibre/core/plugin_loader.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <utility>

#include <dlfcn.h>

#include <EASTL/string.h>
#include <EASTL/string_view.h>

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
// Helper: dlsym wrapper that returns nullptr on failure and stores nothing.
// dlsym itself does not set errno; callers must check the return value.
// ---------------------------------------------------------------------------
[[nodiscard]] void* resolve_symbol(void* handle, const char* name) noexcept {
    // Clear any prior dlerror state before the call.
    (void)dlerror();
    return dlsym(handle, name);
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
        // Capture dlerror() text into a NUL-safe buffer before attaching to
        // ErrorContext::detail.  dlerror() returns a pointer to a
        // thread-local static; it is valid until the next dlerror() call.
        const char* const dl_err = dlerror();
        const eastl::string_view detail_view =
            (dl_err != nullptr) ? eastl::string_view{dl_err, std::strlen(dl_err)}
                                : eastl::string_view{"dlopen returned null (no dlerror text)"};

        return std::unexpected(glibre::Error{
            core::Error::PluginDlopenFailed,
            ErrorContext{
                .file   = __FILE__,
                .line   = __LINE__,
                .detail = detail_view,
            },
        });
    }

    // Step 2: dlsym the four required symbols.
    //
    // plugin-abi.md §"Plugin file shape" lists the canonical names:
    //   glibre_plugin_abi_hash          → const char*
    //   glibre_plugin_manifest          → const std::byte*
    //   glibre_plugin_manifest_size     → std::size_t
    //   glibre_plugin_register          → RegisterFn
    //
    // Any missing symbol is a loader refusal at step 2.  dlclose before
    // returning so we do not hold a handle to an unusable plugin.

    // glibre_plugin_abi_hash
    void* const sym_abi_hash = resolve_symbol(handle, "glibre_plugin_abi_hash");
    if (sym_abi_hash == nullptr) {
        dlclose(handle);
        return std::unexpected(glibre::Error{
            core::Error::PluginMissingEntryPoint,
            ErrorContext{
                .file   = __FILE__,
                .line   = __LINE__,
                .detail = "missing symbol: glibre_plugin_abi_hash",
            },
        });
    }

    // glibre_plugin_manifest
    void* const sym_manifest = resolve_symbol(handle, "glibre_plugin_manifest");
    if (sym_manifest == nullptr) {
        dlclose(handle);
        return std::unexpected(glibre::Error{
            core::Error::PluginMissingEntryPoint,
            ErrorContext{
                .file   = __FILE__,
                .line   = __LINE__,
                .detail = "missing symbol: glibre_plugin_manifest",
            },
        });
    }

    // glibre_plugin_manifest_size
    void* const sym_manifest_size = resolve_symbol(handle, "glibre_plugin_manifest_size");
    if (sym_manifest_size == nullptr) {
        dlclose(handle);
        return std::unexpected(glibre::Error{
            core::Error::PluginMissingEntryPoint,
            ErrorContext{
                .file   = __FILE__,
                .line   = __LINE__,
                .detail = "missing symbol: glibre_plugin_manifest_size",
            },
        });
    }

    // glibre_plugin_register
    void* const sym_register = resolve_symbol(handle, "glibre_plugin_register");
    if (sym_register == nullptr) {
        dlclose(handle);
        return std::unexpected(glibre::Error{
            core::Error::PluginMissingEntryPoint,
            ErrorContext{
                .file   = __FILE__,
                .line   = __LINE__,
                .detail = "missing symbol: glibre_plugin_register",
            },
        });
    }

    // Step 3: read sidecar manifest.
    //
    // Attempt to read <dylib_path>.manifest from disk.  If no sidecar exists
    // (MVP stubs / reference plugins) PluginManifest::open() returns
    // PluginManifestNotFound; if the file is corrupt it returns
    // PluginManifestInvalid.  Both cases are stored in manifest_result_
    // without aborting the load — plan #230 enforces "manifest must be valid".
    const std::string manifest_path = path_c + ".manifest";
    glibre::Result<PluginManifest> manifest_result =
        PluginManifest::open(eastl::string_view{manifest_path.data(), manifest_path.size()});

    // Construct a valid PluginLoader and transfer all ownership.
    //
    // The casts below convert the void* dlsym results to the typed pointer
    // types declared in the plugin ABI.  The function-pointer cast for
    // sym_register goes through reinterpret_cast: casting a data void* to
    // a function pointer is undefined in standard C++ but well-defined on
    // POSIX platforms (dlsym is declared to return void*; POSIX mandates the
    // double-pointer trick or an explicit cast per IEEE Std 1003.1).
    PluginLoader loader;
    loader.handle_ = handle;
    loader.abi_hash_ = *static_cast<const char* const*>(sym_abi_hash);
    loader.manifest_blob_ = *static_cast<const std::byte* const*>(sym_manifest);
    loader.manifest_blob_size_ = *static_cast<const std::size_t*>(sym_manifest_size);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    loader.register_fn_ = reinterpret_cast<RegisterFn>(sym_register);

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

const char* PluginLoader::abi_hash() const noexcept {
    return abi_hash_;
}

const std::byte* PluginLoader::manifest_blob() const noexcept {
    return manifest_blob_;
}

std::size_t PluginLoader::manifest_blob_size() const noexcept {
    return manifest_blob_size_;
}

RegisterFn PluginLoader::register_fn() const noexcept {
    return register_fn_;
}

const eastl::string& PluginLoader::dylib_path() const noexcept {
    return dylib_path_;
}

}  // namespace glibre::core
