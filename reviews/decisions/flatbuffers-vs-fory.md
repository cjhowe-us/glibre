# Decision: Flatbuffers Supersedes Apache Fory as Serialization Substrate

- **Status**: Accepted
- **Date**: 2026-05-11
- **Supersedes**: [`reviews/decisions/fory-codegen.md`](fory-codegen.md)
- **Author**: cjhowe (via go-design)
- **Refs**: parent sub-epic #5 (Persistence + plugin ABI), epic #2
  (cross-cutting foundation), PR #920 (Apache Fory overlay port — held),
  PR #961 (foryc ABI hash export — held), thinker analysis run
  `abf6e7f18120d0120`.
- **Owner contexts**: `data`, `core` (plugin loader).
- **Amends**: `PHILOSOPHY.md` §11, `CLAUDE.md` (tech-stack lock),
  `reviews/decisions/plugin-abi.md` §"Plugin Manifest Schema".

## Context

`fory-codegen.md` (the prior ADR, now superseded) chose **Apache Fory**
as glibre's serialization substrate. The rationale at the time hinged
on three properties: (1) compact tagged binary with explicit schema
evolution; (2) a code-generation path producing native POD-like C++
structs; (3) cross-language parity. The downstream design was an
in-tree codegen tool (`glibre-foryc`) feeding a middleman
`glibre-types.dylib` whose blake3 hash gates every plugin load.

Three facts surfaced during execution that the original ADR did not
anticipate:

1. **PR #920 is stuck red.** The Apache Fory vcpkg overlay port cannot
   build cleanly against our locked toolchain (clang ≥ 21, libc++
   on macOS 26 / Apple Silicon). Multiple iterations of the overlay
   port have failed CI; the upstream Fory C++ build pulls Abseil
   transitively, and the resulting dependency cascade is incompatible
   with glibre's pinned vcpkg manifest. The PR has been marked
   `do-not-merge` while we evaluate alternatives.
2. **Fory has no first-party vcpkg port.** As recorded in the prior
   ADR's "Apache Fory in vcpkg" note, microsoft/vcpkg ships no `fory`
   port; we own the overlay indefinitely. Maintaining an out-of-tree
   port is a sustained tax on every toolchain bump.
3. **Flatbuffers is now first-party in vcpkg, with end-to-end tooling
   that obsoletes most of `tools/foryc/`.** Specifically: (a)
   `flatc --cpp` emits the generated C++ headers we would otherwise
   hand-roll; (b) `flatc --conform <old.fbs>` machine-checks that a
   schema evolution is backward-compatible, which is exactly the
   "reserved-tag enforcement" the prior ADR flagged as an open
   question; (c) the `Verifier` API gives us bounded-time
   well-formedness checks against untrusted bytes; (d)
   `reflection.fbs` plus the `.bfbs` binary-schema format provide an
   editor-mode reflection blob without us writing a descriptor
   emitter; (e) Flatbuffers' offset-table layout is zero-copy on read,
   which directly answers PHILOSOPHY §7 (determinism, byte-equal
   snapshots) without any of the canonicalization plumbing the prior
   design pulled in.

