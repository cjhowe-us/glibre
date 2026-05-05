# Middleman-Dylib Detailed Design

> Detailed design for the `data` context's `Middleman` aggregate
> (`glibre-types.dylib`, SPEC §4.3) — the single, per-process shared
> library hosting every `RegistryEntry` plus the `extern "C"` entry-
> point set every plugin and host binary links against. Refines
> §4.3 / §4.10 / §6.1.2 of `specs/data/SPEC.md` and the
> `reviews/decisions/fory-codegen.md` decision record.
> All conclusions re-derived; harmonius prior art (`harmonius/docs/
> design/core-runtime/`) cited as research input only.

Refs: spike #734 — `[SPIKE] design-data-middleman-dylib-detailed`.
Parent sub-epic #729. Sibling task-breakdown spike blocked-by this
deliverable.

## 1. Purpose

`glibre-types.dylib` is the **one** shared library every plugin dylib
and every host binary (runtime, editor, tools) links to obtain a
canonical layout for every persistent type in the engine. Its single
responsibility is **owning the descriptor table**: a static, FQN-sorted
array of `RegistryEntry` records produced by `glibre-foryc`, exposed
through a fixed set of `extern "C"` symbols, gated by a single
compiled-in `AbiHash`. Plugins do not link Apache Fory, do not link
`blake3`, and do not include any header `glibre-foryc` wrote except
through this dylib (SPEC §4.10 inv. 4).

The middleman is a **build artifact**, not a runtime mutator. Every
byte it carries is decided at link time:

- The `RegistryEntry` array is `constinit`, sorted by FQN at codegen
  emission, and `const` after the static-init pass that wires
  per-`FQN` migration tables (SPEC §4.5 inv. 4).
- The `AbiHash` literal is a `constexpr const char*` embedded by
  `glibre-foryc` (SPEC §4.4 inv. 4; SPEC §6.4).
- The exported C symbols form a closed set whose signatures are
  patch-stable (SPEC §4.3 inv. 2).

What the middleman explicitly refuses to own:

- **Runtime serdes orchestration** — the `MigrationDispatcher`
  *implementation* lives inside the dylib (SPEC §6.1.2,
  `data/runtime/src/migration_dispatcher.cpp`), but its design is
  spec'd separately (#736); this document treats the dispatcher as a
  consumer of the middleman's descriptor table.
- **The schema registry's lookup design** — `SchemaRegistry::lookup`
  semantics are spec'd separately (#730); this document only fixes
  *how the descriptor table is laid out* in the dylib's `.rodata`.
- **The codegen pipeline** — `glibre-foryc`'s lex/parse/validate/emit
  stages are spec'd separately (#732); the middleman is the *output
  consumer* of stage 4. This design pins the linker's view of those
  outputs, not the codegen's view of its inputs.
- **Plugin-side responsibilities** — re-exporting `glibre_plugin_abi_hash`
  and embedding the manifest blob are the plugin's `manifest.cpp`
  TU's job (`reviews/decisions/plugin-abi.md` §"Plugin file shape"
  step 3); the middleman provides the *value* the plugin re-exports.
- **Hot-reload of itself** — the middleman is locked at process start
  and never reloaded mid-session in MVP (SPEC §8.3 default; §8 below).

The middleman's SRP boundary is sharp: if the **dylib's link layout**,
the **descriptor-table memory shape**, the **`extern "C"` symbol set
and signatures**, the **`AbiHash` export mechanic**, the **static-init
order that wires the per-`FQN` migration tables**, or the
**process-lifetime invariant that makes the descriptor table immutable**
change, this design changes. Anything else is out of scope.

## 2. Requirements Coverage

Mapping of harmonius requirements (`R-1.6.*`,
`harmonius/docs/design/core-runtime/`) to glibre MVP refusal-or-coverage
for the middleman aggregate. Every entry is independently re-derived;
harmonius is research input only.

| Harmonius clause                                               | Glibre disposition (MVP) | Coverage site                                                                                                  |
|----------------------------------------------------------------|--------------------------|----------------------------------------------------------------------------------------------------------------|
| R-1.6.9 single shared types library across all plugins         | **Covered**              | One `.dylib` per process; SPEC §4.3 inv. 1; §3.1 / §3.2 below.                                                 |
| R-1.6.7 ABI hash gate at plugin admission                      | **Covered**              | `glibre_types_abi_hash()` literal embedded by codegen; §4.3 below; SPEC §4.4 / §6.4.                           |
| R-1.6.5 stable C-ABI surface across plugin reload              | **Covered**              | `extern "C"` only at the dylib boundary; no class templates cross; §4.1 / §4.3 below.                          |
| R-1.6.10 per-process layout uniqueness                         | **Covered**              | RTLD_LOCAL on plugin loads + global `glibre-types.dylib`; refuse two middleman builds in one process; §3.6.    |
| R-1.6.6 reload-safe descriptors (no plugin-fragmenting layout) | **Covered**              | Descriptors live *outside* every plugin; plugin reload never edits the table; §5 / §8 below.                   |
| R-1.6.4 single static-init order across the type set           | **Covered**              | Codegen emits the registry TU last in link order; static-init populates `migrations` table once; §3.5 below.   |
| R-1.6.8 runtime reflection of registered types                 | **Refused**              | PHILOSOPHY §6 forbids runtime reflection in shipping. `ReflectionBlob` (SPEC §4.9) is the static substitute, gated off in shipping builds. The middleman never offers an introspection API in shipping. |
| R-1.6.11 dynamic registration of types after process start     | **Refused**              | The descriptor table is `const` after static-init (SPEC §4.5 inv. 4). Adding a type requires a rebuild of the dylib; this is an explicit ABI surface, not a runtime extension point. |
| R-1.6.12 per-plugin descriptor partitioning                    | **Refused (collapsed)**  | All plugins consume one global table keyed by FQN. Per-plugin partitions would re-introduce the layout-fragmentation problem the middleman exists to solve; the FQN namespace already partitions ownership at authoring time (`data/schemas/<ctx>/`).                       |
| R-1.6.13 rebuild abi-hash from plugin set at runtime           | **Refused**              | The hash is a build-time constant of the *contract*, not the loaded set (SPEC §4.4 inv. 4, §6.4). Recomputing at runtime would defeat the gate by definition.                                                                                                                |
| R-1.6.14 self-reload of types library                          | **Refused (deferred)**   | SPEC §8.3 default — engine restart on middleman change. The §8.3 contract exists for a future spike; MVP does not enable it.                                                                                                                                                  |

Coverage rule: every harmonius clause above either lands in this design
or is refused with a one-line rationale. No silent drops.

Glibre-native obligations added beyond harmonius:

- **One canonical descriptor per type per process.** The hash gate
  (SPEC §4.4) is necessary but not sufficient — the design here
  pins the *layout* of the descriptor table so plugin-A and
  plugin-B that both serialize a `Transform` cannot disagree on
  field offsets even when both link the same `glibre-types.dylib`
  build (§3.4 inv. 1 below).
- **Read-only after init.** The dispatcher holds the registry by
  `const&`; no public symbol mutates the table; no allocation of
  any kind happens after the dynamic linker finishes static-init
  (SPEC §4.3 inv. 5; §6 below).
- **Refusal on duplicate-middleman load.** Two distinct
  `glibre-types.dylib` builds in one process is undefined; the
  plugin loader refuses to bring up such a process at startup
  (SPEC §4.3 inv. 1; §3.6 below).

## 3. Detailed Model

### 3.1 Aggregate composition

```text
glibre-types.dylib  (one per process; SONAME = libglibre-types.<MAJ>.dylib)
├── .text
│   ├── glibre_types_abi_hash               (extern "C" — §4.3)
│   ├── glibre_types_register_migration     (extern "C" — §4.3)
│   ├── glibre_types_serialize_<fqn>        (extern "C" — N copies, §4.3)
│   ├── glibre_types_deserialize_<fqn>      (extern "C" — N copies, §4.3)
│   ├── SchemaRegistry::lookup              (C++ method, instance-only)
│   ├── SchemaRegistry::entries             (C++ method, instance-only)
│   ├── SchemaRegistry::instance            (C++ method, returns const&)
│   ├── MigrationDispatcher                 (TU-private; consumer of registry)
│   └── Envelope<T> trampolines             (TU-private; per-type)
├── .rodata
│   ├── kAbiHashLiteral[65]                 ("…64-hex…\0", §3.7)
│   ├── kRegistryEntries[N]                 (sorted, constinit, §3.4)
│   ├── kReflectionFields[Σ]                (per-type tag-sorted arrays)
│   ├── kReflectionBlobs[N]                 (one per type; null in shipping)
│   ├── kFqnInternTable[N]                  (string_view storage, §3.4 inv. 3)
│   └── kManifestBlob_<plugin>[K_p]         (per-discovered-plugin §3.8 + §3.9)
├── .bss
│   └── kPerFqnMigrationTable[N]            (filled at static-init, §3.5)
└── exports (otool -l output is exactly the list in §4.3)
```

Plugin dylibs link `glibre-types.dylib` **publicly** in CMake; the
generated header set lives under `${GEN_DIR}/include/` (§6.1.3 of SPEC
§6.1.3) and is added to plugins' `target_include_directories` by the
CMake function `glibre_plugin()` introduced in the build-graph spike.
No plugin links Apache Fory, `blake3`, or any other private dependency
of the middleman (SPEC §4.10 inv. 4).

### 3.2 Dylib link layout

`glibre-types.dylib` compiles from three source families, in this
order:

