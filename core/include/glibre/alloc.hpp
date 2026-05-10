#pragma once
// core/include/glibre/alloc.hpp
//
// glibre::PerContextAllocator — tag-tracked heap allocator with per-context
// ceiling enforcement.
//
// Also exports:
//   glibre::PerContextAllocatorResource — std::pmr::memory_resource adaptor
//     backed by a PerContextAllocator so that PMR containers (std::pmr::vector,
//     etc.) track their allocations under the per-context ceiling.  Lifted here
//     from TypeRegistry (SRP: the adaptor is owned by PerContextAllocator, not
//     TypeRegistry; any future per-context PMR consumer — PhaseRegistry, plugin-
//     owned-type tables — reuses this class without pulling type_registry.hpp).
//
// Authority: reviews/decisions/perf-budget.md §Allocator Rules #1-3, plan #238.
//
// ## Design
//
//   Every bounded context owns one PerContextAllocator instance, stamped with
//   a ContextTag at construction.  Allocation bytes are tracked via an atomic
//   counter (uint64_t) so that concurrent allocations from multiple threads
//   within the same context are safely counted.
//
//   When GLIBRE_ALLOC_STRICT=1 (diagnostic/debug builds), allocate() returns
//   std::unexpected{core::Error::OutOfBudget} if the requested allocation
//   would push bytes_used() above the ceiling.  In shipping builds the ceiling
//   check is elided; callers always receive a valid pointer.  Ceiling overruns
//   in shipping builds emit a one-time spdlog::warn per ContextTag (Rule #3).
//
//   Backing allocations delegate to posix_memalign / free (PHILOSOPHY §11
//   carve-out: raw allocation below all allocator abstractions).  This class
//   is NOT a C++ Allocator concept; it is an engine-level allocator handle.
//
// ## Per-context ceiling table
//
//   Ceilings come from perf-budget.md §Per-Context Budget Table (heap column):
//
//     core      =  64 MiB
//     platform  =  16 MiB
//     data      =  32 MiB
//     shader    =  32 MiB
//     render    = 512 MiB
//     geometry  = 256 MiB
//     physics   = 128 MiB
//     content   = 256 MiB
//     tools     = 256 MiB
//
//   These are declared in ContextTag order in kContextCeilings[].
//
// ## Thread safety
//
//   bytes_used() is maintained by an atomic<uint64_t> with relaxed ordering
//   for reads (telemetry only) and memory_order_acq_rel on the
//   compare-exchange in allocate() so that the byte-counter increment has
//   acquire-release happens-before guarantees within each PerContextAllocator
//   instance.
//
// ## Registry stub
//
//   glibre::register_allocator(PerContextAllocator&) provides a forward point
//   for the perf-budget framework (#241) to enumerate all allocators.  MVP
//   implementation is a no-op stub; the registry is wired in plan #241.
//   The constructor calls register_allocator(*this) so the wiring is automatic
//   once plan #241 provides a real registry.
//
// ## Shipping-build soft warning (Rule #3)
//
//   When GLIBRE_ALLOC_STRICT is NOT set, a ceiling overrun emits
//   spdlog::warn once per ContextTag PER INSTANCE (not process-global).
//   The warn-once flag is a per-instance atomic<bool> member so that:
//     (a) tests can construct independent PerContextAllocator instances
//         and each will see its own untripped warn-once state; and
//     (b) the flag is clearly owned and reset when the allocator is
//         destroyed, with no hidden process-global side effects.
//   Full once-per-tag-per-frame throttling (tied to FrameLoop phase 9 reset)
//   is deferred to the perf-budget framework in plan #241; the MVP throttle
//   is once-per-tag-per-instance-lifetime.
//
// ## -fno-exceptions clean
//   No exceptions thrown or propagated.  Error path uses std::unexpected.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory_resource>

#include "glibre/error.hpp"

