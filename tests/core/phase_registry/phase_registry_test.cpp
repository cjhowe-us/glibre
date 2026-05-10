// tests/core/phase_registry/phase_registry_test.cpp
//
// Catch2 unit tests for glibre::core::PhaseRegistry.
//
// Authority: plan #245 (phase ownership + system register API).
//            plan #1045 (migrate core/phase-registry EASTL -> libc++ stdlib).
//            reviews/decisions/frame-phases.md §Decision.
//            reviews/decisions/eastl-removal.md §1 (matrix rows 1-3, 9).
//
// Named test cases (plan #245 Unit Test Plan / DoD):
//   - core/phase_registry: register_system_idempotent
//   - core/phase_registry: iteration_order_matches_registration_order
//   - core/phase_registry: iteration_order_matches_registration_order frameloop smoke
//   - core/phase_registry: per_phase_isolation
//
// Named test cases (plan #1045 Unit Test Plan / DoD):
//   - core/phase_registry: system_fn_uses_std_move_only_function
//
// Design constraints:
//   - -fno-exceptions (error-model.md §Decision 3).
//   - No REQUIRE_THROWS usage.
//   - libc++ is the container substrate per reviews/decisions/eastl-removal.md.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <string_view>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "glibre/alloc.hpp"
#include "glibre/compat/move_only_function.hpp"
#include "glibre/core/frame_loop.hpp"
#include "glibre/core/frame_phase.hpp"
#include "glibre/core/phase_registry.hpp"