1. **`data/runtime/src/*.cpp`** — hand-written runtime translation
   units (`schema_registry.cpp`, `envelope.cpp`,
   `migration_dispatcher.cpp`, `register_migration.cpp`,
   `arena.cpp`, `abi_hash.cpp`, `plugin_manifest.cpp`; SPEC §6.1.2).

   TU-to-aggregate mapping — which SPEC aggregate each TU implements and
   its single reason to change:

   | TU                          | SPEC aggregate         | Reason to change                                                  | Changes with        |
   |-----------------------------|------------------------|-------------------------------------------------------------------|---------------------|
   | `schema_registry.cpp`       | SchemaRegistry (§4.5)  | Per-entry record shape — fields added/removed from `RegistryEntry`| §4.3 / §4.5 / #730 |
   | `envelope.cpp`              | SchemaRegistry (§4.5)  | `Envelope<T>` call-site protocol — out-pointer layout or error shape | §4.1 / §4.3      |
   | `migration_dispatcher.cpp`  | Middleman (§4.3)       | Dispatcher consumer — chains applied to `RegistryEntry::migrations`; behavioral rules belong to #736 | #736 |
   | `register_migration.cpp`    | Middleman (§4.3)       | `.bss` storage + `glibre_types_register_migration` C-ABI entry point signature | §4.3 / #736 |
   | `arena.cpp`                 | SchemaRegistry (§4.5)  | Per-call scratch allocator shape used by Envelope<T> during deserialization | §4.5         |
   | `abi_hash.cpp`              | Middleman (§4.3)       | Dylib binary contract — ABI hash trampoline signature or storage form | §4.4 / §6.4      |
   | `plugin_manifest.cpp`       | Middleman (§4.3)       | Plugin discovery set — how manifest blobs are placed in the dylib | §6.5 / §3.9        |
   | `static_init_check.cpp`     | Middleman (§4.3)       | Invariant assertions — Phase C validator rules derived from §4.5  | §3.5 / §4.5 / §4.7 |

   An implementer touching §4.3 changes (dylib binary contract, ABI hash,
   symbol-export set, manifest placement) edits `abi_hash.cpp`,
   `register_migration.cpp`, or `plugin_manifest.cpp`. An implementer
   rotating the SchemaRegistry lookup design (#730) edits
   `schema_registry.cpp`. An implementer acting on #736's behavioral
   specification edits `migration_dispatcher.cpp` and possibly
   `register_migration.cpp`. No TU spans more than one aggregate.
2. **`data/codegen-output/src/<ctx>/<Type>.cpp`** — per-type
   serializer / deserializer trampolines emitted by `glibre-foryc`
   (SPEC §6.2 stage 4 step 1).
3. **`data/codegen-output/src/_registry.cpp`**,
   **`data/codegen-output/src/_abi_hash.cpp`**, and
   **`data/codegen-output/src/_manifest_<plugin>.cpp`** —
   single-TU emissions that close the link (SPEC §6.2 stage 4
   steps 3, 4, 5).

The CMake target follows `reviews/decisions/fory-codegen.md`
§"CMake Integration" verbatim:

```cmake
add_library(glibre-types SHARED
    ${DATA_RUNTIME_SRCS}        # data/runtime/src/*.cpp
    ${GEN_TYPE_SRCS}            # data/codegen-output/src/<ctx>/*.cpp
    ${GEN_REGISTRY_SRC}         # data/codegen-output/src/_registry.cpp
    ${GEN_ABI_HASH_SRC}         # data/codegen-output/src/_abi_hash.cpp
    ${GEN_MANIFEST_SRCS})       # data/codegen-output/src/_manifest_*.cpp

set_target_properties(glibre-types PROPERTIES
    SOVERSION       1                       # SONAME bump (SPEC §4.3 inv. 3)
    VERSION         1.0.0
    CXX_VISIBILITY_PRESET hidden            # default-hidden; explicit exports
    VISIBILITY_INLINES_HIDDEN ON
    INSTALL_RPATH   "@loader_path")         # plugins resolve us by rpath

target_include_directories(glibre-types
    PUBLIC                                  # consumers see public headers
        "${CMAKE_SOURCE_DIR}/data/runtime/include"
        "${GEN_DIR}/include")

target_link_libraries(glibre-types
    PRIVATE                                 # never leaks to plugins
        Fory::fory                          # SPEC §4.10 inv. 4
        blake3::blake3)

add_dependencies(glibre-types glibre-types-codegen)
```

Visibility discipline:

- The default visibility on the target is `hidden`; only the
  `extern "C"` symbols listed in §4.3 plus the public C++ methods of
  `SchemaRegistry` carry `__attribute__((visibility("default")))` via
  the `GLIBRE_TYPES_API` macro defined in
  `data/runtime/include/glibre/types/visibility.hpp`.
- `Fory::fory` and `blake3::blake3` are `PRIVATE`; their symbols are
  hidden by `-fvisibility=hidden` and never appear in the dylib's
  exported set. Plugins linking `glibre-types` see only the
  `glibre::types::*` surface.
- `SCHEMA_REGISTRY_*` symbols are not exported by name; consumers
  obtain the registry only through `SchemaRegistry::instance()`,
  which is itself an exported symbol.

### 3.3 Symbol export set

The dylib exports exactly the following symbols (defaulting to hidden
otherwise; `nm -gU --defined-only libglibre-types.dylib | sort` is the
audited set):

```text
# C-ABI surface (extern "C")
_glibre_types_abi_hash                      (T)
_glibre_types_last_register_error           (T)   # TLS getter — §4.1 TLS surface; §4.3
_glibre_types_register_migration            (T)
_glibre_types_serialize_<fqn>               (T)   # one per registered FQN
_glibre_types_deserialize_<fqn>             (T)   # one per registered FQN

# C++ surface (mangled; consumed only via the public headers in §5 of SPEC)
_ZN6glibre5types14SchemaRegistry8instanceEv (T)   # SchemaRegistry::instance
_ZNK6glibre5types14SchemaRegistry6lookupENS0_8SchemaIdE (T)
_ZNK6glibre5types14SchemaRegistry7entriesEv (T)
```

`_glibre_types_last_register_error` is included in the exported set
because the `GLIBRE_REGISTER_MIGRATION` macro in generated
`<Type>_migrations.hpp` headers resolves it at link time across
different translation units (the macro lives in TUs that belong to
owning contexts, not to the middleman itself; they must call through
the dylib boundary). If this symbol were internal-linkage only, the
macro's failure-path read would fail to link in owning-context TUs.
The CI test `middleman.exported_symbol_set_audit` (§11.1) must
include this symbol in `EXPECTED_EXPORTS.txt` so that an
accidentally-hidden `last_register_error` fails the PR.

The per-FQN serialize / deserialize symbols are an *additive* set:
adding a new schema appends two new symbols (one for each direction)
without bumping SONAME (SPEC §4.3 inv. 3). Removing a schema removes a
pair of symbols, which is a layout-breaking change and bumps SONAME.

CI-side guardrail: `tests/data/middleman/exported_symbols.test`
diffs the live `nm` output against a checked-in `EXPECTED_EXPORTS.txt`
generated alongside `glibre-foryc`'s output. Drift either way (extra
or missing symbols) fails the PR. This catches both accidentally-
exported private helpers and accidentally-hidden public ones.

### 3.4 Descriptor-table memory layout

The registry's contiguous array of `RegistryEntry` lives in
`.rodata`, emitted as:

```cpp
// data/codegen-output/src/_registry.cpp (illustrative; one TU)

namespace glibre::types::detail {

extern "C" const char* glibre_types_serialize_glibre_core_Transform(/*…*/) noexcept;
extern "C" const char* glibre_types_deserialize_glibre_core_Transform(/*…*/) noexcept;
// … one declaration pair per FQN …

// FQN intern table — one entry per registered type, byte-sorted on FQN.
constinit const char* const kFqnIntern[] = {
    "glibre.core.Asset",            // 0
    "glibre.core.Entity",           // 1
    "glibre.core.PluginManifest",   // 2
    "glibre.core.Transform",        // 3
    // … N total …
};

// Per-type RegistryEntry array; same indexing as kFqnIntern (§4.5 inv. 3).
constinit const RegistryEntry kRegistry[] = {
    // [0] glibre.core.Asset
    { .schema      = SchemaId{eastl::string_view{kFqnIntern[0]}},
      .version     = 1,
      .source_hash = { /* 32 bytes from canonicalize(...) */ },
      .serialize   = &glibre_types_serialize_glibre_core_Asset,
      .deserialize = &glibre_types_deserialize_glibre_core_Asset,
      .migrations  = {},                            // wired at static-init
      .reflection  = &kReflectionBlobAsset },        // null in shipping
    // [1] glibre.core.Entity   …
    // [2] glibre.core.PluginManifest …
    // [3] glibre.core.Transform …
    // …
};

constinit const std::size_t kRegistryCount = std::size(kRegistry);

}  // namespace glibre::types::detail
```

Memory-layout invariants (the load-bearing guarantees the descriptor
table promises every consumer):

1. **One canonical descriptor per type per process.** The hash gate
   in §3.7 ensures every plugin links the *same* middleman build;
   this layout invariant ensures that build's `kRegistry` array
   has one entry per FQN, with one set of function-pointer slots
   and one source-hash. Two plugins that both serialize a
   `Transform` resolve to the same `&kRegistry[3]` at link time
   (SPEC §4.10 inv. 1).
2. **FQN-sorted ascending.** Entries appear in canonical Unicode
   code-point order over `FQN` — the same ordering `AbiHash`
   consumes (SPEC §4.4 inv. 1, §4.5 inv. 3, §6.2 stage 4 step 3).
   `SchemaRegistry::lookup` is a binary search over this array
   (SPEC §5 §registry.hpp); the spec'd `O(log N)` cost falls out
   of the layout.
3. **FQN string storage is the intern table.** Each
   `RegistryEntry::schema.fqn` is an `eastl::string_view` over a
   null-terminated literal in `kFqnIntern`, which itself lives in
   `.rodata`. The view's `.data()` is process-lifetime stable
   (SPEC §4.5 inv. 3); it never points into a plugin's address
   space, so no plugin reload can dangle it.
4. **Function-pointer slots are non-null at static-init exit.**
   Codegen emits both `serialize` and `deserialize` for every type
   (SPEC §4.5 inv. 2); the linker resolves them within the same
   dylib. A null function pointer at static-init exit is an
   invariant violation that the static-init validator catches
   (§3.5 below).
