// tests/core/type_registry/type_registry_test.cpp
//
// Catch2 unit tests for glibre::core::TypeRegistry.
//
// Authority: specs/core/SPEC.md §4.9; plan #597 Unit Test Plan.
//
// Named test cases (plan #597 Unit Test Plan + DoD):
//   - core/type_registry: lookup_unregistered_yields_TypeUnregistered
//   - core/type_registry: register_after_seal_yields_TypeRegistryClosed
//   - core/type_registry: register_during_plugin_register_admitted
//   - core/type_registry: lookup_index_based_no_string_no_reflection
//   - core/type_registry: bootstrap_from_static_init_populates_count
//
// Design constraints:
//   - -fno-exceptions (error-model.md §Decision 3).
//   - No REQUIRE_THROWS usage.
//   - No std:: containers in test helpers (PHILOSOPHY §11).
//   - Tests exercise the public API surface only; internals (entries_ vector)
//     are not accessed directly.
//
// ## Friend access for extend_during_load() (HIGH-2 round-1 review fix)
//
//   The prior approach defined a stub `class PluginLoader` in namespace
//   glibre::core inside this TU.  That stub shared the real PluginLoader's
//   name and namespace, which is an ODR violation against the real
//   plugin_loader.hpp definition — the stub linked today only because it
//   contained a single static method, but any COMDAT merge or LTO pass
//   could silently pick the wrong definition.
//
//   The fix: the real plugin_loader.hpp is included here.  Access to the
//   private extend_during_load() is provided by the `TypeRegistryTestHook`
//   friend class, which is declared as a friend in type_registry.hpp.
//   TypeRegistryTestHook is defined here in this TU; it calls set_loading_()
//   (the latch) and extend_during_load() directly via the friend grant.
//
// ## is_loading_ latch in tests (MED-4 round-1 review fix)
//
//   extend_during_load() now asserts is_loading_ == true.  Tests must call
//   TypeRegistryTestHook::begin_load(reg) before any extend_during_load call
//   and TypeRegistryTestHook::end_load(reg) afterward to keep the state
//   consistent.

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <EASTL/variant.h>
#include <catch2/catch_test_macros.hpp>
#include <glibre/alloc.hpp>               // PerContextAllocator, ContextTag
#include <glibre/core/plugin_loader.hpp>  // real PluginLoader — included to avoid ODR violation
#include <glibre/core/type_registry.hpp>
#include <glibre/error.hpp>

// ---------------------------------------------------------------------------
// TypeRegistryTestHook — friend test accessor for extend_during_load() and
// the is_loading_ latch (set_loading_()).
//
// Declared as a friend in type_registry.hpp.  This class exists ONLY in test
// builds (compiled only when GLIBRE_TESTING=1, or here in the test TU).
// The real production build never sees this class.
// ---------------------------------------------------------------------------

namespace glibre::core {

class TypeRegistryTestHook {
public:
    // begin_load(reg) — set the is_loading_ latch to true.
    //
    // Call before any extend_during_load() invocation in a test to satisfy
    // the MED-4 assert.  Mirrors what PluginLoader does at the entry of its
    // loading critical section.
    static void begin_load(TypeRegistry& reg) noexcept { reg.set_loading_(true); }

    // end_load(reg) — clear the is_loading_ latch.
    //
    // Call after all extend_during_load() invocations to mirror PluginLoader's
    // exit from the loading critical section.
    static void end_load(TypeRegistry& reg) noexcept { reg.set_loading_(false); }

    // extend(reg, id, desc) — call extend_during_load() directly.
    //
    // Precondition: begin_load(reg) has been called (is_loading_ == true).
    static Result<void> extend(TypeRegistry& reg, TypeId id, ColumnDescriptor desc) noexcept {
        return reg.extend_during_load(id, desc);
    }
};

}  // namespace glibre::core

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