namespace glibre {

// ---------------------------------------------------------------------------
// ContextTag — nine bounded-context tags (perf-budget.md §Allocator Rules #1)
// ---------------------------------------------------------------------------

enum class ContextTag : std::uint8_t {
    core = 0,
    platform,
    data,
    shader,
    render,
    geometry,
    physics,
    content,
    tools,
};

// Number of distinct ContextTag values.
inline constexpr std::size_t kContextTagCount = 9;

// ---------------------------------------------------------------------------
// kContextCeilings — ceiling in bytes per ContextTag
//
// Indexed by static_cast<std::uint8_t>(tag).  Values from perf-budget.md
// §Per-Context Budget Table, heap column.
//
// Static asserts below (kContextCeilings_static_checks) pin each enum
// enumerator to its positional index.  If the ContextTag enum is reordered,
// the assert fires at compile time instead of silently scrambling ceilings.
// ---------------------------------------------------------------------------

inline constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;

inline constexpr std::uint64_t kContextCeilings[kContextTagCount] = {
    //  [0] core     [1] platform  [2] data     [3] shader   [4] render
    64 * kMiB,
    16 * kMiB,
    32 * kMiB,
    32 * kMiB,
    512 * kMiB,
    //  [5] geometry  [6] physics   [7] content  [8] tools
    256 * kMiB,
    128 * kMiB,
    256 * kMiB,
    256 * kMiB,
};

// Compile-time index pinning: if ContextTag enum order changes these fail.
static_assert(static_cast<std::uint8_t>(ContextTag::core) == 0);
static_assert(static_cast<std::uint8_t>(ContextTag::platform) == 1);
static_assert(static_cast<std::uint8_t>(ContextTag::data) == 2);
static_assert(static_cast<std::uint8_t>(ContextTag::shader) == 3);
static_assert(static_cast<std::uint8_t>(ContextTag::render) == 4);
static_assert(static_cast<std::uint8_t>(ContextTag::geometry) == 5);
static_assert(static_cast<std::uint8_t>(ContextTag::physics) == 6);
static_assert(static_cast<std::uint8_t>(ContextTag::content) == 7);
static_assert(static_cast<std::uint8_t>(ContextTag::tools) == 8);
// Ceiling spot-checks: verify representative entries match perf-budget.md values.
static_assert(kContextCeilings[static_cast<std::uint8_t>(ContextTag::core)] == 64 * kMiB);
static_assert(kContextCeilings[static_cast<std::uint8_t>(ContextTag::render)] == 512 * kMiB);
static_assert(kContextCeilings[static_cast<std::uint8_t>(ContextTag::physics)] == 128 * kMiB);

// ---------------------------------------------------------------------------
// PerContextAllocator — tag-tracked, ceiling-enforced heap allocator
// ---------------------------------------------------------------------------

class PerContextAllocator {
public:
    // Construct a PerContextAllocator with an explicit tag and ceiling.
    //
    // The `tag` is stored immutably for telemetry.  `ceiling_bytes` is the
    // hard ceiling enforced in GLIBRE_ALLOC_STRICT builds (diagnostic / debug).
    // Pass kContextCeilings[static_cast<uint8_t>(tag)] for the canonical ceiling,
    // or a smaller value in unit tests to exercise the ceiling enforcement path.
    //
    // Calls register_allocator(*this) so the allocator is enumerable by the
    // perf-budget framework once plan #241 wires the real registry.
    explicit PerContextAllocator(ContextTag tag, std::uint64_t ceiling_bytes) noexcept;

    // Convenience constructor: ceiling is taken from kContextCeilings[tag].
    explicit PerContextAllocator(ContextTag tag) noexcept;

    // Non-copyable, non-movable.  Allocators are long-lived context singletons.
    PerContextAllocator(const PerContextAllocator&) = delete;
    PerContextAllocator& operator=(const PerContextAllocator&) = delete;
    PerContextAllocator(PerContextAllocator&&) = delete;
    PerContextAllocator& operator=(PerContextAllocator&&) = delete;

    ~PerContextAllocator() noexcept = default;