// ===========================================================================
// Test: system_fn_uses_std_move_only_function
//
// Verifies (plan #1045 §Unit Test Plan):
//   (a) PhaseSystemFn is exactly std::move_only_function<void() const>
//       (compile-time type-identity pin).
//   (b) PhaseSystemFn is invocable via a const PhaseSystemFn reference,
//       which is the exact invocation path used inside for_each_system().
//       Under the polyfill (std::function backing), only copy-constructible
//       callables can be stored; the const-invocability assertion is the
//       strongest portable check available without the real C++23 type.
//   (c) When the real std::move_only_function is present, a move-only callable
//       (unique_ptr-capturing lambda) is used to exercise actual move-only
//       semantics.  This guard is conditioned on __cpp_lib_move_only_function
//       because std::function (the polyfill backing) requires the callable to
//       be CopyConstructible and cannot hold a non-copyable lambda.
// ===========================================================================
TEST_CASE("core/phase_registry: system_fn_uses_std_move_only_function", "[core][phase_registry]") {
    using namespace glibre::core;

    // (a) Compile-time type-identity pin.
    // PhaseSystemFn MUST be std::move_only_function<void() const> per
    // reviews/decisions/eastl-removal.md matrix row 9.
    static_assert(
        std::is_same_v<PhaseSystemFn, std::move_only_function<void() const>>,
        "PhaseSystemFn must be std::move_only_function<void() const> per "
        "reviews/decisions/eastl-removal.md matrix row 9"
    );

#ifdef __cpp_lib_move_only_function
    // (c) Real C++23 std::move_only_function — exercise move-only callable.
    // unique_ptr-capturing lambdas are NOT CopyConstructible.  Constructing
    // PhaseSystemFn from one proves the callable type genuinely accepts
    // move-only closures (the polyfill cannot make this guarantee).
    {
        int called = 0;
        auto sentinel = std::make_unique<int>(42);

        PhaseSystemFn fn = [&called, s = std::move(sentinel)]() noexcept {
            if (s && *s == 42) {
                ++called;
            }
        };

        // (b) Const-ref invocation path (mirrors for_each_system).
        const PhaseSystemFn& const_ref = fn;
        const_ref();
        REQUIRE(called == 1);

        // Verify PhaseSystemFn is NOT copy-constructible under the real type.
        static_assert(
            !std::is_copy_constructible_v<PhaseSystemFn>,
            "PhaseSystemFn must be move-only when real std::move_only_function is in use"
        );
    }
#else
    // (b) Polyfill path: std::function backing requires CopyConstructible callables.
    // Use a plain lambda to exercise the const-ref invocation path — the
    // strongest assertion portable across the polyfill.
    {
        int called = 0;

        PhaseSystemFn fn = [&called]() noexcept { ++called; };

        // const-ref invocation — mirrors for_each_system()'s call site.
        const PhaseSystemFn& const_ref = fn;
        const_ref();
        REQUIRE(called == 1);
    }

    // Round-trip: register_system → for_each_system, mirroring the real-type
    // branch above.  Couples the polyfill-path type-identity assertion to the
    // actual seam (register_system accepts PhaseSystemFn; for_each_system invokes
    // it via const ref) so that both compile paths exercise the full
    // register → invoke contract and not just isolated type properties.
    {
        auto* mr = std::pmr::get_default_resource();
        PhaseRegistry reg{mr};

        int called = 0;
        reg.register_system(Phase::Transform, "test.polyfill.round_trip", [&called]() noexcept {
            ++called;
        });
        REQUIRE(reg.system_count(Phase::Transform) == 1U);

        reg.for_each_system(Phase::Transform, [](const PhaseSystemFn& fn) noexcept { fn(); });
        REQUIRE(called == 1);
    }
#endif  // __cpp_lib_move_only_function
}

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
// ===========================================================================
TEST_CASE("core/phase_registry: register_system_idempotent", "[core][phase_registry]") {
    using namespace glibre::core;

    // Use the default PMR heap resource for test fixtures.
    // Engine code passes a PerContextAllocatorResource{ContextTag::core} here
    // for ceiling enforcement; tests use the default heap resource to keep
    // test fixtures simple and free of allocator-lifetime ordering concerns.
    auto* mr = std::pmr::get_default_resource();
    PhaseRegistry reg{mr};

    // Initially: no systems in any phase.
    REQUIRE(reg.system_count(Phase::Transform) == 0U);
    REQUIRE(reg.total_system_count() == 0U);

    int call_count = 0;

    // Register a system for Phase::Transform.
    reg.register_system(Phase::Transform, "core.transform.propagate", [&call_count]() noexcept {
        ++call_count;
    });
    CHECK(reg.system_count(Phase::Transform) == 1U);
    CHECK(reg.total_system_count() == 1U);

    // (a) Register the same fqn again — must be a no-op.
    reg.register_system(Phase::Transform, "core.transform.propagate", [&call_count]() noexcept {
        ++call_count;
    });
    CHECK(reg.system_count(Phase::Transform) == 1U);  // still 1, not 2
    CHECK(reg.total_system_count() == 1U);

    // (b) Register a different fqn in the same phase — must create a new entry.
    reg.register_system(Phase::Transform, "core.transform.shadow_update", [&call_count]() noexcept {
        ++call_count;
    });
    CHECK(reg.system_count(Phase::Transform) == 2U);
    CHECK(reg.total_system_count() == 2U);

    // (c) Idempotency is by string value, not pointer identity.
    // Construct fqn from a separate string_view to rule out any pointer-equality
    // short-circuits (the literal address may differ but the content is equal).
    const std::string_view fqn_again{"core.transform.propagate"};
    reg.register_system(Phase::Transform, fqn_again, []() noexcept {});
    CHECK(reg.system_count(Phase::Transform) == 2U);  // unchanged

    // (d) After idempotent re-registrations, invoke once: only the two distinct
    //     systems should fire.
    call_count = 0;
    reg.for_each_system(Phase::Transform, [](const PhaseSystemFn& fn) noexcept { fn(); });
    CHECK(call_count == 2);  // only 2 distinct systems
}

