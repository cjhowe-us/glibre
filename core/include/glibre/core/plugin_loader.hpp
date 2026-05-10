#pragma once
// core/include/glibre/core/plugin_loader.hpp
//
// PluginLoader — wraps dlopen + dlsym + manifest read for a single plugin .dylib.
//
// DESIGN (reviews/decisions/plugin-abi.md §"Loader Sequence" steps 1–3):
//
//   Step 1: dlopen the candidate .dylib with RTLD_NOW | RTLD_LOCAL.
//           RTLD_NOW forces immediate symbol resolution so missing middleman
//           symbols surface as a load failure rather than a later crash.
//           RTLD_LOCAL keeps the plugin's symbols out of the global namespace.
//           Failure → core::Error::PluginDlopenFailed; dlerror() text
//           emitted to stderr at the refusal site; ErrorContext::detail
//           holds a stable string literal (HIGH-1 round-1, addressed).
//
//   Step 2: dlsym the four required export symbols.  Any missing symbol →
//           core::Error::PluginMissingEntryPoint; dlclose and abort.
//           Required symbols (plugin-abi.md §"Plugin file shape"):
//             glibre_plugin_abi_hash         — const char*
//             glibre_plugin_manifest         — const std::byte*
//             glibre_plugin_manifest_size    — std::size_t
//             glibre_plugin_register         — registration entry-point
//
//   Step 3: Read the manifest sidecar file (<dylib>.manifest) via
//           PluginManifest::open().  The result (success or error) is stored
//           in manifest_result_ without aborting the load at this layer.
//           Validation gates (PluginManifestInvalid, ABI hash, engine
//           version, name collision, deps) land in plan #230.
//
//   Steps 4–11 (ABI hash check, engine version check, name collision,
//   dependency resolution, register call, schedule rebuild, migration) land
//   in plans #230 and #231.
//
// OWNERSHIP:
//   PluginLoader owns the dlopen handle.  The destructor calls dlclose().
//   PluginLoader is movable (transfers handle ownership) and non-copyable.
//
// THREAD SAFETY:
//   PluginLoader::open() must be called from the HotReload phase (phase 8)
//   only (plugin-abi.md §"Loader Sequence" preamble, frame-phases.md §8).
//   No internal locking — single-threaded phase-8 invariant assumed.
//
// EASTL per PHILOSOPHY §11:
//   All containers and strings in the public surface use eastl::, not std::.
//   std:: is retained only for std::expected (Result alias) and std::byte.

#include <cstddef>
#include <cstdint>

#include <EASTL/string.h>
#include <EASTL/string_view.h>
#include <glibre/alloc.hpp>              // AllocatorHandle, ContextTag
#include <glibre/core/plugin_entry.hpp>  // RegisterFn — single source of truth (LOW-6)
#include <glibre/core/plugin_manifest.hpp>
#include <glibre/error.hpp>

namespace glibre::core {

// RegisterFn is the canonical function-pointer type for glibre_plugin_register.
// It is defined in plugin_entry.hpp and imported here to avoid duplication.
// Rationale (LOW finding round-1, addressed):
//   plugin_loader.hpp previously re-declared the same typedef independently.
//   Drift between the two would be silent.  plugin_entry.hpp is the C-ABI
//   surface header and owns the single definition; the loader imports it.

// ---------------------------------------------------------------------------
// PluginLoader
//
// Wraps the OS dylib handle, the resolved symbol table, and the deserialized
// manifest for one plugin.  Lifetime matches the plugin's loaded state;
// destruction closes the dylib via dlclose().
//
// Typical usage (plan #230 caller):
//
//   auto result = PluginLoader::open("plugins/render/librender.dylib");
//   if (!result) { handle_error(result.error()); return; }
//   PluginLoader& loader = *result;
//   // inspect loader.manifest_result(), loader.abi_hash(), etc.
//   // plan #230 adds the ABI-hash gate here.
//
// PluginLoader is returned by value (as Result<PluginLoader>); the caller
// receives a fully-initialised object or an error — never a half-open state.
// ---------------------------------------------------------------------------

class PluginLoader {
public:
    // ------------------------------------------------------------------
    // open — factory entry-point (loader steps 1–3)
    //
    // Executes loader steps 1–3 from plugin-abi.md §"Loader Sequence":
    //   1. dlopen(path, RTLD_NOW | RTLD_LOCAL)
    //   2. dlsym four required symbols:
    //        glibre_plugin_abi_hash
    //        glibre_plugin_manifest
    //        glibre_plugin_manifest_size
    //        glibre_plugin_register
    //   3. Read sidecar manifest via PluginManifest::open(<path>.manifest)
    //
    // Returns:
    //   glibre::Result<PluginLoader>  with a valid loader on success.
    //   Error::PluginDlopenFailed     if dlopen returned null (step 1).
    //   Error::PluginMissingEntryPoint if a required symbol is absent (step 2).
    //
    // The step-3 manifest result is stored internally regardless of
    // whether it succeeded or failed.  Plan #230 enforces the
    // "manifest must be valid" gate; at this layer we only collect data.
    //
    // @param dylib_path  Filesystem path to the plugin .dylib.
    // ------------------------------------------------------------------
    [[nodiscard]] static glibre::Result<PluginLoader> open(eastl::string_view dylib_path);

