// tests/core/phase_registry/phase_registry_test.cpp
//
// Catch2 unit tests for glibre::core::PhaseRegistry.
//
// Authority: plan #245 (phase ownership + system register API).
//            reviews/decisions/frame-phases.md §Decision.
//
// Named test cases (plan #245 Unit Test Plan / DoD):
//   - core/phase_registry: register_system_idempotent
//   - core/phase_registry: iteration_order_matches_registration_order
//   - core/phase_registry: per_phase_isolation
//
// Design constraints:
//   - -fno-exceptions (error-model.md §Decision 3).
//   - No REQUIRE_THROWS usage.
//   - EASTL is the container substrate (PHILOSOPHY §11).

#include <array>
#include <cstddef>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "glibre/core/frame_loop.hpp"
#include "glibre/core/frame_phase.hpp"
#include "glibre/core/phase_registry.hpp"

// ===========================================================================
// Test: register_system_idempotent
//
// Verifies that:
//   (a) Registering the same (phase, fqn) pair twice results in exactly one
//       entry in the phase's system list (idempotency guarantee per
//       hot-reload-protocol.md step 4.1).
//   (b) A different fqn in the same phase creates a distinct entry.
//   (c) The idempotency check is by fqn string equality, not pointer identity.
//   (d) The idempotent duplicate registration does not change the system count.
//   (e) FrameLoop::tick() succeeds with a registry attached (integration smoke).
// ===========================================================================
TEST_CASE("core/phase_registry: register_system_idempotent", "[core][phase_registry]") {
    using namespace glibre::core;

    PhaseRegistry reg;

    // Initially: no systems in any phase.
    REQUIRE(reg.system_count(Phase::Transform) == 0U);
    REQUIRE(reg.total_system_count() == 0U);

    int call_count = 0;

    // Register a system for Phase::Transform.
    reg.register_system(Phase::Transform, "core.transform.propagate", [&call_count]() {
        ++call_count;
    });
    CHECK(reg.system_count(Phase::Transform) == 1U);
    CHECK(reg.total_system_count() == 1U);

    // (a) Register the same fqn again — must be a no-op.
    reg.register_system(Phase::Transform, "core.transform.propagate", [&call_count]() {
        ++call_count;
    });
    CHECK(reg.system_count(Phase::Transform) == 1U);  // still 1, not 2
    CHECK(reg.total_system_count() == 1U);

    // (b) Register a different fqn in the same phase — must create a new entry.
    reg.register_system(Phase::Transform, "core.transform.shadow_update", [&call_count]() {
        ++call_count;
    });
    CHECK(reg.system_count(Phase::Transform) == 2U);
    CHECK(reg.total_system_count() == 2U);

    // (c) Idempotency is by string value, not pointer identity.
    // Construct fqn from a separate string literal pointer to rule out any
    // pointer-equality short-circuits (the literal address differs from the
    // one used in the first registration above, but the content is equal).
    const eastl::string_view fqn_again{"core.transform.propagate"};
    reg.register_system(Phase::Transform, fqn_again, []() {});
    CHECK(reg.system_count(Phase::Transform) == 2U);  // unchanged

    // (d) After idempotent re-registrations, invoke once: only the two distinct
    //     systems should fire.
    call_count = 0;
    reg.for_each_system(Phase::Transform, [](SystemFn& fn) { fn(); });
    CHECK(call_count == 2);  // only 2 distinct systems

    // (e) Smoke: attach registry to FrameLoop and tick — must succeed.
    FrameLoop loop;
    loop.set_phase_registry(&reg);
    auto r = loop.tick();
    REQUIRE(r.has_value());
    // After one tick the systems ran once more in Phase::Transform.
    CHECK(call_count == 4);  // 2 (for_each call above) + 2 (tick)
}

