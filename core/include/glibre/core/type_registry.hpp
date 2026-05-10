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
//   Storage grows via push_back into the PMR vector; the capacity is bounded
//   by the 64 MiB core heap ceiling (perf-budget.md Allocator Rules).
//
// ## PMR allocator
//
//   The constructor accepts a std::pmr::memory_resource* from the caller
//   (typically the core PerContextAllocator's backing resource).  Passing
//   nullptr selects std::pmr::get_default_resource().
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
#include <span>
#include <vector>

#include "glibre/error.hpp"

namespace glibre {
namespace core {

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
class PluginLoader;  // friend — may call extend_during_load() after seal().

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
// Declared separately here so type_registry.hpp is self-contained;
// the canonical definition lives in specs/core/SPEC.md §5.2 and the
// full §5 stub header.  This duplicate must stay in sync.
// ---------------------------------------------------------------------------
struct TypeId {
    std::uint64_t value{};
    friend constexpr bool operator==(TypeId, TypeId) noexcept = default;
};

// ---------------------------------------------------------------------------
// TypeRegistry
//
// Public API (from plan #597 Scope):
//   lookup(TypeId)       -> Result<const ColumnDescriptor*>
//   is_registered(TypeId) -> bool
//   count()              -> size_t
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
//      -- bypasses seal() check; used during glibre_plugin_register.
// ---------------------------------------------------------------------------
class TypeRegistry {
public:
    // Construct an empty registry backed by `resource`.
    // Passing nullptr selects std::pmr::get_default_resource().
    explicit TypeRegistry(std::pmr::memory_resource* resource = nullptr) noexcept;

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
    //   Ok(&descriptor)               — id is registered.
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
    //   Ok(void)                         — registration succeeded.
    //   Err(core::Error::TypeRegistryClosed) — seal() has been called.
    //
    // The TypeId must equal count() before the call (the next available
    // slot).  Sparse or out-of-order registration is rejected with
    // core::Error::TypeUnregistered to signal a codegen contract violation
    // (the caller should assert rather than handle this in production code).
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
    // Preconditions (same as register_type):
    //   id.value == entries_.size()  — must be the next slot.
    //   desc.size > 0 && desc.align > 0
    //
    // Returns:
    //   Ok(void)                        — registration succeeded.
    //   Err(core::Error::TypeUnregistered) — id.value is not the next slot
    //                                       (codegen contract violation).
    [[nodiscard]] Result<void> extend_during_load(TypeId id, ColumnDescriptor desc) noexcept;

    friend class PluginLoader;

    // Flat descriptor table: slot i holds the descriptor for TypeId{i}.
    // Null entry (size==0) is the sentinel for an unpopulated slot (the
    // vector is only ever grown by appending; no sparse gaps are permitted).
    std::pmr::vector<ColumnDescriptor> entries_;

    bool sealed_{false};
};

}  // namespace core
}  // namespace glibre
