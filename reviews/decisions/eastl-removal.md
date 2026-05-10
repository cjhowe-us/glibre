# Decision Record — EASTL Removal: libc++ stdlib + std::ranges + PMR

## Status

Accepted (spike #1033, parent initiative #1032). This record reverses
PHILOSOPHY.md §11 (EASTL adoption). It is the universal reference every
downstream per-context migration `[PLAN]` cites; it does **not** open
those plans (the next planning spike does that).

Builds on:

- `reviews/decisions/error-model.md` — `std::expected<T, glibre::Error>`
  is the public-boundary contract. `glibre::Error` currently uses
  `eastl::variant`; this record switches it to `std::variant` without
  altering the boundary contract.
- `reviews/decisions/perf-budget.md` — §Allocator Rules #1-#4 (per-tag
  ceiling, strict-mode `OutOfBudget`, soft-warn shipping, transient
  arena exemption). The PMR adapter chosen here is the smallest
  surface that lets `std::pmr::*` containers honour those rules.
- `reviews/decisions/plugin-abi.md` — plugin-ABI surfaces remain POD
  spans + handles. PMR / std::ranges affect only intra-context code,
  not the four `extern "C"` plugin-loader symbols.
- `core/include/glibre/alloc.hpp` (PR #1028, on `feat/core-type-registry`)
  — already drafts `glibre::PerContextAllocatorResource`, the PMR
  adapter this record canonicalises engine-wide.
- `core/include/glibre/error.hpp` — current `eastl::variant`-based
  `glibre::Error` arrangement that this record migrates.

## Context

PHILOSOPHY.md §11 made EASTL the substrate for runtime data structures
on five claimed advantages: explicit allocator-by-value, no exceptions
on the hot path, frame / fixed / inline allocators, deterministic
iteration, and game-development debug instrumentation. The reality
discovered after several contexts landed on the EASTL substrate:

1. **Allocator-by-value is now a stdlib primitive.** `std::pmr` (C++17,
   shipping in libc++ on the locked toolchain) gives type-erased
   per-instance allocators that thread through every `std::pmr::*`
   container. The `PerContextAllocator` already in tree
   (`core/include/glibre/alloc.hpp`) has been wrapped as a
   `std::pmr::memory_resource` adapter (`PerContextAllocatorResource`,
   PR #1028, recently lifted out of TypeRegistry per round-2 MED-2 SRP
   feedback) and proven on `std::pmr::vector<ColumnDescriptor>` in
   `TypeRegistry`. The §11 motivation collapses into one substrate:
   `PerContextAllocator` + `PerContextAllocatorResource` +
   `std::pmr::polymorphic_allocator<T>`.

2. **No-exceptions on the hot path is a build-flag concern, not a
   library concern.** Engine code already compiles with
   `-fno-exceptions` (`cmake/Compile.cmake` `glibre::compile_contract`,
   per `error-model.md` §Decision #3). libc++ stdlib functions whose
   only failure mode is `throw` (`std::variant::get`, `std::vector::at`,
   `std::stoi`) are simply forbidden by the same lint that already
   forbids `new`/`malloc`. `std::get_if<T>` / `std::visit` / range-for
   replace them. EASTL adds nothing to the no-exceptions story.

3. **Frame / fixed / inline allocators are still wanted, but the
   substrate is unchanged.** The frame allocator is
   `core::TransientArena` (already in tree); fixed allocators are PMR
   `std::pmr::monotonic_buffer_resource` or our own
   `PerContextAllocatorResource`; inline storage in containers is
   `std::array<T, N>` (no allocation) or, when libc++ ships it,
   `std::inplace_vector<T, N>` (P0843). None of these need EASTL.

4. **Deterministic iteration order.** `std::pmr::flat_map` /
   `std::pmr::flat_set` (C++23) keep insertion-sorted order; standard
   `std::map` / `std::set` use comparator-sorted order. Both are
   deterministic. `std::unordered_*` is not, but neither was
   `eastl::hash_*`; both require the same "use a sorted container if
   determinism matters" discipline.

5. **Toolchain interop.** Catch2, spdlog, fmt, and the macOS standard
   library all speak `std::*`. EASTL forces a translation layer at
   every test assertion (`Catch2 REQUIRE(eastl::vector{...} == ...)`
   loses ADL, custom matchers, and the implicit `std::ranges`
   integration that arrived with Catch2 v3.5+).

The five §11 advantages either no longer hold or are now stdlib
primitives. The cost of keeping EASTL — a parallel container universe
that breaks ADL, breaks `std::ranges` pipe-syntax, breaks Catch2
matchers, and adds a vcpkg dependency that will not survive a libc++
SONAME bump — is not justified by any remaining benefit.

What this spike answers, that initiative #1032 did not:

1. The exact per-EASTL-type → libc++-replacement matrix, with
   shipping-status check on the locked toolchain.
2. The `std::ranges` adoption posture for boundary signatures and
   internal materialization sites.
3. The PMR adapter shape (`PerContextAllocatorResource`) and its
   `do_is_equal` semantics, which constrain whether `std::pmr`
   containers may move-elide allocations across context boundaries.
4. The `glibre::Error` migration from `eastl::variant` to
   `std::variant`, including the arm-count `static_assert` and the
   `std::visit` / `std::get_if` rules.

The record is **scoping**, not **mechanics**. The per-context migration
PRs (one per `[PLAN] migrate(...)` leaf the next planning spike will
author) execute the changes file-by-file; this record gives them a
single source-of-truth for the replacement choices so each PR cites
the same row of the matrix.

## Decision

### 1. Per-EASTL-type → libc++-replacement matrix

libc++ availability is asserted against the locked toolchain
(macOS 26 / Apple Silicon clang ≥ 21, which ships libc++ ≥ 19; see
`CLAUDE.md` Tech Stack row). C++26 features whose libc++ shipping
status is **not yet** in the locked toolchain receive a polyfill home
under `core/include/glibre/compat/<name>.hpp`. The polyfill is a
header-only thin shim whose **only reason to change** is the libc++
availability flip-day (SRP: one responsibility per compat header —
expose the future stdlib name today; delete the file when libc++
ships it). The `core/include/glibre/compat/` directory is created by
the first migration PLAN that needs it; it does not exist on `main`
today.

| EASTL type                         | libc++ replacement                                                              | C++ standard           | Locked toolchain ships? | Fallback / polyfill                                                                                                |
|------------------------------------|---------------------------------------------------------------------------------|------------------------|-------------------------|--------------------------------------------------------------------------------------------------------------------|
| `eastl::string`                    | `std::pmr::string` (engine code) / `std::string` (tools, ImGui, FBX wrappers)   | C++17                  | yes                     | n/a                                                                                                                |
| `eastl::string_view`               | `std::string_view`                                                              | C++17                  | yes                     | n/a                                                                                                                |
| `eastl::vector`                    | `std::pmr::vector<T>` (engine) / `std::vector<T>` (tools, tests fixture data)   | C++17                  | yes                     | n/a                                                                                                                |
| `eastl::fixed_vector<T, N>`        | `std::inplace_vector<T, N>` (P0843R14)                                          | C++26                  | **no** (libc++ tracking)| `core/include/glibre/compat/inplace_vector.hpp` — `std::array<T,N>` + `size_t size_` member, push_back returns Result on overflow |
| `eastl::array`                     | `std::array<T, N>`                                                              | C++11                  | yes                     | n/a                                                                                                                |
| `eastl::fixed_function<N, Sig>`    | `std::move_only_function<Sig>` (heap; engine default) — see Open Question 1     | C++23                  | yes                     | inline-storage variant deferred until measured hot path; if needed, polyfill `core/include/glibre/compat/inline_function.hpp` |
| `eastl::function`                  | `std::move_only_function<Sig>`                                                  | C++23                  | yes                     | `std::function` only at editor / scripting boundaries that genuinely require copyability                           |
| `eastl::optional`                  | `std::optional`                                                                 | C++17                  | yes                     | n/a                                                                                                                |
| `eastl::variant`                   | `std::variant`                                                                  | C++17                  | yes                     | n/a (see §4 below for `glibre::Error`-specific migration)                                                          |
| `eastl::tuple`                     | `std::tuple`                                                                    | C++17                  | yes                     | n/a                                                                                                                |
| `eastl::pair`                      | `std::pair`                                                                     | C++11                  | yes                     | n/a                                                                                                                |
| `eastl::unique_ptr`                | `std::unique_ptr`                                                               | C++14                  | yes                     | n/a; prefer `std::make_unique` / `std::make_unique_for_overwrite` (C++20)                                          |
| `eastl::shared_ptr`                | `std::shared_ptr`                                                               | C++11                  | yes                     | discouraged in engine code; engine prefers handles (see plugin-abi.md)                                             |
| `eastl::weak_ptr`                  | `std::weak_ptr`                                                                 | C++11                  | yes                     | discouraged for the same reason                                                                                    |
| `eastl::span`                      | `std::span`                                                                     | C++20                  | yes                     | n/a                                                                                                                |
| `eastl::hash_map<K,V>`             | `std::pmr::flat_map<K,V>` (default) / `std::pmr::unordered_map<K,V>` (hot mutate) | C++23 (flat) / C++17  | flat_map: **partial** in libc++ 19 — verify per migration PLAN | if `std::flat_map` not yet shipped at the migration PLAN's commit, use `std::pmr::unordered_map` and TODO-link to a follow-up PLAN |
| `eastl::hash_set<K>`               | `std::pmr::flat_set<K>` (default) / `std::pmr::unordered_set<K>` (hot mutate)   | C++23 (flat) / C++17  | flat_set: same as flat_map | same                                                                                                            |
| `eastl::map<K,V>` / `eastl::set<K>`| `std::pmr::flat_map<K,V>` / `std::pmr::flat_set<K>`                             | C++23                  | partial                 | same; node-based `std::pmr::map` / `std::pmr::set` is the C++17 fallback if `flat_*` unavailable                   |
| `eastl::deque`                     | `std::pmr::deque<T>`                                                            | C++17                  | yes                     | n/a; replace with ring buffer in measured hot paths                                                                |
| `eastl::list`                      | `std::pmr::list<T>`                                                             | C++17                  | yes                     | n/a; near-zero use today; intrusive lists in hot paths                                                             |
| `eastl::expected`                  | `std::expected<T, E>`                                                           | C++23                  | yes                     | n/a — error-model.md already mandates `std::expected`; the lone EASTL use is a defect to be removed                |
| `eastl::transparent_string_hash`   | `glibre::TransparentStringHash` (4-line struct: `using is_transparent = void;` + `operator()(std::string_view)`) | n/a — local utility | yes (header-only)  | `core/include/glibre/compat/transparent_string_hash.hpp` — keeps the polyfill cluster co-located even though no future stdlib version owns it |
| `eastl::nullopt`                   | `std::nullopt`                                                                  | C++17                  | yes                     | n/a                                                                                                                |
| `eastl::holds_alternative`         | `std::holds_alternative`                                                        | C++17                  | yes                     | n/a                                                                                                                |
| `eastl::get_if<T>`                 | `std::get_if<T>`                                                                | C++17                  | yes                     | n/a                                                                                                                |
| `eastl::get<T>` (throwing)         | **forbidden** in engine code (throws `std::bad_variant_access`)                 | C++17                  | yes                     | use `std::get_if<T>` + null-check, or `std::visit`; lint catches misuse                                            |
| `eastl::visit`                     | `std::visit`                                                                    | C++17                  | yes                     | n/a; pair with `Overloaded { lambdas... }` helper for ad-hoc visitors                                              |
| `eastl::variant_size_v<V>`         | `std::variant_size_v<V>`                                                        | C++17                  | yes                     | n/a — used in §4 for the arm-count `static_assert`                                                                 |
| `eastl::variant_npos`              | `std::variant_npos`                                                             | C++17                  | yes                     | n/a                                                                                                                |
| `eastl::move`                      | `std::move`                                                                     | C++11                  | yes                     | n/a                                                                                                                |
| `eastl::equal_to`                  | `std::equal_to<>`                                                               | C++14                  | yes                     | n/a; transparent (`<>` form) preferred for heterogeneous lookup                                                    |
| `eastl::allocator`                 | `std::pmr::polymorphic_allocator<T>`                                            | C++17                  | yes                     | substrate is `glibre::PerContextAllocatorResource` (see §3)                                                        |

**SRP justification (per row, condensed).** Where the matrix offers a
choice between two libc++ candidates, the chosen winner satisfies the
"one reason to change" test against the **allocation-tag invariant**
(perf-budget.md §Allocator Rules #1) — the chosen primitive's only
reason to change is the locked toolchain's libc++ shipping status, not
the per-context allocation rules:

- `std::pmr::vector<T>` over `std::vector<T>`: the latter would require
  every container site to either accept a `std::allocator` template
  parameter (rebind hell) or silently allocate from the global heap
  (breaks Rule #1). PMR's type-erased `polymorphic_allocator<T>` keeps
  the container's type identity stable across allocator changes.
- `std::pmr::flat_map<K,V>` over `std::pmr::unordered_map<K,V>` as the
  default: deterministic iteration (PHILOSOPHY #7), one cache line per
  entry, no node-pointer chase. `unordered_map` is the carve-out for
  measured hot-mutate sites only.
- `std::move_only_function<Sig>` over `std::function<Sig>`: function
  copyability is a separate axis from callability; engine callbacks
  are overwhelmingly move-only (one-shot continuations, system
  registrations). Forcing copyability everywhere is the SOLID
  Liskov-style violation §11 was implicitly accepting via
  `eastl::function`.
- `std::pmr::polymorphic_allocator<T>` over a hand-rolled allocator
  template parameter: type-erased allocators are exactly the
  Open/Closed boundary that lets a single `std::pmr::vector<T>` type
  accept any backing resource (frame arena, ceiling allocator, test
  fixture allocator) without the container's instantiation count
  exploding.

### 2. std::ranges adoption posture

`std::ranges` (C++20) is the boundary-iteration vocabulary for engine
code. Concrete rules (one per "shape" so authors can grep this section
when writing a new function):

**R1 — Public-boundary signatures take ranges, not iterator pairs.**

```cpp
// Preferred — accept any contiguous-range of T:
glibre::Result<void> register_systems(std::ranges::input_range auto&& systems);

// Acceptable when contiguous storage is required (e.g. handed to Metal):
glibre::Result<void> upload_indices(std::span<const uint32_t> indices);

// Forbidden — iterator pairs leak STL implementation choices:
//   void register_systems(SystemPtr* begin, SystemPtr* end);
```

`std::span<const T>` is the preferred shape for "contiguous, no
allocation, no ownership transfer" boundaries (it is the most-restrictive
range concept; consumers can `std::ranges::input_range` over it
trivially). `std::ranges::range auto` / `std::ranges::input_range auto`
constrained-auto parameters are used when the function legitimately
accepts non-contiguous ranges (filtered views, `transform_view` chains).

**R2 — Pipe-syntax replaces hand-rolled for-loops at materialization
points.**

```cpp
// Preferred:
auto active_handles = entities
    | std::views::filter([](const Entity& e) { return e.alive(); })
    | std::views::transform(&Entity::handle)
    | std::ranges::to<std::pmr::vector<EntityHandle>>(allocator_resource_);

// Forbidden — manual loop where a pipe expresses the same intent:
//   std::pmr::vector<EntityHandle> active_handles{&allocator_resource_};
//   active_handles.reserve(entities.size());
//   for (const auto& e : entities) {
//       if (e.alive()) active_handles.push_back(e.handle());
//   }
```

The pipe form keeps the *intent* (filter, then project, then
materialise) one line per concept. `std::ranges::to<C>(args...)`
threads the PMR allocator into the materialised container so the
allocation lands under the calling context's tag (Rule #1).

**R3 — Hot-path carve-outs.** Pipes are not free. Two specific cases
keep the hand-rolled form:

- **Allocation-sensitive inner loops.** `std::ranges::to` always
  allocates. Inside the per-frame critical path (frame-phases.md
  phases 5, 6, 7), code that already owns a sized output buffer
  writes into it directly via `std::ranges::copy_if` / `std::ranges::transform`
  without an intermediate `to<>`. The cell-budget micro-benchmark in
  the owning context's SPEC §9 is the arbiter when the choice is
  ambiguous.
- **Lazy-evaluation cost cases.** `std::views::filter` + `transform`
  re-evaluates the predicate per consumer iteration. When the
  pipeline output is iterated *more than once*, materialise once
  with `std::ranges::to<std::pmr::vector<T>>(...)` and iterate the
  vector. When iterated once, keep the lazy view.

**R4 — Range algorithms over iterator algorithms.** `std::ranges::sort`,
`std::ranges::find`, `std::ranges::for_each` replace the pre-C++20
`std::sort(begin, end, ...)` form everywhere. Range algorithms accept
projections (`&Member`) which removes the lambda-over-getter noise
that plagued the EASTL-era code.

**R5 — `std::views::zip` / `enumerate` (C++23) are encouraged.** Both
ship in libc++ on the locked toolchain. `std::views::enumerate` removes
the canonical `for (size_t i = 0; i < v.size(); ++i)` boilerplate in
favour of `for (auto [i, x] : std::views::enumerate(v))` — preserves
range-for ergonomics, no manual index/end management.

The migration PLANs do not retroactively rewrite every loop into a
pipe. R1, R2, R4 apply to **new** boundary signatures and **new** call
sites authored after the migration lands; existing hand-rolled loops
are left alone unless the surrounding code is being touched for an
unrelated reason.

### 3. PMR allocator adapter — `PerContextAllocatorResource`

**Adapter name and home.** `glibre::PerContextAllocatorResource`,
declared in `core/include/glibre/alloc.hpp` next to `PerContextAllocator`
and `AllocatorHandle`. The adapter was first drafted on PR #1028
(`feat/core-type-registry`) co-located inside `TypeRegistry`; round-2
review MED-2 lifted it out (SRP: the adapter is owned by the allocator,
not the registry; any future per-context PMR consumer reuses it
without pulling `type_registry.hpp`). This record canonicalises the shape introduced via PR #1028 (merged
2026-05-10) — no further relocation is required.

**Why one `core/include/glibre/alloc.hpp` and not a sibling
`pmr_adapter.hpp`.** The adapter has zero state of its own beyond a
reference to a `PerContextAllocator`; splitting it into a sibling
header would force every PMR consumer to include two headers (`alloc.hpp`
for the allocator + `pmr_adapter.hpp` for the resource) when one
suffices. The cohesion-AND-completeness principle (PHILOSOPHY #2)
keeps both types in one header so that "include the header, get the
allocator AND its PMR adapter" is the single primitive.

**Public interface (canonical shape, mirroring PR #1028).**

```cpp
// core/include/glibre/alloc.hpp  (already on feat/core-type-registry; lift to main with this initiative)
namespace glibre {

class PerContextAllocatorResource final : public std::pmr::memory_resource {
public:
    explicit PerContextAllocatorResource(PerContextAllocator& alloc) noexcept
        : alloc_{alloc} {}

    PerContextAllocatorResource(const PerContextAllocatorResource&) = delete;
    PerContextAllocatorResource& operator=(const PerContextAllocatorResource&) = delete;
    PerContextAllocatorResource(PerContextAllocatorResource&&) = delete;
    PerContextAllocatorResource& operator=(PerContextAllocatorResource&&) = delete;
    ~PerContextAllocatorResource() noexcept override = default;

    // tag() — convenience accessor; forwards to the wrapped allocator.
    [[nodiscard]] ContextTag tag() const noexcept { return alloc_.tag(); }

protected:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override;
    void  do_deallocate(void* p, std::size_t bytes, std::size_t alignment) noexcept override;
    bool  do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

private:
    PerContextAllocator& alloc_;
};

}  // namespace glibre
```

**`do_allocate` — forward + abort on ceiling breach.**

```cpp
void* PerContextAllocatorResource::do_allocate(std::size_t bytes, std::size_t alignment) {
    auto r = alloc_.allocate(bytes, alignment);
    if (!r) {
        // Ceiling breach in GLIBRE_ALLOC_STRICT builds.  PMR contract
        // expects a valid pointer or a thrown exception; engine compiles
        // -fno-exceptions, so abort is the only correct option (matches
        // PerContextAllocator's OOM contract — alloc.hpp §-fno-exceptions).
        std::abort();
    }
    return *r;
}
```

The abort path is reachable only when `GLIBRE_ALLOC_STRICT=1` and a
PMR container's growth crosses the per-context ceiling. In shipping
builds (`GLIBRE_ALLOC_STRICT=0`) the soft-warn path inside
`PerContextAllocator::allocate` fires once per `ContextTag` and the
allocation succeeds, so PMR containers continue to grow past the
budget at the price of a logged warning (perf-budget.md
§Allocator Rules #3). Engine code MUST NOT catch the abort — there is
no recovery contract here, by design.

**Recoverability concession.** Adopting `std::pmr::*` containers via
`PerContextMemoryResource` collapses the recoverable-error surface from
`OutOfBudget` (typed via `std::expected<T, glibre::Error>`) down to
`std::abort()` — since `std::pmr::memory_resource::do_allocate` cannot
return failure without throwing (and the engine compiles
`-fno-exceptions`). The native `PerContextAllocator::allocate` API
preserves the `OutOfBudget` typed path; callers that need structured
recovery (e.g., graceful-degrade on a ceiling breach rather than
hard-abort) MUST stay on the native API instead of the PMR adapter.

Practical guidance: callers reaching for `std::pmr::*` containers
should pre-budget against the per-tag ceiling at construction time
(perf-budget.md §Allocator Rules #2 — hard ceiling in
diagnostic/debug builds). If a call site is recovery-critical or lives
on a hot path where ceiling breach is plausible and the failure must
surface as an `OutOfBudget` error rather than a process abort, keep
that site on `PerContextAllocator::allocate` directly; use
`PerContextAllocatorResource` only where the abort-on-breach contract
is acceptable (the common case — non-critical steady-state allocations
that are already pre-verified to stay within budget).

**`do_deallocate` — straight forward.**

```cpp
void PerContextAllocatorResource::do_deallocate(void* p, std::size_t bytes, std::size_t /*alignment*/) noexcept {
    alloc_.deallocate(p, bytes);
}
```

**`do_is_equal` — pointer-identity. This is the load-bearing
constraint.**

```cpp
bool PerContextAllocatorResource::do_is_equal(const std::pmr::memory_resource& other) const noexcept {
    return this == &other;
}
```

**Why pointer-identity, not "same wrapped allocator".** PMR containers
consult `do_is_equal` during move-assignment (and swap) to decide
whether to *steal* allocated storage from the source container or
*re-allocate* in the destination's resource. Storage transfer means
"the bytes that were allocated under resource A's accounting are now
considered to belong to resource B's accounting." If two different
`PerContextAllocatorResource` instances wrap two *different*
`PerContextAllocator` instances (different `ContextTag`s, different
ceilings), allowing storage transfer would silently move bytes from
tag A's accounting to tag B's accounting — a direct breach of
perf-budget.md §Allocator Rules #1 ("Per-context tag … the allocator
tracks live bytes per `ContextTag`"). Pointer-identity is the only
`do_is_equal` semantics that prevents this; it forces re-allocation
across context boundaries, keeping the per-tag accounting honest.

The accepted cost is move-elision *between distinct
`PerContextAllocatorResource` instances is disabled even when both wrap
the same underlying `PerContextAllocator`*. This is fine because
`PerContextAllocator` is non-copyable (Rule #1 makes it a long-lived
context singleton), so each context has exactly one
`PerContextAllocator` and downstream code that wants storage transfer
within that context simply reuses the same `PerContextAllocatorResource`
instance (hold it as a class member and pass `&mr_` to every PMR
container in the class — see PR #1028 TypeRegistry pattern).

**`ContextTag` threading.** The tag is reachable two ways from the
PMR adapter:

1. `PerContextAllocatorResource::tag()` — explicit accessor for
   diagnostics / structured logging. Forwards to `alloc_.tag()`.
2. Implicit — every byte allocated through the adapter is accounted
   under `alloc_.tag()` by `PerContextAllocator::allocate`'s atomic
   counter. PMR consumers never *supply* a tag; the tag is bound to
   the resource at construction time by which `PerContextAllocator`
   the resource wraps.

This matches the `AllocatorHandle` design (alloc.hpp §"Tag stamping"):
plugin code obtains a stamped allocator at registration time and never
supplies the tag per-call. The PMR adapter generalises the same
guarantee to `std::pmr::*` containers.

**Lifetime contract.** The wrapped `PerContextAllocator` MUST outlive
every `PerContextAllocatorResource` instance referencing it, which
MUST outlive every `std::pmr::*` container holding the resource
pointer. The standard pattern (PR #1028 TypeRegistry):

```cpp
class Foo {
    glibre::PerContextAllocatorResource mr_{*alloc_};  // declared first
    std::pmr::vector<T> items_{&mr_};                  // references mr_
};
```

Class-member declaration order enforces destruction order; PMR
containers destruct first, then the resource, then (transitively) the
allocator. The non-movable / non-copyable contract on
`PerContextAllocatorResource` makes the "declared first" requirement
checkable by code review and (eventually) by a clang-tidy rule.

**Transient-arena interplay.** The PMR adapter wraps `PerContextAllocator`,
which is the *ceiling-tracked heap* allocator. The per-context
`TransientArena` (perf-budget.md §Allocator Rules #4) is exempt from
the ceiling and lives under a different, dedicated PMR adapter
(`TransientArenaResource`, to be authored by the per-context migration
PLAN that touches per-frame transient sites). This record names the
seam; the transient adapter's shape is left to that PLAN because
neither its `do_is_equal` semantics nor its drain hook matters for the
per-EASTL-type matrix above.

### 4. `glibre::Error` — `eastl::variant` → `std::variant` migration

**Decision: thin alias, not wrapper class.**

```cpp
// core/include/glibre/error.hpp  (post-migration shape)
namespace glibre {

class Error {
public:
    using Variant = std::variant<
        core::Error,
        render::Error,
        tools::Error,
        shader::Error
        // physics::Error, data::Error, ... appended as each context lands
    >;

    template <class E>
        requires std::constructible_from<Variant, E>
    constexpr Error(E e, ErrorContext ctx = {}) noexcept
        : variant_{e}, ctx_{ctx} {}

    [[nodiscard]] constexpr const Variant& code() const noexcept { return variant_; }
    [[nodiscard]] constexpr const ErrorContext& where() const noexcept { return ctx_; }

private:
    Variant variant_;
    ErrorContext ctx_;
};

}  // namespace glibre
```

The `class Error` wrapper (already in tree) stays. Only the `Variant`
alias's underlying type changes from `eastl::variant<...>` to
`std::variant<...>`. **This is intentionally conservative** — wrapping
`std::variant` in a fresh `class Error` with a different surface would
ripple through every `glibre::Error::code()` caller, every
`std::visit` site, every structured-log emit; the wrapper class
already owns the `ErrorContext` attachment and the per-context arm
constraint, which is the only "wrapping" `glibre::Error` was ever
doing.

**SRP justification.** `class Error` exists to attach `ErrorContext`
and to constrain construction to per-context-enum arms via the
`requires std::constructible_from<Variant, E>` clause. That is one
responsibility: "an engine error is a per-context enum value plus a
where-was-it-raised payload." `std::variant` provides the tagged-union
mechanics. The wrapper class is the right level of abstraction;
adding a second wrapper layer would split that one responsibility
across two types (Liskov / SRP failure).

**Arm-count `static_assert`.** The current
`tests/core/error_register/error_register_test.cpp` and
`error_register.hpp` chain pin the arm count via
`eastl::variant_size_v<Variant> == kExpectedArmCount`. The migration
substitutes `std::variant_size_v`:

```cpp
// core/include/glibre/error_register.hpp  (post-migration)
static_assert(
    std::variant_size_v<glibre::Error::Variant> == kExpectedArmCount,
    "glibre::Error::Variant arm count drifted; update kExpectedArmCount and the per-context "
    "arm registration table when adding/removing a context's Error enum."
);
```

`kExpectedArmCount` and the per-context registration table do **not**
move; only the trait spelling changes. The assertion's intent (catch
silent arm-count drift between the variant alias and the
SPEC §10 registration) is preserved verbatim.

**`std::visit` and `std::get_if<T>` rules.**

- `std::visit` replaces `eastl::visit`. Pair with the canonical
  `Overloaded { ... }` helper for ad-hoc visitors:

  ```cpp
  template <class... Ts> struct Overloaded : Ts... { using Ts::operator()...; };
  template <class... Ts> Overloaded(Ts...) -> Overloaded<Ts...>;
  ```

  The helper lives in `core/include/glibre/overloaded.hpp` (new
  header, authored by the first migration PLAN that needs it). It is
  one struct + one deduction guide; SRP-clean, no other reason to
  change.

- `std::get_if<T>(&variant)` replaces `eastl::get_if<T>` and is the
  **only** indexed-access form permitted in engine code. `std::get<T>`
  throws `std::bad_variant_access` and is forbidden by the same lint
  that already forbids `new` / `malloc` (perf-budget.md §Allocator
  Rules — to be extended in the per-context migration PLAN that wires
  the lint).

- `std::holds_alternative<T>(variant)` replaces
  `eastl::holds_alternative<T>` for the test-side cheap-check pattern.

**Behavioural-difference audit (libc++ `std::variant` vs EASTL
`eastl::variant` on the locked toolchain):**

| Behaviour                                              | `eastl::variant`       | `std::variant` (libc++)         | Migration impact                                                                 |
|--------------------------------------------------------|------------------------|---------------------------------|----------------------------------------------------------------------------------|
| `get<T>` on wrong-arm                                  | throws (or asserts)    | throws `std::bad_variant_access`| Forbidden in engine code; lint enforces. No call sites to update.                |
| `get_if<T>` on wrong-arm                               | returns `nullptr`      | returns `nullptr`               | Identical; no migration impact.                                                  |
| Default-construction                                   | engages first arm      | engages first arm               | Identical.                                                                       |
| Valueless-by-exception (`variant_npos`)                | reachable on throw during emplace | unreachable in `-fno-exceptions` builds (no throwing emplace path) | No call sites in engine code rely on `variant_npos`; tests stay green.    |
| `std::visit` overload resolution                       | EASTL's is permissive  | libc++ requires exhaustive match (all `operator()` overloads must accept every arm) | Visitors using `Overloaded { ... }` already enumerate every arm; no migration impact. |
| `variant_size_v<V>`                                    | constexpr `size_t`     | constexpr `size_t`              | Identical; the arm-count `static_assert` migrates spelling-only.                 |
| Trivially-copyable arms → trivially-copyable variant   | yes                    | yes (libc++ ≥ 13)               | Identical; the per-context enum is `enum class : std::uint16_t` so all arms are trivially copyable. |

The only behavioural delta of any consequence is the
`valueless-by-exception` state. Engine code compiles with
`-fno-exceptions`, so `std::variant`'s assignment / emplace paths
cannot enter the valueless state (no throw → no rollback failure);
the state is unreachable in practice. Tests that probe the variant
state never call `valueless_by_exception()` today; the migration
sweep verifies no new uses appear.

**`std::expected<T, glibre::Error>` is unaffected.** `error-model.md`
§Decision #1 mandates `std::expected<T, glibre::Error>` at every
public boundary. `std::expected` is C++23, libc++ ships it on the
locked toolchain (already in use across the engine), and the alias

```cpp
template <class T> using Result = std::expected<T, Error>;
```

is unchanged by this migration. The two `static_assert`s in
`error.hpp` that pin `Result<int>` and `Result<void>` to the
`std::expected` shape remain verbatim; only the inner `glibre::Error`
implementation switches its `Variant` alias from EASTL to libc++.

**`ErrorContext` migration.** `ErrorContext` uses
`eastl::string_view` today; the migration substitutes
`std::string_view`. Both are trivially-copyable two-pointer values;
no call site needs adjustment beyond the include change
(`<EASTL/string_view.h>` → `<string_view>`).

**Per-context error enums are unaffected.** `core::Error`,
`render::Error`, `tools::Error`, `shader::Error` are
`enum class : std::uint16_t` declarations with no EASTL dependency.
The four `static_assert`s in `error.hpp` that pin the underlying type
remain verbatim.

## Rationale

- **Cheapest decomposition is "switch the substrate, keep every
  abstraction."** The matrix above changes one type per cell; the
  PMR adapter wraps the existing `PerContextAllocator`; the
  `glibre::Error` migration is a six-line edit to `error.hpp` plus a
  `static_assert` spelling change. No public-boundary signature
  changes (POD spans + handles per plugin-abi.md, `std::expected<T,
  glibre::Error>` per error-model.md). The SOLID Open/Closed
  principle is satisfied: every existing extension point (per-context
  enum, per-context allocator, per-context transient arena) is closed
  to modification and open to extension via the same primitives.

- **§11's five claimed advantages are now stdlib primitives.**
  Per-system arenas → `std::pmr::polymorphic_allocator<T>` +
  `PerContextAllocatorResource`. No-exceptions hot path →
  `-fno-exceptions` build flag + `std::get_if` discipline + lint.
  Frame / fixed / inline allocators → `TransientArena` +
  `std::pmr::monotonic_buffer_resource` + `std::array<T, N>` (and
  `std::inplace_vector` polyfill until libc++ ships it).
  Deterministic iteration → `std::pmr::flat_*` (C++23) keeps
  insertion-sorted order; `std::map` / `std::set` keep
  comparator-sorted order; `std::pmr::unordered_*` is the carve-out
  that already required the same discipline under EASTL.
  Debug instrumentation → libc++'s `_LIBCPP_DEBUG` / `_LIBCPP_HARDENING_*`
  modes provide per-container bounds checks, dangling-iterator
  detection, and incompatible-iterator detection in diagnostic
  builds; equivalent to EASTL's debug instrumentation, integrated
  with `std::*` everywhere.

- **Toolchain integration.** Every third-party library on the locked
  stack (Catch2, spdlog, fmt, SDL3, Jolt, FBX SDK, Fory, Slang,
  metal-cpp, FreeImage, HarfBuzz, meshoptimizer, Draco) speaks
  `std::*`. Removing EASTL collapses the translation layer at every
  test assertion (`REQUIRE(eastl::vector{...} == std::vector{...})`
  no longer requires custom Catch2 matchers), every `spdlog` format
  argument, every fmt `formatter` specialisation. The "two parallel
  container universes" problem dissolves.

- **C++26 features whose libc++ shipping status is partial.**
  `std::inplace_vector` (P0843), `std::function_ref` (P0792),
  `std::flat_*` partial-availability windows. The `compat/` polyfill
  pattern is the standard SOLID-clean way to handle stdlib lag: a
  header-only thin shim under `core/include/glibre/compat/<name>.hpp`
  whose only reason to change is the libc++ availability flip-day
  (delete the file, change the include in callers from
  `<glibre/compat/inplace_vector.hpp>` to `<inplace_vector>`). One
  header per polyfill keeps SRP clean.

- **PMR `do_is_equal` pointer-identity is the load-bearing constraint
  of this whole record.** Without it, `std::pmr::*` move-assignment
  would silently transfer bytes across `ContextTag` boundaries and
  break perf-budget.md §Allocator Rules #1. Pointer-identity is the
  smallest possible semantics that prevents this; it is also the
  semantics that PR #1028 already implemented and that round-2
  review accepted. This record canonicalises the choice rather than
  re-litigating it.

- **`std::ranges` posture is permissive at boundaries, opt-in inside
  hot loops.** The R1-R5 rules apply to *new* code authored after
  this record lands; existing hand-rolled loops are not retroactively
  rewritten. This is the "cohesion AND completeness" principle
  applied to migration scope: the cohesion of the new posture is
  preserved without forcing a completeness-bang that would balloon
  every per-context migration PLAN's scope past one session.

## Consequences

**Plugin authors (every per-context PLAN that touches plugin code):**

- Replace `#include <EASTL/...>` with the matching libc++ header
  (`<vector>` + `<memory_resource>`, `<string_view>`, `<variant>`,
  `<optional>`, `<span>`, `<memory>`, `<functional>`).
- Container instantiations switch from `eastl::vector<T> v(allocator)`
  to `std::pmr::vector<T> v{&memory_resource}`. The plugin obtains its
  `PerContextAllocatorResource&` from `PluginContext` (added by the
  per-context migration PLAN that touches `PluginContext`); the
  registration code wires the resource at `glibre_plugin_register`
  time, mirroring the existing `AllocatorHandle` path.
- Variant access uses `std::get_if<T>(&v)` exclusively; `eastl::get<T>`
  call sites are rewritten as `if (auto* p = std::get_if<T>(&v); p) { ... }`.
- `std::ranges` adoption is opt-in for existing code; new public
  boundaries follow R1 (range concepts or `std::span<const T>`).

**`core::Error` callers (every consumer of `glibre::Error`):**

- No call-site change. `glibre::Error::code()` still returns `const Variant&`;
  the variant's spelling changed but its `index()`,
  `holds_alternative<T>`, `visit`, and `get_if<T>` behaviours are
  identical to EASTL's on the supported (no-exception, well-formed)
  paths.
- `std::visit` over `glibre::Error::code()` requires the visitor to
  exhaustively cover every arm (libc++'s overload-resolution rule).
  Existing visitors that use the `Overloaded { ... }` helper already
  satisfy this; visitors that do not are flagged by the migration
  PLAN's compile pass.
- The arm-count `static_assert` in `error_register.hpp` flips from
  `eastl::variant_size_v` to `std::variant_size_v`; the
  `kExpectedArmCount` constant and its update protocol (bump when a
  new context lands) are unchanged.

**Test fixtures (every test that constructs containers or variants):**

- Catch2 `REQUIRE` / `REQUIRE_THAT` switch from EASTL's container
  comparison helpers to Catch2's built-in `Catch::Matchers::Equals`
  for `std::*` containers. No custom matchers required.
- Test-fixture allocations that needed an `eastl::allocator` instance
  now allocate from a test-local `std::pmr::monotonic_buffer_resource`
  or from the engine's `PerContextAllocatorResource` (depending on
  whether the test wants ceiling enforcement or not).

**Build system (`vcpkg.json`, `cmake/Compile.cmake`, every per-target
`CMakeLists.txt`):**

- `vcpkg.json` drops `"eastl"` from `dependencies`. The eastl pull is
  replaced by libc++ (already provided by the toolchain — no manifest
  change needed for libc++).
- `core/CMakeLists.txt` drops `find_package(EASTL CONFIG REQUIRED)`,
  drops `EASTL` from `target_link_libraries(... PUBLIC ...)`, drops
  `core/src/eastl_alloc.cpp` from sources (the EASTL `operator new[]`
  substrate file is no longer needed).
- `cmake/Compile.cmake`'s `glibre::compile_contract` interface target
  is unaffected — the `-fno-exceptions` / `-fno-rtti` /
  `-fvisibility=hidden` flags remain (they were never EASTL-specific).
- The `-Werror=switch` flag stays — its job is to enforce `to_string`
  / `tag_string` completeness across the per-context error enums,
  which are unchanged by this migration.

**Documentation:**

- `PHILOSOPHY.md` §11 receives a one-line "SUPERSEDED — see
  reviews/decisions/eastl-removal.md" pointer at the top of §11
  (this PR adds it). The §11 body is left untouched until the
  follow-up `[CHORE] update-philosophy-md-eastl-removal` lands and
  rewrites the section in full.
- `CLAUDE.md` Tech Stack row update is a separate `[CHORE]` per
  the initiative's child list; not in scope for this spike.
- Every per-context SPEC §11 (where it exists) that quotes EASTL
  types is updated by the migration PLAN that touches that context.

**Hot-reload contract (`hot-reload-protocol.md`):**

- No change. Hot-reload survives at the plugin-ABI surface, which is
  POD spans + handles regardless of whether engine internals use
  EASTL or libc++.

## Open Questions

1. **`std::function_ref` / inline-storage callable.** Engine has no
   measured hot-path callable that requires inline storage today.
   `eastl::fixed_function<N, Sig>` was used for "small callable, no
   heap allocation" but the use sites are below the perf-budget
   noise floor. Resolution: the migration PLAN that touches each
   `eastl::fixed_function` site picks `std::move_only_function<Sig>`
   by default and only opens an inline-storage polyfill if the
   per-context SPEC §9 micro-benchmark fires. If a polyfill is
   needed, it lives at `core/include/glibre/compat/inline_function.hpp`.

2. **`std::flat_map` / `std::flat_set` shipping status on the locked
   toolchain at PR-time.** libc++ 19 ships partial `<flat_map>` /
   `<flat_set>`; the locked toolchain version may be 19 or newer at
   the time each migration PLAN executes. Resolution: each migration
   PLAN that introduces a `std::pmr::flat_*` site verifies the
   include compiles in CI; if it does not, falls back to
   `std::pmr::unordered_*` and opens a follow-up PLAN to flip when
   libc++ catches up. The matrix row above documents this fallback.

3. **`TransientArenaResource` shape.** This record names the seam
   (a separate PMR adapter for per-context transient arenas, exempt
   from ceiling enforcement). Its `do_is_equal` semantics, its drain
   hook against frame-phases.md phase 9, and its leak-detection
   reporting are deferred to the per-context migration PLAN that
   first touches a transient-allocation site under a PMR container.

4. **Lint that forbids `std::get<T>` and raw `new` / `malloc` in
   engine code.** Today the project relies on code review for the
   `new` / `malloc` ban (perf-budget.md §Allocator Rules describes
   the rejection but the clang plugin is "decided in the
   implementation plan"). Resolution: the migration PLAN that
   removes the last EASTL include also wires a clang-tidy
   configuration (`bugprone-throwing-static-assertion` is unrelated;
   the right knobs are
   `cppcoreguidelines-no-malloc`,
   `bugprone-unchecked-optional-access`,
   plus a project-local `glibre-no-throwing-variant-access` matcher).
   The matcher's exact name is decided by that PLAN; this record
   names only the requirement.

5. **`PerContextAllocatorResource` thread safety.** The wrapped
   `PerContextAllocator` is thread-safe (atomic byte counter with
   `acq_rel` on the increment). `std::pmr::memory_resource` is
   itself thread-safe per the standard. The composition is
   thread-safe. Open question: do we need a `std::pmr::synchronized_pool_resource`
   layer for any context, or is the engine's per-context
   single-thread-of-execution model (frame-phases.md) sufficient?
   Resolution: defer until a measured contention case appears; the
   default answer is "no, because each context's per-frame work
   runs on the driver thread."

6. **Polyfill clean-up automation.** When libc++ ships
   `std::inplace_vector`, the corresponding
   `core/include/glibre/compat/inplace_vector.hpp` should be
   deleted and call sites updated. Resolution: the migration PLAN
   that introduces the polyfill also registers a CI tripwire (a
   simple `grep` job) that fails when `__cpp_lib_inplace_vector` is
   defined by libc++ but the polyfill header still exists. This is
   the SOLID-aligned way to keep compat/ from accumulating: each
   polyfill carries its own deletion gate.
