#pragma once
// core/include/glibre/alloc.hpp
//
// glibre::PerContextAllocator — tag-tracked heap allocator with per-context
// ceiling enforcement.
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
//   spdlog::warn once per ContextTag for the lifetime of the process.
//   Full once-per-tag-per-frame throttling (tied to FrameLoop phase 9 reset)
//   is deferred to the perf-budget framework in plan #241; the MVP throttle
//   is once-per-tag-per-process.
//
// ## -fno-exceptions clean
//   No exceptions thrown or propagated.  Error path uses std::unexpected.

#include <atomic>
#include <cstddef>
#include <cstdint>

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
// ---------------------------------------------------------------------------

inline constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;

inline constexpr std::uint64_t kContextCeilings[kContextTagCount] = {
    //  core      platform  data      shader    render     geometry   physics   content    tools
    64 * kMiB, 16 * kMiB, 32 * kMiB, 32 * kMiB, 512 * kMiB, 256 * kMiB, 128 * kMiB, 256 * kMiB, 256 * kMiB,
};

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
    [[nodiscard]] glibre::Result<void*>
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
};

// ---------------------------------------------------------------------------
// register_allocator — stub registration point for the perf-budget framework
//
// Called once per PerContextAllocator at construction.  Plan #241 will wire
// a real registry that the perf-budget CI gate and HUD enumerate.  The MVP
// stub is a no-op; the signature is stable so callers can opt in now.
// ---------------------------------------------------------------------------

void register_allocator(PerContextAllocator& alloc) noexcept;

}  // namespace glibre
