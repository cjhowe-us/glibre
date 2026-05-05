# data — Detailed Design: fory-codegen aggregate (`glibre-foryc`)

> Detailed design for the `Foryc` aggregate declared in
> `specs/data/SPEC.md` §4.2 — the host-only `glibre-foryc` codegen
> tool. Refines §4.2, §6.1.1, §6.2, §6.4, §6.5, §6.6, §7.1, §7.3,
> §10 of `specs/data/SPEC.md` in place; cites
> `reviews/decisions/fory-codegen.md`,
> `reviews/decisions/plugin-abi.md`,
> `reviews/decisions/hot-reload-protocol.md`,
> `reviews/decisions/error-model.md`,
> `reviews/decisions/frame-phases.md`, and
> `reviews/decisions/perf-budget.md`. Sibling design siblings on
> main: `specs/core/plugin-loader-design.md`,
> `specs/core/type-registry-design.md`.
> All conclusions re-derived; harmonius prior art
> (`harmonius/docs/requirements/core-runtime/serialization.md`) is
> cited as research input only — every clause re-derived against
> PHILOSOPHY and the data SPEC.

Refs: spike #732 — `[SPIKE] design-data-fory-codegen-detailed`.
Parent sub-epic governing the data-context detailed design pass.
Sibling task-breakdown spike `task-breakdown-data-fory-codegen-detailed`
is blocked-by this deliverable.

## 1. Purpose

`glibre-foryc` is the **host-only codegen executable** that closes
the build-time gap between `data/schemas/<ctx>/<Type>.fory` source
files and the C++ translation units that populate
`glibre-types.dylib`. Its single responsibility is **emission**:
parse `.fory` sources into the `Schema` aggregate (SPEC §4.1),
validate each `Schema` against §4.1 / §4.2 / §7.1, and write a
deterministic, byte-identical-across-hosts set of generated C++
artefacts comprising:

1. One header (`<glibre/types/<ctx>/<Type>.hpp>`) and one body
   (`<ctx>/<Type>.cpp`) per `.fory` file (SPEC §6.2 stage 4 #1).
2. One `_registry.cpp` aggregating every `RegistryEntry` in
   `FQN`-sorted order (SPEC §4.5, §6.2 #3).
3. One `_abi_hash.cpp` embedding the codegen-time-computed
   `glibre_types_abi_hash()` constant (SPEC §4.4, §6.4).
4. One `_manifest_<plugin>.cpp` per `plugins/*/plugin.fory`
   embedding the Fory-serialized `PluginManifest` blob into the
   plugin's `.rodata` (SPEC §6.5;
   `reviews/decisions/plugin-abi.md` §"Manifest source-of-truth").
5. One per-type `<Type>_migrations.hpp` companion exposing the
   `GLIBRE_REGISTER_MIGRATION` macros the originating context's
   migration TUs include (SPEC §5 §migration.hpp; SPEC §4.6).
6. The `AbiHashManifest` audit artefact (SPEC §7.2.3) recording
   every contributing schema's source hash + version, so reviewers
   can reproduce the ABI hash from sources.

What `glibre-foryc` **explicitly refuses to own**:

- **Runtime serdes mechanics.** `Envelope<T>::serialize` /
  `deserialize` bodies are emitted as trampolines into Apache
  Fory's encoder/decoder; the dispatcher composition itself
  (SPEC §4.7, §6.3) lives in `data/runtime/`. Sibling spike #736
  owns that aggregate.
- **The schema registry's runtime container.** `glibre-foryc`
  emits the static-init insert calls; `data/runtime/` owns the
  `SchemaRegistry::instance()` lookup surface. Sibling spike #730.
- **Migration policy.** `glibre-foryc` emits the dispatcher
  *hookup* (the `GLIBRE_REGISTER_MIGRATION` macro expansion and
  the `MigrationEntry` slot) but never the migration body — those
  live in the originating context's source tree per
  `reviews/decisions/fory-codegen.md` §"Migration Mechanic" and
  SPEC §4.6. Sibling spike #738 owns the policy decisions
  (invertibility, arena handling, idempotence).
- **The schema language semantics beyond the §7.1 grammar.**
  Adding a new `Builtin` to `glibre/types/_builtins.hpp` is a
  data-spec amendment, not a codegen change. `glibre-foryc`
  consumes the audited builtin set; it does not extend it.
- **Build-graph orchestration.** CMake wiring is locked by
  `reviews/decisions/fory-codegen.md` §"CMake Integration" and
  SPEC §6.6. `glibre-foryc` is invoked by the build; it does not
  invoke CMake.
- **Runtime presence.** The tool is not shipped at runtime; it
  contributes zero bytes to `glibre-types.dylib` and is excluded
  from the runtime perf budget (SPEC §9.2 row `Foryc`).

The SRP boundary is sharp: if the `.fory` grammar (§7.1), the
canonicalization rule (§7.3), the layout-additive rule
(SPEC §4.2 inv. 3), the reserved-tag rule (SPEC §4.1 inv. 3), or
the four-stage pipeline (SPEC §6.2) changes, this design changes.
Anything else is out of scope.

## 2. Requirements Coverage

Mapping of harmonius `core-runtime/serialization.md` requirements
to glibre disposition. Every entry is independently re-derived;
no requirement is silently dropped.

| Harmonius req                                                | Glibre disposition (MVP)        | Coverage site                                                                                       |
|--------------------------------------------------------------|---------------------------------|-----------------------------------------------------------------------------------------------------|
| Static codegen produces serializer/deserializer pairs        | **Covered** (re-derived)        | §3 emit pipeline; SPEC §6.2 stage 4; §4 below.                                                      |
| Schema versioning with monotonic integer + envelope          | **Covered**                     | SPEC §4.1 inv. 2, §4.8 inv. 5; codegen embeds version into envelope per §3.4 below.                 |
| Forward migrations as pure free functions                    | **Covered (emission half)**     | §3.5 dispatcher hookup; bodies owned by sibling #738 + originating contexts.                        |
| Tag-stable wire identity, reserved-tag enforcement           | **Covered**                     | §3.3 validator; §10 row `ReservedTagViolation`.                                                     |
| ABI hash gate + middleman dylib                              | **Covered**                     | §3.6 below; SPEC §4.4, §6.4; `reviews/decisions/fory-codegen.md` §"middleman dylib exposes".        |
| `rkyv`-style derive macros on every persistent type          | **Refused** (collapse)          | Glibre uses one Fory pipeline rather than per-type derives. SPEC §3.2 Occam collapse documents this.|
| Per-type runtime reflection / `Reflect` trait                | **Refused** (PHILOSOPHY §6)     | `ReflectionBlob` is *static codegen-emitted data*, never runtime reflection. SPEC §4.9 inv. 1.      |
| Cross-language schema parity (Python tooling reading bytes)  | **Out-of-scope (post-MVP)**     | `.fory` files are still readable by Apache Fory's reference impl, but glibre tooling is C++-only.   |
| Schema migration determinism across hosts                    | **Covered**                     | §3.4 emitter determinism rule; §4.2 inv. 1, §4.10 inv. 6.                                           |
| Build-time codegen, zero runtime reflection in shipping      | **Covered**                     | §3 four-stage pipeline; SPEC §4.9 inv. 5; PHILOSOPHY §6.                                            |
| Plugin manifest as Fory-serialized blob in `.rodata`         | **Covered**                     | §3.7 manifest emission; SPEC §6.5; `reviews/decisions/plugin-abi.md` §"Manifest source-of-truth".   |
| Hot-reload state preservation via migrate                    | **Covered (emission half)**     | §3.5 dispatcher hookup; runtime composition owned by sibling #736.                                  |

Coverage rule: every harmonius requirement either lands in this
design or is refused with a one-line rationale. The collapse
(N rkyv-derives → 1 Fory pipeline) is the load-bearing data-context
decision (SPEC §3.2); this design transcribes the **emitter half**
of that collapse.

Glibre-native requirements added beyond harmonius:

- **Determinism by codegen-binary identity** (PHILOSOPHY §7;
  SPEC §4.2 inv. 1). `glibre-foryc` is bit-deterministic: identical
  schema bytes plus identical tool binary produce byte-identical
  generated sources on every supported host. This is the load-
  bearing acceptance criterion for SPEC §11 story #364 (data/foryc:
  deterministic, host-stable codegen).
- **Layout-additive proof at codegen time** (SPEC §4.2 inv. 3).
  `glibre-foryc` compares each schema against its prior committed
  version's parsed form and refuses emission on a non-additive
  change. This collapses two failure modes (forgot-to-bump-version,
  accidental-reorder) into one build error.