5. **`migrations` is filled in `.bss` at static-init.** The
   `RegistryEntry::migrations` field is an
   `eastl::span<const MigrationEntry>` whose `.data()` points into
   the per-FQN `kPerFqnMigrationTable[N]` storage in `.bss`, filled
   by the static-init pass that consumes
   `glibre_types_register_migration` calls (§3.5). Before that
   pass completes, the span is empty; after, it is `const` for the
   process lifetime (SPEC §4.5 inv. 4).
6. **`reflection` is `nullptr` in shipping builds.** Codegen emits
   `kReflectionBlob<Type>` arrays unconditionally but wires them
   into `RegistryEntry::reflection` only when the build flag
   `GLIBRE_TYPES_REFLECTION=ON` is set (SPEC §4.9 inv. 5). Shipping
   profiles set the flag off; tools / editor profiles set it on.
   Consumers that may run in shipping check for null before
   dereferencing.
7. **The array itself is `constinit`.** No dynamic allocation, no
   user-defined constructors with side effects, no
   `std::vector`-in-disguise. The dynamic linker's image-loading
   pass produces the array fully-formed; static-init only wires
   `migrations` and verifies invariants (§3.5).

### 3.5 Static-init order

The middleman's static-init runs in three phases, ordered by codegen-
emitted file precedence (`_registry.cpp` before any
`<Type>_migrations.hpp` consumer; `glibre-foryc` enforces this by
emitting the registry TU first in the CMake source list):

**Phase A — Registry materialization.** The dynamic linker loads
`kRegistry` and `kFqnIntern` from `.rodata`; no executable code runs.
After this phase the registry's `kRegistryCount` entries are
addressable, FQN-sorted, with all function-pointer slots populated and
all `migrations` spans empty.

**Phase B — Migration registration.**

*(a) Middleman's contribution — storage slots and C-ABI entry point.*
The middleman supplies the `.bss` storage (`kPerFqnMigrationTable[N]`,
one slot per registered FQN) and the single exported C-ABI entry point
`glibre_types_register_migration` (§4.3) into which owning contexts
call at static-init time. The middleman's `register_migration.cpp` body
collects incoming calls into `kPerFqnMigrationTable[N]`; once all
calls have run (end of `__cxx_global_var_init`), the runtime patches
each `RegistryEntry::migrations` span to point at its FQN's slice of
that storage. This is the *only* mutation of the registry the middleman
permits, and it terminates before `main()` begins (SPEC §4.3 inv. 5).