namespace {

/// Make a non-trivial ColumnDescriptor with identifiable field values.
glibre::core::ColumnDescriptor make_desc(std::size_t size, std::size_t align) noexcept {
    return glibre::core::ColumnDescriptor{
        .size = size,
        .align = align,
        .drop = nullptr,
        .layout = 0,
    };
}

/// Returns true iff the glibre::Error wraps core::Error::TypeUnregistered.
bool is_type_unregistered(const glibre::Error& err) noexcept {
    using glibre::core::Error;
    if (auto* e = eastl::get_if<Error>(&err.code())) {
        return *e == Error::TypeUnregistered;
    }
    return false;
}

/// Returns true iff the glibre::Error wraps core::Error::TypeRegistryClosed.
bool is_type_registry_closed(const glibre::Error& err) noexcept {
    using glibre::core::Error;
    if (auto* e = eastl::get_if<Error>(&err.code())) {
        return *e == Error::TypeRegistryClosed;
    }
    return false;
}

/// Returns true iff the glibre::Error wraps core::Error::TypeRegistryGap.
bool is_type_registry_gap(const glibre::Error& err) noexcept {
    using glibre::core::Error;
    if (auto* e = eastl::get_if<Error>(&err.code())) {
        return *e == Error::TypeRegistryGap;
    }
    return false;
}

}  // anonymous namespace

// ===========================================================================
// Test: lookup_unregistered_yields_TypeUnregistered
//
// §4.9 invariant 2: each registered TypeId corresponds to exactly one
// descriptor.  A TypeId not present in the registry must be refused with
// core::Error::TypeUnregistered (SPEC §10 error table).
//
// Cases covered:
//   A. Empty registry — any TypeId is unregistered.
//   B. Non-empty registry — TypeId.value >= count() is unregistered.
//   C. is_registered() mirrors lookup() — returns false for the same ids.
// ===========================================================================

TEST_CASE(
    "core/type_registry: lookup_unregistered_yields_TypeUnregistered", "[core][type_registry]"
) {
    glibre::PerContextAllocator alloc{glibre::ContextTag::core};
    glibre::core::TypeRegistry reg{alloc};

    SECTION("empty registry — any lookup yields TypeUnregistered") {
        auto result = reg.lookup(glibre::core::TypeId{0});
        REQUIRE_FALSE(result.has_value());
        CHECK(is_type_unregistered(result.error()));
    }

    SECTION("lookup with id.value >= count yields TypeUnregistered") {
        // Register one descriptor at slot 0.
        auto r = reg.register_type(glibre::core::TypeId{0}, make_desc(4, 4));
        REQUIRE(r.has_value());

        // Slot 0 is registered; slot 1 is not.
        auto ok = reg.lookup(glibre::core::TypeId{0});
        auto bad = reg.lookup(glibre::core::TypeId{1});

        REQUIRE(ok.has_value());
        CHECK(ok.value()->size == 4u);

        REQUIRE_FALSE(bad.has_value());
        CHECK(is_type_unregistered(bad.error()));
    }

    SECTION("is_registered mirrors lookup") {
        CHECK_FALSE(reg.is_registered(glibre::core::TypeId{0}));

        auto r = reg.register_type(glibre::core::TypeId{0}, make_desc(8, 8));
        REQUIRE(r.has_value());

        CHECK(reg.is_registered(glibre::core::TypeId{0}));
        CHECK_FALSE(reg.is_registered(glibre::core::TypeId{1}));
    }
}

// ===========================================================================
// Test: register_after_seal_yields_TypeRegistryClosed
//
// §4.9 invariant 1: no entries may be added after World construction completes.
// Attempted mutation after seal() returns core::Error::TypeRegistryClosed.
//
// Cases covered:
//   A. seal() then register_type() → TypeRegistryClosed.
//   B. Already-registered descriptors are still readable after seal.
//   C. is_sealed() reflects seal() state.
// ===========================================================================

TEST_CASE(
    "core/type_registry: register_after_seal_yields_TypeRegistryClosed", "[core][type_registry]"
) {
    glibre::PerContextAllocator alloc{glibre::ContextTag::core};
    glibre::core::TypeRegistry reg{alloc};

    // Pre-seal: register two types.
    REQUIRE(reg.register_type(glibre::core::TypeId{0}, make_desc(4, 4)).has_value());
    REQUIRE(reg.register_type(glibre::core::TypeId{1}, make_desc(8, 8)).has_value());
    CHECK_FALSE(reg.is_sealed());

    reg.seal();
    CHECK(reg.is_sealed());

    SECTION("register_type after seal returns TypeRegistryClosed") {
        auto result = reg.register_type(glibre::core::TypeId{2}, make_desc(16, 8));
        REQUIRE_FALSE(result.has_value());
        CHECK(is_type_registry_closed(result.error()));
    }

    SECTION("sealed registry still answers lookups correctly") {
        auto d0 = reg.lookup(glibre::core::TypeId{0});
        auto d1 = reg.lookup(glibre::core::TypeId{1});
        REQUIRE(d0.has_value());
        REQUIRE(d1.has_value());
        CHECK(d0.value()->size == 4u);
        CHECK(d1.value()->size == 8u);
        CHECK(reg.count() == 2u);
    }
}