- **Bootstrap rule respect** (SPEC §7.6). Meta-schemas under
  `data/schemas/meta/` are emitted with no migration providers
  (the §7.4 rule #5 coverage requirement is lifted for them) and
  with the same pipeline shape as domain schemas. `glibre-foryc`
  recognises the `meta/` prefix and routes accordingly.

## 3. Detailed Model

### 3.1 Aggregate composition

`glibre-foryc` is structured as a driver (`Foryc`) composing five
internal value objects, each with one reason to change. The driver
is the only aggregate root; the others are private to the tool.

```text
Foryc (driver, owned by main)
├── Lexer          (.fory bytes → token stream)             — §7.1 grammar
├── Parser         (tokens → Schema AST)                    — SPEC §4.1
├── Validator      (Schema → validated Schema)              — SPEC §4.2 / §7.4
├── Canonicalizer  (validated Schema → canonical bytes)     — SPEC §7.3
└── Emitter        (validated Schema set → C++ TUs)         — SPEC §6.2 stage 4
    ├── HeaderEmitter         (<ctx>/<Type>.hpp)
    ├── BodyEmitter           (<ctx>/<Type>.cpp)
    ├── RegistryEmitter       (_registry.cpp)
    ├── AbiHashEmitter        (_abi_hash.cpp)
    ├── ManifestEmitter       (_manifest_<plugin>.cpp)
    ├── ReflectionEmitter     (per-type ReflectionField arrays)
    └── AuditEmitter          (AbiHashManifest.fory.bin audit blob)
```

SRP note: the six `*Emitter` siblings (Header through Reflection) each
change only when the C++ emission format for their output type changes.
`AuditEmitter` changes for a structurally distinct reason: the
`AbiHashManifest` Fory schema (field additions, tag renumbering) or
Fory-encode parameters. The two reasons are kept separate by grouping
both under `Emitter` ownership while treating `AuditEmitter`'s
single axis of change as `AbiHashManifest` schema evolution, not C++
text format.

Translation-unit shape (illustrative, normative only insofar as
it names symbols the rest of the spec or sibling designs reference):

```
tools/glibre-foryc/
  include/glibre/foryc/             # internal headers; no public surface
    schema.hpp                      # Schema AST node (§3.2)
    diagnostic.hpp                  # one-arm diagnostic carrier (§3.8)
    options.hpp                     # CLI options struct (§3.10)
  src/
    main.cpp                        # arg parsing, exit-code mapping
    walker.cpp                      # deterministic file walk
    lexer.cpp
    parser.cpp
    validator.cpp
    canonicalize.cpp
    blake3_wrapper.cpp              # blake3 over canonical bytes
    emitter/
      header_emitter.cpp
      body_emitter.cpp
      registry_emitter.cpp
      reflection_emitter.cpp
      abi_hash_emitter.cpp
      manifest_emitter.cpp
      audit_emitter.cpp
      common.cpp                    # shared text writer + symbol formatter
    fory_runtime.cpp                # private linkage to Apache Fory's
                                    # serializer for the manifest blob
  CMakeLists.txt                    # links Apache Fory + blake3 privately
  tests/                            # Catch2 unit tests; see §11
```

The tool's own internals use `eastl::` containers (PHILOSOPHY §11)
inside an arena it owns; no global heap allocation past the arena.
Apache Fory and `blake3` are private link-time dependencies; they
do not leak past the executable's boundary (SPEC §6.1.1).

### 3.2 `Schema` AST shape

The in-memory representation of one `.fory` file. Mirrors SPEC §4.1
verbatim and is the input to every validator and emitter pass.

```cpp
// include/glibre/foryc/schema.hpp — internal, never installed.
namespace glibre::foryc {

struct TypeRef {
    enum class Kind : std::uint8_t { Builtin, Generated, List, Map, Option };
    Kind                    kind{Kind::Builtin};
    eastl::string_view      builtin{};      // when kind == Builtin
    eastl::string_view      fqn{};          // when kind == Generated
    const TypeRef*          inner{};        // List<inner> / Option<inner>
    const TypeRef*          map_value{};    // Map<builtin, value>
    eastl::string_view      map_key_builtin{}; // u8/u16/.../i64 only
};

struct DefaultExpr {
    enum class Kind : std::uint8_t { Number, Bool, String, Composite, None };
    Kind                            kind{Kind::None};
    eastl::string_view              text{};        // canonical-form text
    eastl::span<const DefaultExpr>  children{};    // composite { ... }
};

struct FieldClause {
    eastl::string_view  name{};
    TypeRef             type{};
    std::uint16_t       tag{0};
    std::uint32_t       since{0};
    bool                has_default{false};
    DefaultExpr         default_value{};
    SourceAnchor        anchor{};   // file:line:col for diagnostics
};

struct ReservedClause {
    std::uint16_t       tag{0};
    std::uint32_t       removed_in{0};   // 0 = unspecified
    eastl::string_view  comment{};
    SourceAnchor        anchor{};
};

struct MigrationClause {
    std::uint32_t       from_version{0};
    std::uint32_t       to_version{0};
    eastl::string_view  provider{};      // FQ C++ symbol name
    SourceAnchor        anchor{};
};

struct Schema {
    eastl::string_view              fqn{};
    std::uint32_t                   version{0};
    eastl::string_view              since_semver{};
    eastl::span<const FieldClause>     fields{};
    eastl::span<const ReservedClause>  reserved{};
    eastl::span<const MigrationClause> migrations{};
    eastl::string_view              source_path{};   // workspace-relative
    SourceAnchor                    anchor{};
    bool                            is_meta{false};  // §7.6 bootstrap
};

struct SourceAnchor {
    eastl::string_view file{};       // workspace-relative
    std::uint32_t      line{0};
    std::uint32_t      column{0};
};

}  // namespace glibre::foryc
```

Every span in `Schema` aliases an arena buffer the parser owns;
the AST is immutable post-parse and is read-only to validators
and emitters. The tool's arena lifetime spans one invocation of
`glibre-foryc`; nothing is retained between runs.

### 3.3 Pipeline stages

Re-stated in detail. SPEC §6.2 fixes the four-stage shape; this
section binds each stage's preconditions, postconditions, error
arms, and arena discipline.

#### Stage 1 — File walk + lex

**Trigger.** `Foryc::run()` after CLI parsing.

**Inputs.** The configured schema root (default `data/schemas/`)
and the configured output root (default
`${CMAKE_BINARY_DIR}/generated/glibre-types/`).

**Algorithm.**

1. Recursively enumerate every `*.fory` file under the schema
   root. The walker uses `std::filesystem::recursive_directory_
   iterator` and **sorts the result by canonical Unicode
   code-point order over the workspace-relative path** before
   any further processing — determinism (SPEC §4.2 inv. 5) does
   not depend on filesystem iteration order.
2. Recursively enumerate every `plugins/*/plugin.fory` under the
   workspace root the same way.
3. For each file, mmap the bytes (or fall back to read-into-arena
   on platforms where mmap is unsuitable; macOS supports it).
4. UTF-8 / NFC-normalize the bytes; reject any non-NFC input
   with `EncodingViolation` (an internal codegen-front-end arm
   that surfaces as `ReservedTagViolation` per SPEC §6.2 stage 1).
5. Lex into a token stream against the §7.1 grammar. Each token
   carries a `SourceAnchor`. The lexer is streaming and allocates
   only inside `Foryc`'s arena.

**Postcondition.** For every input file, an in-memory token
stream rooted at the file's first non-whitespace token. Files
are pinned in `FQN`-sorted order in the driver's worklist.

**Errors.** Lexer errors raise the codegen-front-end arm
`ReservedTagViolation` (SPEC §10 lifts this arm to cover the
codegen-time-only syntactic-error category) with the offending
file/line/column anchor. The build fails before any emit pass
runs.

#### Stage 2 — Parse

**Trigger.** Lex success for one file.

**Algorithm.** The parser is a single-pass recursive-descent
implementation of the §7.1 BNF. It produces one `Schema` value
per file. Cross-file information is **not consulted** at this
stage; the parser refuses to look outside the current file's
bytes.

**Postcondition.** One immutable `Schema` value per `.fory`
input file, all rooted in the driver arena, sorted by `fqn`
ascending in the driver's worklist.

**Errors.** Parser errors raise the same codegen-front-end arm
as the lexer (`ReservedTagViolation` per SPEC §10's coverage)
with the anchor pointing at the offending token.

#### Stage 3 — Validate

**Trigger.** All files parsed successfully.

**Per-schema invariants** (SPEC §4.1 invariants 1, 2, 4, 5;
SPEC §7.1 field-set rules 1–5):

1. `fqn` matches `[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+\.[A-Z][A-Za-z0-9]*`.
2. `version` is `> 0` and integer.
3. Every `tag` integer in the file is unique across active and
   reserved clauses.
4. `since` on every field satisfies `1 <= since <= version`.
5. A non-`option<T>` field whose `since > 1` carries a `default`
   clause; `option<T>` fields default to `none` implicitly.
6. Every `TypeRef` resolves to a `Builtin` from the audited set
   (`u8/u16/u32/u64/i8/i16/i32/i64/f32/f64/bool/string/bytes/
   vec3f/quatf/entity/list<T>/map<K,V>/option<T>`) or to another
   `Schema` in the current run.
7. `default` expressions are typed against their `TypeRef` per
   SPEC §7.1 rule 5.

**Cross-schema invariants** (SPEC §4.10 inv. 1, §4.5 inv. 1,
SPEC §4.1 inv. 4):

1. No two `Schema`s share a `fqn`. Duplicate → `SchemaRegistryConflict`
   (SPEC §10 row 5; codegen-time path of that arm).
2. Every `TypeRef::Generated` resolves to some `Schema` in the
   current run; otherwise → `SchemaUnknown` (SPEC §10 row 6;
   codegen-time variant of that arm — a build with a dangling
   reference fails before any source is emitted).
3. The type graph is acyclic. A cycle → `TypeRefCycle` (SPEC §10
   row 8 — the codegen-time path for type-graph cycles).

**Reserved-tag enforcement** (SPEC §4.1 inv. 3, §4.2 inv. 4).
The validator loads the **prior committed schema** from a
side-channel: the workspace's `data/schemas/_history/<fqn>.fory.history`
file (one history file per FQN, append-only, committed to the
repo). Each history entry records `(version, canonical_form_bytes)`
for every shipped version. The validator computes the prior
version's canonical form, parses it as a `Schema`, and compares
its tag set against the in-tree schema's tag set. Any tag number
present in the prior version's active or reserved set that is
re-used as an active tag in the new version raises
`ReservedTagViolation` with the offending tag number in the
diagnostic payload.

The history file is the single source of truth for "ever
shipped" tags; if it is absent the validator treats the in-tree
schema as the first version and skips the prior-comparison step.
Adding a new schema therefore creates the history file as a
side effect of the first emission (§3.5.2 below).

**Layout-additive rule** (SPEC §4.2 inv. 3). The validator
compares the tag-sorted offset table of the prior version
against the new version. A new tag is permitted only if its
sorted position appends past the prior version's last field's
offset. Any other change forces a `version` bump *and* a
`migration` clause covering `(prior_version → new_version)`.
The validator computes both offset tables (using the audited
builtin sizes from `glibre/types/_builtins.hpp`) and refuses
emission on a non-additive change with the codegen-front-end
arm `ReservedTagViolation` (SPEC §10's coverage of codegen-
time structural violations) plus a structured diagnostic
identifying the conflicting tag.

**Migration coverage** (SPEC §4.7 inv. 1; §7.4 rule 5). For
every schema with `version >= 2`, the union of `migration`
clauses must cover `1 → 2, 2 → 3, …, version-1 → version`
exactly. Missing any step → `MigrationStepMissing` (SPEC §10
row 7; codegen-time variant). Multi-step clauses (e.g.
`v1_to_v3`) are illegal and raise the same arm; only adjacent-
version steps are legal (§7.4 rule 1).

Meta-schemas under `data/schemas/meta/` lift this rule: their
chains may be empty regardless of `version` (SPEC §7.6 rule 2).
The validator routes `is_meta = true` schemas around the
coverage check.

**Postcondition.** Each `Schema` passes every invariant; the
validated schema set is FQN-sorted and ready for emission.

**Errors.** Any validator failure raises the matching
`data::Error` arm (per SPEC §10) with file/line/column anchor
and emits no output — the entire build is refused before any
generated file is written. This is the SPEC §4.10 inv. 1
biconditional enforcement point: the validator is the gate that
keeps the registry and the schema set in lock-step.

#### Stage 4 — Emit

**Trigger.** All schemas validated successfully.

**Algorithm.** The emitter writes the seven artefact families
listed in §1. Emission order is fixed and deterministic:

1. For each schema in FQN-sorted order, write the header and body
   (§3.4).
2. Write `_registry.cpp` aggregating every `RegistryEntry` (§3.5).
3. Write `_abi_hash.cpp` embedding the codegen-time-computed
   `AbiHash` (§3.6).
4. For each plugin in plugin-name-sorted order, write
   `_manifest_<plugin>.cpp` (§3.7).
5. For each schema in FQN-sorted order, write the per-type
   `<Type>_migrations.hpp` companion (§3.5.3).
6. Write the `AbiHashManifest.fory.bin` audit blob (§3.6.4).
7. Append/update each schema's `data/schemas/_history/<fqn>.fory.history`
   entry with the new `(version, canonical_form_bytes)` pair —
   only when the version is fresh (i.e. not already in the
   history file). This step is the side-effect that tracks the
   "ever shipped" tag set (§3.3 reserved-tag enforcement).
8. Touch the `.stamp` file the CMake graph depends on
   (`reviews/decisions/fory-codegen.md` §"CMake Integration").

The emitter never reads back its own outputs (SPEC §4.2 inv. 1).
It writes through a buffered text writer that flushes to disk
in one `pwrite` per file; partial writes on failure are detected
by the writer's checksum-on-flush and surface as `IOError`
(§3.8).

**Postcondition.** The output directory contains exactly the
artefact set listed in SPEC §6.1.3, and the history files are
up to date.

**Errors.** Emit failures (disk full, permission denied,
checksum mismatch on flush) raise the codegen-front-end arm
`IOError` (an internal arm that surfaces as a non-zero exit code
mapped to `core::Error::EmitError` when invoked from the editor —
§10 below). The driver removes any partially written file before
exiting so the build's next invocation re-runs from clean slate.

### 3.4 Generated header + body shape

For one schema `S` with FQN `glibre.<ctx>.<Type>` at version `V`,
the emitter writes:

#### Header `include/glibre/types/<ctx>/<Type>.hpp`

```cpp
// AUTO-GENERATED by glibre-foryc — do not edit.
// Source: data/schemas/<ctx>/<Type>.fory  (blake3 of canonical
//         form: <hex>; see _abi_hash.cpp for the AbiHash digest).
#pragma once

#include <EASTL/array.h>
#include <EASTL/span.h>
#include <EASTL/string_view.h>
#include <cstddef>
#include <cstdint>
#include <expected>

#include <glibre/types/identity.hpp>
#include <glibre/types/envelope.hpp>
#include <glibre/types/error.hpp>

namespace glibre::types::<ctx> {

// Tag-sorted layout (SPEC §4.2 inv. 2). Field order is
// independent of authoring order; offsets are content-addressable.
struct <Type> final {
    // Fields in tag-sorted ascending order. One member per active
    // tag. No vtable, no virtuals, no user-defined ctors beyond
    // = default. trivially-copyable when the field set permits.
    <type-of-tag-1>  <name-of-tag-1>{};
    <type-of-tag-2>  <name-of-tag-2>{};
    // ...
    static_assert(std::is_trivially_copyable_v<<Type>>
                  || /* contains list/map/string */ true,
                  "non-trivially-copyable schemas must opt in");

    constexpr bool operator==(const <Type>&) const noexcept = default;
    static constexpr ::glibre::types::SchemaId schema_id() noexcept;
    static constexpr ::glibre::types::SchemaVersion current_version() noexcept;
};

}  // namespace glibre::types::<ctx>

namespace glibre::types {

// Envelope<T> specialization: wires the typed wrappers onto the
// extern "C" trampolines emitted in the body (§3.4 below).
template <>
struct Envelope<::glibre::types::<ctx>::<Type>> {
    static auto serialize(
        const ::glibre::types::<ctx>::<Type>& value,
        std::span<std::byte> dst) noexcept
        -> std::expected<std::size_t, data::Error>;

    static auto deserialize(std::span<const std::byte> src) noexcept
        -> std::expected<::glibre::types::<ctx>::<Type>, data::Error>;
};

}  // namespace glibre::types
```

Notes:

- **Tag-sort.** The emitter sorts `S.fields` by ascending tag and
  emits the C++ members in that order, regardless of authoring
  order in the `.fory` file (SPEC §4.2 inv. 2). This is the load-
  bearing rule for ABI stability: two authors who disagree on
  field-declaration order produce identical C++ structs.
- **`final`, no virtuals, no user-ctors.** Enforced by the
  emitter's own template; non-conforming schema features are
  rejected at validate time.
- **`operator==`** is `= default` — the emitter does not generate
  a hand-written body. POD comparison is sufficient because the
  tag-sorted fields fully determine identity.
- **`schema_id()` / `current_version()`** are `constexpr` static
  members returning the FQN string view and `version` integer.
  Used by `Envelope<T>` and by tests; never branched on at
  runtime.
- **No reflection in the header.** The `ReflectionBlob` (§3.4
  below) is body-side data, gated by a build flag. The header is
  identical between editor and shipping builds.

#### Body `src/<ctx>/<Type>.cpp`

```cpp
// AUTO-GENERATED by glibre-foryc — do not edit.
#include <glibre/types/<ctx>/<Type>.hpp>
#include <glibre/types/registry.hpp>

#include <fory/serializer.hpp>   // Apache Fory; private to glibre-types.dylib

namespace glibre::types::<ctx> {

constexpr ::glibre::types::SchemaId
<Type>::schema_id() noexcept {
    return ::glibre::types::SchemaId{ "glibre.<ctx>.<Type>" };
}

constexpr ::glibre::types::SchemaVersion
<Type>::current_version() noexcept { return <V>; }

}  // namespace glibre::types::<ctx>

namespace {

// Per-tag Fory descriptor; built once at static-init.
constexpr eastl::array kFieldDescriptors{
    /* one entry per active tag, in tag-sorted order:
       (tag, name_view, offsetof(member), Fory type id, since)  */
};

// Optional reflection blob (§3.4.1). Compiled in only when
// GLIBRE_TYPES_REFLECTION is defined; the registry slot holds
// nullptr otherwise.
#if defined(GLIBRE_TYPES_REFLECTION)
constexpr eastl::array kReflectionFields{ /* ... */ };
constexpr ::glibre::types::ReflectionBlob kReflection{
    .schema  = ::glibre::types::SchemaId{"glibre.<ctx>.<Type>"},
    .version = <V>,
    .fields  = std::span(kReflectionFields),
};
#endif

}  // namespace

extern "C" std::expected<std::size_t, ::glibre::types::data::Error>
glibre_types_serialize_glibre_<ctx>_<Type>(
    const void* value, std::span<std::byte> dst) noexcept;

extern "C" std::expected<void, ::glibre::types::data::Error>
glibre_types_deserialize_glibre_<ctx>_<Type>(
    std::span<const std::byte> src, void* out_value) noexcept;

namespace glibre::types {

template <>
auto Envelope<::glibre::types::<ctx>::<Type>>::serialize(
    const ::glibre::types::<ctx>::<Type>& value,
    std::span<std::byte> dst) noexcept
    -> std::expected<std::size_t, data::Error>
{
    return ::glibre_types_serialize_glibre_<ctx>_<Type>(&value, dst);
}

template <>
auto Envelope<::glibre::types::<ctx>::<Type>>::deserialize(
    std::span<const std::byte> src) noexcept
    -> std::expected<::glibre::types::<ctx>::<Type>, data::Error>
{
    ::glibre::types::<ctx>::<Type> out;
    if (auto r = ::glibre_types_deserialize_glibre_<ctx>_<Type>(src, &out);
        !r) return std::unexpected(r.error());
    return out;
}

}  // namespace glibre::types

// Body of the extern "C" trampolines: they wrap Apache Fory's
// encoder/decoder (private to glibre-types.dylib) and translate
// Fory's exception-raising paths into data::Error arms.
// Implementation skeleton (one-arm-per-failure shape):
//
//   serialize:
//     1. Write EnvelopeHeader (FQN, V, payload_length=tbd, flags=0)
//        via the runtime envelope writer.
//     2. For each field in tag-sorted order, encode (tag, type-id,
//        bytes) via Fory.
//     3. Backpatch payload_length.
//     4. Return bytes_written.
//   deserialize:
//     1. Read EnvelopeHeader via the runtime envelope reader.
//     2. If header.fqn != schema_id().fqn → DeserializeError.
//     3. If header.version > <V> → DeserializeError (newer-than-host).
//     4. If header.version < <V> → defer to MigrationDispatcher
//        (sibling #738; this body returns its result unchanged).
//     5. Decode tag-by-tag; assemble *out.
```

The emitter's job stops at producing the skeleton above plus the
backed-by-Fory call sites; the actual Fory invocation lives in
`data/runtime/` and is owned by sibling spike #736
(`design-data-envelope-serdes-detailed`). This design pins only
the **shape** of the emitted code, not the Fory mechanics.

#### 3.4.1 Optional `ReflectionBlob`

`GLIBRE_TYPES_REFLECTION` is a compile-time flag, defined in
editor / tools build profiles, undefined in shipping. When
defined, the body emits a `kReflection` blob keyed by `(SchemaId,
version)` and the registry entry holds a non-null pointer. When
undefined, the registry entry's `reflection` slot is `nullptr`
(SPEC §4.9 inv. 5). The emitter encodes the same field metadata
either way; the linker drops the unused symbol from shipping
builds.

### 3.5 Registry, migration-hookup, history

#### 3.5.1 `_registry.cpp`

```cpp
// AUTO-GENERATED by glibre-foryc — do not edit.
#include <glibre/types/registry.hpp>

// One include per generated header, in FQN-sorted order:
#include <glibre/types/<ctx>/<Type>.hpp>
// ... etc

namespace glibre::types {

// Static-init entry point. Inserts every codegen-emitted
// RegistryEntry into the SchemaRegistry's static array, in
// FQN-sorted order. The array is contiguous and binary-searched
// at runtime (SPEC §4.5 inv. 3).
constexpr eastl::array kRegistryEntries{
    RegistryEntry{
        .schema       = ::glibre::types::<ctx>::<Type>::schema_id(),
        .version      = <V>,
        .source_hash  = SchemaSourceHash{ /* 32 bytes */ },
        .serialize    = &::glibre_types_serialize_glibre_<ctx>_<Type>,
        .deserialize  = &::glibre_types_deserialize_glibre_<ctx>_<Type>,
        .migrations   = std::span(kMigrationsFor_<ctx>_<Type>),
        .reflection   = /* nullptr or &kReflection */,
    },
    // ... one entry per schema, FQN-sorted ascending ...
};

const SchemaRegistry& SchemaRegistry::instance() noexcept {
    static const SchemaRegistry kInstance{};  // exposes kRegistryEntries
    return kInstance;
}

}  // namespace glibre::types
```

The emitter sorts entries by `fqn` byte order (SPEC §4.4 inv. 1
canonicalization) so binary search is contiguous and the entry
order matches the AbiHash input order. Insertion order is fixed:
two builds from identical schemas produce byte-identical
`_registry.cpp` content.

#### 3.5.2 Per-type `<Type>_migrations.hpp` companion

```cpp
// AUTO-GENERATED by glibre-foryc — do not edit.
// Companion header for the migration dispatcher hookups for
// glibre::types::<ctx>::<Type>. The originating context's
// migration TUs include this file to expand each
// GLIBRE_REGISTER_MIGRATION call into the appropriate
// MigrationEntry slot.
#pragma once

#include <glibre/types/migration.hpp>
#include <glibre/types/<ctx>/<Type>.hpp>

namespace glibre::types::<ctx> {

// One static-init MigrationEntry slot per declared (N → N+1)
// migration in the schema. The provider symbol is forward-
// declared here; the originating context links its definition
// at glibre-types.dylib link time. Missing symbol → link error,
// not runtime failure (SPEC §7.2.2 source_hash_to/from rule).
extern "C" auto migrate_<Type>_v<N>_to_v<Nplus1>(
    const <Type>_v<N>&  src,
    <Type>_v<Nplus1>&   dst,
    ::glibre::types::Arena& arena) noexcept
    -> std::expected<void, ::glibre::Error>;

}  // namespace glibre::types::<ctx>

// Convenience macro the originating context expands once per file.
// Expansion appends the entry to kMigrationsFor_<ctx>_<Type>.
#define GLIBRE_REGISTER_MIGRATION_<ctx>_<Type>_v<N>_to_v<Nplus1>() \
    /* expansion produces a MigrationEntry slot via static-init. */
```

The emitter generates one such header per schema with `version >= 2`.
Schemas at version 1 emit no companion (no migrations to register).
Each `MigrationEntry` slot is wired into the registry entry's
`migrations` span at static-init in ascending `from_version` order
(SPEC §4.7 inv. 1; SPEC §6.3 dispatcher invocation contract).

The legacy version-N struct types (`<Type>_v<N>` for `N <
current_version`) are themselves emitted as full headers under
`<glibre/types/<ctx>/_legacy/<Type>_v<N>.hpp>` so migration
authors have a typed input value to read from. The emitter
synthesizes them from the schema's history file (§3.5.3); they
share the same tag-sorted layout rules as the current version.

#### 3.5.3 History files (`data/schemas/_history/<fqn>.fory.history`)

The history file is the codegen-time equivalent of the
`AbiHashManifest` audit blob (SPEC §7.2.3): a committed-to-repo
record of every shipped version of the schema, used by the
validator to enforce the reserved-tag rule and the layout-additive
rule, and by the emitter to synthesize legacy-version structs.

Format (one record per version, append-only, LF-terminated):

```
# AUTO-GENERATED by glibre-foryc — append-only.
v<N>  blake3=<hex>  shipped=<iso8601-utc-date>
<canonical_form_bytes_of_v<N>, indented one space per line>
---
v<N+1> blake3=<hex>  shipped=<iso8601-utc-date>
...
```

The file is read by the validator (stage 3) and **appended to**
by the emitter (stage 4 step 7) only when the in-tree schema's
version is a fresh value relative to the file's last entry. Two
builds from identical schemas produce identical history files;
re-running `glibre-foryc` against an unchanged tree is a no-op
that does not touch the history.

The `shipped` field carries the date but is not an input to any
hash (SPEC §4.4 inv. 4 — `AbiHash` is a function of the contract,
not the build environment). It exists for human reviewers; the
codegen-time validator ignores it.

### 3.6 ABI-hash construction and embedding

#### 3.6.1 Computation

`AbiHashEmitter` consumes the validated `Schema` set produced by
stage 3 and produces the `_abi_hash.cpp` translation unit by:

1. For each `Schema s`, compute
   `schema_source_hash(s) = blake3(canonicalize(s))` where
   `canonicalize` is the parsed-form canonicalization defined in
   SPEC §7.3 (SPEC §4.4 inv. 2). The result is a 32-byte digest.
2. Sort the schemas by their `fqn` in canonical Unicode code-point
   order (SPEC §4.4 inv. 1).
3. Build the preimage by concatenating, for each schema `s` in
   sorted order, the entry
   `fqn_utf8(s) || ":" || version_le(s) || ":" || schema_source_hash(s)`
   where `fqn_utf8` is the FQN as UTF-8 bytes, `version_le` is the
   declared `version` integer as 4 bytes little-endian, and
   `schema_source_hash` is the 32-byte blake3 digest from step 1.
   Entries are separated by a single LF byte (`0x0A`); no trailing
   newline. This is the formula from
   `reviews/decisions/plugin-abi.md` §"ABI Hash Function" rule 1,
   which is the locked, normative definition.
4. Compute `blake3(preimage)` — the resulting 32-byte digest
   is the canonical `AbiHash`.
5. Hex-encode the digest (lowercase, 64 characters) and embed it
   as a `constexpr` `const char*` string literal in
   `_abi_hash.cpp`.

This is the same construction `reviews/decisions/fory-codegen.md`
§"middleman dylib exposes" and SPEC §6.4 specify. This design
adds: the canonicalization step (1) operates on the validator's
output, not the raw `.fory` bytes, so authoring whitespace and
comment placement do not perturb the hash. The collapse: two
authors who format the same schema differently produce the same
ABI hash.

**What is NOT in the hash** (SPEC §4.4 inv. 4): header
timestamps, compiler identity, optimization flags, `__DATE__` /
`__TIME__`, the `glibre-foryc` binary's own version, the
`shipped=...` field of the history file, the on-disk path of
the schema source. The hash is a property of the **contract**,
not the build environment. This is what makes two plugins built
on different machines compatible iff they share the same schema
set.

#### 3.6.2 Emission

```cpp
// AUTO-GENERATED by glibre-foryc — do not edit.
// blake3 over the FQN-sorted concatenation of every schema's
// canonical-form source hash. Stable across hosts; recomputed
// on every schema change.
#include <glibre/types/abi_hash.hpp>

namespace {
constexpr const char* kAbiHashHex =
    "<64-char-lowercase-hex-blake3-digest>";
}

extern "C" const char* glibre_types_abi_hash() noexcept {
    return kAbiHashHex;
}
```

The export is `noexcept`, allocates nothing, and is callable
from static-init contexts (SPEC §6.4). The body is the only
runtime presence of `AbiHash`; everything else is the linker's
job.

#### 3.6.3 Plugin-side re-export

`reviews/decisions/plugin-abi.md` §"ABI Hash Function" rule 3
makes plugins re-export the same string under
`glibre_plugin_abi_hash`. The emitter handles this in the
manifest emission step (§3.7) — every per-plugin `manifest.cpp`
also emits the plugin-side trampoline:

```cpp
extern "C" const char* glibre_plugin_abi_hash() noexcept {
    return ::glibre_types_abi_hash();
}
```

This is a pass-through re-export; the plugin's TU links against
`glibre-types.dylib` so the linker resolves the inner call to
the middleman's symbol. The plugin and the host therefore agree
on the hash by construction.

#### 3.6.4 `AbiHashManifest` audit blob

In addition to `_abi_hash.cpp`, the emitter writes a Fory-serialized
`AbiHashManifest` (SPEC §7.2.3) at `<output-root>/audit/AbiHashManifest.fory.bin`.
This is the build-time artefact reviewers consult to reproduce
the hash from sources without running `glibre-foryc`:

```text
AbiHashManifest {
  abi_hash_hex  : "<64-char-hex>",
  foryc_version : <SemVer of glibre-foryc>,
  entries       : [<one SchemaSourceRecord per Schema, FQN-sorted ascending>],
}
```

The blob is **not** linked into `glibre-types.dylib`; it lives
under a build-output `audit/` directory and is consumed by the
optional `glibre-foryc --verify-hash` mode (§3.10). The
`foryc_version` field is recorded for diagnostic reproducibility
but is not an input to the hash (SPEC §7.2.3 rule 2; SPEC §4.4
inv. 4).

### 3.7 Plugin manifest emission

For each `plugins/<plugin>/plugin.fory` discovered during the
file walk (§3.3 stage 1), the emitter writes one
`<output-root>/src/_manifest_<plugin>.cpp`:

```cpp
// AUTO-GENERATED by glibre-foryc — do not edit.
// Source: plugins/<plugin>/plugin.fory
#include <cstddef>

namespace {

alignas(std::byte) constinit
const std::byte kManifestBlob[] = {
    /* Fory-serialized PluginManifest bytes; one byte per
       array entry, comma-separated, 16 bytes per line. */
};

constexpr std::size_t kManifestSize = sizeof(kManifestBlob);

}  // namespace

extern "C" const std::byte* glibre_plugin_manifest() noexcept {
    return kManifestBlob;
}

extern "C" std::size_t glibre_plugin_manifest_size() noexcept {
    return kManifestSize;
}

// Definition emitted above (§3.6.3) in the same TU.
extern "C" const char* glibre_plugin_abi_hash() noexcept {
    return ::glibre_types_abi_hash();
}
```

The Fory-serialized blob is produced by linking Apache Fory
*privately* into `glibre-foryc` and feeding the parsed
`PluginManifest` value through Fory's encoder. The blob layout
is the wire format defined in `reviews/decisions/plugin-abi.md`
§"Plugin Manifest Schema" + SPEC §5 §plugin_manifest.hpp; the
emitter never hand-rolls bytes. The plugin's CMake target
compiles `_manifest_<plugin>.cpp` into the plugin dylib, and the
plugin loader (`reviews/decisions/plugin-abi.md` §"Loader Sequence"
step 3) reads the blob via the `glibre_plugin_manifest` symbol
and deserializes it through `glibre-types.dylib`.

The data context owns the *bytes*; the loader owns the *read
path*. This design owns only the emission of the bytes.

### 3.8 Internal diagnostic shape

`Foryc`'s internal diagnostics carry a single closed-sum value
that maps onto the SPEC §10 closed sum at exit time. The internal
arms are:

```cpp
// internal — never crosses the executable boundary except as
// an exit code.
namespace glibre::foryc {

enum class DiagnosticKind : std::uint16_t {
    EncodingViolation     = 1,  // → ReservedTagViolation (SPEC §10 row 4)
    LexError              = 2,  // → ReservedTagViolation
    ParseError            = 3,  // → ReservedTagViolation
    SchemaInvariant       = 4,  // → ReservedTagViolation / SchemaUnknown
    DuplicateFqn          = 5,  // → SchemaRegistryConflict (codegen-time)
    DanglingTypeRef       = 6,  // → SchemaUnknown (codegen-time)
    TypeGraphCycle        = 7,  // → TypeRefCycle (SPEC §10 arm 10, codegen-time — see §12)
    ReservedTagReuse      = 8,  // → ReservedTagViolation
    LayoutNonAdditive     = 9,  // → ReservedTagViolation (with structured detail)
    MigrationCoverageGap  = 10, // → MigrationStepMissing (codegen-time)
    MigrationCycle        = 11, // → MigrationCycle (codegen-time)
    EmitFailure           = 12, // → IOError (internal); → core::Error::EmitError externally
    HistoryFileCorrupt    = 13, // → ReservedTagViolation (validator surfacing)
};

struct Diagnostic {
    DiagnosticKind  kind{};
    SourceAnchor    anchor{};         // file:line:col
    eastl::string_view  detail{};     // human-readable; never load-bearing
    std::uint16_t   tag{0};           // ReservedTagReuse / LayoutNonAdditive
    eastl::string_view fqn{};         // affected FQN
    std::uint32_t   from_version{0};  // migration-coverage / cycle arms
    std::uint32_t   to_version{0};
};

}  // namespace glibre::foryc
```

The driver collects diagnostics into an `eastl::vector<Diagnostic>`
during stages 1–3; emission (stage 4) does not run when any
diagnostic is present. On exit, the driver writes one structured
log line per diagnostic to stderr (one `Diagnostic` per line,
JSON-encoded for tools consumption) and maps the diagnostic kind
onto the process exit code (§3.9).

The mapping `DiagnosticKind → data::Error` is fixed at the
boundary the executable surfaces — i.e. when a tool consumer (the
editor, `glibre-foryc --verify-hash`, or a future codegen API)
parses the structured output. The mapping is the single point at
which internal arms collapse onto the SPEC §10 closed sum, and
it is unit-tested against every arm (§11).

### 3.9 Exit-code mapping

`glibre-foryc` returns one of a small set of exit codes that map
onto the SPEC §10 / §3.8 arms. The codes are stable across patch
releases; CMake reads them via `add_custom_command`'s exit
status to decide whether to fail the build cleanly.

| Code | Meaning                                                                 | Internal arm(s)                              | SPEC §10 arm                |
|------|-------------------------------------------------------------------------|----------------------------------------------|------------------------------|
| 0    | Success — every input parsed, validated, and emitted.                  | (none)                                       | (none)                       |
| 1    | `ParseError` family — lex / parse / encoding diagnostic.               | EncodingViolation, LexError, ParseError      | ReservedTagViolation         |
| 2    | `SchemaInvariant` — single-schema validator failure.                   | SchemaInvariant                              | ReservedTagViolation / SchemaUnknown |
| 3    | `SchemaCycle` — cross-schema duplicate FQN, dangling type ref, or type-graph cycle. | DuplicateFqn, DanglingTypeRef, TypeGraphCycle | SchemaRegistryConflict / SchemaUnknown / TypeRefCycle |
| 4    | `VersionRegression` — reserved-tag reuse, layout non-additive, migration coverage gap or cycle. | ReservedTagReuse, LayoutNonAdditive, MigrationCoverageGap, MigrationCycle | ReservedTagViolation / MigrationStepMissing / MigrationCycle |
| 5    | `EmitError` — disk / permission / checksum failure during stage 4.     | EmitFailure                                  | (codegen-time only; surfaces externally as `core::Error::EmitError`)|
| 6    | `IOError` — stage-1 file walk failure (schema root missing, history file corrupt, etc.). | HistoryFileCorrupt + I/O variants  | (codegen-time only)          |
| 7    | `OptionsError` — CLI argument validation failure.                      | (none)                                       | (codegen-time only)          |

The driver writes the structured-diagnostic JSON to stderr before
exiting with the matching code. CMake's `add_custom_command`
fails the build when the exit code is non-zero; the editor's
codegen-on-save flow reads the diagnostics from stderr and maps
them onto `core::Error::EmitError` (§10 below) for surfacing in
the inspector.

### 3.10 CLI surface

`glibre-foryc` is a CLI tool, **not a library** — there is no
in-process API. Re-invocation is the only composition point.

```text
Usage: glibre-foryc [options]

Options:
  --in <dir>            Schema root (default: ./data/schemas).
  --plugins <dir>       Plugin manifests root (default: ./plugins).
  --out <dir>           Output root (default:
                        ${CMAKE_BINARY_DIR}/generated/glibre-types).
  --history <dir>       History root (default: ./data/schemas/_history).
  --stamp <file>        Stamp file path for CMake dependency tracking.
  --reflection          Emit ReflectionBlob entries (editor / tools build).
  --verify-hash         Read AbiHashManifest.fory.bin from --out and
                        assert it matches the hash recomputed from --in
                        sources. Exits 4 on mismatch, 0 on match.
                        Used by CI to detect drift.
  --update-history      Permit appending new versions to history files.
                        Default: refuse to update history (CI mode); only
                        the developer-facing build profile passes this.
  --diag-format <text|json>
                        Diagnostic output format on stderr (default: json).
  --version             Print glibre-foryc SemVer; exit 0.
  --help                Print usage; exit 0.
```

CLI invariants:

1. **No network I/O.** The tool never opens a socket and never
   resolves DNS (SPEC §4.2 inv. 5). CI sandboxes that block
   network egress are honored.
2. **No path escape.** Every file the tool opens is within
   `--in`, `--plugins`, `--history`, or `--out`. Symlinks
   pointing outside those roots are rejected with `IOError`.
3. **No environment dependence on the hash.** The tool reads no
   environment variables that influence the AbiHash. Logging
   verbosity (`GLIBRE_FORYC_LOG_LEVEL`) is the only env var the
   tool inspects, and it does not affect output bytes.
4. **Single-invocation determinism.** Two invocations with
   identical CLI arguments and identical inputs produce
   byte-identical outputs (SPEC §4.2 inv. 1). The `--stamp`
   file's timestamp is the only output that varies, and it is
   not consumed by the build for content.

`--verify-hash` is the load-bearing CI mode: it lets the gate
job recompute the hash from sources and reject a PR whose
committed `_abi_hash.cpp` disagrees with the schemas, catching
hand edits or stale outputs.

## 4. Public Surface

`glibre-foryc` is a CLI tool, not a library. Its **public
surface** is therefore the set of artefacts the tool *writes*,
not a C++ API consumers call into. Consumers (CMake, the editor,
tests) interact with the tool by:

1. Invoking the executable with the §3.10 CLI.
2. Reading the exit code (§3.9).
3. Consuming the JSON diagnostics on stderr (§3.8).
4. Linking the generated C++ TUs into `glibre-types.dylib`
   (`reviews/decisions/fory-codegen.md` §"CMake Integration").

The C++ types the emitter writes — `<Type>` structs,
`Envelope<T>` specializations, `RegistryEntry` array,
`glibre_types_abi_hash()` — are the **runtime** public surface
of `data`, owned by SPEC §5 verbatim. This design pins the
**emission shape** of those types; it does not redefine the
runtime contract.

### 4.1 Emitted-symbol contract

Per generated schema `S` with FQN `glibre.<ctx>.<Type>` at
version `V`, the emitter guarantees:

1. **`glibre::types::<ctx>::<Type>`** (struct) — `final`, no
   virtuals, tag-sorted layout, `is_trivially_copyable_v` when
   the field set permits, `= default` ctors, `operator==` `=
   default`. Two builds from identical schema bytes produce
   byte-identical struct layout (SPEC §4.2 inv. 1, 2).
2. **`glibre::types::<ctx>::<Type>::schema_id()`** (constexpr) —
   returns the `SchemaId` whose `fqn` is the schema's FQN.
3. **`glibre::types::<ctx>::<Type>::current_version()`** (constexpr) —
   returns `V`.
4. **`glibre::types::Envelope<T>::serialize` /
   `::deserialize`** (free template-specialization functions) —
   wrap the per-FQN extern-C trampolines and translate Apache
   Fory exceptions into `data::Error` arms.
5. **`glibre_types_serialize_glibre_<ctx>_<Type>` /
   `_deserialize_*`** (extern "C") — the C-stable trampolines
   per `reviews/decisions/fory-codegen.md` §"ABI Stability Rules"
   #4. Their signatures are stable across patch releases; adding
   a new `<fqn>` is additive (SPEC §4.3 inv. 2).
6. **Per-type `<Type>_migrations.hpp`** companion with
   `extern "C" auto migrate_<Type>_v<N>_to_v<Nplus1>(...)`
   forward declarations; bodies live in originating contexts.
7. **`_registry.cpp`** static-init array `kRegistryEntries`
   producing `SchemaRegistry::instance()` exactly as SPEC §5
   §registry.hpp specifies.
8. **`_abi_hash.cpp`** body of `glibre_types_abi_hash()`
   returning the codegen-embedded hex string (SPEC §6.4).
9. **`_manifest_<plugin>.cpp`** plugin-side `glibre_plugin_manifest`,
   `_manifest_size`, and `_abi_hash` exports (SPEC §6.5;
   `reviews/decisions/plugin-abi.md` §"Manifest source-of-truth").

### 4.2 Symbols the emitter NEVER produces

To make the SRP boundary executable in code review:

- **No `extern "C"` symbols beyond the four in
  `reviews/decisions/fory-codegen.md` §"ABI Stability Rules" #4
  (`glibre_types_abi_hash`, `glibre_types_serialize_<fqn>`,
  `glibre_types_deserialize_<fqn>`,
  `glibre_types_register_migration_<fqn>`)** plus the three
  plugin-side exports per `reviews/decisions/plugin-abi.md`
  §"Plugin file shape" (`glibre_plugin_abi_hash`,
  `glibre_plugin_manifest`, `glibre_plugin_manifest_size`). Any
  drift here is a codegen change subject to this design's
  amendment process.
- **No runtime reflection helpers** beyond the optional
  `ReflectionBlob` slot — no `for_each_field`, no `get_field_by_name`,
  no `Reflect` trait specializations (PHILOSOPHY §6).
- **No `std::*` containers in plugin ABI surfaces** (PHILOSOPHY
  §11 plugin-ABI rule). Plugin-side spans cross the boundary as
  `std::span<const std::byte>` only; no `std::vector`, no
  `eastl::vector` either.
- **No I/O calls in generated headers/bodies.** Generated code
  is pure data + pure functions; reading or writing files at
  runtime is forbidden.
- **No `migration` body emission.** The `<Type>_migrations.hpp`
  companion exposes only the macro hookup; the body lives in
  the originating context's source tree.

### 4.3 CLI exit-code stability

The exit codes in §3.9 are part of the tool's public contract.
Adding a new code is additive (must be > the highest existing);
removing or repurposing one is a SemVer-major bump on
`glibre-foryc`. CMake call sites and the editor's codegen-on-save
flow may rely on the table.

## 5. Hot/Cold Path Split

`glibre-foryc` is **build-time only**. It has no runtime presence;
zero bytes of the executable ship in `glibre-types.dylib`, in any
plugin, in the runtime, or in the editor binary. This makes the
tool a **pure cold path** in the engine's perf accounting.

| Path  | Trigger                              | Frequency                          | Budget                                        |
|-------|--------------------------------------|------------------------------------|------------------------------------------------|
| Cold  | CMake configure / build (regen)      | Once per `.fory` change            | Sub-second per schema file (§9 below)         |
| Cold  | Editor's "regenerate types" action   | Tens per session                   | Same                                          |
| Cold  | CI `--verify-hash` gate              | Every PR, every push to main       | < 200 ms over MVP-scale schema set            |
| Hot   | (none — tool is not in the runtime path) | n/a                            | n/a                                           |

Hot-path invariants for the tool itself: there are none, by
construction. The tool is single-shot: it parses, validates,
emits, exits.

The tool's **outputs** — the generated C++ TUs — *are* part of
the runtime hot path (every `Envelope<T>::deserialize` consults
the codegen-emitted dispatcher table). This design ensures the
emitted code is hot-path-friendly:

- **No virtual dispatch** in generated structs (SPEC §4.2 inv. 2).
- **No allocations** in generated serialize / deserialize
  bodies; the per-payload `Arena` (SPEC §4.6) is the only
  permitted allocation surface.
- **No exceptions** crossing generated function boundaries; the
  Apache-Fory wrapping in `data/runtime/` translates Fory's
  exception paths to `data::Error` arms before they reach the
  generated code (`reviews/decisions/error-model.md`
  §"Consequences" — exception-aware libs wrapped at first
  ingress).
- **No per-call `RegistryEntry` lookup**. Generated `Envelope<T>`
  specializations call the trampoline by name; the registry is
  consulted only at deserialize-with-unknown-FQN paths.

The cold-only nature of the tool is what makes its own
performance budget (§9) generous: a sub-second per-schema budget
is acceptable because no runtime frame depends on the tool's
latency.

## 6. Concurrency

`glibre-foryc` runs **single-threaded** by design. The four-stage
pipeline (lex → parse → validate → emit) is sequential, and the
tool's determinism (SPEC §4.2 inv. 1) requires byte-identical
output across runs regardless of host scheduling.

### 6.1 Why single-threaded

Three reasons collapse onto one decision:

1. **Determinism is byte-equal across hosts** (SPEC §4.2 inv. 1).
   A multi-threaded emit pass would have to fix iteration order
   over schemas anyway (FQN-sorted) and serialize writes to
   `_registry.cpp` and `_abi_hash.cpp` (single-file outputs); the
   parallel speedup is bounded by the sequential phases.
2. **Sub-second per-schema budget is unconstrained** (§9 below).
   With ≤ 100 schemas in MVP and < 5 ms per-schema lex+parse+
   validate cost on M1, the whole pipeline fits inside a single
   thread's budget. Adding concurrency would buy nothing.
3. **Single-threaded simplifies the diagnostic carrier**.
   `eastl::vector<Diagnostic>` requires no synchronization; the
   driver collects every diagnostic in source-encounter order
   and emits them deterministically.

### 6.2 What runs in parallel (build graph, not in-tool)

CMake parallelizes *across* `add_custom_command` invocations,
but `glibre-foryc` is a single command per build. The build's
parallelism comes from compiling the emitted C++ TUs in parallel
with other plugin TUs (PHILOSOPHY §11 EASTL containers in
runtime data; `data/runtime/` compiles alongside generated
sources). This is build-system territory, not tool territory.

If the per-schema budget ever grows (a future schema language
extension), introducing per-file parallelism inside the tool
would require:

- A merge step that re-imposes FQN-sorted order before the
  registry / abi-hash / manifest emitters run.
- A diagnostic-collection rendezvous that preserves source
  order in the structured-output stream.
- A CI gate verifying `glibre-foryc -j1 == glibre-foryc -jN` for
  bytes-out.

The current design records these requirements as a precondition
for a future amendment; MVP runs single-threaded and the gate
is moot.

### 6.3 Static-init ordering of generated code

The runtime side (the emitted TUs running inside `glibre-types.dylib`)
*does* care about static-init ordering — the registry must be
populated before any plugin's `glibre_plugin_register` runs (SPEC
§4.3 inv. 5). The emitter contributes to this guarantee by:

1. Naming every generated `kRegistryEntries` array `constexpr` —
   the array exists at translation-unit load time, not at first
   use.
2. Emitting the builtin-registration TU(s) lexicographically
   before generated-type TUs (`a/_builtins.cpp` < `b/...`), so
   the loader's lazy-link path encounters builtins first (SPEC
   §6.6 — "the emitter writes builtin-registration TUs
   lexicographically before generated-type TUs").
3. Avoiding any cross-TU static-init dependencies: the registry
   array is `constexpr`, the abi-hash string is `constexpr`, and
   the migration entries are `constexpr` slots populated by
   companion-header macros that the originating context's TU
   expands at its own static-init time.

Generated TUs therefore have **no order-dependent statics**;
they may be linked in any order without changing observable
behaviour.

## 7. Persistence + ABI

The emitter is the single producer of the data context's ABI
surface. This section pins what crosses the dylib boundary, how
the hash composition stabilizes that boundary, and how versioning
guarantees evolve over time.

### 7.1 The emitted artefacts ARE the ABI surface

`glibre-types.dylib`'s ABI is exactly:

1. The `extern "C"` symbol set the emitter writes (§4.1):
   `glibre_types_abi_hash`, per-FQN `glibre_types_serialize_*`,
   per-FQN `glibre_types_deserialize_*`, per-FQN
   `glibre_types_register_migration_*`. Plus three plugin-side
   exports (`glibre_plugin_abi_hash`, `glibre_plugin_manifest`,
   `glibre_plugin_manifest_size`) replicated into every plugin
   dylib by `_manifest_<plugin>.cpp`.
2. The struct layouts of every `glibre::types::<ctx>::<Type>` and
   meta-type. Layouts are fully determined by the schema set and
   the audited builtin sizes; no compiler / linker option
   perturbs them.
3. The `RegistryEntry` slot shape (SPEC §5 §registry.hpp). New
   fields are appended only (SPEC §4.3 inv. 2); reordering is a
   SONAME bump.

### 7.2 Hash composition (transcribed)

Normative definition: `reviews/decisions/plugin-abi.md`
§"ABI Hash Function" rule 1. The description below transcribes
that rule in full for local readability; the decision record is
the single authority in case of discrepancy.

For each schema in the scope set — every `data/schemas/**/*.fory`
source including `data/schemas/meta/*.fory` and
`data/schemas/core/PluginManifest.fory` (§7.3 of SPEC; §3.3
stage 3 of this design) — one entry is formed:

```
<fqn_utf8> ":" <version_le> ":" <schema_source_blake3>
```

where `fqn_utf8` is the FQN as UTF-8 bytes, `version_le` is the
declared `version` integer as 4 bytes little-endian, and
`schema_source_blake3` is the 32-byte blake3 digest of the
canonicalized source (§7.3 of SPEC; §4.4 inv. 2). Entries are
ordered by FQN in canonical Unicode code-point order and
separated by a single LF byte (`0x0A`); no trailing newline.
The outer `blake3` of the resulting preimage is the `AbiHash`.
The 32-byte digest is hex-encoded (lowercase, 64 chars) and
embedded by `_abi_hash.cpp` (§3.6).

Note: SPEC §4.4 inv. 1 currently reads as hash-only (no fqn or
version in the preimage). SPEC §4.4 inv. 1 is amended to match
this formula; the `reviews/decisions/plugin-abi.md` rule 1 is
the locked decision.

What is NOT in the hash (SPEC §4.4 inv. 4):

- Header timestamps, compiler identity, optimization flags,
  `__DATE__` / `__TIME__`.
- The `glibre-foryc` binary's own SemVer.
- The on-disk path of any schema source.
- The `shipped=...` field of the history file.

**Stability guarantees** (SPEC §4.4):

1. Two builds from byte-identical schema sets produce the same
   `AbiHash` regardless of host, compiler, or build flags.
2. Adding a new schema bumps the hash (canonical order changes).
3. Adding a new field at a new tag whose sorted offset appends
   past the prior version's last field bumps the hash and the
   schema's `version` (the field is ABI-additive, no migration
   required).