*(b) Registration protocol — behavioral rules belong to #736.*
The behavioral rules that govern what constitutes a valid
`glibre_types_register_migration` call — validation of `(from, to)`
version pairs, duplicate-registration detection, ordering constraints,
partial-chain handling, and the error path via `glibre_types_last_register_error` — are the
`MigrationDispatcher` design's responsibility and are spec'd separately
in #736. This document purposely does not enumerate those rules: if
#736 changes the validation protocol (arena-reset semantics between
steps, handling of partial chains, or error-payload fields), only §3.5
(b) and `migration_dispatcher.cpp` / `register_migration.cpp` change,
not the descriptor-table layout documented in (a). The SRP boundary is
here: the middleman owns the storage and the entry point; the
dispatcher design (#736) owns what is and is not a valid call.

**Phase C — Static-init validator.** A single `__attribute__((constructor(65535)))`
function (`data/runtime/src/static_init_check.cpp`) runs after every
other static-init in the dylib. It walks `kRegistry` once and
asserts:

1. Every `serialize` and `deserialize` pointer is non-null
   (SPEC §4.5 inv. 2).
2. Every `migrations` span is either empty (if `version == 1`) or has
   length `version - 1` (SPEC §4.10 inv. 5).
3. Within each `migrations` span, `from_version` ascends 1, 2, …,
   `version - 1` with no gaps and no back-edges (SPEC §4.7 inv. 1, 2;
   `MigrationCycle` arm in §10).
4. No two entries share an FQN (SPEC §4.5 inv. 1;
   `SchemaRegistryConflict`).
5. Entries are FQN-sorted ascending (§3.4 inv. 2).

Failure at any check is a fatal `glibre::log_error(err, fatal)` +
`std::abort()` — the spine refuses to run a build whose own
descriptor table contradicts §4.

After Phase C, no code in the dylib mutates the registry. The
dispatcher holds it by `const&`; the public C++ surface
(`SchemaRegistry::lookup`, `entries`, `instance`) is read-only by
construction (SPEC §4.5 inv. 4).

### 3.6 Per-process uniqueness

The plugin loader's startup path (SPEC §4.3 inv. 1) refuses any
process that loads two distinct `glibre-types.dylib` builds. The
mechanism:

1. The host binary (runtime / editor / tools) statically resolves
   `glibre_types_abi_hash` at link time. The literal it gets baked in
   becomes the host's compiled-in hash.
2. At plugin admission, the loader calls `dlsym(handle,
   "glibre_plugin_abi_hash")` and compares the returned literal
   byte-for-byte with the host's. Mismatch ⇒
   `core::Error::PluginAbiHashMismatch` (`reviews/decisions/plugin-abi.md`
   §"Loader Sequence" step 4; SPEC §4.4 inv. 3).
3. The loader additionally verifies that
   `dlsym(handle, "glibre_types_abi_hash")` resolves to the *same
   address* as the host's bound symbol — i.e. the plugin and the
   host both refer to the *one* `glibre-types.dylib` mapped into the
   process. A mismatched address means the dynamic linker resolved
   two distinct mappings (e.g. via `DYLD_LIBRARY_PATH` shenanigans);
   the loader refuses with `core::Error::PluginAbiHashMismatch`
   carrying `ErrorContext::detail = "duplicate middleman in process"`.

This is the §3.6 collapse: the hash gate (#1, #2) catches builds with
mismatched contracts; the address gate (#3) catches builds with
matching contracts but distinct mappings. The two together make a
two-middleman process impossible to bring up.

The address-equality check is omitted for the host's own bindings
(they trivially match themselves) and runs only on plugin-side
symbols. The check is constant-time and runs once per plugin at
admission, never per-frame.

### 3.7 `glibre_types_abi_hash` storage and emission

The `AbiHash` literal lives in `.rodata` as a 65-byte array:

```cpp
// data/codegen-output/src/_abi_hash.cpp (one TU)

namespace glibre::types::detail {
// 64 lowercase hex chars + NUL terminator. Computed by glibre-foryc
// per SPEC §6.4 from canonicalize(<every schema>).
constinit const char kAbiHashLiteral[65] =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
}

extern "C" const char* glibre_types_abi_hash() noexcept {
    return ::glibre::types::detail::kAbiHashLiteral;
}
```

The trampoline lives in `data/runtime/src/abi_hash.cpp` (hand-written,
not codegenned — only the literal is codegenned). The export is
`noexcept`, allocates nothing, and is callable from static-init
contexts (SPEC §6.4); the loader can call it before any other
middleman symbol resolves.

Plugins re-export the identical 64-char string under
`glibre_plugin_abi_hash`, captured at the plugin's compile time
against the same `glibre/types/abi_hash.hpp` header
(`reviews/decisions/plugin-abi.md` §"Plugin file shape" step 3; SPEC
§4.4 inv. 3). The plugin's re-export looks like:

```cpp
// plugins/<name>/src/manifest.cpp (codegen-emitted by glibre-foryc)
#include <glibre/types/abi_hash.hpp>
namespace { constinit const char kPluginAbiHashLiteral[65] = "<same 64 hex>"; }
extern "C" const char* glibre_plugin_abi_hash() noexcept {
    return kPluginAbiHashLiteral;
}
```

Hash equality is byte-string equality on 64 ASCII characters — no
parsing, no `strcmp`-with-locale concerns. The loader uses
`std::memcmp(host, plugin, 64) == 0` (SPEC §4.4 inv. 3).

### 3.8 Per-context type partitioning

The single registry array is keyed by FQN and never partitioned by
plugin or context. This is the §3.8 collapse: per-plugin partitions
would re-introduce the layout-fragmentation problem the middleman
exists to solve, and the FQN namespace already carries the partition
information at the authoring layer (`data/schemas/<ctx>/<Type>.fory`
maps to `<ctx>::<Type>` ⇒ FQN `glibre.<ctx>.<Type>`).

What this means concretely:

1. A plugin that owns `glibre.physics.RigidBody` and a plugin that
   owns `glibre.render.Mesh` both link the *same* `kRegistry`; their
   types occupy *different* slots in the array. Neither plugin sees
   any per-plugin filtering at lookup time.
2. The loader's `PluginManifest.components` declares which FQNs a
   plugin *owns* (`reviews/decisions/plugin-abi.md` §"Plugin
   Manifest Schema" → `ComponentDecl`); the middleman is unaware of
   ownership. The loader uses the manifest for compensating-rollback
   bookkeeping (specs/core/plugin-loader-design.md §6.2), not the
   middleman.
3. Hot-reloading a plugin neither adds nor removes registry entries
   (SPEC §4.5 inv. 4); the plugin's owned FQNs were registered in
   the middleman build and remain valid across plugin reload — this
   is the property §1 calls out: "plugin reload doesn't fragment
   ABI."
4. Two plugins claiming ownership of the same FQN is a conflict the
   *loader* catches via `PluginManifest.components`
   (`PluginNameCollision` / a future `ComponentOwnerConflict` arm);
   the middleman has no opinion, because both plugins linked the
   same registry slot at build time.

The middleman's view: one process, one dylib, one descriptor table,
one FQN keyspace. Partitioning is the loader's concern and lives
upstream of any descriptor-table consumer.

### 3.9 Manifest-blob placement — two copies and their reconciliation

`kManifestBlob_<plugin>[K_p]` (shown in §3.1) and the identical blob
in the plugin's own `.rodata` are **two copies of the same byte
sequence**. This is intentional, not an oversight. The two-copy
design follows directly from the separate responsibilities of
`glibre-foryc` and the plugin-abi decision record
(`reviews/decisions/plugin-abi.md` §"Plugin file shape" step 2 +
§"Consequences" bullet 1 + SPEC §6.5):

1. **Copy A — the plugin's own `manifest.cpp` (in the plugin's dylib).** 
   `glibre-foryc` walks `plugins/*/plugin.fory` and emits one
   `manifest.cpp` per discovered plugin. That TU is compiled into the
   **plugin's** dylib (not the middleman). It exports
   `glibre_plugin_manifest` / `glibre_plugin_manifest_size` /
   `glibre_plugin_abi_hash` — the three C symbols the loader reads
   *before any plugin C++ code runs*
   (`reviews/decisions/plugin-abi.md` §"Loader Sequence" step 3).
   This copy lets the loader deserialize the manifest from the plugin's
   own address space using only `dlsym` + a raw byte-span read.

2. **Copy B — `kManifestBlob_<plugin>` in the middleman's `.rodata`.**
   The same `glibre-foryc` invocation also emits
   `data/codegen-output/src/_manifest_<plugin>.cpp` TUs that compile
   into the **middleman** (SPEC §6.5; SPEC §6.2 stage 4 step 5). This
   copy gives the middleman's `Envelope<PluginManifest>` a pre-baked
   blob it can round-trip as a sanity baseline: the loader can compare
   the blob it read from the plugin's own `.rodata` (Copy A) against
   the blob compiled into the middleman (Copy B) to detect a mismatch
   that would indicate the plugin's `manifest.cpp` was generated by a
   different `glibre-foryc` invocation than the middleman's. In practice
   the ABI-hash gate (§3.6) already catches this case, so the middleman's
   Copy B is primarily a determinism artifact and a cross-check baseline
   for the codegen pipeline; it is not an authoritative runtime oracle.

**Reconciliation rule (normative):**

- Copy A (plugin's dylib) is the *loader's* authoritative read. The
  loader calls `dlsym` on Copy A's exports and deserializes from there.
- Copy B (middleman's `.rodata`) is a *codegen cross-check* only. If
  the byte sequences disagree, the codegen pipeline is corrupt (the
  middleman and the plugin were not produced from the same `plugin.fory`
  input); the `abi_hash` gate catches this before the loader ever reads
  Copy A.
- Adding a new plugin appends one new `_manifest_<plugin>.cpp` to the
  middleman's source list, triggering a middleman relink. This is the
  expected consequence of the codegen-driven design (SPEC §6.5): the
  middleman must know every plugin's FQN set at configure time because
  `glibre-foryc` walks `plugins/*/plugin.fory` at that point. The dylib's
  compile-time knowledge of the plugin set is not a circular dependency —
  it is the intended build-graph topology where the codegen tool is the
  single driver of both the middleman and the per-plugin manifests.

## 4. Public Surface

### 4.1 ABI shape — `extern "C"` only

The middleman's dylib boundary is C-ABI by construction (SPEC §4.3
inv. 4). No C++ class template, no `std::expected`, no
`eastl::variant` crosses the boundary as a return value of an exported
symbol. This is the §4.1 collapse: `std::expected<T, E>` is not a
C-ABI type (its layout is implementation-defined across libc++
versions), so we use *plain enum returns* + *out-pointers* at the
exported boundary, and lift back into `std::expected` inside thin
wrapper templates that resolve to those `extern "C"` calls at link
time (SPEC §4.3 inv. 4 verbatim).

The chosen ABI shape — two distinct plain-enum types, chosen per the
number of outcomes each call admits:

```cpp
// data/runtime/include/glibre/types/abi_shape.hpp

namespace glibre::types {

// Nine-arm plain-enum return for serialize/deserialize trampolines.
// Tag values match data::ErrorTag (SPEC §10.1) on the failure path.
// Used by glibre_types_serialize_<fqn> and
// glibre_types_deserialize_<fqn> which can surface multiple error arms.
enum class CStatus : std::uint16_t {
    Ok                       = 0,
    AbiHashMismatch          = 1,   // mirrors data::ErrorTag::AbiHashMismatch
    SchemaMigrationFailure   = 2,
    DeserializeError         = 3,
    SchemaRegistryConflict   = 5,
    SchemaUnknown            = 6,
    MigrationStepMissing     = 7,
    MigrationCycle           = 8,
    EnvelopeTruncated        = 9,
    // (4 / ReservedTagViolation does not cross — codegen-time only.)
};

// Two-arm plain-enum return for glibre_types_register_migration.
// SPEC §5 migration.hpp (data::RegisterStatus) specifies exactly two
// outcomes for registration: success or duplicate FQN conflict.
// The narrower type is intentional: register_migration cannot surface
// DeserializeError, MigrationCycle, EnvelopeTruncated, etc. — those
// arise only in the serialize/deserialize trampolines. Using the
// full nine-arm CStatus here would mislead implementers about which
// arms are reachable and would diverge from the normative SPEC §5
// declaration (data::RegisterStatus). The failure payload (when arm
// SchemaRegistryConflict or SchemaUnknown fires) is surfaced via TLS
// rather than an out-pointer: register_migration has no useful
// per-call out-pointer slot (it registers an entry, it does not
// produce a value), so the TLS getter glibre_types_last_register_error()
// is the correct mechanism. This asymmetry between RegisterStatus and
// CStatus is therefore load-bearing and must not be collapsed.
enum class RegisterStatus : std::uint16_t {
    Ok                       = 0,
    SchemaRegistryConflict   = static_cast<std::uint16_t>(
        data::ErrorTag::SchemaRegistryConflict),  // 5 — duplicate (from,to) or FQN unknown
};

}  // namespace glibre::types
```

Three consequences:

1. **Plain-enum return, payload via out-pointer.** A C-ABI call that
   needs to surface a `data::Error` writes the failure-arm payload
   into a caller-supplied `data::Error*` and returns the matching
   `CStatus` enumerator. The C++ wrapper template
   (`Envelope<T>::deserialize` etc.) checks the return code and
   constructs a `std::expected<T, data::Error>` by reading the
   out-pointer on the failure path (SPEC §5 §envelope.hpp).
2. **TLS-getter for ambient context.** `glibre_types_register_migration`
   uses `RegisterStatus` (two-arm) and carries no per-call out-pointer
   slot. On the failure path it writes into a thread-local `data::Error`
   buffer reachable through `glibre_types_last_register_error()`
   (`extern "C"`, returns a const-pointer into the calling thread's
   TLS). The C++ wrapper macro reads it on the failure path. The
   TLS is reset at every successful call so a stale failure cannot
   leak. This design follows SPEC §5 `migration.hpp` directly: the
   normative C++ `data::RegisterStatus` has exactly two enumerators
   (`Ok = 0`, `SchemaRegistryConflict = 5`), and `glibre_types_RegisterStatus`
   is its C-ABI projection with the same values.
3. **Two-arm narrowness is intentional.** `glibre_types_register_migration`
   is called only at static-init (Phase B, §3.5). The only ways it
   fails are: the FQN is not in the live registry (`SchemaUnknown`,
   which the design maps to `SchemaRegistryConflict` at the C-ABI
   surface — both indicate the call cannot be honored) or the
   `(from, to)` pair is already registered (`SchemaRegistryConflict`
   proper). All other error arms (`MigrationCycle`, etc.) are detected
   by Phase C's validator *after* all registrations complete, not during
   individual calls. Using a nine-arm `CStatus` here would make the
   call's contract misleadingly broad.

The §4.1 rule, restated: **`std::expected` does not cross the C-ABI
boundary**. `glibre_types_register_migration` returns two-arm
`RegisterStatus`; `glibre_types_serialize_<fqn>` and
`glibre_types_deserialize_<fqn>` return nine-arm `CStatus`;
`std::expected` is reconstructed in headers, on the caller side. Two
builds that disagree on `std::expected`'s layout still agree on the
wire because the wire is the plain enum.

### 4.2 Public C++ surface

Re-stated from SPEC §5, the C++ surface every plugin and host binary
consumes through `glibre/types/*.hpp`:

```cpp
// data/runtime/include/glibre/types/registry.hpp (lifted from SPEC §5)

namespace glibre::types {

class GLIBRE_TYPES_API SchemaRegistry {
public:
    auto lookup(SchemaId schema) const noexcept -> const RegistryEntry*;
    auto entries() const noexcept -> std::span<const RegistryEntry>;

    static auto instance() noexcept -> const SchemaRegistry&;

private:
    SchemaRegistry() = default;
};

template <class T>
struct Envelope {
    static auto serialize(const T& value, std::span<std::byte> dst) noexcept
        -> std::expected<std::size_t, data::Error>;
    static auto deserialize(std::span<const std::byte> src) noexcept
        -> std::expected<T, data::Error>;
};

}  // namespace glibre::types
```

The `Envelope<T>` primary template is **declaration-only** in the
public header; per-FQN specializations live in
`<glibre/types/<ctx>/<Type>.hpp>` and are emitted by codegen (SPEC §6.2
stage 4 step 1). Each specialization is a thin wrapper around the
matching `extern "C"` trampoline (§4.3 below); the `std::expected`
shape lives inside the wrapper, not at the dylib boundary.

`SchemaRegistry::instance()` returns a `const&` to the one process-
global instance, whose internal pointer is `&detail::kRegistry[0]` and
whose count is `detail::kRegistryCount`. Consumers either:

- Walk `entries()` (FQN-sorted iteration; SPEC §4.5 inv. 3); or
- Call `lookup(SchemaId{fqn})` (binary search; `O(log N)` over the
  contiguous array).

No public C++ method mutates the registry; the class has no `setter`,
no `register_*`, no `add_*`. Migration registration runs through the
`extern "C"` `glibre_types_register_migration` (§4.3) — itself called
only from `GLIBRE_REGISTER_MIGRATION(...)` macros at static-init
(§3.5 phase B), never at runtime.

**Normative: `RegistryEntry::serialize` and `::deserialize` are
internal-only implementation slots.** Although `RegistryEntry` is
accessible from outside the dylib (via `SchemaRegistry::lookup()` and
`entries()`), its `serialize` and `deserialize` fields are C++ function
pointers whose return types contain `std::expected<…, data::Error>` and
`std::expected<void, data::Error>`. These types do not have a stable
C-ABI layout across libc++ versions and must never be called directly
across a dylib boundary (§4.1 rule). These fields are implementation
slots consumed exclusively within the dylib by:

- the `Envelope<T>` specialization trampolines (codegen-emitted, live
  inside `glibre-types.dylib`); and
- the `MigrationDispatcher` (TU-private; resolves them at link time
  within the same dylib).

**External callers — plugin code and host binaries — MUST use
`Envelope<T>::serialize` / `Envelope<T>::deserialize` exclusively.**
Direct invocation of `entry->serialize(…)` or `entry->deserialize(…)`
from a plugin or host-binary TU is undefined behavior per §4.1 (C++
function pointer with `std::expected` return crosses the dylib seam).
Code review and the exported-symbol-set CI test (§3.3, §11.1) guard
the C-ABI surface; nothing in the public headers exposes a path by
which an external caller can call these slots without going through the
`Envelope<T>` wrapper. If a future refactor needs to expose the raw
function pointers externally, that requires a new C-ABI trampoline pair
(SPEC §4.3 inv. 4) and a SPEC §4.3 / §4.4 hash change.

### 4.3 Public ABI surface (extern "C")

The complete exported `extern "C"` set, in alphabetical order. Every
signature is patch-stable; adding a new `<fqn>` triplet
(`serialize_<fqn>`, `deserialize_<fqn>`, `register_migration_<fqn>` if
it ever exists) is additive and keeps SONAME (SPEC §4.3 inv. 2, 3).

```cpp
// data/runtime/include/glibre/types/c_abi.hpp

#ifdef __cplusplus
extern "C" {
#endif

// 1. Hash export. 64-char lowercase hex + NUL. Process-lifetime
//    constant; identical across calls. Callable from static-init.
const char* glibre_types_abi_hash(void) noexcept;

// 2. Migration registration. Called by GLIBRE_REGISTER_MIGRATION at
//    each owning context's static-init time. Returns Ok on success;
//    SchemaRegistryConflict on duplicate (from, to) for the same FQN
//    or if the FQN is not in the live registry. On the failure path,
//    writes a data::Error payload into the calling thread's TLS
//    reachable via glibre_types_last_register_error().
//    Return type is two-arm glibre_types_RegisterStatus (not the
//    nine-arm CStatus used by serialize/deserialize) — the narrower
//    type matches SPEC §5 data::RegisterStatus and signals to
//    implementers that only two outcomes are possible. See §4.1.
glibre_types_RegisterStatus glibre_types_register_migration(
    glibre_types_SchemaId schema,           // borrowed string_view
    glibre_types_MigrationEntry entry) noexcept;

// 3. Per-FQN serialize trampolines. One per registered FQN. The body
//    Fory-encodes `value` into `dst` and writes the byte count to
//    `*out_written`. Ok on success; DeserializeError on under-sized
//    `dst`, with payload via out-pointer.
glibre_types_CStatus glibre_types_serialize_<fqn>(
    const void* value,                      // pointer to T
    std::byte* dst, std::size_t dst_size,
    std::size_t* out_written,
    glibre_types_data_Error* out_err) noexcept;

// 4. Per-FQN deserialize trampolines. One per registered FQN. The body
//    Fory-decodes from `src` into `out_value`. Migration-chain dispatch
//    (when payload version < current) lives inside this trampoline,
//    which calls into the dispatcher via the registry entry's
//    migrations span.
glibre_types_CStatus glibre_types_deserialize_<fqn>(
    const std::byte* src, std::size_t src_size,
    void* out_value,                        // pointer to T
    glibre_types_data_Error* out_err) noexcept;

// 5. TLS-getter for register_migration's failure payload. Reads the
//    calling thread's last-register-error buffer; returns nullptr if
//    no failure has been recorded since the last successful call.
const glibre_types_data_Error* glibre_types_last_register_error(void) noexcept;

#ifdef __cplusplus
}  // extern "C"
#endif
```

The header `glibre/types/c_abi.hpp` declares C-shaped projections of
`SchemaId`, `MigrationEntry`, `data::Error`, and the `CStatus` enum,
all of which are POD aggregates so the C-ABI boundary stays free of
non-trivial types. The C++ public surface in §4.2 wraps these with
typed templates that resolve to the matching `extern "C"` trampoline
at link time (SPEC §4.3 inv. 4 — "thin wrapper templates exist in
headers but resolve to `extern "C"` symbols at link time").

Wrapper resolution example:

```cpp
// data/codegen-output/include/glibre/types/core/Transform.hpp

namespace glibre::types {

template <>
struct Envelope<::glibre::core::Transform> {
    static auto serialize(const ::glibre::core::Transform& v,
                          std::span<std::byte> dst) noexcept
        -> std::expected<std::size_t, data::Error> {
        std::size_t       written = 0;
        data::Error       err{};
        auto status = ::glibre_types_serialize_glibre_core_Transform(
            &v, dst.data(), dst.size(), &written, &err);
        if (status != CStatus::Ok) return std::unexpected(err);
        return written;
    }

    static auto deserialize(std::span<const std::byte> src) noexcept
        -> std::expected<::glibre::core::Transform, data::Error> {
        ::glibre::core::Transform out{};
        data::Error               err{};
        auto status = ::glibre_types_deserialize_glibre_core_Transform(
            src.data(), src.size(), &out, &err);
        if (status != CStatus::Ok) return std::unexpected(err);
        return out;
    }
};

}  // namespace glibre::types
```

Every plugin and every host binary consumes the typed surface; the
underlying C-ABI symbols are link-time-resolved into the *one*
`glibre-types.dylib` mapped into the process (§3.6). No plugin's
`std::expected` layout choice can cross the boundary because the
boundary is `CStatus` + out-pointer.

## 5. Hot / Cold Path Split

The middleman is structurally a **cold-path** aggregate: every
mutation it owns happens at process start (static-init) or never.
Runtime callers obtain a *cached pointer* from the registry once and
reuse it:

| Path  | Trigger                                      | Frequency                     | Budget                                  |
|-------|----------------------------------------------|-------------------------------|-----------------------------------------|
| Cold  | Image load (dynamic linker)                  | Once per process              | dyld budget; <100 ms for MVP type set    |
| Cold  | Static-init phase A (registry materialization)| Once per process              | <0.1 ms — no executable code            |
| Cold  | Static-init phase B (migration registration) | Once per process              | <1 ms — N × O(N) range insert (small N) |
| Cold  | Static-init phase C (validator)              | Once per process              | <0.5 ms — single walk of `kRegistry`     |
| Cold  | Plugin admission — `glibre_types_abi_hash`   | Once per plugin per session   | <0.001 ms — single literal return       |
| Hot   | `SchemaRegistry::lookup` (during hot-reload) | Per migrated row in phase 8   | ~10 ns (binary search; SPEC §9.2)       |
| Hot   | `Envelope<T>` per-call (incidental)          | Per save / snapshot capture   | per-schema; capped at 0.1 ms / frame    |

Hot-path invariants — the middleman's contribution to every-frame
budgets:

- **Lookup is allocation-free, branchless on the success path.**
  `SchemaRegistry::lookup` runs a binary search over `kRegistry`
  (§3.4 inv. 2); on a hit it returns `&kRegistry[i]` with no locks,
  no atomics, and no allocator activity (SPEC §9.2 / §9.5).
- **Cached pointer reuse.** Callers (`Envelope<T>::serialize`,
  `Envelope<T>::deserialize`, the dispatcher) resolve their per-FQN
  `RegistryEntry*` *once* at static-init through the codegen-emitted
  trampoline's link-time binding, then reuse the function-pointer
  table embedded in the entry. There is no per-call lookup on the
  hot path (SPEC §6.3).
- **Phase 8 idle is zero work.** When no hot-reload is pending, the
  middleman is not consulted. Hot-reload phase 8 issues lookups
  *only* for FQNs whose stored version differs from the current
  (SPEC §6.3, §8.2 step 2); idle frames touch nothing in the dylib's
  `.rodata` or `.bss`.

Cold-path invariants:

- **Static-init runs exactly once.** Phases A / B / C in §3.5 fire
  during the dynamic linker's image-load pass and before `main()`.
  The middleman has no `init()` API; there is no second
  initialization opportunity (SPEC §4.3 inv. 5).
- **Plugin admission is single-shot per plugin.** The hash-equality
  compare is one `memcmp` over 64 bytes (§3.7); the address-equality
  check is one pointer compare (§3.6). Both are single-call
  guarantees, never per-frame.

The cold/hot split is what makes the middleman's heap and CPU cells
(SPEC §9.2: `Middleman` aggregate row, "0 per frame ... 0
sub-ceiling") **independent of plugin count**. Plugin count grows
the *registry* size linearly at codegen time, which grows the
binary-search depth by `log N`; the per-call hot-path cost scales
sub-linearly even in the unrealistic case of many lookups per frame.

## 6. Concurrency

The middleman is **read-only after static-init**. Every public
boundary is thread-safe by being immutable.

The full thread-safety story:

1. **Static-init is single-threaded by construction.** The dynamic
   linker runs static initializers serially on the loading thread
   (`dyld` does not parallelize `__cxx_global_var_init`). Phases A,
   B, C of §3.5 therefore observe single-threaded semantics with no
   need for synchronization.
2. **Post-init reads are wait-free.** The descriptor table is `const`
   after Phase C; readers see a stable, fully-formed array with no
   atomics, no locks, no fences. C++23 memory model: no thread can
   observe a mutation to a `const` object after its initialization
   completes, because no thread mutates it.
3. **`SchemaRegistry::instance()` returns the same `const&`.**
   The instance is `constinit`-initialized at Phase A; its address
   is process-lifetime stable. Multiple threads calling
   `instance()` simultaneously see the same pointer with no race.
4. **`Envelope<T>::serialize` / `::deserialize` are thread-safe per
   *value*, not per *type*.** The trampolines read only the const
   `RegistryEntry`; per-call state lives on the caller's stack and
   in the caller-supplied `dst` / `src` spans. Two threads
   serializing two different `Transform` values race only on each
   other's spans, not on the registry. The dispatcher's per-payload
   `Arena` is caller-supplied, so two concurrent deserializes use
   two distinct arenas.
5. **`glibre_types_register_migration` is single-threaded by phase.**
   The function may *only* be called during static-init (Phase B);
   its preconditions assert the linker thread is single-threaded
   (the function is `extern "C"` and has no atomic semantics
   internally because none are needed). A post-`main()` call is
   undefined behavior the static-init validator (Phase C) cannot
   detect; the GLIBRE_REGISTER_MIGRATION macro's `__attribute__((constructor))`
   placement makes such a call structurally impossible from
   well-formed code.
6. **TLS surfaces (`glibre_types_last_register_error`) are
   thread-local.** The buffer is per-thread by definition; no
   cross-thread visibility is implied or required. A thread that
   never registered a migration sees a default-constructed
   `data::Error` (tag = 0 = invalid sentinel; the wrapper macro
   never reads it on the success path).

The middleman publishes **no mutex, no atomic, no read-write lock**.
This is the §6 collapse: by being immutable after init, the dylib
removes every reason a synchronization primitive would exist. Hot-
reload of the middleman itself is refused (§8); plugin reload does
not edit the table (§3.8); no API mutates the registry. There is
nothing to lock.

## 7. Persistence + ABI

The middleman's content **is** the engine ABI surface. Every
persistent byte the engine writes — save files, world snapshots,
hot-reload prelude payloads — passes through a `glibre_types_serialize_<fqn>`
or `glibre_types_deserialize_<fqn>` trampoline whose layout this
dylib's link-time output decides.

### 7.1 ABI hash composition

The `glibre_types_abi_hash` literal is computed by `glibre-foryc`
over the validated `Schema` set per SPEC §6.4 (re-stated in
§3.7 above). Inputs:

1. For each `Schema s`, `schema_source_hash(s) = blake3(canonicalize(s))`
   where `canonicalize` is the parsed-form rule of SPEC §7.3. The
   output is a 32-byte digest.
2. Form a per-schema entry string:
   `fqn_utf8(s) || ":" || version_le(s) || ":" || schema_source_hash(s)`
   where `version_le` is the declared version as 4 bytes little-endian,
   and `schema_source_hash` is the hex-encoded 32-byte Blake3 digest.
   Sort these entry strings by FQN in canonical Unicode code-point order
   (SPEC §4.4 inv. 1).
3. Join the sorted entry strings with a single LF byte (`\n`) between
   each adjacent pair; no trailing newline. This LF-separated
   concatenation is the outer blake3 input
   (`blake3( join("\n", sorted_entries) )` per SPEC §4.4 inv. 1 and
   `reviews/decisions/plugin-abi.md` §"ABI Hash Function" rule 1).
   **Rationale:** LF separation is the normative rule established by
   two independent decision sources (SPEC §4.4 inv. 1 and plugin-abi.md
   §"ABI Hash Function"). An earlier draft of §6.4 in the SPEC described
   "no separators"; that description was incorrect and has been corrected
   in SPEC §6.4 (aligned in this PR). The separator is not needed for
   fixed-width fields in isolation, but the entry strings are
   variable-length (FQN is unbounded), making LF separation necessary
   for unambiguous decoding and required by the normative sources.
4. Hex-encode lowercase, 64 chars + NUL ⇒ the embedded literal.

Inputs that are **not** in the hash (intentionally): build-host
identity, compiler version, link flags, header timestamps,
`__DATE__` / `__TIME__`, the order in which schemas were authored,
the order in which they sit in the codegen output's source list. Two
builds from the same `data/schemas/` tree on different hosts produce
the byte-equal literal (SPEC §4.4 inv. 4; PHILOSOPHY §7).

### 7.2 ABI stability discipline

Per SPEC §4.3 inv. 3 (transcribed verbatim):

1. **SONAME bumps only on layout-breaking changes** — those that
   violate the layout-additive rule (SPEC §4.2 inv. 3) or remove an
   exported entry point. SONAME = `1` at MVP; bump to `2` only
   when an existing field's offset moves or a registered FQN is
   removed.
2. **Additive changes keep SONAME and bump the hash.** Adding a new
   FQN appends two trampolines (`serialize_<fqn>`,
   `deserialize_<fqn>`) and one `RegistryEntry` slot; the hash
   recomputes over the new schema set; SONAME stays. The loader's
   hash compare (§3.7) catches plugins built against the old hash;
   `dyld` does not need to intervene because SONAME matches.
3. **Removing an FQN bumps SONAME.** A schema that was shipped and is
   now retired removes its registry slot, its trampoline pair, and
   its `_manifest_<plugin>.cpp` reference. This is a breaking
   change for any plugin still mentioning the FQN; the SONAME bump
   makes the dynamic linker refuse to bind the old plugin to the
   new dylib *before* the loader's hash compare runs.
4. **Reordering / renaming FQNs is forbidden.** A rename is a
   remove + add at the schema level; codegen catches the layout
   shift in stage 3 (SPEC §6.2 stage 3 layout-additive check) and
   refuses the build. Reordering exists only as a sort-key change,
   which would invalidate every hash; not permitted.

The CI pipeline validates these rules mechanically: a job runs
`glibre-foryc` against the prior-committed schema set and the head
schema set, compares emitted output byte-for-byte for the additive-
only assertion, and fails the PR if SONAME and hash discipline
diverge.

### 7.3 Persistent forms the middleman touches

The dylib carries no persistent files of its own; what it *contains*
governs how every persistent byte the engine writes is decoded:

| Surface                                      | Owned by              | Middleman's role                                  |
|----------------------------------------------|-----------------------|---------------------------------------------------|
| `data/schemas/<ctx>/<Type>.fory`             | data context (authoring)| Source for `kRegistry` entries (SPEC §7.1).     |
| `plugins/<plugin>/plugin.fory`               | core context (authoring)| Source for `_manifest_<plugin>.cpp` (§3.8).     |
| Save files / world snapshots (Fory envelopes)| consumer contexts      | Decoded via `glibre_types_deserialize_<fqn>`.    |
| Plugin `glibre_plugin_manifest` (`.rodata`)  | plugin's `manifest.cpp`| Manifest blob deserialized via the middleman's typed `Envelope<PluginManifest>` (the loader's call site lives in `core`, but the wire format and the deserializer body live here). |

The middleman never opens a file, never writes a file, never
allocates beyond static-init. Persistence flows through it as bytes
in / bytes out via `Envelope<T>` and the trampolines.

## 8. Hot-Reload

**The middleman does not hot-reload in MVP.** This is a hard
refusal, not a deferral with a planned enabling condition. The
dylib is locked at process start; replacing it requires engine
restart (SPEC §8.3 default; PHILOSOPHY §8;
`reviews/decisions/hot-reload-protocol.md` §"Consequences").

The §8 collapse — every reason this refusal is correct:

1. **Dyld cannot re-map a SONAME-stable dylib safely.** macOS does
   not provide a supported re-`dlopen` of the same SONAME with
   pointer-stable resolution; even if it did, every consumer that
   captured a `RegistryEntry*` at link time would dangle.
2. **The `RegistryEntry::migrations` span aliases `.bss`-backed
   storage filled at static-init.** A second static-init pass
   (impossible under dyld) would either reset the spans (mutating
   the supposedly-`const` registry) or duplicate them (introducing
   the two-middleman process the §3.6 gate refuses).
3. **`AbiHash` is the gate, not a value.** Every plugin in the
   process compiled against the live hash. Replacing the dylib
   would invalidate every plugin's compiled-in
   `glibre_plugin_abi_hash` and force a process-wide reload of
   every plugin to a new middleman — at which point engine restart
   has the same outcome with simpler machinery.

Plugin reload's diff-validation against the middleman:

- A plugin may reload only against the *same* middleman build that
  was loaded at process start. The loader's swap path (`reviews/decisions/plugin-abi.md`
  §"Loader Sequence" steps 4–5) re-checks
  `manifest.abi_hash == glibre_types_abi_hash()` *and*
  `glibre_plugin_abi_hash` symbol address equality (§3.6 mechanism)
  on the candidate dylib. A candidate built against a *different*
  middleman fails step 4 ⇒ `core::Error::PluginAbiHashMismatch`
  ⇒ refuse load, previous-good plugin keeps running.
- The plugin's owned components (declared in
  `manifest.components`) must reference FQNs the live registry
  already knows (SPEC §4.10 inv. 1, biconditional). A candidate
  manifest that mentions an FQN absent from `kRegistry` fails the
  loader's component-validation step ⇒
  `core::Error::PluginManifestInvalid` carrying the missing FQN.
- Schema versions inside the live registry never change at plugin
  reload. The candidate plugin's `manifest.components[*].schema_hash`
  is matched against the live `RegistryEntry.source_hash` byte-for-
  byte; mismatch ⇒ refuse load. This is the property §1 calls out:
  plugin reload does not fragment ABI because the descriptor table
  the candidate links is still the one the host links.

If a developer needs to evolve `glibre-types.dylib` itself (add a
schema, bump a version), the workflow is: edit `.fory` source ⇒
rebuild the middleman ⇒ rebuild every plugin against the new dylib
⇒ restart the engine. SPEC §8.3 records the future-spike contract
that *might* enable in-process middleman reload; this design does
not implement it.

The §8 closed cases:

| Refusal symptom                                     | Trigger                                              | core::Error wrapping       |
|-----------------------------------------------------|------------------------------------------------------|----------------------------|
| Middleman swap requested                            | (no API exists; engineering refusal at design level) | n/a                        |
| Two middleman builds in one process                 | `DYLD_LIBRARY_PATH` shenanigans; mismatched links    | `PluginAbiHashMismatch`    |
| Candidate plugin against different middleman build  | Plugin rebuilt against newer hash; engine not restarted | `PluginAbiHashMismatch` |
| Candidate manifest references unknown FQN           | Plugin schemas updated but not regenerated           | `PluginManifestInvalid`    |

## 9. Performance

The middleman's contribution to per-frame and per-load budgets, locked
against `reviews/decisions/perf-budget.md` §"Per-Context Budget Table"
(`data` row) and SPEC §9.2 (`Middleman` aggregate row):

### 9.1 Per-frame (steady-state)

| Cell                                    | Budget          | Source                                       |
|-----------------------------------------|-----------------|----------------------------------------------|
| `data` CPU sim                          | 0.20 ms total   | perf-budget.md row `data`                    |
|   of which `Middleman` aggregate        | **0 ms**        | SPEC §9.2 `Middleman` row                     |
| `data` CPU submit                       | 0.00 ms         | data records no GPU work                      |
| `data` heap (Middleman's share)         | **0 sub-ceiling** | SPEC §9.2 `Middleman` row                  |

The middleman aggregate is a **link-time presence**, not a per-frame
work item. Its `RegistryEntry` array sits in `.rodata` (cost
amortized into image-load); its function-pointer slots resolve at
link time (cost amortized into dyld's symbol-binding pass); its
`AbiHash` literal returns a single pointer (cost: one instruction).

The aggregate's heap sub-ceiling is **0 MiB**: the registry's `8 MiB`
sub-ceiling (SPEC §9.2 `SchemaRegistry` row) is accounted *under
SchemaRegistry*, not under Middleman, because the registry is the
data structure the middleman *contains*. The middleman aggregate is a
zero-cost wrapper in budget terms; every byte it carries belongs to
some other aggregate's accounting.

### 9.2 Per-load (cold path)

| Step  | Operation                                  | Budget       | Notes                                                |
|-------|--------------------------------------------|--------------|------------------------------------------------------|
| Load  | dyld image load + relocations              | <100 ms      | Driven by macOS dyld; bounded by binary size + N exports. Outside engine budget. |
| Init A| Registry materialization (`.rodata` map)   | <0.1 ms       | No code; `dyld` mmap.                                 |
| Init B| Migration registration walk                | <1 ms        | N × O(M) per-FQN insert where M = chain length ≤ 8 in MVP. |
| Init C| Static-init validator                      | <0.5 ms      | Single linear walk of `kRegistry`.                    |
| Plugin| `glibre_types_abi_hash` retrieve + compare | <0.001 ms    | One literal return + 64-byte memcmp.                  |
| Plugin| Per-FQN trampoline resolve (link)          | <0.05 ms     | dyld two-level namespace bind, N ≤ 64 per plugin.     |

**Total cold-init budget:** dominated by image load (dyld). The
spine's contribution (Phases A + B + C) is <2 ms even for the
maximum-MVP type set (N = 256 schemas, average chain length = 3).
Not in the steady-state frame budget.

### 9.3 Heap accounting

The middleman's resident bytes are:

- `kRegistry[N]` — `N × sizeof(RegistryEntry) ≈ N × 96 B`.
  At MVP scale (N ≤ 256), this is ≤ 24 KiB.
- `kFqnIntern[N]` — N pointers (≤ 2 KiB) plus the FQN strings
  themselves (~32 B each ⇒ ≤ 8 KiB total).
- `kPerFqnMigrationTable[N]` (`.bss`) — `Σ_i (chain_length_i ×
  sizeof(MigrationEntry))`. At MVP scale (avg chain length = 3, N = 256),
  this is ~16 KiB.
- `kReflectionFields[Σ]` (tools build only) — proportional to
  total field count across all schemas. Estimated ≤ 256 KiB at MVP
  scale; counts against the `ReflectionBlob` 4 MiB sub-ceiling
  (SPEC §9.2), not the Middleman aggregate.
- `kAbiHashLiteral` — 65 B.
- Per-plugin `kManifestBlob_<plugin>[K_p]` — Fory-serialized
  `PluginManifest` for each discovered plugin; ~2–8 KiB each.
  16 plugins × 4 KiB = 64 KiB.

Total resident: ~120 KiB at MVP scale (excluding ReflectionBlob).
Well inside the `data` 32 MiB row; consumes none of the row by the
SPEC §9.2 accounting rule (the bytes belong to SchemaRegistry,
ReflectionBlob, and the codegen-output TUs, not to the middleman
aggregate).

### 9.4 Headroom posture

The middleman is the one `data` aggregate explicitly excluded from
headroom consumption: its budget is 0 / 0 / 0 by construction, and
future growth of the *dylib* (more schemas, more plugins, more
trampolines) is amortized into the aggregates the table is *part
of* (SchemaRegistry, ReflectionBlob, Envelope), not into Middleman.
This is the SPEC §9.6 rule applied to this aggregate: a future per-
frame work item would belong to one of the other rows, never this
one.

## 10. Failure Modes

The middleman aggregate raises three failure modes through the
`data::Error` closed sum (SPEC §10.1) plus one
`core::Error::PluginAbiHashMismatch` wrapping at the loader's call
site. Mapping to the SPEC §10.1 arms:

### 10.1 Middleman-emitted `data::Error` arms

| Arm                       | Trigger                                                       | Detection point                              | Recovery       | Severity      | core::Error wrapping        |
|---------------------------|---------------------------------------------------------------|----------------------------------------------|----------------|---------------|------------------------------|
| `AbiHashMismatch`         | A loaded plugin's `glibre_plugin_abi_hash` ≠ host's (§3.6 mechanism A); OR plugin and host resolve to distinct middleman mappings (§3.6 mechanism B). | Plugin loader at admission (`reviews/decisions/plugin-abi.md` §"Loader Sequence" step 4); SPEC §10.2 row 1. | refuse load — `dlclose` candidate, log at `warn`, leave previous-good plugin live. | warn          | `PluginAbiHashMismatch`      |
| `SchemaRegistryConflict`  | Static-init Phase C validator detects two `kRegistry` entries with the same FQN — only reachable if codegen output is corrupt (cosmic-ray-class). | Middleman static-init Phase C (§3.5; SPEC §4.5 inv. 1). | process abort — fatal log + `std::abort()`. | fatal         | None (process never reached `main()`). |
| `MigrationCycle`          | Static-init Phase C validator detects a back-edge in the per-FQN migration chain (a step `(N → M)` with `M ≤ N`) — only reachable if codegen output is corrupt or an owning context's `GLIBRE_REGISTER_MIGRATION` macro was invoked with bogus version arguments. | Middleman static-init Phase C (§3.5; SPEC §4.7 inv. 2). | process abort — fatal log + `std::abort()`. | fatal         | None.                        |

Three operator-action notes:

- **`AbiHashMismatch`**: Rebuild the offending plugin against the
  current `glibre-types.dylib`. The hex prefix in
  `ErrorContext::detail` (carried via `data::Error::host_hash`,
  `plugin_hash`) tells operators which build is which.
- **`SchemaRegistryConflict`** at static-init: the codegen output
  is wrong. The only way this fires is that `glibre-foryc`'s stage
  3 validation (SPEC §6.2) failed to catch a duplicate FQN — a
  codegen bug. Reproduce with `glibre-foryc --in data/schemas
  --out /tmp/check`; report a build bug.
- **`MigrationCycle`** at static-init: a `GLIBRE_REGISTER_MIGRATION`
  macro invocation has a bogus `(from, to)` pair, OR `glibre-foryc`'s
  chain emission is corrupt. Reproduce with the failing fixture under
  `tests/data/middleman/`; almost always a hand-edited
  `<Type>_migrations.hpp` where it should be codegenned.

### 10.2 What the middleman does NOT raise

Out-of-scope (raised by other aggregates and propagated through the
dylib boundary):

- `SchemaMigrationFailure`, `MigrationStepMissing`,
  `EnvelopeTruncated`, `DeserializeError`, `SchemaUnknown` — raised
  by the dispatcher and the per-FQN trampolines, not by the
  middleman aggregate itself. The dylib *contains* the code that
  raises them (SPEC §6.3) but the *aggregate* responsible is the
  dispatcher (#736), the envelope (#735), or the registry (#730).
- `ReservedTagViolation` — raised by `glibre-foryc` at codegen
  time; never reaches runtime (SPEC §10.2 row "ReservedTagViolation").
  The middleman is a *consumer* of codegen output, not an emitter
  of codegen errors.
- `core::Error::PluginManifestInvalid`, `PluginMissingEntryPoint`,
  `PluginDlopenFailed` — raised by the plugin loader; the
  middleman's role is providing the deserializer body for
  `PluginManifest`, not detecting malformed plugin dylibs.
- `core::Error::SchemaMigrationFailed`, `HotReloadRefused` — raised
  by the loader's wrapping of `data::Error` arms 2 / 7
  (`SchemaMigrationFailure` / `MigrationStepMissing`); the
  middleman provides the un-wrapped arms.

### 10.3 Failure modes the design *added* over `data::Error`

This design introduces one new failure mode beyond the SPEC §10.1
closed sum:

- **MiddlemanLoadFailed** (loader-side, SPEC §10.1's
  `core::Error::PluginAbiHashMismatch` carrying detail
  `"duplicate middleman in process"` per §3.6 mechanism B). This
  is *not* a new arm; it is the existing `PluginAbiHashMismatch`
  arm carrying a specific `ErrorContext::detail` string the loader
  emits when address-equality of `glibre_types_abi_hash` between
  host and plugin fails. Operators reading logs can distinguish
  the byte-mismatch case from the duplicate-mapping case via the
  detail string. Adding a new top-level enum arm was rejected per
  SPEC §10 closed-sum discipline (no new arms without amendment);
  the existing arm + detail-string discrimination satisfies the
  §3.6 invariant without enum churn.

The closed-sum discipline of SPEC §10 is honored: this design adds
*no new `data::Error` arms*. Every middleman-detectable failure
already maps to an existing arm in SPEC §10.1.

## 11. Test Plan

### 11.1 Unit tests (Catch2, `tests/data/middleman/`)

Each row of §10.1 plus every §3 / §4 invariant maps to one or more
unit tests:

| Test name                                          | Drives invariant / arm                         | Fixture                                              |
|----------------------------------------------------|------------------------------------------------|------------------------------------------------------|
| `middleman.abi_hash_byte_equal_across_hosts`       | §3.7 / SPEC §4.4 inv. 4                         | Build `glibre-types.dylib` on two host configs (clang flags varied); assert `glibre_types_abi_hash()` literal byte-equal. |
| `middleman.abi_hash_excludes_build_env`            | §7.1 (build-env independence)                   | Build twice with `__DATE__` / `__TIME__` macros varied; assert hash unchanged. |
| `middleman.descriptor_table_fqn_sorted`            | §3.4 inv. 2                                     | Walk `SchemaRegistry::entries()`; assert ascending byte order on `entry.schema.fqn`. |
| `middleman.descriptor_serialize_deserialize_nonnull` | §3.4 inv. 4                                  | For every entry, assert `serialize` and `deserialize` non-null. |
| `middleman.descriptor_migrations_cover_chain`      | §3.4 inv. 5 / SPEC §4.10 inv. 5                | For every entry with `version > 1`, assert `migrations.size() == version - 1` and ascending `from_version` 1..version-1. |
| `middleman.descriptor_reflection_null_in_shipping` | §3.4 inv. 6                                     | Build with `GLIBRE_TYPES_REFLECTION=OFF`; assert every `entry.reflection == nullptr`. |
| `middleman.lookup_finds_registered`                | SPEC §4.5 inv. 3                                | `lookup(SchemaId{"glibre.core.Transform"})` returns non-null. |
| `middleman.lookup_returns_nullptr_for_unknown`     | SPEC §4.5 inv. 3                                | `lookup(SchemaId{"glibre.test.NeverRegistered"}) == nullptr`. |
| `middleman.lookup_log_n_complexity`                | SPEC §9.2 (`SchemaRegistry` row, ~10 ns)        | Catch2 `BENCHMARK`; assert `<=10 ns` per call over 100k iterations. |
| `middleman.exported_symbol_set_audit`              | §3.3                                            | `nm -gU` diff against `EXPECTED_EXPORTS.txt`; PR fails on drift. |
| `middleman.no_unexported_internals_leak`           | §3.2 visibility discipline                      | `nm` for `Fory::*` and `blake3::*`; assert empty. |
| `middleman.address_equality_self`                  | §3.6 mechanism B                                | Within the host, `&glibre_types_abi_hash` resolves to one address. |
| `middleman.static_init_validator_aborts_on_dup_fqn`| §3.5 Phase C / `SchemaRegistryConflict`         | Death-test: link a fixture middleman with two entries sharing FQN; assert process aborts with the expected log. |
| `middleman.static_init_validator_aborts_on_cycle`  | §3.5 Phase C / `MigrationCycle`                 | Death-test: link a fixture middleman with chain `[(1→2),(2→1)]`; assert abort. |
| `middleman.register_migration_records_failure_in_tls` | §4.1 TLS surface                             | Call `glibre_types_register_migration` with duplicate `(from, to)`; assert `glibre_types_last_register_error()` returns the `SchemaRegistryConflict` payload on the calling thread. |
| `middleman.register_migration_resets_tls_on_success` | §4.1 TLS surface                              | After a failed call, a successful call resets the TLS to a default `data::Error`. |
| `middleman.validator_priority_uniqueness`            | §3.5 Phase C / §12 open question 5            | Build-system check (via `nm --just-symbols` or linker map) that asserts only `static_init_check.cpp` contributes a constructor section at priority 65535 in the dylib. Converts the §12 open question from a documentation note into an enforceable invariant: if any other TU in the dylib acquires `__attribute__((constructor(65535)))`, the check fails the PR before the ordering hazard can silently arise. |

### 11.2 Integration tests (Catch2, `tests/data/integration/`)

- `integration.full_plugin_link_round_trip` — build a fixture
  plugin against a fresh `glibre-types.dylib`; load via the core
  plugin loader; serialize + deserialize one of every registered
  type via the plugin's API; assert byte-equal round-trip and
  zero hot-path allocations.
- `integration.plugin_reload_sweep` — load 3 plugins, reload each
  one in succession, asserting the registry's `kRegistry` and
  `kPerFqnMigrationTable` are bit-equal before and after every
  reload (§8 invariant: plugin reload does not edit the table).
- `integration.duplicate_middleman_refused` — set
  `DYLD_LIBRARY_PATH` to point at a second `glibre-types.dylib`
  copy; load a plugin built against the first; assert
  `core::Error::PluginAbiHashMismatch` with detail
  `"duplicate middleman in process"` (§3.6 mechanism B).
- `integration.cross_plugin_descriptor_identity` — load plugin A
  (which serializes `glibre.core.Transform`) and plugin B (which
  also serializes `glibre.core.Transform`); assert both resolve to
  the *same* `RegistryEntry*` via `SchemaRegistry::lookup` (§3.4
  inv. 1: one canonical descriptor per type per process).

### 11.3 E2E coverage

E2E traces under `tests/e2e/data/` ship one fixture exercising the
middleman's load-time invariants:

- `middleman-build-determinism` — builds the data context twice on
  the CI runner with different `--jobs` settings; asserts the
  emitted `_abi_hash.cpp` and `_registry.cpp` are byte-equal
  (PHILOSOPHY §7; SPEC §4.4 inv. 4).

The trace format records (a) the emitted hash literal, (b) the
exported symbol set (`nm` output), and (c) the registry's FQN list
in `entries()` order. CI replays assert byte-equal trace output
across runs (deterministic-replay obligation, SPEC §10.2 #7).

### 11.4 Performance microbenchmarks

Catch2 `BENCHMARK` blocks under `tests/data/perf/middleman_bench.cpp`:

- `bench.abi_hash_call` — asserts `<10 ns` for one
  `glibre_types_abi_hash()` invocation (a literal pointer return).
- `bench.lookup_average` — asserts `<10 ns` per `lookup(SchemaId)`
  over the MVP 256-entry registry (binary search, cache-warm).
- `bench.exported_symbol_resolve` — asserts `<0.05 ms` for one
  cold dlsym of `glibre_types_serialize_glibre_core_Transform`.

CI gates per `reviews/decisions/perf-budget.md` §"CI Gate Spec" #1:
any benchmark exceeding its budget fails the PR.

### 11.5 Code coverage

Per SPEC §10.5 obligation, every `data::Error` arm the middleman
raises (§10.1 above) carries at least one Catch2 case in §11.1.
Lines covered: §3.4 invariants 1–7, §3.5 Phases A / B / C, §3.6
mechanisms A and B, §4.1 ABI shape (CStatus + out-pointer + TLS),
§4.3 every exported symbol, §6 thread-safety claims (verified by
TSan run on the integration suite), §8 refusal cases.

## 12. Open Questions

- **[OPEN] Address-equality check via `dlsym` cost on macOS dyld.**
  §3.6 mechanism B compares `dlsym(plugin, "glibre_types_abi_hash")`
  to the host's bound symbol. On macOS the two-level namespace makes
  the check inexpensive (<0.001 ms per plugin per session) but
  measurement on the CI runner has not yet landed. Resolve in the
  first plan that lands the loader's middleman gate.

- **[OPEN] Single-TU vs split-TU `_registry.cpp` for very large N.**
  At MVP scale (N ≤ 256), one TU compiles in <2 s. At long-term
  scale (N ≥ 4096), one TU may exceed the 30-s build-time budget.
  Splitting is straightforward (one TU per context directory) but
  introduces a partial-link step that complicates the static-init
  ordering of §3.5. Defer until N exceeds 1024 in practice; revisit
  in the post-MVP planning sub-epic.

- **[OPEN] `glibre_types_last_register_error` TLS lifetime on
  worker threads.** §4.1 specifies the TLS buffer is per-thread;
  the engine's static-init runs on the dyld loading thread (one
  thread by construction), but a future post-MVP feature might
  invoke `glibre_types_register_migration` from a worker (e.g. an
  editor-driven type-set hot-swap that loads a sidecar middleman).
  Worker-thread TLS lifetime is well-defined (POSIX), but the
  middleman's contract should pin it explicitly. Resolve when the
  post-MVP self-reload spike (SPEC §8.3) opens.

- **[OPEN] Cross-libc++ `eastl::span<const std::byte>` ABI**
  (`reviews/decisions/fory-codegen.md` §"Open Questions" #4 — open
  in that record, mirrored here). The `serialize` / `deserialize`
  trampolines accept `std::byte*` + `std::size_t` at the C-ABI
  boundary (§4.3) precisely to avoid the libc++-version
  dependency, but the typed C++ wrappers still pass
  `eastl::span<const std::byte>`. Document the libc++ minimum in
  the data SPEC §5 once the determinism spike confirms one.

- **[OPEN] Static-init ordering across multiple translation units
  on macOS dyld.** §3.5 Phase B relies on every owning context's
  `GLIBRE_REGISTER_MIGRATION` static-initializer running *before*
  Phase C's `__attribute__((constructor(65535)))` validator.
  macOS dyld documents priority-based ordering for explicit
  `constructor(N)` attributes, but C++ static-init across TUs
  (which the macros expand to) is unspecified-order *within the
  same priority*. The current design works because the validator
  is the only `constructor(65535)`; if any other piece of code
  takes priority 65535 in the future, the ordering breaks.
  Defend with a build-system check that asserts only
  `static_init_check.cpp` uses priority 65535 in the dylib.
  Resolve in the first plan that lands the validator.
