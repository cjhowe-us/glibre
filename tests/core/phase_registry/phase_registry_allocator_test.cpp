// tests/core/phase_registry/phase_registry_allocator_test.cpp
//
// Catch2 unit test: core/phase_registry allocator routing through
// PerContextAllocatorResource.
//
// Authority: chore #1070 (wire-PerContextAllocatorResource-into-phase-registry).
//            reviews/decisions/perf-budget.md §Allocator Rules #1.
//
// Named test cases (plan #1070 Unit Test Plan / DoD):
//   - core/phase_registry: allocator_ceiling_enforced
//
// This file is a SEPARATE TU from phase_registry_test.cpp because including
// both glibre/alloc.hpp and glibre/core/frame_loop.hpp in the same TU
// triggers a ContextTag redefinition conflict between glibre/alloc.hpp and
// glibre/perf_budget.hpp (two separate ContextTag definitions in glibre::).
// frame_loop.hpp includes perf_budget.hpp transitively; this test does not
// need frame_loop.hpp, so the conflict is avoided by separation.
// (The perf_budget.hpp/alloc.hpp ContextTag unification is a separate chore
// tracked under initiative #1032.)
//
// Design constraints:
//   - -fno-exceptions (error-model.md §Decision 3).
//   - No REQUIRE_THROWS usage.
//   - Does NOT include glibre/core/frame_loop.hpp.

#include <cstdint>
#include <memory_resource>

#include <catch2/catch_test_macros.hpp>

#include "glibre/alloc.hpp"
#include "glibre/core/frame_phase.hpp"
#include "glibre/core/phase_registry.hpp"

// ===========================================================================
// Test: allocator_ceiling_enforced  (chore #1070 §Unit Test Plan)
//
// Verifies that the production-callsite shape
//   glibre::PerContextAllocator alloc{glibre::ContextTag::core};
//   glibre::PerContextAllocatorResource mr{alloc};
//   PhaseRegistry reg{&mr};
// correctly routes PhaseRegistry system-entry allocations through the
// per-context allocator (bytes_used probe, matching the pattern in
// plugin_manifest_test.cpp §"pmr_string_fields_thread_allocator", plan #1065).
//
// Assertions:
//   (a) After register_system() with a long FQN (above the libc++ pmr::string
//       SSO threshold of 22 bytes), bytes_used() on the backing
//       PerContextAllocator is strictly greater than the pre-registration
//       baseline — confirming storage is charged to the per-context ceiling
//       rather than std::pmr::get_default_resource().
//   (b) After drain_all(), bytes_used() is strictly less than
//       bytes_after_register, confirming that SystemEntry destructors released
//       the FQN string storage back through mr.  The vector capacity buffer
//       is not released by clear() (only by destruction or shrink_to_fit),
//       so bytes_after_drain > bytes_before is expected and acceptable.
//
// FQN "core.transform.per_context_allocator_routing_witness" is 52 chars,
// well above the libc++ std::pmr::string SSO threshold (22 bytes), so the
// SystemEntry ctor forces a heap allocation through mr rather than SSO.
//
// Spike #1031 (iterate-phase-registry-allocator-tagging) premise is obsolete
// post-EASTL-removal; the canonical ContextTag lives in glibre/alloc.hpp
// only (the perf_budget.hpp duplicate is a separate cleanup chore, #1032).
// ===========================================================================
TEST_CASE("core/phase_registry: allocator_ceiling_enforced", "[core][phase_registry]") {
    using namespace glibre::core;

    // Stand-alone PerContextAllocator + resource.  alloc must be declared
    // before mr so it outlives it (PerContextAllocatorResource holds a
    // non-owning reference — standard PMR lifetime contract).
    glibre::PerContextAllocator alloc{glibre::ContextTag::core};
    glibre::PerContextAllocatorResource mr{alloc};

    // (a) Construct PhaseRegistry with the per-context resource — the
    // production callsite shape mandated by perf-budget.md §Allocator Rules #1
    // (chore #1070).
    PhaseRegistry reg{&mr};

    // Snapshot baseline after construction.  std::pmr::vector default-init
    // does not allocate, so bytes_before is typically 0 here.
    const std::uint64_t bytes_before = alloc.bytes_used();

    // Register a system whose FQN (52 chars) exceeds the libc++ std::pmr::string
    // SSO threshold (22 bytes).  The SystemEntry ctor copies the FQN into a
    // std::pmr::string backed by mr, which charges alloc.
    bool system_ran = false;
    reg.register_system(
        Phase::Transform,
        "core.transform.per_context_allocator_routing_witness",  // 52 chars
        [&system_ran]() noexcept { system_ran = true; }
    );
    REQUIRE(reg.system_count(Phase::Transform) == 1U);

    // Allocation must have been charged to the per-context allocator.
    const std::uint64_t bytes_after_register = alloc.bytes_used();
    REQUIRE(bytes_after_register > bytes_before);

    // Verify the registered system is callable (not corrupted by the PMR path).
    reg.for_each_system(Phase::Transform, [](const PhaseSystemFn& fn) noexcept { fn(); });
    REQUIRE(system_ran);

    // (b) After drain_all(), all SystemEntry objects in the phase vector are
    // destroyed, which triggers ~std::pmr::string on each fqn field and
    // deallocates the FQN heap storage back through mr.  bytes_used() must be
    // strictly less than bytes_after_register — confirming partial deallocation.
    //
    // Note: drain_all() calls std::pmr::vector::clear(), which destroys elements
    // (freeing FQN string storage) but does NOT release the vector's capacity
    // buffer.  The capacity buffer is freed only when the vector is destroyed
    // (i.e. when the PhaseRegistry goes out of scope) or shrink_to_fit() is
    // called.  Therefore bytes_after_drain is not guaranteed to equal bytes_before;
    // the definitive proof of routing is the bytes_after_register > bytes_before
    // assertion above.
    reg.drain_all();
    REQUIRE(reg.total_system_count() == 0U);
    const std::uint64_t bytes_after_drain = alloc.bytes_used();
    REQUIRE(bytes_after_drain < bytes_after_register);
}