    // allocate(bytes, align) — allocate `bytes` aligned to `align` bytes.
    //
    // `align` must be a power of two.  Passing zero for `align` uses
    // alignof(std::max_align_t).  Values less than sizeof(void*) are rounded
    // up to sizeof(void*) (posix_memalign minimum).
    //
    // In GLIBRE_ALLOC_STRICT builds:
    //   Returns std::unexpected{core::Error::OutOfBudget} if
    //   bytes_used() + bytes > ceiling_bytes().
    //
    // In shipping builds (GLIBRE_ALLOC_STRICT not set):
    //   Ceiling check is skipped; callers always receive a valid pointer.
    //   If bytes_used() + bytes would exceed ceiling_bytes(), a one-time
    //   spdlog::warn is emitted per ContextTag (Rule #3).
    //
    // bytes == 0 is valid and returns a non-null uniquely-aligned pointer
    // without advancing the byte counter.
    //
    // Returns std::unexpected{core::Error::OutOfBudget} only on ceiling breach
    // (strict mode).  A system-level allocation failure (OOM) calls
    // std::abort() — the engine does not attempt to recover from OOM.
    [[nodiscard]] Result<void*>
    allocate(std::size_t bytes, std::size_t align = alignof(std::max_align_t)) noexcept;

    // deallocate(p, bytes) — release memory previously returned by allocate().
    //
    // `bytes` must match the `bytes` argument passed to allocate() for pointer p.
    // Passing the wrong size or a pointer not obtained from this allocator is
    // undefined behaviour (same contract as operator delete).
    //
    // Decrements the internal byte counter by `bytes`.
    void deallocate(void* p, std::size_t bytes) noexcept;

    // bytes_used() — current live heap bytes for this context.
    //
    // Read with relaxed ordering — safe for telemetry / HUD display.
    // Not guaranteed to observe concurrent allocations in progress.
    [[nodiscard]] std::uint64_t bytes_used() const noexcept;

    // bytes_ceiling() — the ceiling enforced in GLIBRE_ALLOC_STRICT builds.
    [[nodiscard]] std::uint64_t bytes_ceiling() const noexcept { return ceiling_; }