4. Removing a field bumps the hash, the schema's `version`, and
   forces a `migration` clause covering the bump (the layout-
   additive rule does not apply).
5. Renaming a field (without changing its tag or type) bumps
   the hash via the canonicalization step (§7.3 of SPEC) but
   does not bump `version` — names are part of the contract,
   tags are part of the wire.
6. Renaming an `FQN` is a *new schema*; the old FQN goes away,
   the new FQN appears, and the hash bumps. Plugins compiled
   against the old FQN must be rebuilt.

### 7.3 SONAME vs hash

`glibre-types.dylib`'s SONAME is bumped only on layout-breaking
changes (SPEC §4.3 inv. 3, `reviews/decisions/fory-codegen.md`
§"ABI Stability Rules" #5). Layout-breaking means: removing an
exported entry point, changing a generated struct's layout in a
non-additive way (which the validator refuses to emit anyway),
or changing the `RegistryEntry` slot shape.

Additive schema changes (new schema, new field append-past-end,
new migration entry) keep SONAME and bump only the embedded
`AbiHash`. The plugin loader's hash check
(`reviews/decisions/plugin-abi.md` §"Loader Sequence" step 4)
catches the latter; the dynamic linker's SONAME check catches
the former before the loader's step 5 ever runs.

### 7.4 Cross-context obligations

Every other context's plugin uses `glibre-foryc` as a black box:
it produces `glibre-types.dylib` and a per-plugin `manifest.cpp`,
and the plugin links the result. The cross-context obligations
this design imposes:

1. **Originating contexts ship `migrate_<Type>_vN_to_vN+1`
   bodies** for every schema-version bump on a type they own
   (SPEC §4.6, §7.4). The emitter generates the forward
   declaration and the `MigrationEntry` slot; the body is the
   originating context's job.
2. **Plugin authors author one `plugin.fory` per plugin** at the
   plugin's crate root (SPEC §6.5). The emitter discovers
   `plugins/*/plugin.fory` and emits the per-plugin manifest TU.
3. **CMake plugin targets compile `_manifest_<plugin>.cpp`**
   into the plugin dylib. The build template for plugins
   includes `${GLIBRE_GEN_DIR}/src/_manifest_${PLUGIN_NAME}.cpp`
   in the plugin's source list automatically.
4. **No plugin links Apache Fory or blake3 directly.** Both
   are private dependencies of `glibre-foryc` (build-time) and
   `glibre-types.dylib` (runtime). The plugin sees only the
   middleman's symbols (§7.1; SPEC §4.10 inv. 4).

## 8. Hot-Reload Integration

`glibre-foryc` is build-time only and does **not** participate in
the hot-reload state machine at frame 8. Its emitted artefacts
participate; the tool itself does not. This section pins what the
emitter must guarantee so the runtime hot-reload barrier
(`reviews/decisions/hot-reload-protocol.md`) and SPEC §8 hold.

### 8.1 What the emitter guarantees for hot-reload

1. **The `AbiHash` is a build-time constant** of the entire
   middleman (SPEC §4.4 inv. 4). Hot-reload at frame 8 in Mode A
   (per-plugin reload) does NOT change `glibre_types_abi_hash()`;
   plugins compiled against the same hash continue to load. Mode
   B (`glibre-types.dylib` self-reload, SPEC §8.3) is the only
   path where a new hash is published, and it requires every
   loaded plugin to re-pass the gate.
2. **The migration dispatcher hookup is static-init populated**
   (SPEC §4.7 inv. 5). When a plugin reloads, its TU re-runs
   static-init and re-registers the same `MigrationEntry` slots
   under the same `(FQN, from, to)` keys. The dispatcher table
   is therefore structurally identical pre- and post-swap; only
   the function pointers may change (pointing into the new
   plugin's `.text` segment).
3. **Generated struct layouts are stable across reloads** when
   the schema set is unchanged. A reload that does not bump any
   schema version is a vtable swap with no migrate work; SPEC §8
   §"Step 3 — Migrate" runs zero rows.
4. **Schema version bumps are codegen-time events.** A schema
   version bump triggers a full middleman rebuild (which bumps
   `glibre_types_abi_hash` and forces a SONAME bump if the
   bump is layout-breaking, per §7.3). The hot-reload protocol's
   §"Step 3 — Migrate" therefore observes a version bump only
   in Mode B, never in Mode A.

### 8.2 The emitter's role at the barrier

The emitter contributes nothing at the barrier. The barrier
calls into `data/runtime/`'s `migrate(...)` function (SPEC §8.2),
which dispatches through the codegen-emitted `MigrationEntry`
table. The emitter's contract is *prior to load*: it must have
emitted a complete chain (SPEC §4.7 inv. 1) for every schema with
`version >= 2`, or the build refuses (§3.3 stage 3).

The biconditional collapses cleanly: codegen guarantees coverage,
and the runtime never observes a partial chain. SPEC §8.4 (refusal
cases owned by data) lists `SchemaMigrationFailure` for *body*
failures only; coverage gaps are caught at codegen.

### 8.3 Editor's "live regen" flow (informational)

The editor may invoke `glibre-foryc` on save when the user edits a
`.fory` file. The flow:

1. Editor invokes `glibre-foryc` with the standard CLI.
2. On exit code 0, the editor signals CMake to rebuild the
   middleman dylib + every plugin dylib.
3. On rebuild success, the editor enqueues a hot-reload of every
   plugin via `HotReloadBarrier::request_reload`
   (`reviews/decisions/hot-reload-protocol.md` §"Test Hooks").
4. At the next phase 8, the barrier swaps every plugin in
   topological order; the new middleman is loaded as part of the
   Mode-B path *if* `glibre-types.dylib` itself was rebuilt with
   a layout-breaking change (SONAME bump). Otherwise, the
   middleman stays loaded and only the plugin dylibs swap.

This flow is **owned by the editor**, not by `glibre-foryc`. The
tool's only obligation is to exit 0 on success, non-zero with
structured diagnostics on failure; the editor handles the rest.

## 9. Performance

The tool is a **cold path with sub-second per-schema budget**.
This section pins the budget against the SPEC §9.2 row `Foryc`
(which records 0 cost in the runtime budget, host-tool cost
governed separately) and against the §11 acceptance criteria.

### 9.1 Budget

| Quantity                                   | Budget        | Source / rationale                                                  |
|--------------------------------------------|---------------|---------------------------------------------------------------------|
| Per-schema lex + parse + validate          | ≤ 5 ms        | Streaming lexer + recursive-descent parser; ≤ 100 fields/schema.    |
| Per-schema emit (header + body + companion)| ≤ 10 ms       | Buffered text writer; one `pwrite` per file.                        |
| `_registry.cpp` emission                   | ≤ 20 ms       | Single TU; one entry per schema; FQN-sorted insert.                 |
| `_abi_hash.cpp` emission                   | ≤ 50 ms       | One blake3 over canonical form per schema + one final blake3.       |
| `_manifest_<plugin>.cpp` emission          | ≤ 10 ms       | One Fory-encode of `PluginManifest` (≤ 64 fields total).            |
| Audit blob `AbiHashManifest.fory.bin`      | ≤ 5 ms        | One Fory-encode of FQN-sorted entries.                              |
| **Total cold run, MVP-scale (≤ 100 schemas, ≤ 16 plugins)** | **≤ 2 s** | Dominated by per-schema emit + registry walk; well under build budget. |
| Memory ceiling (tool process)              | ≤ 256 MiB     | Arena holds full schema set + token streams + emitted text buffers. |

These budgets are guidance for the implementation plan; the
load-bearing CI gate is `glibre-foryc --verify-hash` (§3.10),
which must complete in **≤ 200 ms over MVP-scale schemas** so
PRs feel snappy.

The runtime budget impact is zero. The tool's outputs influence
runtime budgets through:

- `SchemaRegistry` lookup cost (SPEC §9.2 row `SchemaRegistry`,
  ≤ 10 ns per lookup) — the emitter sorts entries to enable
  binary search.
- `Envelope<T>::deserialize` cost (SPEC §9.3 sample-scene S1
  budget, ≤ 0.10 ms per frame across all call sites) — the
  emitter's tag-sorted layout enables Fory's per-tag fast path.
- `MigrationDispatcher` cost (SPEC §9.4 #3, ≤ 50 us per call) —
  the emitter's contiguous `MigrationEntry` span enables index-
  by-`from_version - 1` access without sorting.

### 9.2 Build-graph considerations

`glibre-foryc` re-runs whenever any `.fory` file changes
(`CONFIGURE_DEPENDS` glob in `reviews/decisions/fory-codegen.md`
§"CMake Integration"). On a touched-but-unchanged file (e.g. a
whitespace edit that canonicalizes to identical bytes), the tool
detects no semantic change and emits byte-identical outputs —
Ninja then sees no downstream dirtying. The collapse: editing
formatting in a `.fory` file does NOT trigger a middleman
rebuild.

This is achieved by:

1. The validator's canonicalization step (§3.3 stage 3) producing
   identical canonical bytes for whitespace-only edits.
2. The emitter's atomic-write strategy: write to a temp file,
   compare byte-for-byte against the existing output, replace
   only on difference. CMake's `file(COMPARE)` semantics extend
   to the build target.
3. The `--stamp` file is touched only on actual content change,
   not on every invocation. (CMake's `add_custom_command` re-runs
   the tool on input change; the stamp file mediates downstream
   re-link decisions.)

### 9.3 Memory accounting

The tool's arena is a single `glibre::Arena`-equivalent with a
high-water mark capped at 256 MiB. All allocations during lex,
parse, validate, canonicalize, and emit go through it; the arena
is dropped at exit. There is no global heap call past the
arena's bootstrap.

This makes the tool's memory footprint a function of the schema
set's total size, bounded by the arena ceiling. CI sandbox
limits typically grant 4 GiB; 256 MiB leaves substantial
headroom for parallel test runs.

## 10. Failure Modes

`glibre-foryc`'s failure surface is the codegen-time subset of
the SPEC §10 closed sum, plus a small set of internal arms that
do not cross the executable boundary. This section enumerates
every arm the tool can raise, the detection point, the recovery
action, and the mapping onto SPEC §10 / `core::Error`.

### 10.1 Closed table (codegen-time)

| Internal kind (§3.8)        | Trigger                                                                                                  | Detection point                                          | SPEC §10 arm (when surfaced)            | Exit code (§3.9) | Recovery        | Severity |
|-----------------------------|----------------------------------------------------------------------------------------------------------|----------------------------------------------------------|------------------------------------------|-------------------|------------------|----------|
| `EncodingViolation`         | Input bytes are not UTF-8 / NFC.                                                                         | Stage 1 (lex).                                           | `ReservedTagViolation` (§10 row 4)       | 1                 | abort build      | info     |
| `LexError`                  | Lexer encountered an unexpected character or unterminated string.                                        | Stage 1 (lex).                                           | `ReservedTagViolation`                   | 1                 | abort build      | info     |
| `ParseError`                | Parser encountered a token that does not fit the §7.1 grammar (missing keyword, bad clause shape).       | Stage 2 (parse).                                         | `ReservedTagViolation`                   | 1                 | abort build      | info     |
| `SchemaInvariant`           | Single-schema invariant from SPEC §4.1 / §7.1 violated (FQN regex, version > 0, since ≤ version, default-on-non-option, etc.). | Stage 3 (validate, per-schema). | `ReservedTagViolation` / `SchemaUnknown` (depending on arm) | 2                 | abort build      | info     |
| `DuplicateFqn`              | Two `Schema`s share an `fqn`.                                                                            | Stage 3 (validate, cross-schema).                        | `SchemaRegistryConflict` (§10 row 5; codegen-time path) | 3      | abort build      | info     |
| `DanglingTypeRef`           | A `TypeRef::Generated` resolves to no `Schema` in the current run.                                       | Stage 3 (validate, cross-schema).                        | `SchemaUnknown` (codegen-time path)      | 3                 | abort build      | info     |
| `TypeGraphCycle`            | The schema-to-schema reference graph contains a cycle.                                                   | Stage 3 (validate, cross-schema).                        | `TypeRefCycle` (codegen-time, type-graph variant) | 3        | abort build      | info     |
| `ReservedTagReuse`          | A schema reuses a tag number that the prior committed version of the same FQN retired into reserved.    | Stage 3 (validate, history-comparison).                  | `ReservedTagViolation` (§10 row 4)       | 4                 | abort build      | info     |
| `LayoutNonAdditive`         | A schema introduces a tag whose sorted offset is not append-past-end of the prior version's last field. | Stage 3 (validate, history-comparison).                  | `ReservedTagViolation` (with structured detail) | 4         | abort build      | info     |
| `MigrationCoverageGap`      | A schema with `version >= 2` lacks a `migration vN_to_vN+1` clause for some N in `1..version-1`.         | Stage 3 (validate).                                      | `MigrationStepMissing` (§10 row 7; codegen-time path) | 4    | abort build      | info     |
| `MigrationCycle`            | A schema declares a `migration vN_to_vM` clause where `M ≤ N`, OR multi-step `vN_to_vM` where `M > N+1`. | Stage 3 (validate).                                      | `MigrationCycle` (§10 row 8; codegen-time path) | 4         | abort build      | info     |
| `EmitFailure`               | Disk full, permission denied, checksum mismatch on flush during stage 4 emission.                        | Stage 4 (emit).                                          | (codegen-time only; surfaces externally as `core::Error::EmitError` when invoked from editor) | 5 | abort build (clean up partial files first) | error |
| `HistoryFileCorrupt`        | A `data/schemas/_history/<fqn>.fory.history` file fails parse.                                           | Stage 1 (file walk) / Stage 3 (validate).                | (codegen-time only)                      | 6                 | abort build      | error    |
| `OptionsError`              | CLI argument validation failure (unknown option, conflicting flags, schema root not found).             | CLI parsing, before stage 1.                             | (codegen-time only)                      | 7                 | abort build      | error    |

Recovery vocabulary:

- **abort build** — `glibre-foryc` writes no output (or rolls
  back partial outputs for `EmitFailure`), exits non-zero, CMake
  fails the configure/build step. No runtime path can observe
  these arms — they are caught before any binary is produced.

### 10.2 Mapping into `core::Error` (when invoked from the editor)

When the editor invokes `glibre-foryc` (e.g. on user save of a
`.fory` file), the editor parses the JSON-on-stderr diagnostics
and surfaces them through its own error-handling path. The
mapping the editor uses:

| Tool exit code | Editor's `core::Error` mapping              | Editor surface                                                |
|----------------|---------------------------------------------|---------------------------------------------------------------|
| 0              | (success — no error)                        | Toast: "Types regenerated".                                  |
| 1              | `core::Error::EmitError` (subkind `Parse`)  | Inspector panel: parse diagnostics with file/line/col anchors.|
| 2              | `core::Error::EmitError` (subkind `Validate`) | Inspector panel: validator diagnostics + invariant cite.    |
| 3              | `core::Error::EmitError` (subkind `Cycle`)  | Inspector panel: cycle visualization with FQN chain.         |
| 4              | `core::Error::EmitError` (subkind `Version`) | Inspector panel: version-regression diagnostic + diff.      |
| 5              | `core::Error::EmitError` (subkind `IO`)     | Modal: "Codegen output failed; check disk space / perms".    |
| 6              | `core::Error::EmitError` (subkind `IO`)     | Modal: "History file corrupt; rebuild from sources".         |
| 7              | (editor bug — should not surface)           | Crash report: invalid CLI invocation.                        |

The `core::Error::EmitError` arm is **new** and added to
`core::Error` by this design. It carries the tool's exit code as
a payload (the `subkind` enum mirrors §3.9 categories) so editor
code can branch on the failure family without parsing the JSON.
Adding the arm follows `reviews/decisions/error-model.md`
§"Composition Rules" #5 (variant grows monotonically); it is
appended and never reordered.

For non-editor invocations (CMake, CI), the JSON-on-stderr is
the contract; the SPEC §10 arms are surfaced one per JSON
record, and CI consumers (e.g. `--verify-hash`) parse them
directly without going through `core::Error`.

### 10.3 Logging discipline

Per `reviews/decisions/error-model.md` §"Logging / Telemetry"
#1 every diagnostic is logged exactly once at the boundary that
*handles* it. The tool's handlers are:

- **The CMake build** — handles via the exit code; CMake itself
  prints the JSON-on-stderr to the build log.
- **The editor** — handles via the exit-code mapping above.
- **CI** — handles via `--verify-hash`'s parse-stderr path.

`glibre-foryc` itself never logs to `spdlog`; it is a host tool
and `spdlog` is a runtime dependency. Diagnostics are
JSON-on-stderr only.

### 10.4 Process abort cases

`glibre-foryc` does not call `std::abort` under any path. Every
internal arm maps to a non-zero exit code, the driver writes
diagnostics, and `main` returns. This makes the tool friendly
to crash-reporting harnesses (a process that exits 4 with valid
JSON is a clean-failure signal; a process that aborts is a bug
in the tool itself).

The arms `SchemaRegistryConflict` and `MigrationCycle` *do* abort
the engine at static-init in the runtime path (SPEC §10 row 5,
8), but those abort sites are inside `glibre-types.dylib`'s
runtime, not inside `glibre-foryc`. The codegen-time variants of
those arms exit with code 3 / 4 respectively.

## 11. Test Plan

The tool's tests sit under `tools/glibre-foryc/tests/` and mirror
the four-stage pipeline. Sibling plan #226 (`test(data): codegen
golden output harness`) and #227 (`test(data): schema migration
round-trip goldens`) own the harness implementation; this design
enumerates the per-stage acceptance criteria those plans deliver.

### 11.1 Unit tests (Catch2, `tools/glibre-foryc/tests/`)

Lex / parse / validate / emit each get one test file. The harness
is a single Catch2 binary that links `glibre-foryc`'s internals
as a library facade (only for tests; the production binary is
the executable).

| Test name                                          | Stage   | Drives arm                | Fixture                                                   |
|----------------------------------------------------|---------|---------------------------|-----------------------------------------------------------|
| `lexer.utf8_nfc_violation_returns_arm`             | 1       | `EncodingViolation`       | A schema source with non-NFC UTF-8 bytes.                |
| `lexer.unterminated_string_returns_arm`            | 1       | `LexError`                | A `since "0.1.0` (no closing quote).                     |
| `parser.missing_schema_keyword_returns_arm`        | 2       | `ParseError`              | A file whose first token is `field`, not `schema`.       |
| `parser.malformed_field_clause_returns_arm`        | 2       | `ParseError`              | `field foo : u32 tagX 1 since 1` (typo).                 |
| `validator.fqn_regex_returns_arm`                  | 3       | `SchemaInvariant`         | `schema GLIBRE.foo {...}` (uppercase context).           |
| `validator.duplicate_fqn_returns_arm`              | 3       | `DuplicateFqn`            | Two files declaring `glibre.core.Foo`.                   |
| `validator.dangling_typeref_returns_arm`           | 3       | `DanglingTypeRef`         | A schema referencing `glibre.x.NotShipped`.              |
| `validator.type_graph_cycle_returns_arm`           | 3       | `TypeGraphCycle`          | A → B, B → A.                                             |
| `validator.reserved_tag_reuse_returns_arm`         | 3       | `ReservedTagReuse`        | History file for `glibre.foo.Bar` records reserved tag 5; new schema reuses tag 5. |
| `validator.layout_non_additive_returns_arm`        | 3       | `LayoutNonAdditive`       | New tag 1 inserted before existing tag 2 (non-append).   |
| `validator.migration_coverage_gap_returns_arm`     | 3       | `MigrationCoverageGap`    | Schema at version 3 with only `v1_to_v2` clause.         |
| `validator.migration_cycle_returns_arm`            | 3       | `MigrationCycle`          | Schema with `migration v2_to_v1` clause.                 |
| `emitter.disk_full_returns_arm`                    | 4       | `EmitFailure`             | Output dir mounted on a tmpfs with `--quota=0`.          |
| `walker.history_file_corrupt_returns_arm`          | 1/3     | `HistoryFileCorrupt`      | A truncated `_history/<fqn>.fory.history` file.          |
| `cli.unknown_option_returns_arm`                   | pre-1   | `OptionsError`            | `glibre-foryc --bogus`.                                   |
| `canonicalize.whitespace_invariant_byte_equal`     | 3       | (positive)                | Two schemas differing only in whitespace produce identical canonical bytes. |
| `canonicalize.tag_order_invariant_byte_equal`      | 3       | (positive)                | Two schemas with reordered field declarations produce identical canonical bytes. |
| `abi_hash.deterministic_across_runs`               | 4       | (positive)                | Run `glibre-foryc` twice on the same input; assert byte-equal `_abi_hash.cpp`. |
| `abi_hash.deterministic_across_hosts`              | 4       | (positive, CI matrix)     | Same as above, but across macOS x M1 and macOS Intel runners. |
| `abi_hash.excludes_build_env`                      | 4       | (positive)                | Build same schemas under different `__DATE__` / `__TIME__`; assert hash unchanged. |
| `emitter.tag_sorted_layout`                        | 4       | (positive)                | Assert generated struct's `offsetof(field)` is tag-sort.|
| `emitter.fqn_sorted_registry`                      | 4       | (positive)                | Assert `kRegistryEntries` are FQN-sorted ascending.     |
| `emitter.no_global_heap_alloc`                     | 4       | (positive)                | Run under a custom allocator that aborts on `malloc`; assert no calls. |
| `cli.verify_hash_match_returns_zero`               | --      | (positive)                | `--verify-hash` against an audit blob produced by the same run. |
| `cli.verify_hash_mismatch_returns_four`            | --      | (positive)                | `--verify-hash` against a doctored audit blob; assert exit 4. |

Each test asserts both (a) the correct arm fires (or the correct
positive output appears) and (b) the structured-diagnostic JSON
field set matches the per-arm payload shape from §3.8.

### 11.2 Golden output harness (sibling plan #226)

Plan #226 (`test(data): codegen golden output harness`) delivers
the framework that:

1. Maintains a checked-in `tests/data/codegen-goldens/` tree
   with `expected/<ctx>/<Type>.hpp`, `expected/<ctx>/<Type>.cpp`,
   `expected/_registry.cpp`, `expected/_abi_hash.cpp`, and
   `expected/_manifest_<plugin>.cpp` files for a curated MVP
   schema set.
2. Runs `glibre-foryc` against the matching `tests/data/codegen-
   goldens/input/` schema sources.
3. Asserts each emitted file is byte-equal to its `expected/`
   counterpart.
4. Provides `--update-goldens` to regenerate the expected files
   when a deliberate codegen format change lands.
5. CI gate: golden mismatch fails the PR.

This is the load-bearing acceptance test for SPEC §11 stories
#363 (data/foryc: enforce tag-sort layout-additive rule) and
#364 (data/foryc: deterministic, host-stable codegen).

### 11.3 Migration round-trip goldens (sibling plan #227)

Plan #227 (`test(data): schema migration round-trip goldens`)
delivers the runtime-side counterpart: per-schema `vN.fory.bin`
golden payloads under `tests/data/schemas/<ctx>/<Type>/v<N>.fory.bin`,
plus a Catch2 harness that:

1. Reads each `vN` golden through `Envelope<T>::deserialize`.
2. Asserts the dispatcher walks the chain `vN → vN+1 → … → vM`.
3. Re-serializes the result.
4. Asserts the re-serialized bytes byte-equal the `vM` golden.

This is the load-bearing acceptance test for SPEC §11 stories
#367 (data/envelope: serialize/deserialize round-trip) and #369
(data/migration: per-schema round-trip golden harness). It tests
the runtime side of the codegen contract; the codegen-side
contract is tested by §11.1 and §11.2.

### 11.4 CI gates (`--verify-hash` + golden harness)

The data context's CI workflow runs three gates per PR:

1. **`glibre-foryc --verify-hash`** — recomputes the hash from
   sources and asserts it matches the committed `_abi_hash.cpp`.
   Catches hand edits and stale outputs. Budget: ≤ 200 ms over
   MVP-scale schemas (§9.1).
2. **Golden output diff** (#226) — runs `glibre-foryc` over the
   golden input set and diffs against expected. Budget: ≤ 1 s.
3. **Migration round-trip** (#227) — replays every per-version
   golden through the deserialize-with-migration path. Budget:
   ≤ 5 s for the MVP schema set (Catch2 `BENCHMARK` block under
   `data/runtime/test/perf/`).

All three gates are mandatory for PR merge.

### 11.5 Performance microbenchmarks

Catch2 `BENCHMARK` blocks under `tools/glibre-foryc/tests/perf/`:

- `bench.lex_parse_validate_one_schema` — assert ≤ 5 ms over a
  representative schema.
- `bench.full_pipeline_mvp_schemas` — assert ≤ 2 s over the
  full MVP schema set.
- `bench.verify_hash_mvp_schemas` — assert ≤ 200 ms.

CI gates per `reviews/decisions/perf-budget.md` §"CI Gate Spec"
#1: any benchmark exceeding its budget fails the PR. The tool's
benchmarks are part of the engine-wide CI matrix even though the
tool is host-only — slow codegen makes the build feel slow, which
breaks the developer experience PHILOSOPHY §1 implicitly defends.

## 12. Open Questions

- **[OPEN] History-file authoring discipline.** The reserved-tag
  enforcement (§3.3 stage 3) reads
  `data/schemas/_history/<fqn>.fory.history` as the source of
  truth for "ever shipped" tags. Discipline questions: (a) does
  the history file get a header / version of its own, or is it
  treated as a meta-schema bound by SPEC §7.6? (b) on schema
  rename, does the old FQN's history file get archived or
  deleted? Lean toward "history is data, treat as meta-schema"
  + "archive on rename"; resolve in the first plan that
  introduces a schema rename.

- **[OPEN] Emit-time canonicalization of float defaults.** SPEC
  §7.3 rule 5 mandates `std::to_chars` `chars_format::shortest`
  for float defaults. `std::to_chars` round-trips correctly
  across hosts, but the round-trip for denormals + NaN is
  unspecified. Decision: ban NaN / denormal defaults at the
  validator (§3.3); revisit if a real schema needs them. Tracked
  jointly with the determinism spike (cf.
  `reviews/decisions/fory-codegen.md` §"Open Questions" #5).

- **[OPEN] `glibre-foryc --update-goldens` mode shape.** Plan
  #226 owns the harness; the mode's exact CLI shape (flag vs
  env var vs sub-command) is TBD. Lean toward `--update-goldens`
  flag with an audit log; do not implement until #226 starts.

- **[OPEN] `core::Error::EmitError` payload shape.** §10.2 maps
  exit codes onto a `subkind` enum but does not specify the
  enum's full member list. Resolve in the first plan that lands
  the editor's regen-on-save flow; the design here reserves the
  arm but the payload is editor-context territory.

- **[OPEN] `glibre-foryc` SemVer cadence.** §3.6.4 records
  `foryc_version` in the audit blob for diagnostic
  reproducibility but does not pin the bump cadence. Lean
  toward "SemVer-major on emitted-symbol changes,
  SemVer-minor on diagnostic format changes,
  SemVer-patch on internal-refactors". Decide when the first
  emitter format change is proposed.

- **[OPEN] Cross-language schema parity** (harmonius §"Cross-
  language parity is opt-in" deferred per
  `reviews/decisions/fory-codegen.md` §"Consequences"). Future
  Python tooling reads `.fory` files via Apache Fory's reference
  impl; `glibre-foryc` does not currently publish the
  canonicalization rule in a Python-friendly form. Add a
  `tools/glibre-foryc-py/` shim if and when the first concrete
  consumer lands.

- **[OPEN] `TypeRefCycle` as a distinct SPEC §10 arm.** A
  type-reference cycle — where schema A directly or transitively
  references schema B and B references A — is structurally
  unrelated to a migration-chain cycle (`MigrationCycle`, §10
  row 8). The internal diagnostic arm `TypeGraphCycle` (§3.8)
  maps to a proposed `TypeRefCycle` arm (arm 10 in `ErrorTag`)
  that does not yet exist in SPEC §10. The first plan that
  implements the reference-cycle detector in stage 3 must:
  (a) append `TypeRefCycle = 10` to SPEC §10.1's `ErrorTag`
  enum as a codegen-time-only arm (trigger: `Foryc` stage 3;
  recovery: abort build; severity: error), and (b) update §10.2
  with the payload fields (`fqn_a`, `fqn_b` for the detected
  back-edge). This amendment requires a minor-version bump to
  SPEC (non-breaking: new arm appended).

- **[OPEN] Parallel emit (post-MVP).** §6.2 records the
  preconditions for moving to per-file parallelism inside the
  tool. MVP runs single-threaded; revisit if `bench.full_pipeline_
  mvp_schemas` exceeds its 2 s budget by >2x at MVP+1 schema
  scale.

- **[OPEN] Reflection-blob inclusion in editor builds.**
  `GLIBRE_TYPES_REFLECTION` flag (§3.4.1) is on for editor /
  tools, off for shipping. The remaining decision is *what*
  field metadata to emit — full names + types only, or also
  authoring-time attributes (`@deprecated`, `@experimental`).
  Defer until the editor inspector spec lands; the codegen
  surface is forward-compatible (new metadata is additive).
