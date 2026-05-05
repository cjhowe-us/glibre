<!-- SPDX-License-Identifier: Apache-2.0 -->

# archetype-storage — fixture component layout

This document is the **authored source** for the three component types
referenced by `tests/e2e/core/archetype-storage.glibre-trace` (story
#322). It pins concrete `(size, align)` and chunk capacity values so the
trace's assertions about column alignment, chunk capacity, and
swap-remove integrity are byte-stable.

Per `specs/core/SPEC.md` §4.2 invariants 1–5 and §3.2 collapse #3, the
`core` context owns the codegen-emitted archetype layout end-to-end.
The middleman dylib (per `reviews/decisions/fory-codegen.md`) carries
column descriptors keyed by `TypeId`. These three types are registered
at world-creation time via the same codegen pipeline; no runtime
registration occurs (PHILOSOPHY §6).

## Component types

```cpp
namespace glibre::tests::archetype_storage_fixture {

// `A` — 4 bytes, 4-byte aligned. Smallest natural-alignment column.
struct A {
    std::uint32_t value;
};
static_assert(sizeof(A)  == 4);
static_assert(alignof(A) == 4);

// `B` — 16 bytes, 16-byte aligned. Forces SIMD-width column alignment.
struct alignas(16) B {
    std::array<float, 4> values;
};
static_assert(sizeof(B)  == 16);
static_assert(alignof(B) == 16);

// `C` — 8 bytes, 8-byte aligned. Distinguishes the {A,B,C} archetype
// from {A,B} by exactly one component, so the trace's
// `archetype_count == 2` assertion is sharp.
struct C {
    std::uint64_t value;
};
static_assert(sizeof(C)  == 8);
static_assert(alignof(C) == 8);

}  // namespace glibre::tests::archetype_storage_fixture
```

## Chunk capacity

For the duration of this trace, the fixture registers the {A,B}
archetype with a **chunk capacity of 4 rows** via the debug-overlay
hook `core::testing::set_chunk_capacity_override(ArchetypeKey, u32)`.
This hook is gated behind `cfg(debug_overlay)` (PHILOSOPHY §6 — no
runtime layout mutation in shipping builds). The override is reset on
world destruction.

The natural production `Chunk::CAPACITY` (specs/core/SPEC.md §4.2
invariant 2) is a codegen-emitted constant tuned for cache-line
geometry; testing chunk-spillover at the production capacity would
require ~thousands of entities, which the trace's manual-test sibling
(`#322` Manual Test step 2: 100k entities) covers. The trace itself is
authored for **clarity over stress**: 4-row chunks make the second
chunk's allocation observable in a single AssertState op without the
trace becoming a load test.

## Why these specific shapes

- `A` and `B` differ in `alignof` (4 vs 16). The Block 1 alignment
  assertions therefore prove the per-column `alignof(T) % alignof(T) == 0`
  rule for *both* a natural-alignment type and an over-aligned type
  (specs/core/SPEC.md §4.2 invariant 3).
- `C` exists only to distinguish the {A,B} archetype from the {A,B,C}
  archetype. Component-set membership is a *set*, not a sequence
  (§4.2 invariant 1); `C` adds one element so the two sets are unequal,
  forcing two distinct `Archetype` instances.
- All three sizes are powers of two so the codegen-emitted column
  offset arithmetic remains trivial. The trace does not assert specific
  byte offsets; it asserts the alignment-and-contiguity invariants
  (§4.2 invariant 3) which hold regardless of the offsets the codegen
  picks.

## Re-recording note

Per `specs/e2e/SPEC.md` §7.1.1 invariant 2, this fixture *cannot* be
edited without re-recording the binary trace. Touching `sizeof(A)`,
`alignof(B)`, the chunk-capacity override, or any field name perturbs
the codegen-emitted column descriptors and invalidates the recorded
`EnvHash`. Treat the layout as load-bearing.