// ===========================================================================
// Test: register_system_idempotent — FrameLoop integration smoke
//
// Separated from the idempotency assertions above per SRP: verifying that
// FrameLoop::tick() dispatches registered systems is a distinct concern
// from verifying the idempotency invariant itself.
//
// Verifies that:
//   (e) FrameLoop::tick() succeeds with a PhaseRegistry attached and
//       invokes registered systems the expected number of times.
// ===========================================================================
TEST_CASE(
    "core/phase_registry: register_system_idempotent frameloop smoke", "[core][phase_registry]"
) {
    using namespace glibre::core;

    auto* mr = std::pmr::get_default_resource();
    PhaseRegistry reg{mr};
    int call_count = 0;

    // Register two distinct systems so there is work for the FrameLoop to do.
    reg.register_system(Phase::Transform, "core.transform.propagate", [&call_count]() noexcept {
        ++call_count;
    });
    reg.register_system(Phase::Transform, "core.transform.shadow_update", [&call_count]() noexcept {
        ++call_count;
    });
    REQUIRE(reg.system_count(Phase::Transform) == 2U);

    // Direct invocation baseline: 2 systems fire once each.
    reg.for_each_system(Phase::Transform, [](const PhaseSystemFn& fn) noexcept { fn(); });
    REQUIRE(call_count == 2);

    // Reset before tick so the delta assertion below is independent of the
    // baseline invocation above.  A future refactor that adds or removes a
    // direct for_each_system call cannot silently corrupt the tick assertion.
    call_count = 0;

    // (e) Smoke: attach registry to FrameLoop and tick — must succeed.
    FrameLoop loop;
    loop.set_phase_registry(&reg);
    auto r = loop.tick();
    REQUIRE(r.has_value());
    // One tick dispatches each Transform system exactly once (delta only).
    CHECK(call_count == 2);
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

    auto* mr = std::pmr::get_default_resource();
    PhaseRegistry reg{mr};

    // Register kCount systems in order.  Each appends its index to `sequence`.
    constexpr int kCount = 5;
    // Guard: the FQN construction below uses '0' + i which is only valid when
    // i < 10.  Enforce this at compile time so the guard never bitrotingly
    // silences UB as kCount grows.
    static_assert(kCount < 10, "kCount must be < 10; FQN uses '0'+i single-digit encoding");
    std::array<int, kCount> sequence{};
    int seq_len = 0;

    // Register kCount distinct systems in order.
    for (int i = 0; i < kCount; ++i) {
        // Build a unique FQN.  std::string_view over the char array is safe for
        // the duration of the loop body; the PhaseRegistry copies it into a
        // std::pmr::string on registration.
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
            Phase::PhysicsFixed,
            std::string_view{fqn.data(), 4U},
            [idx, &sequence, &seq_len]() noexcept {
                // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                sequence[static_cast<std::size_t>(seq_len++)] = idx;
                // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
        );
    }

    REQUIRE(reg.system_count(Phase::PhysicsFixed) == static_cast<std::size_t>(kCount));

    // Invoke via for_each_system.
    reg.for_each_system(Phase::PhysicsFixed, [](const PhaseSystemFn& fn) noexcept { fn(); });

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
}