// ===========================================================================
// Test: register_during_plugin_register_admitted
//
// §6.9: hot-reload may add TypeId entries; a newly-loaded plugin may
// register types even after the registry is sealed, via the loader's
// friend hook extend_during_load().
//
// This test exercises the friend hook through TypeRegistryTestHook, the
// dedicated test accessor declared as a friend in type_registry.hpp.
// TypeRegistryTestHook::begin_load() sets the is_loading_ latch (MED-4 fix)
// before calling extend_during_load(); end_load() clears it afterward.
//
// The old approach (stub PluginLoader in namespace glibre::core) was an ODR
// violation against the real plugin_loader.hpp definition and has been
// replaced (HIGH-2 round-1 review fix).
// ===========================================================================

TEST_CASE("core/type_registry: register_during_plugin_register_admitted", "[core][type_registry]") {
    glibre::PerContextAllocator alloc{glibre::ContextTag::core};
    glibre::core::TypeRegistry reg{alloc};

    // Bootstrap: register one type pre-seal.
    REQUIRE(reg.register_type(glibre::core::TypeId{0}, make_desc(4, 4)).has_value());
    reg.seal();
    CHECK(reg.is_sealed());
    CHECK(reg.count() == 1u);

    SECTION("extend_during_load admits registration after seal") {
        // Set the is_loading_ latch to satisfy the MED-4 assert.
        glibre::core::TypeRegistryTestHook::begin_load(reg);

        auto result = glibre::core::TypeRegistryTestHook::extend(
            reg, glibre::core::TypeId{1}, make_desc(16, 8)
        );
        REQUIRE(result.has_value());
        CHECK(reg.count() == 2u);

        glibre::core::TypeRegistryTestHook::end_load(reg);

        // The new type is immediately reachable via lookup.
        auto d = reg.lookup(glibre::core::TypeId{1});
        REQUIRE(d.has_value());
        CHECK(d.value()->size == 16u);
        CHECK(d.value()->align == 8u);
    }

    SECTION("extend_during_load enforces contiguous slot requirement") {
        glibre::core::TypeRegistryTestHook::begin_load(reg);

        // Slot 1 is the next valid slot; slot 3 skips over slots 1 and 2 → TypeRegistryGap.
        auto result = glibre::core::TypeRegistryTestHook::extend(
            reg, glibre::core::TypeId{3}, make_desc(4, 4)
        );
        REQUIRE_FALSE(result.has_value());
        CHECK(is_type_registry_gap(result.error()));

        glibre::core::TypeRegistryTestHook::end_load(reg);
    }

    SECTION("register_type is still refused after seal even for valid slot") {
        // Slot 1 is the next valid slot, but register_type is sealed → closed.
        auto result = reg.register_type(glibre::core::TypeId{1}, make_desc(4, 4));
        REQUIRE_FALSE(result.has_value());
        CHECK(is_type_registry_closed(result.error()));
    }

    SECTION("extend_during_load outside load window returns TypeRegistryClosed (MED-3 fix)") {
        // is_loading_ is false (begin_load was never called).  In release builds
        // the old assert-only check was elided and the call would silently mutate
        // a sealed registry.  The MED-3 fix returns TypeRegistryClosed always.
        auto result = glibre::core::TypeRegistryTestHook::extend(
            reg, glibre::core::TypeId{1}, make_desc(4, 4)
        );
        REQUIRE_FALSE(result.has_value());
        CHECK(is_type_registry_closed(result.error()));

        // Registry must be unmodified — count still 1.
        CHECK(reg.count() == 1u);
    }
}

// ===========================================================================
// Test: lookup_index_based_no_string_no_reflection
//
// Renamed from lookup_o1_no_string_no_reflection (round-1 review LOW-9 fix).
// The previous name claimed O(1) verification which this test does not
// actually measure; the test verifies structural properties — index-based
// lookup with no string keys and no virtual dispatch — which this name
// accurately reflects.
//
// §6.9: "No reflection, no string lookup on the hot path: TypeId is a
// stable codegen-emitted integer."
//
// Verifies:
//   A. Lookup of N registered types completes in N sequential calls with no
//      failures (proving the index path, not a linear scan).
//   B. ColumnDescriptor carries no string fields (compile-time check).
//   C. TypeRegistry carries no virtual functions (compile-time check).
// ===========================================================================

