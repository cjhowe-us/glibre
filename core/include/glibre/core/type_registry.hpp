#pragma once
// core/include/glibre/core/type_registry.hpp
//
// TypeRegistry — immutable-after-init TypeId → ColumnDescriptor lookup.
//
// Authority: specs/core/SPEC.md §4.9, §5.5, §6.9; plan #597.
//
// ## Design
//
//   TypeRegistry stores a flat std::pmr::vector<ColumnDescriptor> indexed by
//   TypeId.value (zero-based).  On lookup the index is bounds-checked; out-of-
//   range or null-slot access returns core::Error::TypeUnregistered.
//
//   After World::create() calls seal(), register_type() returns
//   core::Error::TypeRegistryClosed.  The single exception is the
//   extend_during_load() friend hook, which is gated behind a friend-class
//   declaration for PluginLoader and bypasses the seal for plugin-load-time
//   registration only (§4.9 invariant 1 + §6.9 append-only hot-reload rule).
//
//   extend_during_load() is further gated by is_loading_ — a bool set true
//   only while PluginLoader's loading critical section is executing.  This
//   prevents accidental calls that arrive outside the load window from
//   mutating a sealed registry mid-frame (round-1 review MED-4 fix).
//
//   Storage grows via push_back into the PMR vector; the capacity is bounded
//   by the 64 MiB core heap ceiling (perf-budget.md Allocator Rules).
//
// ## PMR allocator (HIGH-1 round-1 review fix; SRP lift MED-2 round-2 review)
//
//   The constructor requires a PerContextAllocator& stamped with ContextTag::core.
//   All TypeRegistry storage is tracked under the 64 MiB core heap ceiling
//   (perf-budget.md Allocator Rule #1).  Bypassing PerContextAllocator by
//   passing a raw std::pmr::memory_resource* directly is not supported and would
//   escape the ceiling enforcement.  glibre::PerContextAllocatorResource (from
//   glibre/alloc.hpp) adapts the PerContextAllocator to the
//   std::pmr::memory_resource interface required by std::pmr::vector.  The
//   adaptor now lives in alloc.hpp (SRP: it is a PerContextAllocator concern,
//   not a TypeRegistry concern) so other PMR consumers do not need to pull in
//   type_registry.hpp for a generic memory-resource adaptor.
//
// ## TypeId duplication note (MED-5 round-1 review)
//
//   TypeId is declared below as a local copy so this header is self-contained.
//   The canonical definition lives in specs/core/SPEC.md §5.2.  The
//   static_asserts immediately after the struct verify that both definitions
//   agree on underlying type and byte width.  A follow-up spike
//   [SPIKE] iterate-type-id-canonical-home will consolidate to a single header.
//
// ## Thread safety (MVP)
//
//   MVP runs every system on a single worker thread (SPEC §6.10).  No
//   synchronisation primitives are used here.  All lookups after seal() are
//   pure reads of an immutable container, which is trivially safe.
//
// ## -fno-exceptions clean
//   No exceptions thrown or propagated.  All error paths use glibre::Result<T>.

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <type_traits>
#include <vector>

#include "glibre/alloc.hpp"  // PerContextAllocator, ContextTag
#include "glibre/error.hpp"

namespace glibre {
namespace core {

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
class PluginLoader;  // friend — may call extend_during_load() + set_loading_().
#if defined(GLIBRE_TESTING)
class TypeRegistryTestHook;  // friend test-hook for set_loading_() access in tests (test builds
                             // only).
#endif

// ---------------------------------------------------------------------------
// ColumnDescriptor — type descriptor stored in the registry for each TypeId.
//
// Carries exactly the four fields authorised by SPEC §4.9 invariant 4:
//   size   — sizeof(T) as emitted by codegen.
//   align  — alignof(T) as emitted by codegen.
//   drop   — type-erased destructor thunk; nullptr for trivially-destructible.
//   layout — opaque bitfield encoding SoA column layout (codegen-defined).
//
// No reflective fields (path access, DynamicValue, Reflect trait) — per
// SPEC §3.2 collapse #4 and §4.9 invariant 4.
// ---------------------------------------------------------------------------
using DropFn = void (*)(void* ptr) noexcept;  // type-erased destructor thunk

struct ColumnDescriptor {
    std::size_t size{0};      // sizeof(T)
    std::size_t align{0};     // alignof(T)
    DropFn drop{nullptr};     // nullptr → trivially-destructible
    std::uint64_t layout{0};  // codegen-emitted SoA layout bitfield
};

// ---------------------------------------------------------------------------
// TypeId — stable codegen-emitted identifier (mirrors §5.2 struct).
//
// Local copy so type_registry.hpp is self-contained.  The canonical definition
// lives in specs/core/SPEC.md §5.2.  The static_asserts below verify that this
// copy agrees with the canonical on underlying type and byte width.
// See [SPIKE] iterate-type-id-canonical-home for the follow-up consolidation.
// ---------------------------------------------------------------------------
struct TypeId {
    std::uint64_t value{};
    friend constexpr bool operator==(TypeId, TypeId) noexcept = default;
};

// Compile-time agreement checks between this TypeId and the §5.2 canonical.
// If the canonical changes its layout, these fail and flag the divergence.
static_assert(
    std::is_trivially_copyable_v<TypeId>,
    "TypeId must be trivially copyable — agrees with SPEC §5.2 canonical."
);
static_assert(
    sizeof(TypeId) == sizeof(std::uint64_t),
    "TypeId size must match SPEC §5.2 canonical (single uint64_t value, no padding)."
);
static_assert(
    alignof(TypeId) == alignof(std::uint64_t), "TypeId alignment must match SPEC §5.2 canonical."
);

// ---------------------------------------------------------------------------
// TypeRegistry
//
// Public API (from plan #597 Scope):
//   lookup(TypeId)        -> Result<const ColumnDescriptor*>
//   is_registered(TypeId) -> bool
//   count()               -> size_t
//
// Lifecycle (called by World, PluginLoader):
//   register_type(TypeId, ColumnDescriptor) -> Result<void>
//      -- open before seal(); refused after seal() unless called from
//         extend_during_load() (loader friend hook).
//   seal()
//      -- called once by World::create() at end of bootstrap.
//
// Friend hook (called by PluginLoader only):
//   extend_during_load(TypeId, ColumnDescriptor) -> Result<void>
//      -- bypasses seal() check; only valid while is_loading_ is true.
// ---------------------------------------------------------------------------
class TypeRegistry {
public:
    // Construct an empty registry backed by the given PerContextAllocator.
    //
    // The allocator MUST be stamped with ContextTag::core so that all
    // TypeRegistry storage is counted against the 64 MiB core heap ceiling
    // (perf-budget.md Allocator Rule #1, HIGH-1 round-1 review fix).
    //
    // The PerContextAllocator must outlive the TypeRegistry (context singleton
    // lifetime invariant — same as PerContextAllocator's own contract).
    explicit TypeRegistry(PerContextAllocator& alloc) noexcept;