// ===========================================================================
// Test: iteration_order_matches_registration_order — FrameLoop smoke
//
// Separated from the for_each_system ordering assertions above per SRP:
// verifying that FrameLoop::tick() dispatches in registration order is a
// distinct concern from verifying that for_each_system() itself preserves
// registration order.  Coupling both into one named DoD test would make the
// test fail for two independent reasons.
//
// Verifies that:
//   (f) FrameLoop::tick() dispatches Phase::PhysicsFixed systems in
//       registration order (same [0..N-1] sequence as for_each_system).
// ===========================================================================
TEST_CASE(
    "core/phase_registry: iteration_order_matches_registration_order frameloop smoke",
    "[core][phase_registry]"
) {
    using namespace glibre::core;

    auto* mr = std::pmr::get_default_resource();
    PhaseRegistry reg{mr};

    constexpr int kCount = 5;
    static_assert(kCount < 10, "kCount must be < 10; FQN uses '0'+i single-digit encoding");
    std::array<int, kCount> sequence{};
    int seq_len = 0;

    for (int i = 0; i < kCount; ++i) {
        std::array<char, 32> fqn{};
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        fqn[0] = 'p';
        fqn[1] = '.';
        fqn[2] = 's';
        fqn[3] = static_cast<char>('0' + i);
        fqn[4] = '\0';
        // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

        const int idx = i;
        reg.register_system(
            Phase::PhysicsFixed,
            std::string_view{fqn.data(), 4U},
            [idx, &sequence, &seq_len]() noexcept {
                // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                sequence[static_cast<std::size_t>(seq_len++)] = idx;
                // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
        );
    }

    REQUIRE(reg.system_count(Phase::PhysicsFixed) == static_cast<std::size_t>(kCount));

    // (f) Smoke: FrameLoop::tick() must dispatch in registration order.
    FrameLoop loop;
    loop.set_phase_registry(&reg);
    auto r = loop.tick();
    REQUIRE(r.has_value());

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
}

// ===========================================================================
// Test: per_phase_isolation
//
// Verifies that:
//   (a) Systems registered in phase A do not appear when iterating phase B.
//   (b) Systems registered in phase B do not appear when iterating phase A.
//   (c) After drain_all(), all phases have zero systems.
//   (d) New systems can be registered in any phase after drain_all().
//
// Uses three distinct phases to cover cross-phase isolation: Input, Transform,
// and Present.
// ===========================================================================
TEST_CASE("core/phase_registry: per_phase_isolation", "[core][phase_registry]") {
    using namespace glibre::core;

    auto* mr = std::pmr::get_default_resource();
    PhaseRegistry reg{mr};

    bool input_ran = false;
    bool transform_ran = false;
    bool present_ran = false;

    // (a/b) Register one system in each of three distinct phases.
    reg.register_system(Phase::Input, "platform.input.poll", [&input_ran]() noexcept {
        input_ran = true;
    });
    reg.register_system(Phase::Transform, "core.transform.propagate", [&transform_ran]() noexcept {
        transform_ran = true;
    });
    reg.register_system(Phase::Present, "platform.present.flip", [&present_ran]() noexcept {
        present_ran = true;
    });

    REQUIRE(reg.system_count(Phase::Input) == 1U);
    REQUIRE(reg.system_count(Phase::Transform) == 1U);
    REQUIRE(reg.system_count(Phase::Present) == 1U);
    REQUIRE(reg.total_system_count() == 3U);

    // Iterating Phase::Input only triggers the Input system.
    reg.for_each_system(Phase::Input, [](const PhaseSystemFn& fn) noexcept { fn(); });
    CHECK(input_ran == true);
    CHECK(transform_ran == false);
    CHECK(present_ran == false);

    // Iterating Phase::Transform only triggers the Transform system.
    reg.for_each_system(Phase::Transform, [](const PhaseSystemFn& fn) noexcept { fn(); });
    CHECK(transform_ran == true);
    CHECK(present_ran == false);

    // Iterating Phase::Present only triggers the Present system.
    reg.for_each_system(Phase::Present, [](const PhaseSystemFn& fn) noexcept { fn(); });
    CHECK(present_ran == true);

    // No cross-contamination: other phases still have their expected counts.
    CHECK(reg.system_count(Phase::Logic) == 0U);
    CHECK(reg.system_count(Phase::PhysicsFixed) == 0U);
    CHECK(reg.system_count(Phase::Animation) == 0U);
    CHECK(reg.system_count(Phase::CullExtract) == 0U);
    CHECK(reg.system_count(Phase::RenderSubmit) == 0U);
    CHECK(reg.system_count(Phase::HotReload) == 0U);

    // (c) After drain_all(), all phases have zero systems.
    //     drain_all() is the coarse MVP-shutdown / test-fixture clear;
    //     the per-plugin hot-reload drain is a separate future primitive
    //     (hot-reload-protocol.md §Step 1, plan #251).
    reg.drain_all();
    CHECK(reg.total_system_count() == 0U);
    for (const auto& desc : glibre::core::kPhaseTable) {
        INFO("Phase " << static_cast<int>(desc.id) << " should be empty after drain_all");
        CHECK(reg.system_count(desc.id) == 0U);
    }

    // (d) New registrations work cleanly after drain_all().
    bool after_drain_ran = false;
    reg.register_system(
        Phase::Transform, "core.transform.propagate", [&after_drain_ran]() noexcept {
            after_drain_ran = true;
        }
    );
    CHECK(reg.system_count(Phase::Transform) == 1U);

    reg.for_each_system(Phase::Transform, [](const PhaseSystemFn& fn) noexcept { fn(); });
    CHECK(after_drain_ran == true);
}