TEST_CASE(
    "core/type_registry: lookup_index_based_no_string_no_reflection", "[core][type_registry]"
) {
    // Compile-time: TypeRegistry must not have virtual functions.
    static_assert(
        !std::is_polymorphic_v<glibre::core::TypeRegistry>,
        "TypeRegistry must not be polymorphic (no virtual functions)."
    );

    // Compile-time: ColumnDescriptor must be trivially-copyable (plain data).
    static_assert(
        std::is_trivially_copyable_v<glibre::core::ColumnDescriptor>,
        "ColumnDescriptor must be trivially copyable (plain data, no strings)."
    );

    // Compile-time: TypeId must be trivially-copyable (codegen-emitted integer).
    static_assert(
        std::is_trivially_copyable_v<glibre::core::TypeId>, "TypeId must be trivially copyable."
    );

    glibre::PerContextAllocator alloc{glibre::ContextTag::core};
    glibre::core::TypeRegistry reg{alloc};

    constexpr std::size_t kN = 8;
    for (std::size_t i = 0; i < kN; ++i) {
        auto r = reg.register_type(glibre::core::TypeId{i}, make_desc(i + 1, 1));
        REQUIRE(r.has_value());
    }

    SECTION("sequential index-based lookups all succeed with correct descriptors") {
        for (std::size_t i = 0; i < kN; ++i) {
            auto d = reg.lookup(glibre::core::TypeId{i});
            REQUIRE(d.has_value());
            // Each descriptor has a unique size equal to i+1; verify the
            // correct slot was returned (not a linear scan returning slot 0).
            CHECK(d.value()->size == i + 1);
        }
    }

    SECTION("count() == kN after kN registrations") { CHECK(reg.count() == kN); }
}

// ===========================================================================
// Test: bootstrap_from_static_init_populates_count
//
// SPEC §4.9 invariant 1 + §6.9: "The registry is populated at static init
// from glibre-types.dylib's _registry.cpp (codegen-emitted)."
//
// The actual codegen pipeline (glibre-types.dylib, plan TBD) is out of scope
// for plan #597.  This test simulates the static-init contract: a sequence
// of register_type() calls (as the codegen-emitted initialiser would issue)
// before any World construction, followed by seal() and a count() check.
//
// Verifies:
//   A. Simulated static-init sequence — N register_type() calls — succeeds.
//   B. count() equals N after the sequence.
//   C. All N descriptors are readable via lookup().
//   D. After seal(), count() is stable (no further entries).
// ===========================================================================

TEST_CASE(
    "core/type_registry: bootstrap_from_static_init_populates_count", "[core][type_registry]"
) {
    glibre::PerContextAllocator alloc{glibre::ContextTag::core};
    glibre::core::TypeRegistry reg{alloc};

    // Simulate glibre-types.dylib _registry.cpp static-init output:
    // three component types with typical ECS sizes.
    struct SimulatedType {
        std::size_t size;
        std::size_t align;
    };

    const SimulatedType kBootstrapTypes[] = {
        {4, 4},    // e.g. a 32-bit tag component
        {16, 16},  // e.g. a Vec4f position component
        {32, 8},   // e.g. a larger aggregate component
    };
    constexpr std::size_t kTypeCount = 3;

    // Static-init registration (before World::create()):
    for (std::size_t i = 0; i < kTypeCount; ++i) {
        auto r = reg.register_type(
            glibre::core::TypeId{i}, make_desc(kBootstrapTypes[i].size, kBootstrapTypes[i].align)
        );
        REQUIRE(r.has_value());
    }

    SECTION("count() equals number of registered types after bootstrap") {
        CHECK(reg.count() == kTypeCount);
    }

    SECTION("all bootstrap descriptors are readable pre-seal") {
        for (std::size_t i = 0; i < kTypeCount; ++i) {
            auto d = reg.lookup(glibre::core::TypeId{i});
            REQUIRE(d.has_value());
            CHECK(d.value()->size == kBootstrapTypes[i].size);
            CHECK(d.value()->align == kBootstrapTypes[i].align);
        }
    }

    SECTION("seal() — count() is stable; no new registrations admitted") {
        reg.seal();
        CHECK(reg.count() == kTypeCount);

        // Post-seal registration attempt returns TypeRegistryClosed.
        auto post = reg.register_type(glibre::core::TypeId{kTypeCount}, make_desc(4, 4));
        REQUIRE_FALSE(post.has_value());
        CHECK(is_type_registry_closed(post.error()));

        // count() unchanged after refused registration.
        CHECK(reg.count() == kTypeCount);
    }
}