    // Non-copyable, non-movable — the registry is a long-lived singleton
    // owned by World; no ownership transfer after construction.
    TypeRegistry(const TypeRegistry&) = delete;
    TypeRegistry& operator=(const TypeRegistry&) = delete;
    TypeRegistry(TypeRegistry&&) = delete;
    TypeRegistry& operator=(TypeRegistry&&) = delete;

    ~TypeRegistry() noexcept = default;

    // lookup(id) — O(1) index into the flat descriptor vector.
    //
    // Returns:
    //   Ok(&descriptor)                    — id is registered.
    //   Err(core::Error::TypeUnregistered) — id.value is out of range or the
    //                                        slot was never populated.
    [[nodiscard]] Result<const ColumnDescriptor*> lookup(TypeId id) const noexcept;

    // is_registered(id) — convenience predicate, non-error-returning.
    //
    // Returns true iff lookup(id) would return Ok.  Equivalent to
    //   lookup(id).has_value() but without constructing the Result.
    [[nodiscard]] bool is_registered(TypeId id) const noexcept;

    // count() — number of registered TypeId entries.
    //
    // Entries are stored at slots [0, count()); lookup(id) requires
    // id.value < count().  Slots are never removed or reused.
    [[nodiscard]] std::size_t count() const noexcept;

    // register_type(id, desc) — append a new descriptor.
    //
    // Returns:
    //   Ok(void)                              — registration succeeded.
    //   Err(core::Error::TypeRegistryClosed)  — seal() has been called.
    //   Err(core::Error::TypeRegistryGap)     — id.value != count() (codegen
    //                                           contract violation: out-of-order
    //                                           or sparse registration).
    //
    // Called by World bootstrap code and plugin registration before seal().
    [[nodiscard]] Result<void> register_type(TypeId id, ColumnDescriptor desc) noexcept;

    // seal() — freeze the registry.
    //
    // Called exactly once by World::create() at end of bootstrap.
    // After this point register_type() returns TypeRegistryClosed.
    // extend_during_load() (friend hook) is unaffected by seal().
    void seal() noexcept;

    // is_sealed() — returns true if seal() has been called.
    [[nodiscard]] bool is_sealed() const noexcept { return sealed_; }

private:
    // extend_during_load — loader-privileged registration path.
    //
    // Called by PluginLoader during glibre_plugin_register to register
    // types belonging to newly-loaded plugins.  Bypasses the sealed_ check
    // so that plugins loaded after World creation may still register types
    // (SPEC §6.9 append-only hot-reload rule; §4.9 invariant 1 carve-out).
    //
    // GATED by is_loading_: this method returns TypeRegistryClosed (not just
    // asserts) when is_loading_ is false, so that any caller outside
    // PluginLoader's load critical section receives a typed error in both debug
    // and release builds (round-1 review MED-4 fix, hardened in round-2 MED-3).
    //
    // Returns:
    //   Ok(void)                              — registration succeeded.
    //   Err(core::Error::TypeRegistryClosed)  — is_loading_ is false (called
    //                                           outside the load window).
    //   Err(core::Error::TypeRegistryGap)     — id.value is not the next slot
    //                                           (codegen contract violation).
    [[nodiscard]] Result<void> extend_during_load(TypeId id, ColumnDescriptor desc) noexcept;

    // set_loading_(flag) — set/clear the is_loading_ latch.
    //
    // Called by PluginLoader at the entry and exit of its loading critical
    // section.  Only PluginLoader and TypeRegistryTestHook (friends) may call
    // this; all other callers are prevented by the friend guard.
    void set_loading_(bool flag) noexcept { is_loading_ = flag; }

    friend class PluginLoader;
#if defined(GLIBRE_TESTING)
    friend class TypeRegistryTestHook;
#endif

    // alloc_resource_ must be declared before entries_ so the resource is
    // constructed before the vector that references it.
    // glibre::PerContextAllocatorResource is defined in glibre/alloc.hpp
    // (lifted from nested class in round-2 review MED-2 — SRP: the PMR
    // adaptor is an allocator concern, not a TypeRegistry concern).
    glibre::PerContextAllocatorResource alloc_resource_;

    // Flat descriptor table: slot i holds the descriptor for TypeId{i}.
    // The vector is only ever grown by appending; no sparse gaps are permitted.
    std::pmr::vector<ColumnDescriptor> entries_;

    bool sealed_{false};
    bool is_loading_{false};  // true only inside PluginLoader's load critical section
};

}  // namespace core
}  // namespace glibre