    // tag() — the ContextTag this allocator was constructed with.
    [[nodiscard]] ContextTag tag() const noexcept { return tag_; }

private:
    ContextTag tag_;
    std::uint64_t ceiling_;
    std::atomic<std::uint64_t> bytes_used_{0};
#if !defined(GLIBRE_ALLOC_STRICT) || !GLIBRE_ALLOC_STRICT
    // Per-instance warn-once flag for the shipping-build soft-warn path (Rule #3).
    // Per-instance (not TU-static) so multiple PerContextAllocator instances in
    // tests each have independent warn state.  See §Shipping-build soft warning.
    std::atomic<bool> warn_once_flag_{false};
#endif
};

// ---------------------------------------------------------------------------
// register_allocator — stub registration point for the perf-budget framework
//
// Called once per PerContextAllocator at construction.  Plan #241 will wire
// a real registry that the perf-budget CI gate and HUD enumerate.  The MVP
// stub is a no-op; the signature is stable so callers can opt in now.
// ---------------------------------------------------------------------------

void register_allocator(PerContextAllocator& alloc) noexcept;

// ---------------------------------------------------------------------------
// Testing observability hooks (GLIBRE_TESTING only — plan #990)
//
// When GLIBRE_TESTING=1, register_allocator() increments a TU-global atomic
// counter so that unit tests can assert the constructor call fires exactly
// once per PerContextAllocator construction without a full AllocatorRegistry.
//
// These functions are NEVER available in production builds.  They are
// intentionally omitted when GLIBRE_TESTING is not defined so that no
// testing surface leaks into shipping code.
//
// Usage (in a GLIBRE_TESTING=1 test target):
//   testing_reset_register_allocator_call_count();   // zero the counter
//   glibre::PerContextAllocator alloc{...};
//   REQUIRE(testing_register_allocator_call_count() == 1);
// ---------------------------------------------------------------------------

#ifdef GLIBRE_TESTING

// Returns the number of times register_allocator() has been called since
// the last testing_reset_register_allocator_call_count() or program start.
//
// Thread-safe: counter is std::atomic<std::uint64_t> with relaxed ordering
// (sufficient for single-threaded tests; sequential-consistency not required
// because the test observes the counter only after construction completes on
// the same thread).
[[nodiscard]] std::uint64_t testing_register_allocator_call_count() noexcept;

// Resets the call counter to zero.  Call before each test assertion that
// depends on a specific count so that prior constructions (e.g. from other
// test cases or from construction of static/global allocators) do not bleed
// into the assertion.
void testing_reset_register_allocator_call_count() noexcept;

#endif  // GLIBRE_TESTING

// Note: register_allocator() call observability (the GLIBRE_TESTING counter
// above) is shipped by plan #990.  AllocatorRegistry enumeration — the ability
// to iterate all live allocators — remains deferred to plan #241.

// ---------------------------------------------------------------------------
// AllocatorHandle — tag-stamped allocator wrapper for plugin call sites
//
// Authority: reviews/decisions/perf-budget.md §Allocator Rules #1, plan #989.
//
// ## Design
//
//   Plugin call sites obtain an AllocatorHandle once at glibre_plugin_register
//   time (via PluginContext::alloc).  The handle carries a ContextTag stamped
//   at construction so plugin code never needs to supply the tag explicitly
//   on each allocate() / deallocate() call.
//
//   AllocatorHandle holds a non-owning reference to the engine's long-lived
//   PerContextAllocator (owned by the bounded context, not by the plugin).
//   The engine guarantees the allocator outlives any AllocatorHandle derived
//   from it — handles must not be persisted past the plugin's lifetime.
//
//   AllocatorHandle forwards allocate() / deallocate() to the underlying
//   PerContextAllocator, injecting the stamped tag.  No additional state
//   is maintained; all ceiling enforcement and byte counting reside in the
//   PerContextAllocator.
//
// ## Tag stamping
//
//   The tag is passed once at AllocatorHandle construction.  The engine's
//   plugin loader stamps the ContextTag that corresponds to the registering
//   plugin's bounded context, so the plugin binary has no tag-enum dependency.
//
// ## Thread safety
//
//   Thread safety is identical to the underlying PerContextAllocator.
//   AllocatorHandle itself carries no mutable state; concurrent copies of the
//   same handle forwarding to the same PerContextAllocator behave identically
//   to concurrent direct callers of that allocator.
//
// ## Value-category contract
//
//   AllocatorHandle is copy-constructible and move-constructible (both are
//   semantically equivalent — the "copy" is a bitwise copy of the reference
//   and tag, not a deep clone).  Both copy-assignment and move-assignment are
//   deleted because C++ does not permit assigning to reference members.
//
//   In short:
//     AllocatorHandle h1{alloc, tag};   // construct
//     AllocatorHandle h2 = h1;          // copy-construct — OK
//     AllocatorHandle h3 = std::move(h1); // move-construct — OK (same as copy)
//     h2 = h3;                          // ERROR: copy-assignment deleted
//
//   This contract is intentional: rebinding a handle to a different allocator
//   would be a lifetime hazard (the old reference could dangle).  The caller
//   must construct a new handle explicitly.
//
//   PluginContext::alloc holds an AllocatorHandle by value; the loader can
//   copy-construct it into PluginContext during aggregate initialisation
//   because AllocatorHandle is copy-constructible.  Plugins must not store
//   the handle past their own lifetime (the underlying PerContextAllocator is
//   owned by the engine, not by the plugin).
//
// ## Lifetime contract
//
//   The PerContextAllocator passed at construction MUST outlive every
//   AllocatorHandle derived from it.  The engine guarantees this for
//   plugin-lifetime handles (the allocator is a long-lived context singleton).
//   Test code must ensure the allocator is declared before the handle and goes
//   out of scope after it (RAII ordering).
//
// ## -fno-exceptions clean
//   No exceptions thrown or propagated.  Error path uses std::unexpected.
// ---------------------------------------------------------------------------

class AllocatorHandle {
public:
    // Construct an AllocatorHandle that forwards all allocation calls to
    // `alloc`, stamping every call with `tag`.
    //
    // Precondition: `alloc` must outlive all AllocatorHandle instances
    // derived from it (the engine guarantees this for plugin lifetimes).
    explicit AllocatorHandle(PerContextAllocator& alloc, ContextTag tag) noexcept
        : alloc_{alloc},
          tag_{tag} {}