Re-deriving from first principles: the original ADR's three properties
are also satisfied by Flatbuffers. Tagged binary with explicit
evolution → Flatbuffers' field IDs and `deprecated` annotation, plus
`flatc --conform`. Codegen producing native C++ → `flatc --cpp`.
Cross-language parity → Flatbuffers ships generators for ~15
languages; though glibre is C++23-only by PHILOSOPHY, the property
is preserved if future tooling ever needs it. The additional properties
Fory was supposed to provide (custom tagged binary, runtime
introspection through Fory's reflection API) were never load-bearing
in the glibre design — we forbade runtime reflection in shipping
builds (PHILOSOPHY §6) and the introspection use case is editor-only,
which `.bfbs` covers more cheaply.

Concretely, switching substrates:

- Deletes roughly **2100 LOC** of `tools/foryc/` parser + emitter
  (replaced by a thin `flatc` driver wrapper, ~250 LOC).
- Drops the implicit Abseil transitive dependency that was failing PR
  #920's build matrix.
- Replaces our hand-rolled descriptor emitter (`reflection-blob`
  design) with `flatc --bfbs` output and `flatbuffers::reflection::Schema`.
- Replaces our custom canonicalization module (input to schema-source
  hash) with `.bfbs` bytes — canonical by construction because `flatc`
  produces deterministic output.
- Replaces our envelope-wrapper design (`envelope-serdes-design.md`)
  with Flatbuffers' native size-prefixed buffer + `file_identifier`.

These are not optimizations layered on top of the prior decision;
they are the prior decision's primitives being replaced wholesale by
upstream primitives that already satisfy the same invariants.

## Decision

**Switch glibre's serialization substrate from Apache Fory to
Flatbuffers, and re-anchor the engine's offset-stable ABI invariant
on Flatbuffers' offset-table layout rather than on POD-only C-ABI
spans.** The downstream design points are resolved as follows.

| #   | Question                                                  | Resolution                                                                                                                                                                                                                                                                                                                                                                              |
| --- | --------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Q1  | Per-version `migrate_T_vN_to_vN+1` functions?             | **DROP.** Structural evolution only, enforced by `flatc --conform`. Two alias mechanisms: **field-rename aliases** (deprecate old field at original tag, add new field at next free tag; codegen emits both accessors with new aliasing old) AND **type-rename aliases** (SchemaRegistry holds forward-map of old-FQN → new-FQN; loader resolves deprecated FQNs transparently). Any change that is not append-or-deprecate-or-alias becomes a cook-time tool run, not a runtime migration. Issues #368, #492, #539 become **candidates for closure** via follow-up plan PRs that respect the story-closure rule (E2E green + manual PASS for stories; `closes #N` + dod-verify for plans/spikes). This ADR does **not** authorise direct `gh issue close` on these — closure is the verifier's verdict on the eventual cleanup PR, not the ADR's. Deletes `migration-dispatcher-design.md` (in a later PR). |
| Q2  | `map<K,V>` builtin replacement?                            | **`[Pair<K,V>]` table-of-pairs + codegen sugar.** Codegen emits `[Pair<K,V>]` schema; C++ accessor exposes a `map<K,V>`-shaped surface (thin span/view; no `std::map` allocation).                                                                                                                                                                                                       |
| Q3  | Codegen tool location + binary name?                       | Rename `tools/foryc/` → `tools/sergeant/`. Binary name: **`glbr-sergeant`**. *(Note: all other glibre tools use the `glibre-` prefix; this tool intentionally uses the shorter `glbr-` prefix for terser CLI use. The naming deviation is user-chosen; if the preference changes when `tools/sergeant/CMakeLists.txt` is authored, the rename is a one-line CMake change.)*                                                                                                                                                                                                                                                                                                             |
| Q4  | Envelope shape?                                           | **Flatbuffers native size-prefix + `file_identifier`.** 4-byte magic for type tag, size-prefixed buffer for length. The glibre version field moves *inside* the Flatbuffers table as `schema_version: uint32`. The wrapper struct `EnvelopeHeader { fqn, version, payload_length, flags }` is **deleted** as a separate wire envelope; the `flags` field is **not** dropped — it migrates inside the Flatbuffers root table as `flags: uint32 (id: 3)` alongside the version field. The forward-compat reservation in `specs/data/SPEC.md` §7.2.4 continues to apply at the table-field level rather than at the envelope level — new flags append at higher field IDs without invalidating the envelope shape.                                                                              |
| Q5  | Keep `EnvelopeTruncated` / `ReservedTagViolation` errors? | **Keep both.** `EnvelopeTruncated` for explicit truncation reporting (e.g. partial reads from disk); `ReservedTagViolation` for defense-in-depth against non-codegen-authored buffers, even though `flatc --conform` catches the common case at build time.                                                                                                                              |
| Q6  | C++ codegen — `flatc --cpp` direct, or hand-shaped POD?    | **Flip PHILOSOPHY §11.** Use `flatc --cpp` directly and Flatbuffers idioms (offset tables, `flatbuffers::Offset<T>`, `FlatBufferBuilder`, table-accessor pointers) at the plugin ABI surface. Plugins consume Flatbuffers-generated types directly. The §11 amendment explains why this is consistent with engine principles: zero-copy reads, deterministic offset layout, audited upstream library, and host-stable ABI is satisfied by Flatbuffers' offset stability rather than by `std::pmr::*` prohibition. |
| Q7  | `SchemaSourceHash` input?                                 | **`.bfbs` binary schema bytes.** Canonicalization module deleted; `.bfbs` is canonical by construction (`flatc` produces deterministic output).                                                                                                                                                                                                                                          |
| Q8  | `ReflectionBlob` shape?                                   | **Use `.bfbs` bytes directly + thin accessor.** `ReflectionBlob` becomes `std::span<const std::byte>` over the per-type `.bfbs` slice; editor uses `flatbuffers::reflection::Schema` from upstream. Hand-rolled descriptor emitter deleted.                                                                                                                                              |
| Q9  | `vec3f` / `quatf` — Flatbuffers `struct` or `table`?       | **`struct` (fixed-layout, byte-equal deterministic).** These are PHILOSOPHY §7 math primitives whose layout is stable across the industry; evolvability is not needed and `struct` is byte-equal across hosts.                                                                                                                                                                            |
| Q10 | `.bfbs` embed location?                                    | **Sidecar `.bfbs` files per-context manifest.** Build emits `<context>.bfbs` next to the context dylib; runtime loads only in editor mode. Shipping builds carry no `.bfbs` bytes. Honors PHILOSOPHY §6 (no runtime reflection in shipping builds) more strongly than embed-and-strip would.                                                                                              |

### Why each resolution re-derives from glibre primitives

- **Q1** collapses two responsibilities (decode + transform) into one
  (decode-with-alias-resolution). The prior design made `data` depend on
  every domain it serialized for, because each domain registered its
  own `migrate_T_vN_to_vN+1` body — a Liskov substitution violation
  hidden in the dispatcher table. Structural-only evolution + SchemaRegistry
  forward-maps restores SRP: `data` owns decode + alias, owning contexts
  own type semantics, and cook-time tools (not runtime) own one-shot
  semantic transforms. Per PHILOSOPHY §10 (Occam's razor), the
  per-version migration mechanism is replaced by the single primitive
  the schema language already provides (`deprecated`).
- **Q4** removes a duplicate. Flatbuffers' size-prefix + `file_identifier`
  already encode "type tag + payload length"; carrying our own
  `EnvelopeHeader` on top would be a second tag system for the same
  question. Two collapsing requirements become one primitive
  (PHILOSOPHY §10).
- **Q6** is the load-bearing flip. PHILOSOPHY §11's prohibition on
  Flatbuffers types at the plugin ABI boundary was a proxy for the
  real invariant: **host-stable offset layout**. The proxy was
  necessary when the only candidate type system was libc++'s
  `std::*` / `std::pmr::*`, whose layout depends on libc++ version
  and ABI flags. Flatbuffers' generated tables satisfy the underlying
  invariant directly — offset-table layout is part of the Flatbuffers
  binary format specification, not the C++ standard library
  implementation, so it is host-invariant by construction. We retain
  the prohibition on `std::*` / `std::pmr::*` containers at the
  boundary (the underlying reason for it is unchanged); we lift the
  prohibition on Flatbuffers types because they satisfy the same
  invariant via a different mechanism.
- **Q7/Q8/Q10** all consume `flatc`'s `.bfbs` output. The prior design
  had three separate hand-rolled emitters (canonicalization,
  descriptor blob, embed layout) doing the same work the upstream
  tool already does deterministically. Replacing three custom
  modules with one upstream artifact is SRP-positive and
  PHILOSOPHY §6-positive (no runtime reflection in shipping;
  `.bfbs` is editor-only by being a sidecar).
- **Q9** honors PHILOSOPHY §7. Flatbuffers `struct` is fixed-layout
  byte-equal across hosts; Flatbuffers `table` is offset-stable but
  not byte-equal (offsets to optional fields depend on which fields
  are present). For math primitives whose evolvability is not
  needed, `struct` is the strictly stronger choice.

## Consequences

### Positive

- **Performance**: zero-copy reads on every Flatbuffers buffer
  (offset-table accessors, no allocation, no copy). Fory required a
  decode pass before fields were addressable.
- **Fewer dependencies**: drop Apache Fory and its transitive Abseil
  cascade; Flatbuffers is a single header-mostly upstream port with
  no transitive C++ dependencies. CI matrix shrinks.
- **Smaller in-tree toolchain**: `tools/foryc/` shrinks from ~2100
  LOC to ~250 LOC (a thin `flatc` invocation wrapper, renamed
  `tools/sergeant/`).
- **Upstream tooling for hard problems**: `flatc --conform`
  machine-checks schema evolution; `Verifier` API gives us bounded
  well-formedness on untrusted bytes; `reflection.fbs` gives us
  editor-mode reflection without a descriptor emitter.
- **Simpler reflection story**: `.bfbs` sidecars are loaded only in
  editor mode; shipping builds carry no reflection bytes,
  reinforcing PHILOSOPHY §6.

### Negative

- **Golden corpus invalidation**: every `tests/data/schemas/*.fory`
  golden round-trip is wire-incompatible with Flatbuffers and must be
  regenerated. The regeneration is mechanical (re-cook the test
  fixtures through `glbr-sergeant`), but every PR touching the test
  corpus rebases through this.
- **Plugin ABI hash rebumps**: `glibre_types_abi_hash` is recomputed
  over `.bfbs` bytes instead of canonicalized `.fory` source. Every
  plugin in tree must rebuild and re-link against the new middleman
  before the first loader run after this swap merges. Mitigated by
  the plugin ABI gate doing exactly what it was designed to do —
  refuse stale plugins at load time with `PluginAbiHashMismatch`.
- **Semantic-change power loss**: dropping per-version migration
  functions (Q1) gives up the ability to express
  "field meaning changed" at runtime. Mitigated by two alias
  mechanisms (field-rename, type-rename) for the common cases, and
  by cook-time tools for the rare "actually transform values" case.
  Cook-time transforms are strictly better than runtime transforms
  for determinism (PHILOSOPHY §7) — they run once, produce a stable
  output corpus, and the runtime sees only the post-transform
  payload.

### Neutral

- **License**: Flatbuffers is Apache 2.0, identical to Apache Fory.
  No distribution change.
- **Multi-language story**: Flatbuffers supports ~15 language
  generators; glibre remains C++23-only per PHILOSOPHY, so this is
  latent capability, not a current requirement.
- **Determinism claim**: Fory's determinism claim was untested in
  our matrix; Flatbuffers' offset layout is part of its
  specification and is tested by upstream. Net: equal-or-better
  confidence, but not a step change.

## Implementation plan

Sequenced ten-step rollout. Each step is one or more PRs. No time
estimates per CLAUDE.md.

1. **This ADR** (`docs/adr-flatbuffers-vs-fory`). Authors the
   decision record, supersedes `fory-codegen.md`, amends
   `plugin-abi.md` §Plugin Manifest Schema, `PHILOSOPHY.md` §11, and
   `CLAUDE.md` tech-stack lock. No code changes.
2. **`specs/data/SPEC.md` amend.** Rewrite §§1, 2, 4.1–4.10, 7 to
   reference Flatbuffers primitives instead of Fory primitives. Map
   the 10 resolutions to spec sections.
3. **`specs/data/*-design.md` rewrites.** Replace
   `envelope-serdes-design.md`, `fory-codegen-design.md`,
   `middleman-dylib-design.md`, `reflection-blob-design.md`,
   `schema-registry-design.md` with Flatbuffers-anchored variants.
   Delete `migration-dispatcher-design.md`. Replace
   `data-error-design.md` to reflect retained-but-redocumented
   `EnvelopeTruncated` / `ReservedTagViolation` arms.
4. **Issue churn.** Step 4 happens in two phases.

   (a) **Bulk sweep — DONE 2026-05-11**: a single mechanical pass
   retitled and rewrote the bodies of every then-open issue carrying
   Fory/foryc tokens (~58 issues), each with a comment citing this ADR.
   Do **not** touch closed issues (several in the #218–#232 range are
   already closed with dod:verified or dod:failed and must not be
   destructively re-edited).

   (b) **Targeted disposition — pending**: per-issue close-vs-keep
   decisions for the migration-related stories #368 / #492 / #539 and
   any `dod:failed` reopens after this ADR merges. The full list of
   OPEN candidates is recomputed at Step-4 execution time via
   `gh issue list --state open --search "Fory OR foryc"` — it is NOT
   pinned in this ADR because it drifts with every planning pass.
   Disposition is the verifier's verdict on each follow-up PR that uses
   `closes #N`, NOT a direct `gh issue close` from the ADR. Each
   closure must follow the story-closure rule (E2E green + manual PASS
   for stories; `closes #N` + dod-verify for plans/spikes).
5. **vcpkg swap.** Replace `apache-fory` (overlay) with
   `flatbuffers` (first-party) in `vcpkg.json`; delete
   `vcpkg-overlay-ports/fory/`; refresh `vcpkg-configuration.json`.
   Close PR #920 with reference to this ADR.
6. **Tool rewrite.** Move `tools/foryc/` → `tools/sergeant/`. Replace
   the hand-rolled parser + emitter with a thin `flatc` driver
   (`glbr-sergeant`) that handles glibre-specific concerns:
   sidecar `.bfbs` emission, ABI hash computation over `.bfbs`,
   per-plugin `manifest.cpp` generation, field-rename / type-rename
   alias enforcement, FQN forward-map maintenance. Close PR #961.
7. **Middleman implementation.** Re-implement `glibre-types.dylib`
   to expose Flatbuffers-generated accessors, the
   `glibre_types_abi_hash` symbol over `.bfbs` bytes, and the
   SchemaRegistry forward-map for type-rename aliases.
8. **Per-context retargets (parallel).** Seven contexts currently
   carry Fory-anchored schemas (`core`, `scene`, `physics`, `render`,
   `audio`, `editor`, `script`). Each gets its own PR that swaps
   `.fory` files for `.fbs` schemas and regenerates goldens. These
   PRs can land in parallel after step 7.
9. **Golden regen.** Regenerate every `tests/data/schemas/*` and
   `tests/<ctx>/schemas/*` golden corpus. One PR per context;
   merges only after step 8's matching context PR.
10. **Cleanup.** Delete the deprecated paths: any remaining `.fory`
    files, the empty `tools/foryc/` shell if step 6 left one,
    `migration-dispatcher-design.md`, the canonicalization module.
    Update `plans/mvp.md` to reflect both the issue table (retitled
    IDs) and all prose-string occurrences of the old substrate:
    specifically, occurrences of "Apache Fory", "Fory schemas",
    "glibre-foryc" in `plans/mvp.md` (the executor can `grep -n
    'Fory\|foryc' plans/mvp.md` at run time to locate the exact
    lines — the list is not load-bearing in the ADR, but known sites
    include the data-layer description ~line 24 and the data-row
    ~line 49). This step is out of scope for the ADR PR itself and
    belongs in the step-10 cleanup PR.

PRs #920 and #961 remain `do-not-merge` during steps 1–6 and close
out at steps 5 and 6 respectively.

## Risks and mitigations

| Severity | Risk                                                                                                                  | Mitigation                                                                                                                                                                                                   |
| -------- | --------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| HIGH     | Every existing schema golden becomes wire-incompatible.                                                                | Step 9's golden regen is mechanical; each context PR regenerates its own corpus. No payload is hand-edited.                                                                                                  |
| HIGH     | Every plugin's compiled-in `glibre_plugin_abi_hash` is now stale.                                                       | Plugin ABI gate is doing what it was designed for: it refuses stale plugins with `PluginAbiHashMismatch`. Step 8 (per-context retargets) rebuilds every plugin in tree before the loader run.                |
| HIGH     | PRs #920 and #961 carry in-flight work that becomes wasted effort.                                                     | Both PRs were already `do-not-merge`-held pending this decision. The work was substrate-specific; switching substrate is the rational reason to discard it. Both close with reference to this ADR.            |
| MED      | MVP `data` schedule slips while the swap lands.                                                                       | Steps 5–7 are linear and small (vcpkg swap + thin `flatc` wrapper). Steps 8–9 parallelize across seven contexts. Net delta is one to two weeks at worst, within the MVP buffer.                              |
| MED      | Editor reflection regressions: `.bfbs` consumption differs from the hand-rolled descriptor blob.                       | `flatbuffers::reflection::Schema` is the upstream consumer for `.bfbs`; well-tested upstream. Editor reflection plan in `specs/editor/` adds Catch2 round-trips against a known `.bfbs` fixture.              |
| MED      | Dropping per-version migration functions regresses semantic-change capability.                                         | Two alias mechanisms (field-rename, type-rename) cover the common cases. Cook-time tool path absorbs the rare hard cases and is strictly better for determinism. Documented in the `data` SPEC §7 amend.     |
| MED      | Fory had hidden tag-skipping behavior we depended on without noticing.                                                 | Step 9's golden regen runs the full round-trip matrix; any silent behavior dependency surfaces as a golden diff. We re-derive the dependency intentionally or remove it.                                     |
| LOW      | Multi-language tooling loss.                                                                                          | Flatbuffers supports more languages than Fory (~15 vs ~4). This risk inverts.                                                                                                                                |
| LOW      | Some other internal module silently depends on Abseil through the Fory chain.                                          | Step 5's vcpkg lockfile diff surfaces any latent Abseil dependency. If anything inside glibre actually needed Abseil, we add it explicitly; otherwise the cascade drops.                                     |
| LOW      | `flatc` is unavailable in some CI environment.                                                                        | Flatbuffers vcpkg port installs `flatc` as a host tool; CI invokes it through the same vcpkg path that produces every other host tool (Slang, blake3). No special CI configuration.                          |

## Alternatives considered

**Cap'n Proto.** Zero-copy peer; comparable design. Rejected because
it has no first-party vcpkg port (microsoft/vcpkg ships `capnproto`,
but its CMake integration on macOS 26 / Apple Silicon is not as
well-trodden as Flatbuffers' upstream port). Cap'n Proto's RPC
features are unused by glibre; its community is smaller; its
codegen produces less ergonomic C++. Net: peer technology, weaker
on the tooling axis that motivated the Fory replacement.

**Protocol Buffers.** Dominant in industry. Rejected because
Protobuf has no zero-copy read story (every field is parsed into a
Message subclass), which would force a per-deserialize allocation
hit at every plugin boundary. The generated C++ ABI is also
considerably larger and forces an `arena` model that conflicts with
glibre's per-context allocator. The cross-language strength is not
load-bearing given C++23-only PHILOSOPHY.

**MessagePack.** Schemaless. Rejected on the schema-language axis:
without an IDL, every type system invariant becomes hand-maintained.
`flatc --conform` has no MessagePack equivalent. The original Fory
ADR rejected MessagePack for the same reason; nothing has changed.

**Keep Apache Fory and unblock PR #920.** Considered seriously
because the prior ADR is recent and the cost of substrate-swap is
real. Rejected because: (a) the overlay port remains an indefinite
maintenance burden (no upstream `fory` port in microsoft/vcpkg);
(b) the Abseil cascade keeps recurring on every toolchain bump;
(c) we would still hand-roll canonicalization, descriptor blob, and
envelope wrapper — three modules `.bfbs` eliminates; (d) Fory's
determinism claim is unaudited in our matrix while Flatbuffers' is
in its spec. The cost-of-staying exceeds the cost-of-switching.

## Open questions left for execution

The following small judgement calls are deferred to the executor
PRs. Each is a one-line resolution that the executor can make
without coming back to this ADR.

1. **`flatc --cpp-fp` argument.** Whether `glbr-sergeant` invokes
   `flatc` with `--cpp-fp scoped` (scoped enums) or
   `--cpp-fp unscoped`. Defer to step 6; default `--cpp-fp scoped`
   unless `flatc`'s output proves to conflict with our enum naming
   conventions.
2. **Build-time vs install-time `.bfbs` emission.** Whether sidecar
   `.bfbs` files are written into `${CMAKE_BINARY_DIR}` at build
   time and copied to the install tree, or emitted only at
   install-time via a custom command. Defer to step 7; lean toward
   build-time emission for IDE-friendliness.
3. **Retain or delete the C++ `Envelope<T>` wrapper.** Q4 deletes
   the wrapper *struct* (the on-wire envelope is Flatbuffers-native);
   the C++ source-level convenience wrapper (e.g.
   `Envelope<Transform>::serialize(...) -> std::span<const std::byte>`)
   may still be useful as a thin compile-time tag. Defer to step 7;
   keep if it compiles to a zero-overhead identity wrapper, delete
   otherwise.
4. **Field-rename alias accessor naming convention.** When a field
   is renamed (deprecate-and-add pattern), should the new accessor
   alias the old name, vice-versa, or expose both as distinct
   accessors? Defer to step 6; codegen ergonomics call. Default to
   exposing the new name as canonical and the old name as a
   `[[deprecated]]` thin forwarder.

## References

- Thinker analysis: agent run `abf6e7f18120d0120`.
- This PR: `docs(adr): Flatbuffers supersedes Apache Fory as
  serialization substrate` (branch `docs/adr-flatbuffers-vs-fory`).
- In-flight PRs being held: #920 (`pkg(data): Apache Fory vcpkg
  overlay port`), #961 (`feat(data): glibre-foryc — schema
  source-hash + ABI hash export`).
- Parent sub-epic: #5 (Persistence + plugin ABI).
- Superseded ADR: [`reviews/decisions/fory-codegen.md`](fory-codegen.md).
- Amended ADRs / policy docs:
  - [`reviews/decisions/plugin-abi.md`](plugin-abi.md) §Plugin
    Manifest Schema.
  - [`PHILOSOPHY.md`](../../PHILOSOPHY.md) §11.
  - [`CLAUDE.md`](../../CLAUDE.md) Tech Stack (locked).
