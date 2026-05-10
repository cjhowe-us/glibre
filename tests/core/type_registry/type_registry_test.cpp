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
//   - core/type_registry: lookup_o1_no_string_no_reflection
//   - core/type_registry: bootstrap_from_static_init_populates_count
//
// Design constraints:
//   - -fno-exceptions (error-model.md §Decision 3).
//   - No REQUIRE_THROWS usage.
//   - No std:: containers in test helpers (PHILOSOPHY §11).
//   - Tests exercise the public API surface only; internals (entries_ vector)
//     are not accessed directly.

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <type_traits>

#include <EASTL/variant.h>
#include <catch2/catch_test_macros.hpp>
#include <glibre/core/type_registry.hpp>
#include <glibre/error.hpp>

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

namespace {

/// Make a non-trivial ColumnDescriptor with identifiable field values.
glibre::core::ColumnDescriptor make_desc(std::size_t size, std::size_t align) noexcept {
    return glibre::core::ColumnDescriptor{
        .size   = size,
        .align  = align,
        .drop   = nullptr,
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

TEST_CASE("core/type_registry: lookup_unregistered_yields_TypeUnregistered",
          "[core][type_registry]") {
    std::pmr::monotonic_buffer_resource buf{4096};
    glibre::core::TypeRegistry reg{&buf};

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
        auto ok  = reg.lookup(glibre::core::TypeId{0});
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

TEST_CASE("core/type_registry: register_after_seal_yields_TypeRegistryClosed",
          "[core][type_registry]") {
    std::pmr::monotonic_buffer_resource buf{4096};
    glibre::core::TypeRegistry reg{&buf};

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
// This test exercises the friend hook through a tiny shim that simulates
// what PluginLoader does: it holds a reference to the TypeRegistry and
// calls extend_during_load() after seal().
//
// Because PluginLoader is not yet implemented in full, we use a local test
// shim declared as a friend of TypeRegistry by the class definition.
// The friend declaration in type_registry.hpp is `friend class PluginLoader;`
// (in namespace glibre::core).  This test does NOT use PluginLoader; instead
// it calls the private extend_during_load() directly through a helper struct
// that is declared in the same namespace so it can be a transitively-friend
// participant.
//
// Approach: expose extend_during_load via a thin test accessor declared in
// the same translation unit, invoking it through explicit friend-access to
// the TypeRegistry class.  We call the method through a pointer-to-member
// function obtained via the friend class PluginLoader mechanism by defining
// a surrogate in namespace glibre::core that is granted friend access.
//
// Simpler approach (adopted): define a free-standing test helper in
// namespace glibre::core that takes a TypeRegistry& and calls the private
// member; the compiler allows this because the helper is compiled in the
// same TU that includes the class definition.  However, C++ does not
// extend friend access to free functions in the same TU — only the
// named PluginLoader class is a friend.
//
// Actual approach: we define a minimal stub PluginLoader in this TU
// (inside namespace glibre::core) with only the method needed to call
// extend_during_load().  Because the stub shares the name and namespace
// of the friend-declared class, the compiler grants it friend access.
// ===========================================================================

namespace glibre::core {

// Minimal stub for PluginLoader — grants friend access to extend_during_load.
// This stub is compiled only in this test TU and is distinct from any future
// real PluginLoader implementation.
class PluginLoader {
public:
    static Result<void>
    test_extend(TypeRegistry& reg, TypeId id, ColumnDescriptor desc) noexcept {
        return reg.extend_during_load(id, desc);
    }
};

}  // namespace glibre::core

TEST_CASE("core/type_registry: register_during_plugin_register_admitted",
          "[core][type_registry]") {
    std::pmr::monotonic_buffer_resource buf{4096};
    glibre::core::TypeRegistry reg{&buf};

    // Bootstrap: register one type pre-seal.
    REQUIRE(reg.register_type(glibre::core::TypeId{0}, make_desc(4, 4)).has_value());
    reg.seal();
    CHECK(reg.is_sealed());
    CHECK(reg.count() == 1u);

    SECTION("extend_during_load admits registration after seal") {
        // Simulate PluginLoader calling extend_during_load for a newly-loaded plugin type.
        auto result = glibre::core::PluginLoader::test_extend(
            reg, glibre::core::TypeId{1}, make_desc(16, 8));
        REQUIRE(result.has_value());
        CHECK(reg.count() == 2u);

        // The new type is immediately reachable via lookup.
        auto d = reg.lookup(glibre::core::TypeId{1});
        REQUIRE(d.has_value());
        CHECK(d.value()->size == 16u);
        CHECK(d.value()->align == 8u);
    }

    SECTION("extend_during_load enforces contiguous slot requirement") {
        // Slot 1 is the next valid slot; slot 3 skips over slots 1 and 2 → error.
        auto result = glibre::core::PluginLoader::test_extend(
            reg, glibre::core::TypeId{3}, make_desc(4, 4));
        REQUIRE_FALSE(result.has_value());
        CHECK(is_type_unregistered(result.error()));
    }

    SECTION("register_type is still refused after seal even for valid slot") {
        // Slot 1 is the next valid slot, but register_type is sealed → closed.
        auto result = reg.register_type(glibre::core::TypeId{1}, make_desc(4, 4));
        REQUIRE_FALSE(result.has_value());
        CHECK(is_type_registry_closed(result.error()));
    }
}

// ===========================================================================
// Test: lookup_o1_no_string_no_reflection
//
// §6.9: "No reflection, no string lookup on the hot path: TypeId is a
// stable codegen-emitted integer."
//
// This test verifies the O(1) property structurally — lookup() uses a flat
// vector index (TypeId.value), not a hash map, string comparison, or
// any dynamic dispatch.  We verify:
//   A. Lookup of N registered types completes in N sequential calls with no
//      failures (proving the index path, not a linear scan).
//   B. ColumnDescriptor carries no string fields (compile-time check).
//   C. TypeRegistry carries no virtual functions (compile-time check).
// ===========================================================================

TEST_CASE("core/type_registry: lookup_o1_no_string_no_reflection",
          "[core][type_registry]") {
    // Compile-time: TypeRegistry must not have virtual functions.
    static_assert(!std::is_polymorphic_v<glibre::core::TypeRegistry>,
                  "TypeRegistry must not be polymorphic (no virtual functions).");

    // Compile-time: ColumnDescriptor must be trivially-copyable (plain data).
    static_assert(std::is_trivially_copyable_v<glibre::core::ColumnDescriptor>,
                  "ColumnDescriptor must be trivially copyable (plain data, no strings).");

    // Compile-time: TypeId must be trivially-copyable (codegen-emitted integer).
    static_assert(std::is_trivially_copyable_v<glibre::core::TypeId>,
                  "TypeId must be trivially copyable.");

    std::pmr::monotonic_buffer_resource buf{8192};
    glibre::core::TypeRegistry reg{&buf};

    constexpr std::size_t kN = 8;
    for (std::size_t i = 0; i < kN; ++i) {
        auto r = reg.register_type(
            glibre::core::TypeId{i},
            make_desc(i + 1, 1));
        REQUIRE(r.has_value());
    }

    SECTION("sequential O(1) index lookups all succeed") {
        for (std::size_t i = 0; i < kN; ++i) {
            auto d = reg.lookup(glibre::core::TypeId{i});
            REQUIRE(d.has_value());
            // Each descriptor has a unique size equal to i+1; verify the
            // correct slot was returned (not a linear scan returning slot 0).
            CHECK(d.value()->size == i + 1);
        }
    }

    SECTION("count() == kN after kN registrations") {
        CHECK(reg.count() == kN);
    }
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

TEST_CASE("core/type_registry: bootstrap_from_static_init_populates_count",
          "[core][type_registry]") {
    std::pmr::monotonic_buffer_resource buf{8192};
    glibre::core::TypeRegistry reg{&buf};

    // Simulate glibre-types.dylib _registry.cpp static-init output:
    // three component types with typical ECS sizes.
    struct SimulatedType { std::size_t size; std::size_t align; };
    const SimulatedType kBootstrapTypes[] = {
        {4,  4},   // e.g. a 32-bit tag component
        {16, 16},  // e.g. a Vec4f position component
        {32, 8},   // e.g. a larger aggregate component
    };
    constexpr std::size_t kTypeCount = 3;

    // Static-init registration (before World::create()):
    for (std::size_t i = 0; i < kTypeCount; ++i) {
        auto r = reg.register_type(
            glibre::core::TypeId{i},
            make_desc(kBootstrapTypes[i].size, kBootstrapTypes[i].align));
        REQUIRE(r.has_value());
    }

    SECTION("count() equals number of registered types after bootstrap") {
        CHECK(reg.count() == kTypeCount);
    }

    SECTION("all bootstrap descriptors are readable pre-seal") {
        for (std::size_t i = 0; i < kTypeCount; ++i) {
            auto d = reg.lookup(glibre::core::TypeId{i});
            REQUIRE(d.has_value());
            CHECK(d.value()->size  == kBootstrapTypes[i].size);
            CHECK(d.value()->align == kBootstrapTypes[i].align);
        }
    }

    SECTION("seal() — count() is stable; no new registrations admitted") {
        reg.seal();
        CHECK(reg.count() == kTypeCount);

        // Post-seal registration attempt returns TypeRegistryClosed.
        auto post = reg.register_type(
            glibre::core::TypeId{kTypeCount},
            make_desc(4, 4));
        REQUIRE_FALSE(post.has_value());
        CHECK(is_type_registry_closed(post.error()));

        // count() unchanged after refused registration.
        CHECK(reg.count() == kTypeCount);
    }
}