    // Rvalue constructor is deleted: AllocatorHandle(PerContextAllocator{...}, tag) would bind
    // the handle's reference member to a temporary that is destroyed at the end of the full
    // expression — a guaranteed dangling reference.  Deleting this overload makes the hazard a
    // compile-time error rather than silent UB at runtime (MED-4, round-2 review).
    explicit AllocatorHandle(PerContextAllocator&&, ContextTag) = delete;

    // Copy-constructible: multiple handles with the same tag and underlying
    // allocator are permitted.  Copying does not transfer ownership — the
    // handle is non-owning (holds a reference, not a pointer).
    AllocatorHandle(const AllocatorHandle&) noexcept = default;

    // Copy assignment is deleted: C++ does not allow assigning to references,
    // so a type holding a reference member cannot have an assignable copy.
    // Use a new handle to rebind to a different allocator.
    AllocatorHandle& operator=(const AllocatorHandle&) = delete;

    // Move-constructible: semantically equivalent to copy for a non-owning
    // handle (both the "source" and "destination" reference the same
    // PerContextAllocator after the move).
    AllocatorHandle(AllocatorHandle&&) noexcept = default;

    // Move assignment is deleted for the same reason as copy assignment
    // (reference member prevents rebinding via assignment).
    AllocatorHandle& operator=(AllocatorHandle&&) = delete;

    ~AllocatorHandle() noexcept = default;

    // allocate(bytes, align) — allocate `bytes` aligned to `align` bytes.
    //
    // Forwards to alloc_.allocate(bytes, align) with no additional overhead.
    // The tag is stamped by the PerContextAllocator's byte-counter bookkeeping,
    // not by AllocatorHandle — the handle merely removes the per-call tag
    // argument from plugin call sites.
    //
    // In GLIBRE_ALLOC_STRICT builds the ceiling check on the underlying
    // PerContextAllocator may return std::unexpected{core::Error::OutOfBudget}.
    [[nodiscard]] Result<void*>
    allocate(std::size_t bytes, std::size_t align = alignof(std::max_align_t)) noexcept {
        return alloc_.allocate(bytes, align);
    }

    // deallocate(p, bytes) — release memory previously returned by allocate().
    //
    // Forwards to alloc_.deallocate(p, bytes).  `bytes` must match the
    // argument passed to allocate() for pointer p (same contract as the
    // underlying PerContextAllocator).
    void deallocate(void* p, std::size_t bytes) noexcept { alloc_.deallocate(p, bytes); }

    // tag() — the ContextTag stamped at construction.
    [[nodiscard]] ContextTag tag() const noexcept { return tag_; }