    // Destructor — dlclose(handle_) if handle_ is not nullptr.
    ~PluginLoader();

    // Non-copyable: two loaders cannot share the same dlopen handle.
    PluginLoader(const PluginLoader&) = delete;
    PluginLoader& operator=(const PluginLoader&) = delete;

    // Movable: transfers handle ownership.
    // After move, source.handle_ == nullptr and source destructor is a no-op.
    PluginLoader(PluginLoader&&) noexcept;
    PluginLoader& operator=(PluginLoader&&) noexcept;

    // ------------------------------------------------------------------
    // Accessors
    // ------------------------------------------------------------------

    /// Result of the sidecar manifest read (step 3).
    /// Has a value when the sidecar .manifest file was found and parsed.
    /// Holds PluginManifestNotFound / PluginManifestInvalid on failure.
    /// Plan #230 treats a missing manifest as a hard error.
    [[nodiscard]] const glibre::Result<PluginManifest>& manifest_result() const noexcept;

    /// Pointer to the ABI hash string from glibre_plugin_abi_hash symbol.
    /// A 64-char lowercase blake3 hex string embedded in the plugin binary.
    /// nullptr only if the loader was moved from.
    [[nodiscard]] const char* abi_hash() const noexcept;

    /// Pointer to the raw Fory manifest blob from glibre_plugin_manifest.
    /// May be nullptr for MVP stubs that export a null blob.
    [[nodiscard]] const std::byte* manifest_blob() const noexcept;

    /// Byte length of the raw Fory manifest blob.
    [[nodiscard]] std::size_t manifest_blob_size() const noexcept;

    /// Resolved glibre_plugin_register function pointer.
    /// Plan #231 invokes this after all validation gates pass.
    [[nodiscard]] RegisterFn register_fn() const noexcept;

    /// Filesystem path used to open this plugin.  Empty after move.
    [[nodiscard]] const eastl::string& dylib_path() const noexcept;

private:
    // Private default constructor — only open() creates valid instances.
    PluginLoader() = default;

    // OS dylib handle.  nullptr when moved from or before open().
    void* handle_{nullptr};

    // Symbol pointers resolved in open(); stable for the loader lifetime.
    const char* abi_hash_{nullptr};
    const std::byte* manifest_blob_{nullptr};
    std::size_t manifest_blob_size_{0};
    RegisterFn register_fn_{nullptr};

    // Filesystem path for diagnostics and plan #230 name-collision checks.
    eastl::string dylib_path_;

    // Manifest read result from step 3.
    //
    // std::expected<PluginManifest, Error> is default-constructible to
    // the value state (empty PluginManifest{}) because PluginManifest's
    // default constructor (polymorphic_allocator ctor with default argument)
    // leaves all std::pmr::string/vector members empty.  After open() returns
    // success, manifest_result_ always holds either a valid PluginManifest or
    // an error code — never the uninitialised default-construct value (the
    // field is assigned before open() returns).
    glibre::Result<PluginManifest> manifest_result_;
};

}  // namespace glibre::core
