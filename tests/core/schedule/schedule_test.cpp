// tests/core/schedule/schedule_test.cpp
//
// Catch2 unit tests for glibre::core::Schedule + DAG builder.
//
// Authority: plan #584 (Schedule DAG builder from access sets).
//            specs/core/SPEC.md §4.4, §5.6, §6.4.
//
// Named test cases (plan #584 Unit Test Plan / DoD):
//   - core/schedule: register_unregister_idempotent_for_same_id
//   - core/schedule: dag_edge_writes_reads_intersection
//   - core/schedule: dag_edge_writes_writes_intersection
//   - core/schedule: dag_edge_after_before_explicit
//   - core/schedule: cycle_yields_SystemScheduleCycle
//   - core/schedule: access_conflict_no_valid_order_yields_ScheduleAccessConflict
//   - core/schedule: tiebreaker_lex_order_of_fqn
//   - core/schedule: compile_idempotent_when_set_unchanged
//   - core/schedule: register_system_rejects_HotReload_phase        (R1 MED-3)
//   - core/schedule: register_system_rejects_phase_drift_on_same_fqn (R2 MED-1)
//
// Design constraints:
//   - -fno-exceptions (error-model.md §Decision 3); no REQUIRE_THROWS.
//   - No EASTL; libc++ containers throughout.

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "glibre/alloc.hpp"
#include "glibre/core/frame_phase.hpp"
#include "glibre/core/schedule.hpp"
#include "glibre/error.hpp"

// ---------------------------------------------------------------------------
// Test fixture helpers
// ---------------------------------------------------------------------------

namespace {

// Null system body — no-op for scheduling tests (body is never invoked here).
void null_system(glibre::core::SystemContext& /*ctx*/) noexcept {}

// Helper: build an AccessSet from stack-allocated arrays.
// Returned AccessSet holds non-owning spans; arrays must outlive it.
glibre::core::AccessSet make_access(
    std::vector<glibre::core::TypeId>& reads, std::vector<glibre::core::TypeId>& writes
) noexcept {
    return glibre::core::AccessSet{
        .reads = std::span<const glibre::core::TypeId>{reads},
        .writes = std::span<const glibre::core::TypeId>{writes},
        .without = {},
    };
}

// Helper: ordered system IDs extracted from CompiledPhase.
std::vector<std::uint64_t> compiled_ids(const glibre::core::CompiledPhase& cp) {
    std::vector<std::uint64_t> result;
    result.reserve(cp.size());
    for (const auto& sid : cp)
        result.push_back(sid.value);
    return result;
}

}  // namespace

// ===========================================================================
// Test: register_unregister_idempotent_for_same_id
//
// Verifies (plan #584 §Unit Test Plan):
//   (a) register_system() with the same name twice returns the same SystemId.
//   (b) unregister_system() with an unknown id is a no-op (no error).
//   (c) unregister_system() called twice on the same id is idempotent.
// ===========================================================================