// ===========================================================================
// Test: iteration_order_matches_registration_order
//
// Verifies that for_each_system() visits systems in strict registration order
// (first registered = first visited), which is the ordering guarantee stated
// in plan #245 §Scope ("Iteration order is registration order").
//
// Method:
//   Register N systems into a phase, each appending its ordinal index to a
//   std::array.  After for_each_system(), the collected sequence must equal
//   [0, 1, 2, ..., N-1].  This is deterministic regardless of FQN ordering.
// ===========================================================================
TEST_CASE(
    "core/phase_registry: iteration_order_matches_registration_order", "[core][phase_registry]"
) {
    using namespace glibre::core;

    PhaseRegistry reg;

    // Register kCount systems in order.  Each appends its index to `sequence`.
    constexpr int kCount = 5;
    std::array<int, kCount> sequence{};
    int seq_len = 0;

    // Register five distinct systems in order.
    for (int i = 0; i < kCount; ++i) {
        // Build a unique FQN.  eastl::string_view over a literal is safe for
        // the duration of the loop body; the PhaseRegistry copies it into an
        // eastl::string on registration.
        std::array<char, 32> fqn{};
        // Manual integer-to-string to avoid std::sprintf in a -fno-exceptions TU.
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        fqn[0] = 'p';
        fqn[1] = '.';
        fqn[2] = 's';
        fqn[3] = static_cast<char>('0' + i);
        fqn[4] = '\0';
        // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

        // Capture the current value of i by value so each closure is distinct.
        const int idx = i;
        reg.register_system(
            Phase::PhysicsFixed, eastl::string_view{fqn.data(), 4U}, [idx, &sequence, &seq_len]() {
                // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                sequence[static_cast<std::size_t>(seq_len++)] = idx;
                // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
        );
    }

    REQUIRE(reg.system_count(Phase::PhysicsFixed) == static_cast<std::size_t>(kCount));

    // Invoke via for_each_system.
    reg.for_each_system(Phase::PhysicsFixed, [](SystemFn& fn) { fn(); });

    // Verify the collected sequence equals [0, 1, 2, 3, 4] in that order.
    REQUIRE(seq_len == kCount);
    for (int i = 0; i < kCount; ++i) {
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        INFO(
            "position " << i << ": expected " << i << " got "
                        << sequence[static_cast<std::size_t>(i)]
        );
        CHECK(sequence[static_cast<std::size_t>(i)] == i);
        // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }

    // Second smoke pass: integrate with FrameLoop to confirm run_phase dispatch
    // also preserves registration order.
    seq_len = 0;

    FrameLoop loop;
    loop.set_phase_registry(&reg);
    auto r = loop.tick();
    REQUIRE(r.has_value());

    REQUIRE(seq_len == kCount);
    for (int i = 0; i < kCount; ++i) {
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        CHECK(sequence[static_cast<std::size_t>(i)] == i);
        // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
}

// ===========================================================================
// Test: per_phase_isolation
//
// Verifies that:
//   (a) Systems registered in phase A do not appear when iterating phase B.
//   (b) Systems registered in phase B do not appear when iterating phase A.
//   (c) After drain(), all phases have zero systems.
//   (d) New systems can be registered in any phase after drain().
//
// Uses three distinct phases to cover cross-phase isolation: Input, Transform,
// and Present.
// ===========================================================================
TEST_CASE("core/phase_registry: per_phase_isolation", "[core][phase_registry]") {
    using namespace glibre::core;

    PhaseRegistry reg;

    bool input_ran = false;
    bool transform_ran = false;
    bool present_ran = false;

    // (a/b) Register one system in each of three distinct phases.
    reg.register_system(Phase::Input, "platform.input.poll", [&input_ran]() { input_ran = true; });
    reg.register_system(Phase::Transform, "core.transform.propagate", [&transform_ran]() {
        transform_ran = true;
    });
    reg.register_system(Phase::Present, "platform.present.flip", [&present_ran]() {
        present_ran = true;
    });

    REQUIRE(reg.system_count(Phase::Input) == 1U);
    REQUIRE(reg.system_count(Phase::Transform) == 1U);
    REQUIRE(reg.system_count(Phase::Present) == 1U);
    REQUIRE(reg.total_system_count() == 3U);

    // Iterating Phase::Input only triggers the Input system.
    reg.for_each_system(Phase::Input, [](SystemFn& fn) { fn(); });
    CHECK(input_ran == true);
    CHECK(transform_ran == false);
    CHECK(present_ran == false);

    // Iterating Phase::Transform only triggers the Transform system.
    reg.for_each_system(Phase::Transform, [](SystemFn& fn) { fn(); });
    CHECK(transform_ran == true);
    CHECK(present_ran == false);

    // Iterating Phase::Present only triggers the Present system.
    reg.for_each_system(Phase::Present, [](SystemFn& fn) { fn(); });
    CHECK(present_ran == true);

    // No cross-contamination: other phases still have their expected counts.
    CHECK(reg.system_count(Phase::Logic) == 0U);
    CHECK(reg.system_count(Phase::PhysicsFixed) == 0U);
    CHECK(reg.system_count(Phase::Animation) == 0U);
    CHECK(reg.system_count(Phase::CullExtract) == 0U);
    CHECK(reg.system_count(Phase::RenderSubmit) == 0U);
    CHECK(reg.system_count(Phase::HotReload) == 0U);

    // (c) After drain(), all phases have zero systems.
    reg.drain();
    CHECK(reg.total_system_count() == 0U);
    for (const auto& desc : glibre::core::kPhaseTable) {
        INFO("Phase " << static_cast<int>(desc.id) << " should be empty after drain");
        CHECK(reg.system_count(desc.id) == 0U);
    }

    // (d) New registrations work cleanly after drain().
    bool after_drain_ran = false;
    reg.register_system(Phase::Transform, "core.transform.propagate", [&after_drain_ran]() {
        after_drain_ran = true;
    });
    CHECK(reg.system_count(Phase::Transform) == 1U);

    reg.for_each_system(Phase::Transform, [](SystemFn& fn) { fn(); });
    CHECK(after_drain_ran == true);
}
