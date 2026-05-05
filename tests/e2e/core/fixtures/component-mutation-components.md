<!-- SPDX-License-Identifier: Apache-2.0 -->

# component-mutation — fixture component layout

This document is the **authored source** for the component types
referenced by `tests/e2e/core/component-mutation.glibre-trace` (story
#324). It pins concrete `(size, align)` for the registered component
types `A` and `B`, and declares an *unregistered* sentinel `TypeId`
named `PhantomTypeId` used only to exercise the
`core::Error::TypeUnregistered` refusal path.

Per `specs/core/SPEC.md` §4.1 invariant 3 ("`TypeId` registered before
storage allocation") and §4.9 invariants 1–2 (`TypeRegistry` is
immutable-after-init; each registered `TypeId` corresponds to exactly
one descriptor), this trace's third Gherkin block requires a `TypeId`
value that is *not* present in the registry at world-creation time.
The fixture therefore distinguishes:

- **Registered descriptors** for `A` and `B` — emitted by the codegen
  pipeline through `glibre-types.dylib` (per
  `reviews/decisions/fory-codegen.md`) and present in the
  `TypeRegistry` from world construction onward.
- **An unregistered `TypeId` sentinel** (`PhantomTypeId`) — a literal
  64-bit value chosen so its hash collides with no registered
  descriptor. The codegen never emits a descriptor for it; the trace
  passes the bare value into `World::set_component` and asserts the
  `TypeUnregistered` refusal.

## Component types (registered)

```cpp
namespace glibre::tests::component_mutation_fixture {

// `A` — 8 bytes, 8-byte aligned. Carries one u64 payload.
//   Used as the entity's "base" component: every mutation in this
//   trace begins with an entity in archetype {A}.
struct A {
    std::uint64_t value;
};
static_assert(sizeof(A)  == 8);
static_assert(alignof(A) == 8);

// `B` — 4 bytes, 4-byte aligned. Carries one u32 payload.
//   The component added in block 1 / removed in block 2. Distinct
//   from `A` in `(size, align)` so a faulty migration that re-uses
//   the wrong column would surface as a structural mismatch in the
//   forward/reverse map consistency check.
struct B {
    std::uint32_t value;
};
static_assert(sizeof(B)  == 4);
static_assert(alignof(B) == 4);

}  // namespace glibre::tests::component_mutation_fixture
```

## Unregistered sentinel `TypeId`

```cpp
// `PhantomTypeId` — a 64-bit value the codegen pipeline NEVER emits
// a descriptor for. The literal is chosen high in the u64 space to
// be visibly outside the codegen-emitted hash range (codegen hashes
// are blake3-derived and uniformly distributed; the literal below
// is reserved for tests by convention).
namespace glibre::tests::component_mutation_fixture {

inline constexpr glibre::core::TypeId kPhantomTypeId{
    .value = 0xDEAD'BEEF'DEAD'BEEFULL,
};

}  // namespace glibre::tests::component_mutation_fixture
```

The debug overlay's F4 binding (see the trace) calls
`World::set_component(entity, kPhantomTypeId, payload)`; per
`specs/core/SPEC.md` §4.1 invariant 3 the call refuses with
`core::Error::TypeUnregistered` *before* dispatching into archetype
storage. No partial mutation is observable.

## Why these specific shapes

- `A` carries a `u64` and `B` a `u32` so the two columns differ in
  width by exactly a factor of two; this stresses the codegen-emitted
  column-offset arithmetic at migration boundaries (when a row moves
  {A} → {A,B}, the new {A,B} archetype's `B` column is allocated and
  the existing `A` column is preserved — both invariants per
  `specs/core/SPEC.md` §4.2 invariants 1, 3, 5).
- The chunk-capacity override hook is *not* used by this trace.
  Component-set transitions are observable with one or two entities;
  capacity / spillover behaviour is the domain of #322's
  `archetype-storage.glibre-trace`. This trace privileges the
  add/remove migration path over the chunk-spillover path.
- `PhantomTypeId` is a constant — not a runtime-computed hash —
  because runtime registration is forbidden (PHILOSOPHY §6). The
  refusal must be triggered by the *absence* of a descriptor, not by
  an attempt to add one.

## Re-recording note

Per `specs/e2e/SPEC.md` §7.1.1 invariant 2, this fixture *cannot* be
edited without re-recording the binary trace. Touching `sizeof(A)`,
`sizeof(B)`, `alignof(A)`, `alignof(B)`, or the `kPhantomTypeId`
literal perturbs the codegen-emitted column descriptors and
invalidates the recorded `EnvHash`. Treat the layout as load-bearing.