TEST_CASE("core/schedule: register_unregister_idempotent_for_same_id") {
    glibre::PerContextAllocator alloc{glibre::ContextTag::core};
    glibre::core::Schedule sched{alloc};

    glibre::core::SystemDesc desc;
    desc.name = "test::SysA";
    desc.phase = glibre::core::Phase::Transform;
    desc.body = &null_system;

    // (a) First registration succeeds.
    auto r1 = sched.register_system(desc);
    REQUIRE(r1.has_value());
    const glibre::core::SystemId id1 = *r1;
    REQUIRE(id1.value != 0u);  // 0 is the null sentinel.

    // (a) Second registration with identical name returns the same id.
    auto r2 = sched.register_system(desc);
    REQUIRE(r2.has_value());
    REQUIRE((*r2).value == id1.value);

    // (b) Unregister unknown id — no error.
    glibre::core::SystemId bogus{999'999u};
    auto u0 = sched.unregister_system(bogus);
    REQUIRE(u0.has_value());

    // (c) Double-unregister same id — idempotent.
    auto u1 = sched.unregister_system(id1);
    REQUIRE(u1.has_value());

    auto u2 = sched.unregister_system(id1);
    REQUIRE(u2.has_value());
}

// ===========================================================================
// Test: dag_edge_writes_reads_intersection
//
// Verifies (plan #584 §Unit Test Plan / SPEC §6.4 algorithm step 2):
//   When system A writes TypeId X and system B reads TypeId X, compile()
//   produces an order where A appears before B in the CompiledPhase.
//
//   Names chosen so that lexicographic tiebreaker does NOT determine order
//   (B < A alphabetically), ensuring the edge rule wins.
// ===========================================================================

TEST_CASE("core/schedule: dag_edge_writes_reads_intersection") {
    glibre::PerContextAllocator alloc{glibre::ContextTag::core};
    glibre::core::Schedule sched{alloc};

    const glibre::core::TypeId kX{42u};

    // SysB (alphabetically earlier) reads X.
    std::vector<glibre::core::TypeId> b_reads = {kX};
    std::vector<glibre::core::TypeId> b_writes;

    // SysA (alphabetically later) writes X → must run before SysB.
    std::vector<glibre::core::TypeId> a_reads;
    std::vector<glibre::core::TypeId> a_writes = {kX};

    glibre::core::SystemDesc desc_a;
    desc_a.name = "test::SysA_writer";
    desc_a.phase = glibre::core::Phase::Transform;
    desc_a.access = make_access(a_reads, a_writes);
    desc_a.body = &null_system;

    glibre::core::SystemDesc desc_b;
    desc_b.name = "test::SysB_reader";
    desc_b.phase = glibre::core::Phase::Transform;
    desc_b.access = make_access(b_reads, b_writes);
    desc_b.body = &null_system;

    auto ra = sched.register_system(desc_a);
    REQUIRE(ra.has_value());
    auto rb = sched.register_system(desc_b);
    REQUIRE(rb.has_value());

    auto c = sched.compile();
    REQUIRE(c.has_value());

    const auto* cp = sched.compiled_phase(glibre::core::Phase::Transform);
    REQUIRE(cp != nullptr);
    REQUIRE(cp->size() == 2u);

    // A (writer) must appear before B (reader).
    const auto ids = compiled_ids(*cp);
    REQUIRE(ids[0] == (*ra).value);
    REQUIRE(ids[1] == (*rb).value);
}

// ===========================================================================
// Test: dag_edge_writes_writes_intersection
//
// Verifies (plan #584 §Unit Test Plan / SPEC §6.4 algorithm step 2):
//   When system A writes TypeId Y and system B writes TypeId Y, compile()
//   imposes an ordering between them.  The tiebreaker (lex) applies when the
//   only edge is writes∩writes on a shared component.  Lex order:
//   "test::SysAlpha" < "test::SysBeta" → Alpha before Beta.
// ===========================================================================

TEST_CASE("core/schedule: dag_edge_writes_writes_intersection") {
    glibre::PerContextAllocator alloc{glibre::ContextTag::core};
    glibre::core::Schedule sched{alloc};

    const glibre::core::TypeId kY{77u};

    std::vector<glibre::core::TypeId> alpha_writes = {kY};
    std::vector<glibre::core::TypeId> alpha_reads;

    std::vector<glibre::core::TypeId> beta_writes = {kY};
    std::vector<glibre::core::TypeId> beta_reads;

    glibre::core::SystemDesc desc_alpha;
    desc_alpha.name = "test::SysAlpha";
    desc_alpha.phase = glibre::core::Phase::Transform;
    desc_alpha.access = make_access(alpha_reads, alpha_writes);
    desc_alpha.body = &null_system;

    glibre::core::SystemDesc desc_beta;
    desc_beta.name = "test::SysBeta";
    desc_beta.phase = glibre::core::Phase::Transform;
    desc_beta.access = make_access(beta_reads, beta_writes);
    desc_beta.body = &null_system;

    auto ra = sched.register_system(desc_alpha);
    REQUIRE(ra.has_value());
    auto rb = sched.register_system(desc_beta);
    REQUIRE(rb.has_value());

    auto c = sched.compile();
    REQUIRE(c.has_value());

    const auto* cp = sched.compiled_phase(glibre::core::Phase::Transform);
    REQUIRE(cp != nullptr);
    REQUIRE(cp->size() == 2u);

    // writes∩writes: SysAlpha→SysBeta (Alpha writes kY, Beta writes kY → edge Alpha→Beta).
    // Tiebreaker: Alpha < Beta alphabetically confirms Alpha first.
    const auto ids = compiled_ids(*cp);
    REQUIRE(ids[0] == (*ra).value);
    REQUIRE(ids[1] == (*rb).value);
}

// ===========================================================================
// Test: dag_edge_after_before_explicit
//
// Verifies (plan #584 §Unit Test Plan / SPEC §6.4 algorithm step 2):
//   Explicit after/before declarations impose ordering independent of access
//   sets.  SysLate declares after:["test::SysEarly"]; SysEarly has no
//   access-set dependency on SysLate.  Alphabetically Late < Early is false
//   (E < L), but the explicit edge must override lex order.
//
//   Concretely: "test::SysEarly" < "test::SysLate" alphabetically, so lex
//   tiebreaker would put SysEarly first anyway.  To isolate the explicit-edge
//   mechanism, use names where lex order disagrees with desired order:
//   "test::ZzzLast" must run after "test::AaaFirst" per after declaration.
//   Without the after edge, lex gives AaaFirst→ZzzLast (correct); with the
//   edge reversed (ZzzLast.after=[AaaFirst]), it must still be AaaFirst→ZzzLast.
//   This is the expected behavior — add before: to ZzzLast to force it last.
//
//   Simpler isolated scenario: SysZ (lex-last) declares before:["SysA"] so
//   that SysZ must run before SysA even though Z > A.
// ===========================================================================

TEST_CASE("core/schedule: dag_edge_after_before_explicit") {
    glibre::PerContextAllocator alloc{glibre::ContextTag::core};
    glibre::core::Schedule sched{alloc};

    // No shared components — only explicit edges drive order.
    std::vector<glibre::core::TypeId> empty_reads;
    std::vector<glibre::core::TypeId> empty_writes;

    // SysZ declares before: SysA → SysZ must run before SysA.
    std::array<std::string_view, 1> z_before = {"test::schedule_explicit::SysA"};

    glibre::core::SystemDesc desc_z;
    desc_z.name = "test::schedule_explicit::SysZ";
    desc_z.phase = glibre::core::Phase::Transform;
    desc_z.access = make_access(empty_reads, empty_writes);
    desc_z.body = &null_system;
    desc_z.before = std::span<const std::string_view>{z_before};

    glibre::core::SystemDesc desc_a;
    desc_a.name = "test::schedule_explicit::SysA";
    desc_a.phase = glibre::core::Phase::Transform;
    desc_a.access = make_access(empty_reads, empty_writes);
    desc_a.body = &null_system;

    auto rz = sched.register_system(desc_z);
    REQUIRE(rz.has_value());
    auto ra = sched.register_system(desc_a);
    REQUIRE(ra.has_value());

    auto c = sched.compile();
    REQUIRE(c.has_value());

    const auto* cp = sched.compiled_phase(glibre::core::Phase::Transform);
    REQUIRE(cp != nullptr);
    REQUIRE(cp->size() == 2u);

    // SysZ must appear before SysA (explicit edge via before declaration).
    const auto ids = compiled_ids(*cp);
    REQUIRE(ids[0] == (*rz).value);  // SysZ first.
    REQUIRE(ids[1] == (*ra).value);  // SysA second.
}

// ===========================================================================
// Test: cycle_yields_SystemScheduleCycle
//
// Verifies (plan #584 §Unit Test Plan / SPEC §6.4 algorithm step 3):
//   When systems form a cycle via explicit after/before edges, compile()
//   returns core::Error::SystemScheduleCycle.
//
//   Scenario: SysP declares after:["SysQ"], SysQ declares after:["SysP"] →
//   cycle P→Q→P.
// ===========================================================================

TEST_CASE("core/schedule: cycle_yields_SystemScheduleCycle") {
    glibre::PerContextAllocator alloc{glibre::ContextTag::core};
    glibre::core::Schedule sched{alloc};

    std::vector<glibre::core::TypeId> empty_reads;
    std::vector<glibre::core::TypeId> empty_writes;

    std::array<std::string_view, 1> p_after = {"test::cycle_explicit::SysQ"};
    std::array<std::string_view, 1> q_after = {"test::cycle_explicit::SysP"};

    glibre::core::SystemDesc desc_p;
    desc_p.name = "test::cycle_explicit::SysP";
    desc_p.phase = glibre::core::Phase::Transform;
    desc_p.access = make_access(empty_reads, empty_writes);
    desc_p.body = &null_system;
    desc_p.after = std::span<const std::string_view>{p_after};

    glibre::core::SystemDesc desc_q;
    desc_q.name = "test::cycle_explicit::SysQ";
    desc_q.phase = glibre::core::Phase::Transform;
    desc_q.access = make_access(empty_reads, empty_writes);
    desc_q.body = &null_system;
    desc_q.after = std::span<const std::string_view>{q_after};

    REQUIRE(sched.register_system(desc_p).has_value());
    REQUIRE(sched.register_system(desc_q).has_value());

    auto c = sched.compile();
    REQUIRE(!c.has_value());

    // Verify the error code.
    const auto& err = c.error().code();
    const auto* ce = std::get_if<glibre::core::Error>(&err);
    REQUIRE(ce != nullptr);
    REQUIRE(*ce == glibre::core::Error::SystemScheduleCycle);
}

// ===========================================================================
// Test: access_conflict_no_valid_order_yields_ScheduleAccessConflict
//
// Verifies (plan #584 §Unit Test Plan / SPEC §4.4 invariant 2):
//   When access-set intersection edges alone form a cycle (no explicit
//   after/before edges), compile() returns ScheduleAccessConflict.
//
//   Scenario: SysReader writes TypeId 10, SysWriter reads TypeId 10 → edge
//   SysReader→SysWriter (writes∩reads).
//   AND SysWriter writes TypeId 20, SysReader reads TypeId 20 → edge
//   SysWriter→SysReader (writes∩reads).
//
//   = bidirectional cycle from writes∩reads alone → ScheduleAccessConflict.
//   No explicit after/before edges involved.
// ===========================================================================

TEST_CASE("core/schedule: access_conflict_no_valid_order_yields_ScheduleAccessConflict") {
    glibre::PerContextAllocator alloc{glibre::ContextTag::core};
    glibre::core::Schedule sched{alloc};

    const glibre::core::TypeId kCompA{10u};
    const glibre::core::TypeId kCompB{20u};

    // SysP: writes kCompA, reads kCompB.
    // SysQ: writes kCompB, reads kCompA.
    // Edge SysP→SysQ (P.writes∩Q.reads = {kCompA}).
    // Edge SysQ→SysP (Q.writes∩P.reads = {kCompB}).
    // → cycle → ScheduleAccessConflict (pure access-set, no explicit edges).

    std::vector<glibre::core::TypeId> p_reads = {kCompB};
    std::vector<glibre::core::TypeId> p_writes = {kCompA};

    std::vector<glibre::core::TypeId> q_reads = {kCompA};
    std::vector<glibre::core::TypeId> q_writes = {kCompB};

    glibre::core::SystemDesc desc_p;
    desc_p.name = "test::conflict::SysP";
    desc_p.phase = glibre::core::Phase::Transform;
    desc_p.access = make_access(p_reads, p_writes);
    desc_p.body = &null_system;

    glibre::core::SystemDesc desc_q;
    desc_q.name = "test::conflict::SysQ";
    desc_q.phase = glibre::core::Phase::Transform;
    desc_q.access = make_access(q_reads, q_writes);
    desc_q.body = &null_system;

    REQUIRE(sched.register_system(desc_p).has_value());
    REQUIRE(sched.register_system(desc_q).has_value());

    auto c = sched.compile();
    REQUIRE(!c.has_value());

    // Verify the error code is ScheduleAccessConflict (not SystemScheduleCycle).
    const auto& err = c.error().code();
    const auto* ce = std::get_if<glibre::core::Error>(&err);
    REQUIRE(ce != nullptr);
    REQUIRE(*ce == glibre::core::Error::ScheduleAccessConflict);
}

// ===========================================================================
// Test: tiebreaker_lex_order_of_fqn
//
// Verifies (plan #584 §Unit Test Plan / SPEC §4.4 invariant 5):
//   When multiple systems have no access-set or explicit ordering
//   dependencies, they are ordered lexicographically by FQN.
//
//   Scenario: three systems with no shared components, registered in
//   non-alphabetical order.  Expected compiled order: Aardvark, Banana, Crane.
// ===========================================================================

TEST_CASE("core/schedule: tiebreaker_lex_order_of_fqn") {
    glibre::PerContextAllocator alloc{glibre::ContextTag::core};
    glibre::core::Schedule sched{alloc};

    std::vector<glibre::core::TypeId> no_reads;
    std::vector<glibre::core::TypeId> no_writes;

    // Register in non-alphabetical order.
    glibre::core::SystemDesc desc_crane;
    desc_crane.name = "test::lex::Crane";
    desc_crane.phase = glibre::core::Phase::Transform;
    desc_crane.access = make_access(no_reads, no_writes);
    desc_crane.body = &null_system;

    glibre::core::SystemDesc desc_banana;
    desc_banana.name = "test::lex::Banana";
    desc_banana.phase = glibre::core::Phase::Transform;
    desc_banana.access = make_access(no_reads, no_writes);
    desc_banana.body = &null_system;

    glibre::core::SystemDesc desc_aardvark;
    desc_aardvark.name = "test::lex::Aardvark";
    desc_aardvark.phase = glibre::core::Phase::Transform;
    desc_aardvark.access = make_access(no_reads, no_writes);
    desc_aardvark.body = &null_system;

    auto rc = sched.register_system(desc_crane);
    REQUIRE(rc.has_value());
    auto rb = sched.register_system(desc_banana);
    REQUIRE(rb.has_value());
    auto ra = sched.register_system(desc_aardvark);
    REQUIRE(ra.has_value());

    auto c = sched.compile();
    REQUIRE(c.has_value());

    const auto* cp = sched.compiled_phase(glibre::core::Phase::Transform);
    REQUIRE(cp != nullptr);
    REQUIRE(cp->size() == 3u);

    // Lexicographic order: Aardvark < Banana < Crane.
    const auto ids = compiled_ids(*cp);
    REQUIRE(ids[0] == (*ra).value);  // Aardvark
    REQUIRE(ids[1] == (*rb).value);  // Banana
    REQUIRE(ids[2] == (*rc).value);  // Crane
}

// ===========================================================================
// Test: compile_idempotent_when_set_unchanged
//
// Verifies (plan #584 §Unit Test Plan / SPEC §4.4 invariant 3):
//   compile() called twice with no intervening registration changes is
//   idempotent — the second call succeeds and produces the same compiled
//   result.  is_compiled() remains true after the second call.
// ===========================================================================

TEST_CASE("core/schedule: compile_idempotent_when_set_unchanged") {
    glibre::PerContextAllocator alloc{glibre::ContextTag::core};
    glibre::core::Schedule sched{alloc};

    std::vector<glibre::core::TypeId> no_reads;
    std::vector<glibre::core::TypeId> no_writes;

    glibre::core::SystemDesc desc;
    desc.name = "test::idempotent::SysX";
    desc.phase = glibre::core::Phase::Transform;
    desc.access = make_access(no_reads, no_writes);
    desc.body = &null_system;

    auto r = sched.register_system(desc);
    REQUIRE(r.has_value());

    // First compile.
    auto c1 = sched.compile();
    REQUIRE(c1.has_value());
    REQUIRE(sched.is_compiled());

    const auto* cp1 = sched.compiled_phase(glibre::core::Phase::Transform);
    REQUIRE(cp1 != nullptr);
    REQUIRE(cp1->size() == 1u);
    const std::uint64_t first_id = (*cp1)[0].value;

    // Second compile — no registrations changed.
    auto c2 = sched.compile();
    REQUIRE(c2.has_value());
    REQUIRE(sched.is_compiled());

    const auto* cp2 = sched.compiled_phase(glibre::core::Phase::Transform);
    REQUIRE(cp2 != nullptr);
    REQUIRE(cp2->size() == 1u);
    REQUIRE((*cp2)[0].value == first_id);
}

// ===========================================================================
// Test: register_system_rejects_HotReload_phase
//
// Verifies (plan #584 R1 review MED-3 / SPEC §6.5 phase 8):
//   register_system() returns SystemForbiddenInHotReloadPhase when the
//   system descriptor targets Phase::HotReload.  Phase 8 is the FrameLoop-
//   internal plugin-loader seam; plugin-authored systems must not occupy it.
// ===========================================================================

TEST_CASE("core/schedule: register_system_rejects_HotReload_phase") {
    glibre::PerContextAllocator alloc{glibre::ContextTag::core};
    glibre::core::Schedule sched{alloc};

    glibre::core::SystemDesc desc;
    desc.name = "test::hot_reload_guard::SysA";
    desc.phase = glibre::core::Phase::HotReload;
    desc.body = &null_system;

    auto r = sched.register_system(desc);
    REQUIRE_FALSE(r.has_value());

    // Error must be SystemForbiddenInHotReloadPhase, not OutOfBudget or similar.
    const auto* code = std::get_if<glibre::core::Error>(&r.error().code());
    REQUIRE(code != nullptr);
    CHECK(*code == glibre::core::Error::SystemForbiddenInHotReloadPhase);
}

// ===========================================================================
// Test: register_system_rejects_phase_drift_on_same_fqn
//
// Verifies (plan #584 R2 review MED-1 / SPEC §8.6):
//   SPEC §8.6 specifies that idempotency applies only to the (phase, system_fqn)
//   pair.  If a system is re-registered with the same FQN but a different phase,
//   register_system() must return SystemDescriptorConflict rather than silently
//   returning the existing id with the wrong phase.  This guards against hot-reload
//   rollback errors where a plugin re-registers an FQN to a changed phase, which
//   would produce a stale schedule without a visible error.
//
//   Scenario: SysA registered to Phase::Transform; second call with Phase::Physics
//   must be rejected with SystemDescriptorConflict.
//   A third call with Phase::Transform (the original) must remain idempotent.
// ===========================================================================

TEST_CASE("core/schedule: register_system_rejects_phase_drift_on_same_fqn") {
    glibre::PerContextAllocator alloc{glibre::ContextTag::core};
    glibre::core::Schedule sched{alloc};

    std::vector<glibre::core::TypeId> empty_reads;
    std::vector<glibre::core::TypeId> empty_writes;

    glibre::core::SystemDesc desc_a;
    desc_a.name = "test::phase_drift::SysA";
    desc_a.phase = glibre::core::Phase::Transform;
    desc_a.access = make_access(empty_reads, empty_writes);
    desc_a.body = &null_system;

    // First registration — must succeed and assign an id.
    auto r1 = sched.register_system(desc_a);
    REQUIRE(r1.has_value());
    const auto original_id = (*r1).value;

    // Second registration — identical (phase, name): must be idempotent.
    auto r2 = sched.register_system(desc_a);
    REQUIRE(r2.has_value());
    CHECK((*r2).value == original_id);

    // Third registration — same name, different phase: must be rejected.
    glibre::core::SystemDesc desc_a_drifted = desc_a;
    desc_a_drifted.phase = glibre::core::Phase::PhysicsFixed;
    auto r3 = sched.register_system(desc_a_drifted);
    REQUIRE_FALSE(r3.has_value());

    const auto* code = std::get_if<glibre::core::Error>(&r3.error().code());
    REQUIRE(code != nullptr);
    CHECK(*code == glibre::core::Error::SystemDescriptorConflict);
}
