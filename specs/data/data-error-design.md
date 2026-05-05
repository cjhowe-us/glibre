# data — Detailed Design: data-error aggregate

> Detailed design for the closed sum `glibre::types::data::Error`
> declared in `specs/data/SPEC.md` §5 §error.hpp / §5.3 / §10. Refines
> those sections in place; cites
> `reviews/decisions/error-model.md`,
> `reviews/decisions/fory-codegen.md`,
> `reviews/decisions/plugin-abi.md`,
> `reviews/decisions/hot-reload-protocol.md`,
> `reviews/decisions/perf-budget.md`, and
> `reviews/decisions/frame-phases.md`. Style siblings on `main`:
> `specs/platform/platform-error-design.md`,
> `specs/data/schema-registry-design.md`,
> `specs/data/envelope-serdes-design.md`,
> `specs/data/middleman-dylib-design.md`. Introduces no new public
> surface beyond what SPEC §5 / §10 already enumerate; deviations from
> the cited records would require an amendment spike, not an in-place
> edit.
>
> Refs: spike #742 — `[SPIKE] design-data-data-error-detailed`.
> Parent #729. Sibling task-breakdown spike is blocked by this
> deliverable. All conclusions re-derived; harmonius prior art
> (`harmonius/docs/design/core-runtime/`,
> `harmonius/docs/requirements/data-systems/`) is research input
> only — re-derived against PHILOSOPHY, never ported.

## 1. Purpose

The data-error aggregate is the **single closed enumeration** that
unifies every failure surface the `data` context can raise across its
public API boundaries. Its one responsibility — the only reason it
changes — is **"a sibling data aggregate gained or lost a failure
surface"**: a new envelope refusal arrives from envelope-serdes, the
schema-registry adds a source-hash drift refusal, the migration
dispatcher learns to detect a chain back-edge, the middleman static
init wires a second validation pass. Concretely the aggregate owns:

1. The closed enum `glibre::types::data::ErrorTag` and the trivially
   copyable `glibre::types::data::Error` struct that carries it (SPEC
   §10.1; restated in §3 below). Adding / removing / renaming an arm
   is a deliberate central edit gated by SPEC §4.3 inv #3 (SONAME
   bump) plus a fresh `data::Error` schema version (§7).
2. The per-source translation guidance — for each sibling data
   aggregate (schema-registry #730, envelope-serdes #736,
   migration-dispatcher #738, fory-codegen runtime side #732,
   middleman-dylib #734), which `ErrorTag` arm fires under which
   trigger, which payload fields the arm populates, and which
   `core::Error` arm wraps it at the loader's call site (SPEC §10.2,
   §10.3).
3. The construction discipline that keeps `Error` off the success
   path: trivially copyable POD aggregate, no allocation in
   construction, no I/O, no virtual dispatch, no TLS write at the
   value's level (SPEC §10.4 logging discipline; PHILOSOPHY §6
   "zero runtime reflection").
4. The Fory schema for the *log/replay carrier* — a small,
   stable-on-the-wire `(tag, payload-fields-by-arm)` record that
   `glibre::log_error` and the e2e replay harness use to round-trip
   a `data::Error` through `spdlog`'s structured field set without
   losing arm identity (§7).
5. The arm-count invariant tying `ErrorTag` to the data-context ABI
   hash via the `DataErrorRecord` schema (§7.3): every arm-count
   bump rebuilds `glibre-types.dylib` and forces every plugin to
   re-link, the same gate that catches schema-shape drift
   (`reviews/decisions/plugin-abi.md` §"Versioning Rules").

What this aggregate explicitly **refuses to own**:

- **`core::Error`.** `PluginAbiHashMismatch`, `PluginInitFailed`,
  `SchemaMigrationFailed`, `HotReloadRefused`, `FramePhaseMisordered`,
  `OutOfBudget`, `PluginDlopenFailed`, `PluginMissingEntryPoint`,
  `PluginManifestInvalid`, `PluginEngineTooOld`, `PluginNameCollision`,
  `PluginDependencyMissing`, `SystemScheduleCycle` are owned by `core`
  per `reviews/decisions/error-model.md` Type Sketch, `core/SPEC.md`
  §10, and `reviews/decisions/plugin-abi.md` §"Failure Modes →
  core::Error". A data-side translator that fabricated a `core::Error`
  would violate composition rule 1 ("per-context enums are leaves;
  nothing nests another context's enum inside its own").
- **`platform::Error` / `render::Error` / `physics::Error` /
  any other context's enum.** Same rule. Each context owns its own
  closed sum and translates at the call site that crosses the
  boundary, never inward-facing.
- **The per-aggregate failure semantics.** Which arms
  `Envelope<T>::deserialize` may return, the recovery contract for
  `MigrationStepMissing` against an inbound payload from a
  decommissioned schema, and the static-init death-test discipline
  for `SchemaRegistryConflict` are owned by the sibling designs
  (`specs/data/envelope-serdes-design.md` §10,
  `specs/data/schema-registry-design.md` §10,
  `specs/data/middleman-dylib-design.md` §10). This aggregate
  enumerates the typed arm; the sibling decides when to raise it.
- **The `glibre::log_error` formatter.** Owned by `core`. Data
  contributes the structured payload shape (§7) and the arm-name
  table; core's helper does the spdlog dispatch.
- **Cross-arm retry / chain.** Each data aggregate constructs its own
  `Error` at the site the failure happens (SPEC §10.4); this aggregate
  does not auto-collapse one arm into another. A
  `MigrationStepMissing` and a `SchemaMigrationFailure` collapse onto
  one `core::Error::SchemaMigrationFailed` only at the loader's call
  site (`reviews/decisions/plugin-abi.md` §"Failure Modes" row 11),
  and that collapse lives in `core`'s code, not here.
- **The choice of `magic_enum` vs hand-written `to_string`** for
  arm-name logging. Owned by `core/error.hpp` per `error-model.md`
  Open Q #1; this design consumes whatever core decides without
  forking. The only data-side requirement is that the chosen approach
  round-trips every arm name (§7.4).
- **Domain `Error` enums on top of the spine.** Future game-framework
  plugins that author persistent types via `.fory` schemas may surface
  domain-side failures (`InventoryOverflow`, `EffectStackTooDeep`).
  Those are *that plugin's* enum, not `data::Error`. The spine
  guarantees byte round-trip and migration; domain semantics are out
  of scope (SPEC §1, §3.3).

The aggregate's SRP boundary is the sharpest in the data context: if a
sibling data aggregate gains a refusal it cannot collapse onto an
existing arm, this design changes (one new `ErrorTag` value, one new
row in §3.2, one new payload-fields cell in §3.3, one schema version
bump in §7). Anything else is out of scope.

## 2. Requirements coverage

Mapping of harmonius requirements consulted as research input
(`PHILOSOPHY.md` §"How harmonius is used" — re-derived, not ported)
onto the MVP coverage in this aggregate. Every entry is independently
re-derived; coverage does not imply harmonius's design was correct,
only that the underlying *requirement* survives the re-derivation.

| Harmonius source clause                                                                       | Re-derived MVP requirement                                                                                                            | Coverage in this aggregate                                                                                                                                  |
|-----------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `core-runtime/error-handling.md` (engine-wide typed errors, no exceptions across ABI)         | Every public data boundary returns `std::expected<T, data::Error>`; raw OS / library integers never cross the §5 surface.             | §3 closed enum + §4 public surface; §5 hot/cold split (success path never constructs an `Error`).                                                            |
| `data-systems/attributes-effects.md` (rkyv archive failure modes scattered across contexts)   | Refused: per-domain failure semantics live in their plugin contexts. The data spine surfaces only schema / migration / ABI failures. | §1 Refusals — domain-side error enums are out of scope; §3.2 covers only the spine's seven sibling-aggregate triggers.                                       |
| `data-systems/data-tables.md` (per-row constraint validation, FK resolution failures)         | Refused: the spine guarantees byte round-trip; domain validation is the consuming context's responsibility.                          | §1 Refusals; §3.2 has no `ConstraintViolation` arm.                                                                                                          |
| `data-systems/directed-graphs.md` (cycle detection, traversal failures)                       | Re-purposed: `MigrationCycle` is a *meta-cycle* in the migration chain itself, not a domain-graph cycle. Domain cycles stay in plugin code. | §3.2 row `MigrationCycle` — codegen-time / static-init only; runtime cannot observe.                                                                          |
| `core-runtime/plugin-abi.md` (refuse load on hash mismatch)                                   | The data-error aggregate types the refusal; `core` raises the `core::Error::PluginAbiHashMismatch` wrap.                              | §3.2 row `AbiHashMismatch`; §3.4 wrapping table.                                                                                                              |
| `core-runtime/hot-reload.md` (drain → swap → migrate → resume; migration failures are typed)  | The migration arms (`SchemaMigrationFailure`, `MigrationStepMissing`, `MigrationCycle`) cover every refusal mode the dispatcher raises. | §3.2 rows 2 / 7 / 8; §3.4 collapses 2 + 7 onto `core::Error::SchemaMigrationFailed` at the loader's call site.                                              |
| `data-systems/composition.md` (immutable definitions ↔ ECS components binding)                 | Refused: binding lives in `core` (ECS + plugin loader); `data` only types the schema-side failures.                                  | §1 Refusals — `core::Error::PluginInitFailed` is core's, not data's.                                                                                          |

Refusals routed elsewhere (research input from harmonius and from
sibling decision records that **does not** belong in this aggregate,
recorded so the boundary is re-traceable):

- **Per-loader-step error arms** (`PluginDlopenFailed`,
  `PluginMissingEntryPoint`, `PluginManifestInvalid`,
  `PluginEngineTooOld`, `PluginNameCollision`,
  `PluginDependencyMissing`, `SystemScheduleCycle`,
  `PluginInitFailed`). Refused — owned by `core` per
  `reviews/decisions/plugin-abi.md` §"Failure Modes → core::Error".
  Data raises only the four arms `core` wraps as
  `PluginAbiHashMismatch` and `SchemaMigrationFailed` (and the
  `HotReloadRefused` umbrella from `error-model.md`).
- **Auto-retry policies.** Refused — caller decision per SPEC §10.2
  recovery vocabulary; the spine exposes `refuse decode` / `refuse
  load` / `abort build` / `process abort`, never an in-engine retry.
- **Localised error messages.** Refused — `data::Error` payloads
  carry only stable identifiers (FQN, version pair, hex hash, byte
  offset, tag number); UI-side localisation is a `tools` /
  future-`l10n` concern. SPEC §10.4 makes the handler responsible for
  prose, never the constructor.

## 3. Detailed model

### 3.1 The closed `data::Error` aggregate

Restated from SPEC §10.1 for the design audit; this section adds the
*rationale* per arm, the *invariants* that govern addition / removal /
renaming, and the *Occam-collapse audit* that pinned the arm count at
nine.

```cpp
// glibre-types.dylib: data/runtime/include/glibre/types/error.hpp
//
// Public surface; mirrors specs/data/SPEC.md §5 §error.hpp and §10.1
// verbatim. Adding a payload field to one arm is a SPEC §10
// amendment + a §7 schema-version bump, not an in-place edit.

namespace glibre::types::data {

// Wire-time location attached to DeserializeError / EnvelopeTruncated
// / SchemaUnknown. `offset` is the byte index *within the inbound
// payload* at which decoding stopped, measured from the start of the
// EnvelopeHeader (SPEC §4.8 inv. 2). Non-payload arms set this to a
// sentinel — see §3.3.
struct WireSite {
    SchemaId      schema{};       // FQN the envelope claimed
    SchemaVersion version{0};     // version the envelope claimed
    std::uint32_t offset{0};      // byte index where decode failed
};

enum class ErrorTag : std::uint16_t {
    AbiHashMismatch          = 1,
    SchemaMigrationFailure   = 2,
    DeserializeError         = 3,
    ReservedTagViolation     = 4,
    SchemaRegistryConflict   = 5,
    SchemaUnknown            = 6,
    MigrationStepMissing     = 7,
    MigrationCycle           = 8,
    EnvelopeTruncated        = 9,
};

// Closed sum. Plain-aggregate layout so the C-ABI trampolines (SPEC
// §4.3 inv. 4) can return it through std::expected without crossing
// a non-trivial type boundary.
struct Error {
    ErrorTag tag{};

    // Set on tags 3, 6, 9; default-constructed on every other arm.
    WireSite at{};

    // Set on tags 2, 7, 8: identifies the migration step that
    // refused, the missing step in the chain, or the back-edge of
    // the detected cycle. Default on every other arm.
    SchemaId      step_schema{};
    SchemaVersion step_from{0};
    SchemaVersion step_to{0};

    // Set on tag 1: the host's compiled-in hash and the offending
    // plugin's compiled-in hash, hex form (SPEC §4.4 inv. 5).
    // std::string_view, not eastl::string_view: the Error struct
    // crosses the glibre-types.dylib ABI boundary (§3.3 rule 2;
    // PHILOSOPHY §11 final sentence mandates POD-only at public ABI
    // surfaces). Both sides of the boundary compile against the same
    // libc++ (reviews/decisions/plugin-abi.md §"Registration Entry-
    // Point Signature"), so std::string_view's layout is stable.
    // EASTL containers are confined to in-process, non-ABI code.
    // Empty string_views on every other arm.
    std::string_view host_hash{};
    std::string_view plugin_hash{};

    // Set on tag 4 (ReservedTagViolation): the tag number whose
    // reuse was attempted; 0 on every other arm.
    std::uint16_t reserved_tag{0};
};

}  // namespace glibre::types::data
```

Per-arm rationale (one-line existence claim; SPEC §10.2 owns
trigger / recovery / severity per arm; this table justifies
*existence*, not behaviour):

| Arm                         | Reason for existence                                                                                                                                  | Promotion / demotion gate                                                                                                                                                               |
|-----------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `AbiHashMismatch` (1)       | Plugin loader's first refusal point; the only ABI-gating failure the spine surfaces. Carries the two hex strings so operators can identify which build is which. | Frozen.                                                                                                                                                                                |
| `SchemaMigrationFailure` (2)| Single arm covering both (a) a `MigrationFn` body returning `unexpected` and (b) the schema-set continuity check (`outgoing` has FQN that `incoming` lacks). Per SPEC §8.4 the loader's response is identical for both. | Frozen. Splitting "body returned unexpected" from "FQN dropped" was rejected per SPEC §10.3 — `core::Error::SchemaMigrationFailed` collapses both, so the data-side distinction would not propagate.    |
| `DeserializeError` (3)      | Catch-all for structurally-complete-but-semantically-invalid payloads (tag-type mismatch, builtin range-check failure, nested-type decode refusal, newer-than-host version). | Coarse by design; the per-arm payload (`at.offset`) lets log readers narrow the cause without splitting the arm.                                                                       |
| `ReservedTagViolation` (4)  | Codegen-time only; never reaches runtime. Listed in the closed sum so `glibre-foryc` surfaces a typed enumerator alongside its diagnostic message rather than a free-form string. | Frozen — runtime cannot observe.                                                                                                                                                       |
| `SchemaRegistryConflict` (5)| Static-init duplicate-FQN detection; a single fatal arm for the impossible case (codegen would have caught it) where the middleman's static-init pass nonetheless sees a duplicate. | Frozen. Overlaps with `MigrationCycle` semantically (both are "build is corrupt"), but the payloads differ (FQN vs back-edge), and operator action differs (rebuild vs review codegen). |
| `SchemaUnknown` (6)         | Distinct from `SchemaMigrationFailure` (FQN missing from `incoming` *during migration*) and from `DeserializeError` (FQN known, payload malformed). Fires when a save file or snapshot carries an FQN the live build does not register. | Frozen — boundaries with `SchemaMigrationFailure` and `DeserializeError` are SPEC §10.2 cells; future "soft refusal" semantics for save-file loaders ride on the `at.offset = 0` payload, not a new arm. |
| `MigrationStepMissing` (7)  | Older-than-lowest-registered-step inbound payload; distinct from `SchemaMigrationFailure` because operators reading logs benefit from knowing "coverage gap" vs "buggy migration body". The loader collapses them; the data layer does not. | Frozen.                                                                                                                                                                                |
| `MigrationCycle` (8)        | Codegen / static-init detection of a chain back-edge. Distinct from `SchemaRegistryConflict` because the payload is a `(from, to)` back-edge triple, not a duplicated FQN. | Frozen — runtime cannot observe.                                                                                                                                                       |
| `EnvelopeTruncated` (9)     | Distinct from `DeserializeError` because the failure is *physical* (byte run ended before envelope header completed) rather than *semantic* (header complete but payload invalid). Save-file readers handle truncation by skipping past the next valid envelope; per-frame deserializers escalate. Different recovery shape ⇒ different arm. | Frozen.                                                                                                                                                                                |

The nine arms are the closed sum. Adding a tenth is a deliberate
central edit (SPEC §10 closing paragraph) requiring: (a) a SPEC §10
amendment spike, (b) a `DataErrorRecord` schema version bump (§7), (c)
the `glibre_types_abi_hash()` rebuild that the schema-version bump
forces (`reviews/decisions/plugin-abi.md` §"Versioning Rules"), and
(d) every existing handler's `eastl::visit`/`switch` audited for
missing-arm coverage. The `[[nodiscard]]` discipline on every
public-surface return (SPEC §5) plus the closed-sum rule together make
missing-arm handling a compile error in callers that exhaustively
visit (the engine's preferred handler shape; see §4.2).

#### 3.1.1 Occam-collapse audit

Sibling design docs collectively reference more candidate variants
than the closed sum admits. The audit below records every collapse
this aggregate locks in, citing the sibling that proposed the
candidate and the rationale that demoted it:

| Candidate                                  | Source design                                              | Collapsed onto                                  | Reason for collapse                                                                                                                                                                                                                                          |
|--------------------------------------------|------------------------------------------------------------|-------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `SchemaNotRegistered`                      | parent #729 issue body                                     | `SchemaUnknown` (6)                             | Same trigger ("FQN not in live registry"), same payload (`at.schema`), same recovery (refuse decode). Renamed to match the SPEC §10.1 arm.                                                                                                                  |
| `SchemaVersionRegression`                  | parent #729 issue body                                     | `SchemaMigrationFailure` (2)                    | Per SPEC §8.4 schema-set-continuity rule, version regressions are migration-failure-shaped: the loader's response is identical to a chain refusal, and `core::Error::SchemaMigrationFailed` collapses both at the wrap. The data layer keeps the distinction in the `step_*` payload (`from > to`).            |
| `FQNCollision`                             | parent #729 issue body                                     | `SchemaRegistryConflict` (5)                    | Same trigger ("two registry entries share an FQN"), same payload (`step_schema` carrying the duplicated FQN), same recovery (process abort at static-init / refuse load at Mode-B reload).                                                                                                                          |
| `SourceHashMismatch`                       | `schema-registry-design.md` §3.8 (proposed new arm 10)     | Deferred — *not* added in this design           | The schema-registry design proposes a tenth arm at value 10; this design **defers** it to a follow-up SPEC §10 amendment that will land alongside the `task-breakdown-data-schema-registry` plan. Until that amendment ships, sibling code surfaces drift via `SchemaMigrationFailure` with `step_from == step_to` (regression-shape). Captured in §12 [OPEN]. |
| `BadMagic`                                 | `envelope-serdes-design.md` §10 (proposed new arm)         | `EnvelopeTruncated` (9)                         | Same recovery (refuse decode), same payload (`at.offset = 0`); a wrong magic prefix is *physically* an early-truncation-shaped failure from the consumer's perspective. Sibling code surfaces magic-mismatch via `EnvelopeTruncated{at.offset = 0, at.schema = {}}` and the structured log carrier (§7) records the symptom in the `detail` field. Captured in §12 [OPEN] for promotion if a second consumer needs typed dispatch. |
| `VersionUnsupported`                       | `envelope-serdes-design.md` §10 (proposed new arm)         | `DeserializeError` (3)                          | Per SPEC §10.2 row "DeserializeError" the newer-than-host case rides this arm; `at.version` carries the offending version. Sibling code populates the payload; no new arm.                                                                                                                                |
| `PayloadTruncated`                         | `envelope-serdes-design.md` §10 (proposed new arm)         | `EnvelopeTruncated` (9)                         | The two are distinguishable only by `at.schema`-known vs `at.schema`-default; the recovery is identical; the structured log carrier (§7) preserves the distinction in the `detail` field for operators.                                                                                                            |
| `BufferTooSmall`                           | `envelope-serdes-design.md` §10 (proposed new arm)         | Deferred — *not* added in this design           | Serialize-side precheck failure has no SPEC §10 arm. The envelope-serdes design proposes adding it. This design defers — sibling code surfaces it via `DeserializeError` with `at.offset = dst.size()` and a `WireSite{schema = expected_fqn}` so the symptom round-trips. Captured in §12 [OPEN].                |
| `SchemaSourceMismatch`                     | `envelope-serdes-design.md` §10 (proposed new arm)         | Deferred — *not* added in this design           | Same gate as `SourceHashMismatch`: deferred to the SPEC §10 amendment. Sibling code surfaces it via `DeserializeError` with `at.schema` populated. Captured in §12 [OPEN].                                                                                                                                  |
| `MigrateFunctionFailed`                    | parent #729 issue body                                     | `SchemaMigrationFailure` (2)                    | Same trigger ("migration body returned unexpected"); the §10.1 arm name predates the issue body's term. SPEC §10.2 row 2 pins the trigger and the payload.                                                                                                                                              |
| `ArenaExhausted`                           | parent #729 issue body                                     | `core::Error::OutOfBudget` (not a data arm)     | Per `reviews/decisions/perf-budget.md` §"Allocator Rules" #6, migration-arena overflow is a `core::Error::OutOfBudget` arm carrying the migration-arena tag in `ErrorContext::detail`. Data does not own arena budget. The phrasing in the issue body was speculative; reject. Captured in §12 [OPEN] as an audit note. |
| `CycleDetected`                            | parent #729 issue body                                     | `MigrationCycle` (8)                            | Same trigger, renamed to match SPEC §10.1.                                                                                                                                                                                                                  |
| `MiddlemanLoadFailed`                      | parent #729 issue body / `middleman-dylib-design.md` §10.3 | `AbiHashMismatch` (1) with detail string        | Per `middleman-dylib-design.md` §10.3 verbatim: "this is *not* a new arm; it is the existing `PluginAbiHashMismatch` arm carrying a specific `ErrorContext::detail` string the loader emits when address-equality of `glibre_types_abi_hash` between host and plugin fails." The collapse is already locked. |
| `MissingSymbol`                            | parent #729 issue body                                     | `core::Error::PluginMissingEntryPoint` (core's) | Per `reviews/decisions/plugin-abi.md` §"Failure Modes" row 2; the loader detects missing dlsym entries. Data is not the call site; refuse to host this arm. (The middleman aggregate's design in §10.2 explicitly disclaims it.)                            |
| `ABIHashMismatch` (alternative spelling)   | parent #729 issue body                                     | `AbiHashMismatch` (1)                           | Capitalisation: SPEC §10.1 uses `AbiHashMismatch`; the issue body's `ABIHashMismatch` is the same arm.                                                                                                                                                       |

The collapse audit is the load-bearing claim of §3.1.1: SPEC §10.1's
nine arms cover every failure surface the sibling designs expose, and
the three deferred arms (`SourceHashMismatch`, `BufferTooSmall`,
`SchemaSourceMismatch`) are tracked in §12 with explicit gates.

### 3.2 Per-source trigger / payload table

The aggregate's second responsibility (per §1) is the per-source
translation guidance: for each sibling, which arm fires under which
trigger, which payload fields the arm populates. The table lifts SPEC
§10.2 into a per-source pivot and adds the *originating sibling* and
*detection-site call path* columns so the spike audit can verify
"every arm has a documented producer".

| Sibling source                                | Arm raised                  | Trigger                                                                                                                                                                                                                                                                                                                                       | Detection site (call path)                                                                                                                                       | Payload fields populated                                                                                              |
|-----------------------------------------------|-----------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------|
| schema-registry (#730) — runtime ABI handshake| `AbiHashMismatch`           | A loaded plugin's compiled-in `glibre_plugin_abi_hash` byte-string differs from the host's `glibre_types_abi_hash()` (SPEC §4.4 inv. 3).                                                                                                                                                                                                       | `core` plugin loader, step 4 (`reviews/decisions/plugin-abi.md` §"Loader Sequence"). Data does not raise this arm itself; data **defines** it.                  | `host_hash`, `plugin_hash`.                                                                                          |
| middleman-dylib (#734) — duplicate-mapping    | `AbiHashMismatch`           | Address-equality of `&glibre_types_abi_hash` between host and plugin fails (two distinct middleman copies in one process; `middleman-dylib-design.md` §3.6 mechanism B).                                                                                                                                                                       | `core` plugin loader, step 4 (same as above); the loader passes a detail string `"duplicate middleman in process"` into `ErrorContext::detail` per `middleman-dylib-design.md` §10.3. | `host_hash`, `plugin_hash` (both populated; equal byte strings — discrimination via the detail string in `ErrorContext`). |
| migration-dispatcher (#738) — chain failure   | `SchemaMigrationFailure`    | A `MigrationFn` body returned `std::unexpected` (SPEC §4.7 inv. 4) — i.e. a registered migration's pure function refused on a defective payload (foreign-key tag pointing to absent sibling, etc.).                                                                                                                                            | `MigrationDispatcher::dispatch` (`specs/data/SPEC.md` §6.3) — invoked from `Envelope<T>::deserialize` (cold deserialize) or from `migrate(...)` at hot-reload.   | `step_schema`, `step_from`, `step_to`.                                                                              |
| migration-dispatcher (#738) — schema-set continuity | `SchemaMigrationFailure`    | The `migrate(...)` schema-set continuity check (SPEC §8.2 step 1) found an `FQN` registered in `outgoing` but missing from `incoming`. SPEC §8.4 collapses this onto the same arm as chain failure; `core::Error::SchemaMigrationFailed` wraps both.                                                                                          | `migrate(...)` step 1 (SPEC §8.2).                                                                                                                              | `step_schema = dropped_fqn`, `step_from = 0`, `step_to = 0` (sentinel; the missing FQN is the load-bearing payload). |
| migration-dispatcher (#738) — schema-version regression | `SchemaMigrationFailure`    | The barrier diff against a candidate registry sees `(FQN, V_old, H_old)` succeeded by `(FQN, V_new, H_v')` with `V_new < V_old` (`schema-registry-design.md` §8.1 row "version regression"). Per the §3.1.1 collapse audit, regressions ride this arm; `step_from > step_to` distinguishes regression from forward-step refusal in logs.       | Mode-A barrier diff, registry side (`schema-registry-design.md` §8.2).                                                                                          | `step_schema`, `step_from = V_old`, `step_to = V_new` (regression: `step_from > step_to`).                          |
| envelope-serdes (#736) — newer-than-host      | `DeserializeError`          | Inbound payload's `SchemaVersion` exceeds the live registry's current version for the same `FQN` (SPEC §4.10 inv. 2; `envelope-serdes-design.md` §10 row "VersionUnsupported" collapsed onto this arm per §3.1.1).                                                                                                                              | `Envelope<T>::deserialize` body (SPEC §4.8 inv. 5); generated `glibre_types_deserialize_<fqn>` trampoline (SPEC §4.3 inv. 2).                                    | `at.schema`, `at.version` (the inbound version), `at.offset = 0` (the failure is at the envelope header).            |
| envelope-serdes (#736) — body-decode mismatch | `DeserializeError`          | Payload bytes failed Fory's per-tag decode (tag-type mismatch, builtin range-check failure, nested-type decode refusal).                                                                                                                                                                                                                       | `Envelope<T>::deserialize` body — body-decode step.                                                                                                              | `at.schema`, `at.version`, `at.offset` (byte index where decode stopped).                                            |
| envelope-serdes (#736) — bad magic / truncation| `EnvelopeTruncated`         | The inbound byte span ended before the full envelope header was read, OR the envelope's `payload_length` claimed more bytes than the remaining span carries, OR the header's magic prefix did not match `kEnvelopeMagic` (the BadMagic candidate is collapsed onto this arm per §3.1.1).                                                       | `Envelope<T>::deserialize` envelope-read step (SPEC §4.8 inv. 1, 2).                                                                                            | `at.schema` (default-constructed if the FQN field itself was truncated; populated if the header decoded enough to read it), `at.version` (likewise), `at.offset` (byte count read before truncation was detected). |
| envelope-serdes (#736) — unknown FQN          | `SchemaUnknown`             | An inbound payload's envelope `FQN` is not present in the live `SchemaRegistry`. Distinct from `SchemaMigrationFailure` (FQN known, chain refused) and from `DeserializeError` (FQN known, payload malformed).                                                                                                                                  | `Envelope<T>::deserialize` envelope-read step; registry `lookup` returns nullptr.                                                                                | `at.schema = inbound_fqn`, `at.version = inbound_version`, `at.offset = 0` **(sentinel — failure is at the envelope header; no payload bytes were consumed, so offset is always 0 and carries no diagnostic byte-count information; see §3.3 note)**. |
| migration-dispatcher (#738) — coverage gap    | `MigrationStepMissing`      | A `MigrationChain` for a known `FQN` lacks an entry whose `from_version` matches the inbound payload's version. SPEC §4.7 inv. 1's codegen check makes this impossible *for in-build types*; the arm fires only for inbound bytes from a build that has since dropped early-version migrations.                                                | `MigrationChain::dispatch` (SPEC §4.7 inv. 1); §8.3 gate 2 (Mode-B reload precondition).                                                                        | `step_schema`, `step_from = inbound_version`, `step_to = step_from + 1`.                                            |
| migration-dispatcher (#738) — chain back-edge | `MigrationCycle`            | The composed `MigrationChain` for an `FQN` contains a step `(N → M)` where `M ≤ N`. Codegen / static-init only; runtime cannot observe.                                                                                                                                                                                                       | `glibre-foryc` chain construction (SPEC §4.2 inv. 1); middleman static-init Phase C (`middleman-dylib-design.md` §3.5).                                          | `step_schema`, `step_from = N`, `step_to = M` — the back-edge that violated ascending order.                          |
| schema-registry (#730) — duplicate FQN        | `SchemaRegistryConflict`    | Two registry entries share an `FQN` (SPEC §4.5 inv. 1). Codegen would have caught it (§4.10 inv. 1 biconditional); the arm fires when two distinct middleman builds load into one process or when Mode-B reload's candidate registry duplicates an FQN.                                                                                       | Middleman static-init Phase C; Mode-B barrier diff registry side.                                                                                                | `step_schema = duplicated_fqn`.                                                                                      |
| fory-codegen runtime side (#732) — reserved-tag reuse | `ReservedTagViolation`     | A `.fory` source under `data/schemas/` reuses a tag number that the prior committed version of the same `FQN` retired into the `reserved` set (SPEC §4.1 inv. 3). Codegen-time only; runtime cannot observe.                                                                                                                                  | `glibre-foryc` reserved-tag enforcement (SPEC §4.2 inv. 4).                                                                                                     | `step_schema = offending_fqn`, `reserved_tag = reused_tag_number`.                                                  |

The table is exhaustive against the sibling designs as of the spike's
read date; new sibling aggregates that surface a failure surface MUST
add a row here in the same PR that introduces the surface, and the
spike audit checks the row's payload fields against §3.1's struct
declaration.

**Enforcement:** the exhaustiveness obligation above is mechanical once
§11.6 golden-per-source tests land: each row in this table corresponds
to one golden test fixture; a plan PR that adds a new failure surface
without a corresponding golden causes the §11.6 fixture set to be
incomplete, which the CI gate (`tests/data/errors/golden_per_source/`)
detects by asserting that the golden count equals the row count in
this table. Until §11.6 tests are authored (gated on the sibling
implementation plans landing), the obligation is normative-prose-only.
This gap is tracked in §12 [OPEN: exhaustiveness-ci-gate] and the
intent is that the first implementation plan PR (schema-registry or
envelope-serdes, whichever lands first) authors the §11.6 fixture
harness alongside its own golden row.

### 3.3 Payload-fields-by-arm contract

The `Error` aggregate keeps every arm's payload contiguous and
trivially copyable so the C-ABI trampolines (SPEC §4.3 inv. 4) can
return it through `std::expected` without crossing a non-trivial type
boundary. Arms that do not use a particular field leave it
default-constructed; the table below records exactly which payload
fields each arm populates (lifted from SPEC §10.2 and made the
single audit point):

| Arm                       | `at.schema` | `at.version` | `at.offset` | `step_schema` | `step_from` | `step_to` | `host_hash` | `plugin_hash` | `reserved_tag` |
|---------------------------|:-----------:|:------------:|:-----------:|:-------------:|:-----------:|:---------:|:-----------:|:-------------:|:--------------:|
| `AbiHashMismatch`         | -           | -            | -           | -             | -           | -         | YES         | YES           | -              |
| `SchemaMigrationFailure`  | -           | -            | -           | YES           | YES         | YES       | -           | -             | -              |
| `DeserializeError`        | YES         | YES          | YES         | -             | -           | -         | -           | -             | -              |
| `ReservedTagViolation`    | -           | -            | -           | YES           | -           | -         | -           | -             | YES            |
| `SchemaRegistryConflict`  | -           | -            | -           | YES           | -           | -         | -           | -             | -              |
| `SchemaUnknown`           | YES         | YES          | YES (=0, sentinel) | -      | -           | -         | -           | -             | -              |
| `MigrationStepMissing`    | -           | -            | -           | YES           | YES         | YES       | -           | -             | -              |
| `MigrationCycle`          | -           | -            | -           | YES           | YES         | YES       | -           | -             | -              |
| `EnvelopeTruncated`       | YES (opt)   | YES (opt)    | YES         | -             | -           | -         | -           | -             | -              |

Discipline rules:

1. **Default-construct unset fields.** Arms that do not populate a
   field leave it default-constructed (`SchemaId{}` / `0` / empty
   `string_view`). The handler that calls `glibre::log_error` reads
   the table above to know which fields to format and OMITS unset
   fields from the structured log (SPEC §10.4) — never logs an empty
   value as if it were data.
2. **`string_view`s are stable literals or middleman-owned bytes.**
   `host_hash` and `plugin_hash` borrow from
   `glibre-types.dylib`'s `.rodata` (the codegen-emitted hex literal
   per `fory-codegen.md` §"Pipeline" step 4). Lifetime spans the
   process; the `string_view` never dangles. Cross-thread propagation
   (rare; only at the loader's call site) is by-value copy, not by
   reference.
3. **`SchemaId.fqn` is a borrow.** The registry's per-type interning
   table (SPEC §4.5 inv. 3) owns the bytes; an `Error` value
   constructed from a registry lookup borrows that span. Cross-plugin
   propagation (rare; only at the loader's call site) is also by-value
   copy of the `string_view` pointer + length, never a string copy.
4. **`offset` is byte-counted from envelope-start, not body-start.**
   Per SPEC §10.1 `WireSite::offset` documentation. Implementations
   that count from body-start MUST add `sizeof(EnvelopeHeader)` before
   populating the payload.
5. **`at.offset = 0` for `SchemaUnknown` is a sentinel, not a byte count.**
   The failure is at the envelope header (FQN not found in registry);
   no payload bytes are consumed before the error fires. The field is
   always exactly `0` and carries no diagnostic byte-count information.
   A `YES (=0, sentinel)` cell in the table above means the field is
   set but the value is fixed. Implementors MUST NOT treat it as a
   variable offset. Contrast with `DeserializeError` and
   `EnvelopeTruncated`, where `at.offset` reflects a meaningful
   byte position.

### 3.4 `core::Error` wrapping table

Per `reviews/decisions/error-model.md` §"Composition Rules" #2, every
arm above is a leaf in `data`'s context. Three arms (and one
collapsed-pair) have a dedicated `core::Error` wrapping because the
`core` plugin loader is the call site that raises them on `data`'s
behalf; the remaining six surface to outer contexts by passing the
`data::Error` through the `glibre::Error` variant unchanged.

| `data::Error` arm           | When `core` raises it                                   | `core::Error` wrapping                                                                                                                          |
|-----------------------------|---------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------|
| `AbiHashMismatch`           | Phase-8 plugin load step 4.                             | `PluginAbiHashMismatch` (`reviews/decisions/plugin-abi.md` §"Failure Modes" row 4).                                                            |
| `SchemaMigrationFailure`    | Phase-8 plugin load step 11; Mode-B middleman swap (SPEC §8.3). | `SchemaMigrationFailed` (`plugin-abi.md` §"Failure Modes" row 11).                                                                              |
| `MigrationStepMissing`      | Phase-8 step 11 (chain coverage gap discovered during migrate); SPEC §8.3 gate 2. | `SchemaMigrationFailed` — same wrap as `SchemaMigrationFailure`; the loader's response (refuse, log, leave previous-good live) is identical.    |
| `SchemaRegistryConflict`    | Mode-B reload only; the static-init path is fatal.      | `HotReloadRefused` carrying the `data::Error` in `ErrorContext::detail`.                                                                        |
| `MigrationCycle`            | Never — cycles are codegen / static-init exclusively.   | None.                                                                                                                                           |
| `DeserializeError`, `EnvelopeTruncated`, `ReservedTagViolation`, `SchemaUnknown` | Surface only at the `data` layer; `core` does not wrap them. | None — they appear in `glibre::Error::Variant` as the `data::Error` arm directly.                                                              |

The wrapping table is normative for the loader's call sites and is
unit-tested at `tests/data/errors/wrapping_test.cpp` (§11.1 row 7) by
constructing each arm and asserting the loader's wrap matches the row.

### 3.5 Closed-sum invariants enforced by the design

Three invariants survive from SPEC §10 and are made mechanical here:

1. **Closed sum.** The enum is sealed at design time. Tag values are
   stable across patch releases (SPEC §10 closing paragraph);
   removing or reordering an arm is an ABI break that triggers SONAME
   bump (SPEC §4.3 inv. 3). Adding an arm bumps the
   `DataErrorRecord` schema version (§7) and therefore the
   middleman's `glibre_types_abi_hash()`, which forces every plugin
   to be rebuilt — the same gate that catches schema-shape drift
   (`reviews/decisions/plugin-abi.md` §"Versioning Rules"). New arms
   never go through the SONAME-stable path because their tag value
   widens the wire-format `arm_tag` enumeration.
2. **No exceptions cross the boundary.** Every public function that
   constructs an `Error` is `noexcept`; per `error-model.md`
   §"Decision" #3 the engine compiles with `-fno-exceptions`. The
   `Error` aggregate itself contains no resources whose destruction
   could throw — every field is a builtin or a borrow.
3. **Errors are constructed at the site they happen.** No central
   factory exists. The dispatcher does not call into the registry to
   ask "which arm should I raise"; it raises `MigrationStepMissing` /
   `SchemaMigrationFailure` directly with the payload it already
   holds. The registry does not call the dispatcher; it raises
   `SchemaRegistryConflict` / `SchemaUnknown` directly. Cross-arm
   composition is forbidden inside a single sibling — the `core`
   loader is the only call site permitted to translate a `data::Error`
   into a `core::Error`, and that translation is the wrapping table
   above.

## 4. Public surface

This aggregate adds no symbols to the public §5 surface beyond what
SPEC §5 §error.hpp / §5.3 / §10.1 already declare (`SchemaId`,
`SchemaVersion`, `WireSite`, `ErrorTag`, `Error`). The Fory-serialized
log carrier introduced in §7 is itself a generated middleman type
emitted from `data/schemas/data/DataErrorRecord.fory`; the C++
projection compiles into `glibre-types.dylib` alongside every other
generated type.

### 4.1 Construction discipline (free functions, not methods)

Each sibling aggregate constructs `Error` values as plain aggregate
initialisations — there is no constructor, no factory method, no
"error builder" indirection. The construction discipline is the SRP
collapse: each sibling owns the construction site for the arms it
raises (§3.2 detection-site column), and that site fills in only the
fields §3.3 marks YES.

```cpp
// example: schema-registry construction site for SchemaUnknown
//
// from envelope-serdes-design.md §3.1 step 2:
[[nodiscard]] auto Envelope<T>::deserialize(eastl::span<const std::byte> src)
    noexcept -> std::expected<T, glibre::types::data::Error>
{
    auto hdr_or = read_envelope_header(src);
    if (!hdr_or) return std::unexpected(std::move(hdr_or).error());

    const auto* entry = SchemaRegistry::instance().lookup(hdr_or->schema);
    if (entry == nullptr) {
        return std::unexpected(glibre::types::data::Error{
            .tag = glibre::types::data::ErrorTag::SchemaUnknown,
            .at  = WireSite{
                .schema  = hdr_or->schema,
                .version = hdr_or->version,
                .offset  = 0,
            },
            // step_*, *_hash, reserved_tag default-constructed per §3.3
        });
    }
    // ... continue dispatch ...
}
```

The example is illustrative; the envelope-serdes design owns its
boundary code. The load-bearing point is that the construction site
populates exactly the §3.3 YES fields and nothing else — no central
"populate everything for safety" anti-pattern.

### 4.2 Exhaustive handling pattern

Callers that wish to discriminate on the arm use a `switch (err.tag)`
or an `eastl::visit`-style helper; the closed-sum invariant + the
`-Wswitch` warning-as-error compile flag together make missing-arm
handling a compile error. The audit pattern is:

```cpp
// example handler (the loader's wrapping site)
glibre::Error wrap_data_to_core(const glibre::types::data::Error& e) noexcept {
    using glibre::types::data::ErrorTag;
    switch (e.tag) {
        case ErrorTag::AbiHashMismatch:
            return glibre::Error{glibre::core::Error::PluginAbiHashMismatch};
        case ErrorTag::SchemaMigrationFailure:
        case ErrorTag::MigrationStepMissing:
            return glibre::Error{glibre::core::Error::SchemaMigrationFailed};
        case ErrorTag::SchemaRegistryConflict:
            return glibre::Error{glibre::core::Error::HotReloadRefused};
        case ErrorTag::DeserializeError:
        case ErrorTag::SchemaUnknown:
        case ErrorTag::EnvelopeTruncated:
        case ErrorTag::ReservedTagViolation:
        case ErrorTag::MigrationCycle:
            // Pass-through: glibre::Error::Variant carries the
            // data::Error arm directly per error-model.md.
            return glibre::Error{e};
    }
    // -Wswitch makes a missed case here a compile error.
    __builtin_unreachable();
}
```

The `wrap_data_to_core` example lives in `core` (per §3.4); data does
not host it. The example is included here only so the spike audit can
verify that adding a new arm forces the central wrap to gain a case.

### 4.3 Composition with `glibre::Error`

Per `error-model.md` Type Sketch, `glibre::Error` is the engine-wide
`eastl::variant` rolling up every per-context error enum.
`data::Error` is one arm of that variant. The composition is purely
additive: a function returning `std::expected<T, glibre::Error>` may
receive a `data::Error` from a data call and propagate it unchanged,
or it may extract the `data::Error` and map it to its own context's
enum at the call site (composition rule 2). The mapping for `core` is
the §3.4 table; mapping for any other context is that context's source
code.

## 5. Hot/cold path split

Glibre's `>= 1.5 ms headroom` requirement (`perf-budget.md`) and
data's 0.20 ms sim cell (SPEC §9) collapse to one rule for this
aggregate: **error construction is off the success path**. Concretely:

### 5.1 Hot path (success path)

The success path of every public §5 call returns `std::expected<T,
data::Error>` (or `std::expected<T, glibre::Error>`) carrying a `T`.
The `data::Error` value is *not constructed* on this path — `expected`
holds only `T`. The cold-path branch is a single tag check
(`expected::has_value()`); on success, the variant slot is never
visited, no payload field is ever read, and no construction runs.
Steady-state branch-prediction makes the success path a free
fall-through. The `Envelope<T>::serialize` /
`Envelope<T>::deserialize` benchmarks (SPEC §9.4 #1) verify the
success-path cost stays in the 0.10 ms / frame ceiling.

### 5.2 Cold path (failure construction)

When a sibling aggregate raises a failure, it constructs the `Error`
aggregate at the construction site (§3.2 detection-site column). All
construction costs are absorbed in the cold path:

- **Aggregate-init.** `Error{ .tag = ..., .at = ..., ... }` is a
  trivially-constructible aggregate; the compiler emits register
  loads + a few stores. No allocation, no virtual dispatch, no
  vtable.
- **`std::unexpected` wrap.** Move into the `expected` failure slot;
  `data::Error` is small (single cache line: 2-byte tag + 24-byte
  `WireSite` + 16-byte `SchemaId` + 8-byte `step_*` triple + 16-byte
  hash views + 2-byte reserved_tag = ~64 bytes; layout
  implementation-defined but bounded to one cache line).
- **`glibre::Error` lift.** When the call site lifts a `data::Error`
  into `glibre::Error`, the variant tag is updated and the payload is
  copied. Since `data::Error` is trivially copyable, this is a single
  memcpy at the variant-store boundary.

Aggregate cold-path cost is bounded by **<1 µs per failure** on M1
firestorm; the data context's 0.20 ms cell can absorb hundreds of
failures per frame without breaching budget. The CI gate (§9) does
not assert this directly because the cell already covers it as part
of the `Envelope` and `MigrationDispatcher` aggregate budgets that
*construct* the errors.

### 5.3 What is forbidden on the hot path

- **Never**: a `switch (err.tag)` over `data::Error` on the success
  path of a public call. Handlers run only inside `or_else` /
  failure branches.
- **Never**: a payload-field read in steady-state code that does not
  hold an `Error` value.
- **Never**: dynamic allocation inside an `Error`'s construction or
  destruction. The aggregate is POD; its destructor is trivial.
- **Never**: a `dlsym` lookup, virtual dispatch, or vtable call on
  the construction path. Every construction is an aggregate-init at
  the call site.

### 5.4 Small-string source-tag option (cold path only)

Unlike `platform::Error` (which uses a TLS prefix slot for cold-path
discrimination), `data::Error` carries every load-bearing
discriminator in its payload: the `(step_schema, step_from, step_to)`
triple distinguishes migration arms; `at.{schema, version, offset}`
distinguishes envelope arms; `(host_hash, plugin_hash)` distinguishes
the abi-hash arm. There is no TLS prefix slot — the payload itself is
the discriminator.

When `glibre::log_error` formats a `data::Error` for spdlog, it MAY
read an optional thread-local `source_tag` slot that the construction
site can populate with a static string identifying the producing
sibling (`"schema-registry"`, `"envelope-serdes"`,
`"migration-dispatcher"`, `"middleman-static-init"`, `"foryc"`). The
slot is purely informational — no engine code branches on it — and
exists to give operators a fast-path tag in the log carrier (§7
field 4). The `source_tag` is **opt-in**: siblings that benefit from
the disambiguation set it; the default is the empty string.

The slot is a **data-context-owned** `thread_local` variable declared
inside `glibre::types::data` (a separate `__thread` allocation from
any platform-context TLS struct). This keeps the data context
self-contained per PHILOSOPHY §1/§2 (SRP, no cross-context coupling).
The slot holds a `std::string_view` pointing at a `static` string
literal or a `.rodata` byte in the middleman — never a heap pointer;
lifetime is permanent. It is reset to the empty string_view at frame
phase 1 reset by the data context's own phase-1 hook (registered via
`core::SystemRegistry`); no coupling to the platform prefix-slot reset
is required.

If a future spike concludes that data and platform MUST share a single
`__thread` struct for footprint or atomicity reasons, a dedicated
decision record `reviews/decisions/tls-layout.md` MUST be opened that
both `platform` and `data` cite before the coupling is introduced. That
record does not exist today; without it, shared-struct ownership is
not permitted (see §12 [OPEN: tls-layout-decision]).

## 6. Concurrency

### 6.1 Thread-safety by construction

`data::Error` values are POD: a `u16` tag, a `WireSite` (POD), a
`SchemaId` borrow (POD), a `(SchemaVersion, SchemaVersion)` pair
(POD), two `string_view`s (POD), a `u16` reserved-tag. They are
trivially copyable, trivially move-constructible, and have no
non-trivial destructor. Two threads constructing two different
`Error` values share no state; passing an `Error` from thread A to
thread B is a value copy and races on nothing.

This is the load-bearing property that makes the cross-aggregate
error surface work without a lock or atomic anywhere on the value
itself: every `std::expected<T, data::Error>` returned across the
public §5 surface is by-value, and every consumer either propagates
the value (no shared state) or maps it to its own enum (also
by-value).

The `string_view` payloads (`step_schema.fqn`, `host_hash`,
`plugin_hash`) borrow from `glibre-types.dylib`'s `.rodata` or from
the registry's interning table (§3.3 rule 2/3). Those storages are
process-lifetime; cross-thread propagation does not race because the
underlying bytes are never mutated after static-init (SPEC §4.5
inv. 4).

### 6.2 Construction is signal-safe

Crash-dump capture (`platform::SPEC` §4.5; harmonius
`crash-reporting.md`) runs in a SIGSEGV / SIGBUS handler. The handler
must not allocate, must not lock, must not call any non-async-signal-
safe function. Construction of `data::Error` is therefore allowed in
a signal handler:

- Tag-aggregate construction is constexpr-default + trivial copy. Safe.
- `WireSite`, `SchemaId`, `SchemaVersion` field copies are trivial.
  Safe.
- `string_view` field copies (`host_hash`, `plugin_hash`,
  `step_schema.fqn`) are pointer + length copies; the pointed-to
  bytes are static `.rodata` (already mapped). Safe.
- `std::unexpected` wrap is constexpr; safe.

The signal-safe construction discipline matters mostly for the
error-aggregate's *interaction* with platform-side crash capture; the
data layer itself is not invoked inside a signal handler at MVP. The
property is documented here so a future spike adding crash-handler
schema replay (post-MVP, listed in §12) inherits a clear contract.

### 6.3 Cross-thread error propagation

`data::Error` values cross threads only via SPSC ring slots (rare;
data has no per-frame worker threads in MVP). When they do, the value
travels by-copy. The optional `source_tag` TLS slot does **not**
travel — it is per-thread, and the consumer thread reads its own
slot, which reflects its own most recent construction call (or empty
string). Cross-thread `source_tag` propagation, if a future sibling
needs it, copies the literal pointer into the SPSC slot alongside the
`Error`. This is a **per-aggregate** decision (no MVP sibling needs
it); the SPSC slot layout would live in the consuming sibling's
design.

### 6.4 Static-init ordering

The middleman static-init's Phase C validator
(`middleman-dylib-design.md` §3.5) constructs `SchemaRegistryConflict`
and `MigrationCycle` `Error` values during static init. Construction
is allowed at static-init time because:

- The aggregate is a literal type; aggregate-init is constexpr-friendly.
- The `string_view` fields borrow from already-emitted `.rodata`; no
  cross-TU init order dependency.
- The `Error` value flows directly into a fatal log + `std::abort()`
  call; no consumer reads it after construction.

The ordering rule from SPEC §4.3 inv. 5 (builtins register before
generated types; generated types before migrations) is preserved by
the codegen-emitted `_registry.cpp` ordering, not by this aggregate.

## 7. Persistence + ABI

### 7.1 Live state — none

`data::Error` values are by-value error returns. They are not stored
as components, not stored as singletons, not stored in any
plugin-private heap. SPEC §10 confirms: zero live storage, zero
runtime persistence.

### 7.2 Log / replay carrier — `DataErrorRecord`

For `glibre::log_error` to emit a structured spdlog record, and for
the e2e replay harness (`specs/e2e/SPEC.md`) to capture a
`data::Error` into the trace stream, a small Fory schema serialises
the *log carrier* — not the error value itself. The carrier's
purpose is single-shot logging / replay, never reconstruction of a
live `Error` (the aggregate is rebuilt only for replay assertions,
never returned to engine code).

```fory
// data/schemas/data/DataErrorRecord.fory
//
// Wire shape used by glibre::log_error and by the e2e replay trace
// stream. Stable on the wire; field tags immutable per
// fory-codegen.md migration rules. Lives under data/schemas/data/
// — owned by the data context (the only context whose §10 errors
// it represents); contributes to glibre_types_abi_hash() per SPEC
// §4.4 inv. 1.
schema glibre.data.DataErrorRecord {
  version 1
  since   "0.1.0"

  field arm_tag         : u16     tag 1 since 1     # mirrors ErrorTag (1..=9)
  field at_schema       : string  tag 2 since 1     # SchemaId.fqn or empty
  field at_version      : u32     tag 3 since 1     # 0 if unset
  field at_offset       : u32     tag 4 since 1     # 0 if unset
  field step_schema     : string  tag 5 since 1     # FQN of step_schema or empty
  field step_from       : u32     tag 6 since 1     # 0 if unset
  field step_to         : u32     tag 7 since 1     # 0 if unset
  field host_hash       : string  tag 8 since 1     # 64-char hex or empty
  field plugin_hash     : string  tag 9 since 1     # 64-char hex or empty
  field reserved_tag    : u16     tag 10 since 1    # 0 if unset
  field source_tag      : string  tag 11 since 1    # producing sibling, opt
  field detail          : string  tag 12 since 1    # cold-path prose, opt
  field monotonic_ns    : u64     tag 13 since 1    # Clock::now() at construct
  field thread_id       : u32     tag 14 since 1    # POSIX TID for audit
}
```

Schema rules (per `reviews/decisions/fory-codegen.md`):

- `SchemaVersion` starts at 1; field tag numbers immutable.
- Removing a field moves the tag to the reserved set.
- Adding a field bumps the schema version and adds a one-step
  migrate function (pure, allocator-arena-only).
- The schema lives at `data/schemas/data/DataErrorRecord.fory` and
  contributes to `glibre_types_abi_hash()` via
  `glibre-types.dylib` (SPEC §4.4 inv. 1; §6.4).
- Round-trip identity is tested per SPEC §7.5 (a `data::Error` value
  → `DataErrorRecord` → bytes → `DataErrorRecord` → `data::Error` is
  byte-equal end-to-end).

### 7.3 ABI-hash contribution and arm-count gate

The `DataErrorRecord` schema is **the** data-context ABI contribution
of the error aggregate. Per `reviews/decisions/plugin-abi.md`
§"Versioning Rules":

- Any change to the record's field tags / types triggers a SchemaVersion
  bump on `DataErrorRecord`, which triggers an ABI-hash bump on
  `glibre_types_abi_hash()`, which forces every plugin (including
  the data plugin itself) to be rebuilt against the new
  `glibre-types.dylib`.

The closed `data::Error` enum *itself* — its arm count, its arm
names, its tag-discriminant values — is part of `glibre-types.dylib`'s
public surface (declared in the `glibre/types/error.hpp` header that
every plugin consumes). The `DataErrorRecord::arm_tag` field encodes
the `ErrorTag` enum's value space, so adding a new arm to
`data::Error` requires:

1. Bumping the `DataErrorRecord` schema version (`v1` → `v2`).
2. Adding a migrate function that maps every old-record's `arm_tag`
   to the new index (almost always identity if the new arm is
   appended; SPEC §4.7).
3. The `glibre_types_abi_hash()` bump that the schema-version edit
   triggers.
4. Barrier-time validation at hot-reload (§8): the data plugin
   refuses to swap if the live engine's `arm_tag` enumeration
   disagrees with the loaded plugin's compiled-in mapping (this is
   covered by the standard ABI-hash gate; no separate check needed).

This is the **single load-bearing reason `ErrorTag` values are
documented in §3.1 alongside the enum declaration** — the tag values
are the wire-format ordering, not just an implementation detail.

### 7.4 Arm-name `to_string` table

Per `reviews/decisions/error-model.md` §"Logging / Telemetry" #2, the
logger formats `error.code` as the enumerator name. The data context
provides a `to_string(ErrorTag) -> eastl::string_view` table inside
`glibre-types.dylib`:

```cpp
// data/runtime/src/error_strings.cpp (illustrative)
namespace glibre::types::data {
constexpr eastl::string_view kErrorTagNames[] = {
    "(invalid)",                // tag value 0; never constructed
    "AbiHashMismatch",          // 1
    "SchemaMigrationFailure",   // 2
    "DeserializeError",         // 3
    "ReservedTagViolation",     // 4
    "SchemaRegistryConflict",   // 5
    "SchemaUnknown",            // 6
    "MigrationStepMissing",     // 7
    "MigrationCycle",           // 8
    "EnvelopeTruncated",        // 9
};

[[nodiscard]] constexpr eastl::string_view to_string(ErrorTag t) noexcept {
    const auto idx = static_cast<std::uint16_t>(t);
    return idx < eastl::size(kErrorTagNames)
        ? kErrorTagNames[idx]
        : eastl::string_view{"(unknown)"};
}
}  // namespace glibre::types::data
```

The hand-written table is the MVP shape (per `error-model.md` Open
Q #1); a future amendment may switch to `magic_enum`. The
`error-model.md` decision is consumed verbatim — this design forks
no choice.

The string table is itself part of `glibre-types.dylib`'s exported
surface (because plugins consume `glibre/types/error.hpp` and may
call `to_string`); however it is constexpr `.rodata` and does not
contribute to `glibre_types_abi_hash()` (the hash inputs are
`.fory` schema source hashes per SPEC §4.4 inv. 4, and the string
table is generated C++ code, not a schema). Renaming an arm therefore
costs:

- A SPEC §10 amendment + `DataErrorRecord` v-bump (the rename is an
  ABI-visible change to the wire-format `arm_tag` semantics, even if
  the integer value is preserved — log-replay readers compare
  arm names against the table).
- The `kErrorTagNames` literal edit.

## 8. Hot-reload

### 8.1 Per-aggregate disposition

| Concern                                         | Disposition                                                                                                                                                                                  |
|-------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `data::Error` value crossing the swap            | Pass-through (SPEC §8.1). The value is by-value POD; the swap is invisible to it.                                                                                                            |
| Construction-site code (sibling aggregates)     | Re-resolved by Q's `register` (the new plugin's text segment owns the construction code). Engine code never holds a function pointer to a construction site; it constructs at the call site by name only. |
| `string_view` payload borrows from outgoing dylib | A `data::Error` value held across `dlclose(P)` would dangle if its `string_view`s pointed into P's `.rodata`. Per SPEC §8.4 the loader's per-row rollback completes *before* `dlclose`, so no `Error` value survives the close. The discipline is mechanical: no plugin holds a `data::Error` past the loader's step 4.3. |
| `kErrorTagNames` table                          | Lives in `glibre-types.dylib`'s `.rodata`. Survives plugin reload (Mode A). On Mode-B middleman reload the table is replaced wholesale alongside the registry (SPEC §8.3).                                                                                |
| `DataErrorRecord` schema                        | Survives via `glibre-types.dylib` middleman. Migrate function runs at phase 8 step 3 if the schema version bumped (§7.2).                                                                                                                                  |
| `source_tag` TLS slot                           | Reset to empty string at the start of the next phase 1 input drain (idempotent; cheap). Pre-swap pointer values point to literals in P's `.rodata`; reset is the load-bearing rule.                                                                            |
| Closed-sum invariant validation                 | At swap step 2 (ABI hash check): the loaded plugin's compiled-in arm count and ordering must match `glibre-types.dylib`'s `DataErrorRecord::arm_tag` enumeration. Mismatch ⇒ `core::Error::PluginAbiHashMismatch`.                                          |

### 8.2 Barrier-time validation

When the data plugin reloads, the loader runs the following
error-aggregate-specific checks at swap step 2:

1. **ABI hash check** (already required by `plugin-abi.md`). Covers
   the `DataErrorRecord` schema; covers `glibre-types.dylib`
   identity.
2. **Arm-count compile-time check.** The new data plugin compiles in
   a `static_assert` that `static_cast<std::uint16_t>(ErrorTag::EnvelopeTruncated)
   == ARM_COUNT_LITERAL` matching the wire-format max arm value from
   `glibre-types.dylib`. A mismatch is a compile error in the
   plugin's build, not a runtime check; the runtime check is the ABI
   hash.
3. **String-view residence check.** Any `data::Error` value still in
   flight across phase 8 holding a `string_view` payload from the
   outgoing plugin P is a contract violation: SPEC §8.4 requires
   per-row rollback to complete before any vtable swap (so the
   `Error` value has been logged and dropped). The discipline is
   tested in `tests/e2e/data/error_lifetime/` by injecting a
   forced-failure migration and asserting no `data::Error` survives
   into phase 9.

### 8.3 What survives, what is discarded

- **Survives:** the `data::Error` aggregate *type* (it's a
  middleman-relevant shape via `DataErrorRecord::arm_tag`); the
  schema version and migrate chain; the `kErrorTagNames` table; the
  engine-wide `glibre::Error`'s data arm slot.
- **Discarded:** in-flight `Error` values whose payload `string_view`s
  point into the outgoing plugin's `.rodata` (acceptable; the
  per-row rollback rule of SPEC §8.4 ensures no such value crosses
  `dlclose`); the `source_tag` TLS slot contents (acceptable; reset
  on next access); any plugin-private construction state (sibling
  construction is stateless by design).

### 8.4 Refusal cases inherited

Data-error inherits SPEC §8.4 universal refusals (ABI hash mismatch,
plugin init failure) and adds none of its own. The error-aggregate
itself is not a refusal source — it is the *type* of refusals raised
by other data aggregates.

## 9. Performance

The data-error aggregate sits inside data's 0.20 ms sim cell (SPEC
§9.1) and contributes **0 ms / frame** in steady state because error
construction is off the success path (§5). Failure-path costs are
absorbed by the consuming aggregate's budget (`Envelope`,
`MigrationDispatcher`, `SchemaRegistry`), not by an
error-aggregate-specific budget.

### 9.1 Steady-state cost

| Path                                  | Cost                                                                                  |
|---------------------------------------|---------------------------------------------------------------------------------------|
| Success path (no `Error` constructed) | **0** — `expected::has_value()` is a single tag check; success short-circuits.         |
| Source-tag TLS slot access (success)  | **0** — never read on the success path.                                                |
| `Error` aggregate construction        | **<1 µs** — aggregate-init + register loads + a few stores.                            |
| `switch` on `ErrorTag` (handler)      | **<10 ns** — jump-table dispatch over 9 arms.                                          |
| `to_string(ErrorTag)`                 | **<5 ns** — array index + `string_view` return.                                        |

### 9.2 Heap impact

Zero per-frame allocations from this aggregate. The `kErrorTagNames`
table is `.rodata` (zero heap). The `source_tag` TLS slot reuses
platform's TLS struct (§5.4). Per-call cost is a single TLS write at
construction; per-call read at log time.

The `Error` aggregate's heap footprint is its struct size: roughly
**64 bytes** (one cache line). The aggregate is small enough that the
expected-failure-slot in `std::expected<T, glibre::Error>` is bounded
by `sizeof(glibre::Error)` (the engine-wide variant), not by
`sizeof(data::Error)` directly.

### 9.3 CI gate fixture

A Catch2 `BENCHMARK` block under `tests/data/errors/perf/` asserts:

1. **Construction throughput.** 1 000 000 `Error` aggregate-inits
   (one per arm, round-robin) must complete in **≤ 5 ms** wall clock
   on M1 firestorm (sub-5 ns per construction). Verifies the
   aggregate-init compiles to register loads, not to a constructor
   call.
2. **Success-path zero-cost.** A success-path benchmark constructs
   `std::expected<int, data::Error>` 1 000 000 times with `T = 42`
   and verifies the failure slot's `Error` is never constructed
   (counted via a sentinel `arm_tag` initial value of 0); time is
   bounded by 1 ms.
3. **Cold-path budget cap.** A failure-path benchmark constructs
   `std::unexpected(Error{.tag = ErrorTag::SchemaUnknown, .at = ...})`
   100 000 times and asserts wall-clock ≤ 1 ms (≤ 10 ns per
   construction).
4. **`to_string` throughput.** 1 000 000 `to_string(ErrorTag)` calls
   complete in ≤ 1 ms (sub-1 ns per call after compiler folding).

These benchmarks live alongside the sibling-aggregate perf gates
(SPEC §9.4) but do not assert against the 0.20 ms cell directly —
the cell is a per-aggregate budget, and data-error rolls into the
consuming aggregate's row.

## 10. Failure modes

### 10.1 Meta-failure mode — unmapped sibling failure

The `Error` aggregate itself does not "fail to construct" — it is a
trivially-constructible POD. The meta-failure mode worth documenting
is what happens when a *new* sibling-aggregate failure surface arrives
that no existing arm covers.

The contract is: **the sibling does NOT default to a generic catch-all
arm.** Per SPEC §10 closing paragraph, new arms require a SPEC
amendment. The discipline that protects against silent miscoverage is:

1. **No `Internal` / `Other` / catch-all arm exists.** Unlike
   `platform::Error`'s `OsCode` raw-fallback, `data::Error` has no
   "generic" arm. Every `data::Error` value is the named arm that
   matches its `tag`; sibling code that needs a new arm must amend
   SPEC §10 and bump `DataErrorRecord` (§7).
2. **Compile-time fan-out.** Adding a new arm is a SPEC amendment that
   adds an `ErrorTag` value, a row in §3.2, a row in §3.3, and a row
   in §3.4 (or refuses a wrap, as `MigrationCycle` does). Every
   existing handler that switches on `ErrorTag` either explicitly
   handles the new arm or fails to compile (`-Wswitch` rule, §4.2).
3. **Telemetry-driven promotion.** When a sibling discovers a failure
   surface that does not yet have an arm, the immediate stop-gap is
   to surface it through *the closest existing arm with a `detail`
   field*. The sibling logs at `error` severity with the specific
   trigger in `detail`; operators reading logs see the unmapped case
   as a frequency anomaly. The amendment to add a new arm follows
   the §3.1 promotion gate ("a second consumer needs typed dispatch"
   — analogous to the `platform::Error` SPEC §10.8 rule).
4. **Refusal of the catch-all temptation.** The `data::Error` design
   was audited against the temptation to add a catch-all `Internal`
   arm during the §3.1.1 collapse. **Rejected.** The reasoning
   parallels `platform::Error`'s `OsCode` rationale (`platform-error-design.md`
   §10.1): a catch-all silences operational degradation; a refusal
   to ship an unmapped failure surfaces the gap loudly. The cost of
   the refusal is one SPEC amendment per genuinely-new failure
   surface; the cost of a catch-all is silent miscoverage forever.
   Asymmetry favours refusal.

The deferred arms in §12 (`SourceHashMismatch`, `BufferTooSmall`,
`SchemaSourceMismatch`) are the live test of this rule: each is
proposed by a sibling design as a new arm; this design defers each
to a SPEC §10 amendment rather than collapsing them onto an existing
catch-all. Until the amendment lands, the sibling code surfaces the
failure through the documented stop-gap (e.g. `DeserializeError`
with `at.offset = sentinel`, `SchemaMigrationFailure` with
`step_from == step_to`), and the structured log carrier (§7) records
the symptom in the `detail` field for operators.

### 10.2 Construction-site misuse

| Misuse                                                              | Disposition                                                                                                                                                                                                |
|---------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Sibling constructs `Error{.tag = ErrorTag::AbiHashMismatch}` without setting `host_hash` / `plugin_hash` | Compiles; runtime carries empty `string_view`s. Logger omits the unset fields per §10.4. Debug builds add a `GLIBRE_ASSERT` in the construction helpers (per-arm) checking that §3.3 YES fields are populated; the assert names the call site. |
| Sibling sets a `host_hash` pointer to a non-`.rodata` string         | Compiles; runtime may dangle if the buffer goes out of scope before logging. The discipline is enforced by review: payload `string_view`s borrow only from process-lifetime storage (§3.3 rule 2). A future spike may add a clang-tidy lint. |
| Sibling sets two YES fields whose §3.3 rows disagree                | Compiles; the §11 wrapping test (§11.1 row 7) checks that the wrap-to-`core::Error` mapping is invariant under the cell pattern. A misuse where, say, `AbiHashMismatch` carries a populated `step_from` would fail the test as a discipline drift. |
| Sibling adds a public arm without bumping `DataErrorRecord`          | Compile error: the wire-format `arm_tag` enumeration is generated from `ErrorTag`'s declaration order via codegen; a new arm without a schema bump fails the codegen consistency check (mirroring `platform-error-design.md` §10.2 row 5). |

### 10.3 Concurrency-mode failure

| Mode                                                                              | Outcome                                                                                                                                                            |
|-----------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Two threads constructing different `Error` values simultaneously                  | No race. Each writes its own stack-local; payload borrows from process-lifetime `.rodata` / interning table. Trivially safe.                                         |
| Two threads stamping different `source_tag` strings                               | Each writes its own TLS slot; no race. Deterministic per-thread.                                                                                                    |
| Plugin reload mid-construction on another thread                                  | Forbidden by SPEC §8 phase 8 barrier — reload runs only on the game-loop driver thread when no other thread is in plugin code. The TLS source-tag reset (§8.2) runs after every thread's plugin-code re-entry has been suspended by the loader. |
| Static-init `SchemaRegistryConflict` / `MigrationCycle` construction              | Constructed on the main thread before `main()`; no concurrency. The fatal log + abort sequence is synchronous.                                                       |

## 11. Test plan

### 11.1 Unit tests (Catch2)

Live under `tests/data/errors/`. The §3.2 table *is* the test
goldens; every row has a matching test. Test files map 1:1 to arms:

- `tests/data/errors/abi_hash_mismatch_test.cpp` — constructs the
  arm via the loader's wrap-site (with a fixture build of a plugin
  whose abi hash differs); asserts `host_hash` / `plugin_hash` are
  populated.
- `tests/data/errors/schema_migration_failure_test.cpp` — uses the
  SPEC §8.6 `force_migration_failure` fixture to drive a chain step
  refusal; asserts `step_schema` / `step_from` / `step_to` are
  populated.
- `tests/data/errors/deserialize_error_test.cpp` — constructs an
  envelope with a tag-type mismatch; asserts `at.schema` /
  `at.version` / `at.offset` are populated; asserts `at.offset`
  matches the byte index of the offending tag.
- `tests/data/errors/reserved_tag_violation_test.cpp` — drives
  `glibre-foryc` against a fixture `.fory` that reuses a previously-
  committed tag; asserts the codegen exit code and the payload
  fields in the codegen-emitted diagnostic record.
- `tests/data/errors/schema_registry_conflict_test.cpp` —
  death-test linking a fixture middleman with two entries sharing
  FQN; asserts `step_schema` and the `std::abort` path.
- `tests/data/errors/schema_unknown_test.cpp` — envelope whose
  `FQN` is `glibre.test.NeverRegistered`; asserts `at.schema` is
  populated.
- `tests/data/errors/migration_step_missing_test.cpp` — a registry
  whose chain for an FQN at version 4 starts at step `(2 → 3)`;
  deserialise a `v1` payload; assert `step_from = 1`, `step_to = 2`.
- `tests/data/errors/migration_cycle_test.cpp` — `glibre-foryc`
  invocation against synthetic chain `[(1→2), (2→1)]`; asserts the
  codegen exit code and the `(step_from = 2, step_to = 1)` payload.
- `tests/data/errors/envelope_truncated_test.cpp` — `eastl::span`
  smaller than `sizeof(EnvelopeHeader)`; asserts `at.offset = src.size()`.

Cross-cutting tests:

- `tests/data/errors/wrapping_test.cpp` — for every `ErrorTag`
  value, calls the `core` `wrap_data_to_core` function (§4.2) and
  asserts the result matches the §3.4 table.
- `tests/data/errors/payload_fields_invariant_test.cpp` — for
  every `ErrorTag`, constructs an `Error` populating *every* §3.3
  YES field; asserts a round-trip through `DataErrorRecord` Fory
  serialisation preserves the payload byte-equal; constructs again
  populating only the YES fields; asserts the same property.
- `tests/data/errors/to_string_test.cpp` — for every `ErrorTag`
  value, asserts `to_string(t) == kErrorTagNames[t]` and that no
  arm name contains whitespace, escape sequences, or non-ASCII
  bytes.
- `tests/data/errors/closed_sum_size_test.cpp` — `static_assert`
  that `static_cast<std::uint16_t>(ErrorTag::EnvelopeTruncated) ==
  9` (the documented arm count); fails to compile if a future arm
  is added without bumping the SPEC §10 enum.

Every translator test asserts:

1. The returned arm matches the row's expected `ErrorTag`.
2. The §3.3 YES fields are populated; the §3.3 NO fields remain
   default-constructed.
3. The arm round-trips through `DataErrorRecord` Fory serialisation
   and deserialisation byte-equal.
4. `static_cast<std::uint16_t>(ErrorTag::N) ==
   DataErrorRecord::arm_tag` for every `N` (compile-time check).

### 11.2 Cross-aggregate uniformity tests

Live under `tests/data/error_uniformity_test.cpp`. Asserts that every
public `std::expected<T, data::Error>`-returning function in §5
surfaces only `ErrorTag` values drawn from §3.1's closed sum. The
test enumerates every public function (`Envelope<T>::serialize`,
`Envelope<T>::deserialize`, `migrate(...)`, `subscribe_schema_registry_change`,
the test-hook `bump_schema_version`, etc.), calls each under
fixture-induced failure, unwraps the returned `Error`, and asserts
the `tag` is in `{1, ..., 9}`. No fixture may surface a `core::Error`
/ `platform::Error` / etc. through a data call.

### 11.3 Integration tests (against e2e replay)

Live under `tests/e2e/data/error_round_trip/`. Constructs a known
sequence of `data::Error` values (one per arm, one per payload
shape), serialises each through `DataErrorRecord`, captures into a
`.glibre-trace`, and asserts the trace deserialises into the original
arm + payload. The test exercises the §7 wire-format contract
end-to-end and is the single fixture that breaks on a schema-version
regression.

### 11.4 Performance tests

`tests/data/errors/perf/` — four Catch2 `BENCHMARK` blocks matching
§9.3.

### 11.5 Compile-fail tests

Under `tests/data/errors/compile_fail/`, using
`add_test(... CONFIGURATIONS CompileFail)` in CMake:

- A test that adds a hypothetical 10th arm to `ErrorTag` without
  bumping `DataErrorRecord` must fail the codegen consistency check.
- A test that uses `try { } catch (...)` inside data engine code
  must fail under `-fno-exceptions`.
- A test that handles `ErrorTag` via `switch` without a `default:`
  case and missing one arm must fail under `-Wswitch -Werror`.

### 11.6 Cross-aggregate uniformity (sibling-design integration)

Live under `tests/data/errors/golden_per_source/`, one golden file
per row in §3.2. Each golden:

1. Names the sibling-design source (e.g.
   `golden_envelope_serdes_schema_unknown.json`).
2. Provides a deterministic input fixture (a byte buffer, a registry
   state, a `.fory` source).
3. Expects a single `data::Error` value with the §3.2 row's arm and
   payload fields.

The golden harness exercises the §3.2 table as a single integration
test: every row that names a sibling design's detection site is
verified against that sibling's actual implementation. A row that
the sibling's implementation cannot produce is a documentation drift
and fails CI.

## 12. Open questions

- [OPEN] Promotion of `SourceHashMismatch` to a first-class
  `ErrorTag` arm (value 10). Proposed by
  `schema-registry-design.md` §3.8; deferred in §3.1.1 of this
  design pending the SPEC §10 amendment that will land alongside
  the `task-breakdown-data-schema-registry` plan. Until the
  amendment ships, sibling code surfaces drift via
  `SchemaMigrationFailure` with the regression-shape payload
  (`step_from == step_to` against a registered `(FQN, version)`
  pair). Resolution gate: the schema-registry implementation plan
  decides whether the second consumer (operator log analytics) needs
  typed dispatch beyond the existing `SchemaMigrationFailure` arm.
  No data-error-aggregate-side residue if deferred.

- [OPEN] Promotion of `BufferTooSmall` to a first-class `ErrorTag`
  arm. Proposed by `envelope-serdes-design.md` §10; deferred in
  §3.1.1. Sibling code surfaces it via `DeserializeError` with
  `at.offset = dst.size()` and a `WireSite{schema = expected_fqn}`
  so the symptom round-trips. Resolution gate: the envelope-serdes
  implementation plan; the second consumer is the editor's "save
  to ring buffer" path, which is post-MVP. No residue if deferred.

- [OPEN] Promotion of `SchemaSourceMismatch` to a first-class
  `ErrorTag` arm. Proposed by `envelope-serdes-design.md` §10;
  deferred in §3.1.1. Sibling code surfaces it via
  `DeserializeError` with `at.schema` populated. Same gate as
  `SourceHashMismatch`. No residue if deferred.

- [OPEN] Should the `source_tag` TLS slot widen from
  `eastl::string_view` (literal pointer + length) to a small
  `eastl::array<char, 32>` so the slot is self-contained and
  signal-safe-by-construction (no `.rodata` resolver hazard)?
  Trade-off is a doubled TLS slot footprint (32 B vs 16 B) for
  marginal hardening. Re-opened by the `task-breakdown-error-perf`
  plan when CI gate timings land. No data-error-aggregate-side
  residue if deferred.

- [OPEN] Should `DataErrorRecord::arm_tag` be a generated
  enumeration from `ErrorTag`'s declaration (codegen-driven) or a
  hand-maintained `u16` index list? Codegen is tighter
  (drift-impossible) but introduces a build-time dependency between
  `glibre-types-codegen` and the data plugin's TU. Hand-maintained
  is simpler at MVP but invites drift. Resolution gate: the
  fory-codegen plan that introduces
  `data/schemas/data/DataErrorRecord.fory` decides at authoring
  time. Until then the schema's `arm_tag` is hand-maintained with a
  unit test asserting parity (§11.1 row "closed_sum_size_test").

- [OPEN] Migration-arena exhaustion (`ArenaExhausted` candidate
  from the parent #729 issue body). Per `perf-budget.md`
  §"Allocator Rules" #6, arena overflow is a `core::Error::OutOfBudget`
  arm carrying the migration-arena tag in `ErrorContext::detail`.
  Audit note: confirm during the migration-dispatcher implementation
  plan that arena overflow does NOT leak into a `data::Error` arm
  by accident. Resolution gate: the dispatcher plan's tests assert
  arena-overflow surfaces as `core::Error::OutOfBudget`, not as
  any `data::Error` arm.

- [OPEN] Crash-handler schema replay (post-MVP). Should
  `data::Error` construction be fully signal-safe (so a SIGSEGV
  capture path can serialise an in-flight `data::Error` via
  `DataErrorRecord`)? Currently §6.2 documents that construction
  *is* signal-safe; a future spike may exercise the full path
  (signal handler → carrier → trace → assert) and may discover
  hazards in the `glibre::types::data` `string_view` resolution.
  Resolution gate: the platform-error E2E crash-capture fixture
  extends to data-error in a follow-up spike. No data-error-
  aggregate-side residue at MVP.

- [OPEN: exhaustiveness-ci-gate] §3.2 exhaustiveness obligation (one
  row per sibling failure surface) is currently normative-prose-only.
  CI enforcement via §11.6 golden-per-source tests lands with the
  first implementation plan PR that authors a sibling aggregate
  (schema-registry or envelope-serdes). That plan PR must author the
  §11.6 fixture harness at `tests/data/errors/golden_per_source/` and
  assert that the golden count equals the row count in the §3.2 table.
  Until then, drift is possible and must be caught in code review.

- [OPEN: tls-layout-decision] If a future spike concludes that
  `data` and `platform` should share a single `__thread` struct for
  footprint or atomicity reasons, a cross-context decision record
  `reviews/decisions/tls-layout.md` must be authored and approved
  before introducing the shared-struct coupling. Both `data` and
  `platform` SPEC.md files must cite the record. The record does not
  exist today; the §5.4 `source_tag` slot is therefore a separate
  data-context-owned TLS variable.