    // wraps(alloc) — identity check: returns true if this handle wraps the given allocator.
    //
    // Used in tests and diagnostic tooling to verify that the loader stamped the correct
    // PerContextAllocator into a PluginContext without exposing a public mutable reference.
    // Replacing the former underlying() accessor (which returned a non-const ref and allowed
    // plugin code to bypass the tag stamp) with this predicate closes that bypass
    // (MED-3, round-2 review).
    //
    // For test cases that need to inspect the byte counter of the wrapped allocator, declare
    // the PerContextAllocator as a named local variable in the test body — test code already
    // owns the allocator, so no accessor into the handle is needed.
    [[nodiscard]] bool wraps(const PerContextAllocator& alloc) const noexcept {
        return &alloc_ == &alloc;
    }

private:
    PerContextAllocator& alloc_;
    ContextTag tag_;
};

// ---------------------------------------------------------------------------
// PerContextAllocatorResource — std::pmr::memory_resource adaptor
//
// Authority: perf-budget.md §Allocator Rules #1, plan #597 (MED-2 SRP lift).
//
// ## Design
//
//   Wraps a PerContextAllocator so that PMR containers (std::pmr::vector,
//   std::pmr::unordered_map, …) track their storage under the per-context
//   ceiling.  Bypassing PerContextAllocator by constructing a PMR vector with
//   std::pmr::get_default_resource() would silently escape ceiling enforcement.
//
//   do_is_equal() uses pointer identity (this == &other).  The rationale:
//   PerContextAllocator is non-copyable (context singleton invariant per
//   perf-budget.md §Allocator Rules #1); therefore each
//   PerContextAllocatorResource wraps exactly one allocator instance and is
//   equal only to itself.  Self-equality is the only meaningful case and avoids
//   any cross-resource allocation-transfer by PMR containers.
//
// ## Usage
//
//   Store one PerContextAllocatorResource as a named member before any PMR
//   container member — construction order matters because the PMR vector holds
//   a non-owning pointer to its memory resource.
//
//   Example (TypeRegistry pattern):
//     class Foo {
//         glibre::PerContextAllocatorResource mr_{alloc};  // declared first
//         std::pmr::vector<T> items_{&mr_};                // references mr_
//     };
//
// ## Virtual dispatch
//
//   std::pmr::memory_resource requires virtual dispatch.  This is acceptable
//   because PerContextAllocatorResource is used only at construction time (PMR
//   containers resolve the resource pointer once and cache it); the overhead is
//   not on any per-frame hot path.
//
// ## -fno-exceptions clean
//   do_allocate() calls std::abort() on ceiling breach rather than throwing
//   std::bad_alloc.  The engine does not recover from OOM; this matches the
//   PerContextAllocator OOM contract in alloc.hpp.
// ---------------------------------------------------------------------------

class PerContextAllocatorResource final : public std::pmr::memory_resource {
public:
    // Construct a resource backed by the given allocator.
    //
    // Precondition: `alloc` must outlive this resource and any PMR container
    // that holds a pointer to it (same lifetime constraint as AllocatorHandle).
    explicit PerContextAllocatorResource(PerContextAllocator& alloc) noexcept
        : alloc_{alloc} {}

    // Non-copyable, non-movable — holds a reference to the allocator.
    // Construct a new resource if you need a separate resource handle.
    PerContextAllocatorResource(const PerContextAllocatorResource&) = delete;
    PerContextAllocatorResource& operator=(const PerContextAllocatorResource&) = delete;
    PerContextAllocatorResource(PerContextAllocatorResource&&) = delete;
    PerContextAllocatorResource& operator=(PerContextAllocatorResource&&) = delete;

    ~PerContextAllocatorResource() noexcept override = default;

protected:
    // do_allocate — forward to the backing PerContextAllocator.
    //
    // On ceiling breach (GLIBRE_ALLOC_STRICT builds), calls std::abort().
    // The PMR interface expects a valid pointer or a thrown exception; since
    // the engine compiles with -fno-exceptions, abort is the only option.
    // Ceiling in diagnostic/debug builds is set by the ContextTag; production
    // builds use the soft-warning path (see alloc.hpp §Shipping-build soft
    // warning).
    void* do_allocate(std::size_t bytes, std::size_t alignment) override;

    // do_deallocate — forward to the backing PerContextAllocator.
    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) noexcept override;

    // do_is_equal — pointer identity.
    //
    // PerContextAllocator is non-copyable (single-instance-per-context per
    // perf-budget.md §Allocator Rules #1).  Each PerContextAllocatorResource
    // wraps exactly one allocator, so self-equality (this == &other) is the
    // only meaningful equality and prevents cross-resource allocation-transfer
    // by PMR containers (which call is_equal before swapping resources).
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

private:
    PerContextAllocator& alloc_;
};

}  // namespace glibre
