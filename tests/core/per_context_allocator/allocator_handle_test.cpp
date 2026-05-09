// tests/core/per_context_allocator/allocator_handle_test.cpp
//
// Catch2 unit tests for glibre::AllocatorHandle.
// Authority: core/include/glibre/alloc.hpp, plan #989,
//            reviews/decisions/perf-budget.md §Allocator Rules #1.
//
// Named test cases (plan #989 Unit Test Plan):
//   - allocator_handle_forwards_to_per_context_allocator
//   - allocator_handle_stamps_tag_at_construction
//   - allocator_handle_per_plugin_isolated_tag
//
// Design constraints:
//   - -fno-exceptions (error-model.md §Decision 3).
//   - No REQUIRE_THROWS.
//   - GLIBRE_ALLOC_STRICT=1 is set by the CMakeLists so strict-mode ceiling
//     enforcement is active in all test cases.
//   - AllocatorHandle is a non-owning wrapper: every PerContextAllocator
//     instance is declared before the AllocatorHandle that wraps it and
//     outlives it (RAII ordering within each TEST_CASE).

#include <cstddef>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>
#include <glibre/alloc.hpp>

// ===========================================================================
// Test: allocator_handle_forwards_to_per_context_allocator
//
// Verifies that AllocatorHandle::allocate() and ::deallocate() forward to
// the underlying PerContextAllocator, advancing and decrementing the byte
// counter on the allocator, not on the handle.
//
// Rationale (perf-budget.md §Allocator Rules #1):
//   The tag is stamped once at handle creation; the handle is tag-free at
//   the allocate() call site.  The PerContextAllocator tracks the bytes,
//   not the handle — the handle is a thin forwarding wrapper.
//
// DoD: unit_test_named: allocator_handle_forwards_to_per_context_allocator
// ===========================================================================

TEST_CASE("allocator_handle_forwards_to_per_context_allocator", "[core][alloc][handle]") {
    constexpr std::uint64_t kCeiling = 1024ULL * 1024ULL;  // 1 MiB
    glibre::PerContextAllocator alloc{glibre::ContextTag::core, kCeiling};
    glibre::AllocatorHandle handle{alloc, glibre::ContextTag::core};

    REQUIRE(alloc.bytes_used() == 0);

    // Allocate 128 bytes through the handle.
    auto r1 = handle.allocate(128);
    REQUIRE(r1.has_value());

    // The PerContextAllocator's byte counter must reflect the allocation.
    CHECK(alloc.bytes_used() == 128);

    // Allocate an additional 64 bytes.
    auto r2 = handle.allocate(64);
    REQUIRE(r2.has_value());
    CHECK(alloc.bytes_used() == 192);

    // Deallocate via the handle — the counter on the allocator must decrement.
    handle.deallocate(*r1, 128);
    CHECK(alloc.bytes_used() == 64);

    handle.deallocate(*r2, 64);
    CHECK(alloc.bytes_used() == 0);
}

// ===========================================================================
// Test: allocator_handle_stamps_tag_at_construction
//
// Verifies that the ContextTag passed at AllocatorHandle construction is
// preserved and accessible via handle.tag() for the lifetime of the handle.
//
// This property is the core contract of AllocatorHandle:
//   "The tag is supplied once at handle creation (at glibre_plugin_register
//    time via the plugin loader) so plugin call sites are tag-free."
//   — perf-budget.md §Allocator Rules #1, plan #989.
//
// DoD: unit_test_named: allocator_handle_stamps_tag_at_construction
// ===========================================================================

TEST_CASE("allocator_handle_stamps_tag_at_construction", "[core][alloc][handle]") {
    constexpr std::uint64_t kCeiling = 1024ULL * 1024ULL;

    // Verify each ContextTag value is correctly preserved.
    // The test iterates over all nine ContextTag values to exercise the stamping
    // for every bounded context (perf-budget.md §Per-Context Budget Table).
    const glibre::ContextTag tags[] = {
        glibre::ContextTag::core,
        glibre::ContextTag::platform,
        glibre::ContextTag::data,
        glibre::ContextTag::shader,
        glibre::ContextTag::render,
        glibre::ContextTag::geometry,
        glibre::ContextTag::physics,
        glibre::ContextTag::content,
        glibre::ContextTag::tools,
    };
    static_assert(
        sizeof(tags) / sizeof(tags[0]) == glibre::kContextTagCount,
        "tags[] must cover every ContextTag value"
    );

    for (const auto tag : tags) {
        // Construct a PerContextAllocator with a generous ceiling so it can
        // serve all nine iterations without hitting OutOfBudget.
        glibre::PerContextAllocator alloc{tag, kCeiling};
        glibre::AllocatorHandle handle{alloc, tag};

        // The stamped tag must match what was passed at construction.
        CHECK(handle.tag() == tag);

        // underlying() must refer to the same PerContextAllocator object.
        CHECK(&handle.underlying() == &alloc);
    }
}

// ===========================================================================
// Test: allocator_handle_per_plugin_isolated_tag
//
// Verifies that two AllocatorHandle instances with different ContextTags route
// allocations to their respective PerContextAllocator instances independently.
// Allocating through handle_A does not affect handle_B's underlying allocator
// and vice versa.
//
// This exercises the isolation guarantee:
//   "The tag is supplied by the caller via the allocator handle obtained at
//    glibre_plugin_register time; the registration code stamps the tag into
//    the handle so plugin call sites are tag-free."
//   — perf-budget.md §Allocator Rules #1.
//
// Two plugins with different tags get separate PerContextAllocator instances;
// their byte budgets are isolated.
//
// DoD: unit_test_named: allocator_handle_per_plugin_isolated_tag
// ===========================================================================

TEST_CASE("allocator_handle_per_plugin_isolated_tag", "[core][alloc][handle]") {
    constexpr std::uint64_t kCeiling = 1024ULL * 1024ULL;

    // Simulate two plugins with distinct bounded-context tags.
    glibre::PerContextAllocator alloc_render{glibre::ContextTag::render, kCeiling};
    glibre::PerContextAllocator alloc_physics{glibre::ContextTag::physics, kCeiling};

    glibre::AllocatorHandle handle_render{alloc_render, glibre::ContextTag::render};
    glibre::AllocatorHandle handle_physics{alloc_physics, glibre::ContextTag::physics};

    // Pre-condition: both allocators start at zero.
    REQUIRE(alloc_render.bytes_used() == 0);
    REQUIRE(alloc_physics.bytes_used() == 0);

    // Allocate 200 bytes through the render handle.
    auto r_render = handle_render.allocate(200);
    REQUIRE(r_render.has_value());

    // Only the render allocator's counter advances.
    CHECK(alloc_render.bytes_used() == 200);
    CHECK(alloc_physics.bytes_used() == 0);  // physics is unaffected

    // Allocate 100 bytes through the physics handle.
    auto r_physics = handle_physics.allocate(100);
    REQUIRE(r_physics.has_value());

    // Each allocator tracks its own bytes independently.
    CHECK(alloc_render.bytes_used() == 200);
    CHECK(alloc_physics.bytes_used() == 100);

    // Verify that the stamped tags match the allocators they wrap.
    CHECK(handle_render.tag() == glibre::ContextTag::render);
    CHECK(handle_physics.tag() == glibre::ContextTag::physics);

    // Cleanup.
    handle_render.deallocate(*r_render, 200);
    handle_physics.deallocate(*r_physics, 100);

    CHECK(alloc_render.bytes_used() == 0);
    CHECK(alloc_physics.bytes_used() == 0);
}
