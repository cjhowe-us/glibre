# Decision: Apache Fory Codegen + Middleman Dylib

- Refs: spike #12 (research-fory-codegen), parent sub-epic #5, epic #2
- Owner context: `data`
- Domains touched: `core` (plugin loader), every plugin (link target)

## Status

Accepted (spike output). Implementation deferred to the
`task-breakdown-data-plugin` planning spike, which will translate the
pipeline below into `type:plan` issues.

## Context

Glibre is a plugin-only engine. Every domain ships as a `.dylib`. The
core hosts ECS, the plugin loader, the hot-reload barrier, the type
registry, and asset handles (PHILOSOPHY §3). Persistent state crosses
plugin boundaries (scenes, components, assets, world snapshots).

We need:

1. **One canonical wire format** so two plugins agree on a `Transform`
   without either depending on the other's headers.
2. **Static codegen, zero runtime reflection** in shipping builds
   (PHILOSOPHY §6) — schemas compile to C++, not interpreted at runtime.
3. **Determinism** — byte-equal snapshots across hosts (PHILOSOPHY §7).
4. **ABI safety** — refuse load on hash mismatch (PHILOSOPHY §9).
5. **Hot-reload migrations** — schema-version bumps must not strand
   live sessions; migration runs at the frame-8 barrier
   (plan §B, PHILOSOPHY §8).

Apache Fory was chosen (vcpkg.json) over flatbuffers / cap'n'proto
because Fory provides (a) compact tagged binary with explicit schema
evolution, (b) a code-generation path producing native POD-like C++
structs, and (c) cross-language parity (relevant for future tooling /
editor-IPC story) without dragging an IDL VM at runtime.

## Decision

1. Schemas live at `data/schemas/<context>/<Type>.fory` — one file per
   persistent aggregate, owned by the bounded context that originates
   the type.
2. A CMake custom target `glibre-types-codegen` invokes the
   `glibre-foryc` host tool (a thin wrapper around Apache Fory's C++
   generator) over `data/schemas/**/*.fory` and emits sources into
   `${CMAKE_BINARY_DIR}/generated/glibre-types/`.
3. Those sources compile into **`glibre-types.dylib`** — the
   "middleman". Every plugin and the core `runtime` / `editor` binaries
   link against this dylib. No plugin links Fory directly; Fory is a
   private dependency of `glibre-types.dylib`.
4. The middleman dylib exposes:
   - POD-like generated struct types (`glibre::types::Transform`, …).
   - `glibre::types::serialize<T>(const T&, std::span<std::byte>)` and
     `deserialize<T>(std::span<const std::byte>) -> std::expected<T,
     glibre::Error>` — thin generated wrappers over Fory.
   - `glibre::types::Schema` registry: per-type `(fqn, version,
     blake3-hash-of-schema-source)` entries.
   - `glibre_types_abi_hash()` returning a single blake3 over the
     concatenated, sorted schema-source hashes — the value the plugin
     loader compares.
5. Plugins call `glibre_plugin_register(World&, Registry&)` (plan §F);
   the loader compares their compiled-in `glibre_types_abi_hash` to
   the host's. Mismatch → refuse load with `Error::AbiHashMismatch`.

## Pipeline

```text
data/schemas/<ctx>/<Type>.fory
        │
        │  (1) glibre-foryc  (host tool, invoked by custom target)
        ▼
build/generated/glibre-types/
    include/glibre/types/<ctx>/<Type>.hpp     (generated)
    src/<ctx>/<Type>.cpp                      (generated)
    src/_registry.cpp                         (generated; collects all)
    src/_abi_hash.cpp                         (generated; embeds blake3)
        │
        │  (2) add_library(glibre-types SHARED ...)
        ▼
build/lib/libglibre-types.dylib   ← linked by every plugin + binary
        │
        │  (3) plugin .dylibs link glibre-types
        ▼
runtime / editor load plugins; loader compares
    plugin's glibre_types_abi_hash() vs host's → load or refuse
```

Each generated `.hpp` is purely structural (PODs + free functions). No
templates from Fory leak through; the dylib boundary is C-ABI-safe by
construction (only `extern "C"` registry entry points cross dylib
boundaries; struct layouts are stable per the ABI rules below).

## Schema File Format (sketch)

`data/schemas/core/Transform.fory`:

```fory
schema glibre.core.Transform {
  version  3
  since    "0.1.0"

  field translation : vec3f   tag 1 since 1
  field rotation    : quatf   tag 2 since 1
  field scale       : vec3f   tag 3 since 1   default { 1.0, 1.0, 1.0 }
  field flags       : u32     tag 4 since 2
  field parent      : entity  tag 5 since 3
}

migration v2_to_v3 {
  // Provided by the owning context as a free function:
  //   glibre::core::migrate_Transform_v2_to_v3(...).
  // Codegen emits only the dispatcher hookup; the body lives in the
  // owning context's source tree.
  provider "glibre::core::migrate_Transform_v2_to_v3"
}
```

Rules:

- `tag` numbers are immutable once shipped. Removing a field marks the
  tag `reserved` rather than reusing it.
- `since` enables deterministic default synthesis when an older payload
  omits a newer field.
- Built-in scalar names (`u32`, `f32`, `vec3f`, `quatf`, `entity`,
  `string`, `bytes`, `list<T>`, `map<K,V>`, `option<T>`) compile to a
  fixed, audited set of C++ types in `glibre/types/_builtins.hpp`.

## CMake Integration

```cmake
# tools/foryc/CMakeLists.txt
add_executable(glibre-foryc ...)        # host tool, links Apache Fory
target_link_libraries(glibre-foryc PRIVATE Fory::fory)

# data/CMakeLists.txt
file(GLOB_RECURSE GLIBRE_SCHEMAS
     CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/data/schemas/*.fory")

set(GEN_DIR "${CMAKE_BINARY_DIR}/generated/glibre-types")
add_custom_command(
  OUTPUT  "${GEN_DIR}/.stamp"
  COMMAND glibre-foryc
          --in   "${CMAKE_SOURCE_DIR}/data/schemas"
          --out  "${GEN_DIR}"
          --stamp "${GEN_DIR}/.stamp"
  DEPENDS glibre-foryc ${GLIBRE_SCHEMAS}
  COMMENT "glibre-foryc: regenerating middleman types")

add_custom_target(glibre-types-codegen DEPENDS "${GEN_DIR}/.stamp")

file(GLOB_RECURSE GEN_SRCS CONFIGURE_DEPENDS "${GEN_DIR}/src/*.cpp")
add_library(glibre-types SHARED ${GEN_SRCS})
add_dependencies(glibre-types glibre-types-codegen)
target_include_directories(glibre-types
  PUBLIC  "${GEN_DIR}/include")
target_link_libraries(glibre-types
  PRIVATE Fory::fory blake3::blake3)

# Every plugin: target_link_libraries(<plugin> PUBLIC glibre-types)
```

Notes:

- `CONFIGURE_DEPENDS` on the schema glob causes Ninja to re-glob each
  build; acceptable cost for ~tens of schemas.
- `glibre-foryc` is built **before** `glibre-types`; vcpkg-driven Fory
  flows in only via `Fory::fory`, so the standard
  `cmake --preset macos-debug` driven by `vcpkg.json` is unbroken.
- The codegen tool is host-only; cross-compilation (post-MVP) will use
  CMake's `IMPORTED` executable pattern.

## Migration Mechanic (N → N+1)

1. Each generated type carries `current_version` and a static
   `migrations` table populated at static-init time inside the
   `glibre-types` dylib via the codegen-emitted dispatcher.
2. The owning context provides the body of each migration as a free
   function (`migrate_<Type>_v<N>_to_v<N+1>(const VN&, VNplus1&) ->
   std::expected<void, glibre::Error>`) and registers it via a generated
   header macro the codegen places in
   `glibre/types/<ctx>/<Type>_migrations.hpp`. The owning context's
   library links `glibre-types` and contributes its migration TU.
3. On `deserialize<T>`:
   - Read the `(fqn, version)` envelope (Fory header).
   - If `version == current`, decode directly.
   - Else look up a chain `version → current` in the migrations table;
     execute each step into a temporary; final `T` is yielded.
   - Missing chain → `Error::SchemaMigrationFailure`.
4. **Hot-reload integration**: at frame-8 barrier the loader drains
   the world, swaps dylibs, then for each persistent component runs
   the deserialize-with-migration path against the snapshot. Failure
   → reject the swap, restore prior dylibs (plan §B refusal cases).
5. Migrations are **pure functions**: no allocation outside the supplied
   arena, no I/O, deterministic. Tested at the plan level by Catch2
   golden round-trips (`vN payload → vN+1 struct → re-serialize`).

## ABI Stability Rules

1. **Field order in generated structs is tag-sorted ascending**, not
   declaration-order. This makes the layout independent of how a
   schema author orders the file.
2. Generated structs are `final`, contain only built-in scalars or
   other generated types, and have no virtuals, no vtables, no
   user-defined ctors beyond `= default`. They are
   `std::is_trivially_copyable_v` wherever the field set permits.
3. Adding a field at a new tag is ABI-additive only if it appends past
   the last existing field's offset; otherwise codegen bumps the
   schema's major version and forces a migration. The codegen tool
   asserts this at generation time and fails the build on violation.
4. The dylib's exported C entry points are limited to:
   `glibre_types_abi_hash`, `glibre_types_serialize_<fqn>`,
   `glibre_types_deserialize_<fqn>`,
   `glibre_types_register_migration_<fqn>`. These have stable C
   signatures; the C++ wrapper templates live in headers.
5. The middleman dylib's SONAME is bumped only on ABI-breaking schema
   changes; minor schema additions keep SONAME but bump the embedded
   `glibre_types_abi_hash`. The plugin loader's hash check catches the
   latter; the dynamic linker's SONAME check catches the former.

## Rationale

- **One middleman, not per-plugin codegen**: avoids N copies of every
  type, makes ABI hash checking a single comparison, and lets the
  editor / runtime ship one shared library users can update.
- **Static codegen at configure-time**: keeps with PHILOSOPHY §6 (no
  runtime reflection in ship builds) and gives clang full visibility
  for inlining serialize/deserialize.
- **Migrations owned by the originating context, not by `data`**: the
  context that owns the type's invariants is the only one that can
  write a correct vN→vN+1 transform. `data` owns only the registry
  plumbing.
- **Tag-sorted layout + codegen-time ABI assertions**: collapses two
  failure modes (forgot-to-bump-version, accidental-reorder) into one
  build error.

## Consequences

- **Build graph adds a tool target**. `glibre-foryc` must build before
  any plugin; CI cold-build cost rises by one C++ link of the host
  tool. Acceptable.
- **Schema files are now load-bearing**. Changes require code review
  attention equal to public headers. The `data` SPEC §7 will require
  every persistent type to ship with at least one Catch2 round-trip
  test under `tests/data/schemas/`.
- **Hot-reload tests grow a new axis**: per-schema migration goldens.
  The `e2e` context's trace format records pre-migration payloads so
  CI can replay them against newer middleman builds.
- **Cross-language parity is opt-in**: future Python tooling can read
  `data/schemas/*.fory` directly via Fory's reference impl without
  pulling glibre headers.

## Apache Fory in vcpkg — overlay port required

As of this spike (`gh api repos/microsoft/vcpkg/contents/ports/fory` →
404), there is **no upstream `fory` port in microsoft/vcpkg**, despite
the dependency being listed in `vcpkg.json`. Upstream Apache Fory C++
sources exist under `apache/fory/cpp` (CMake build). To unblock the
build we will:

1. Author an overlay port at
   `vcpkg-overlay-ports/fory/{vcpkg.json, portfile.cmake, usage}` that
   pins a tagged Apache Fory release, builds the `cpp/` subdirectory,
   and exports the `Fory::fory` target.
2. Add `"overlay-ports": ["./vcpkg-overlay-ports"]` to
   `vcpkg-configuration.json` (new file).
3. Track the upstream merge of a real port and switch when available.

This is captured as an open question (below) and converted to a
`type:plan` issue by the planning spike.

## Open Questions

1. **Overlay port owner**: which spike opens the
   `pkg(fory): overlay port` plan issue — this one's planning sibling,
   or `task-breakdown-data-plugin`? Recommend the latter, as it owns
   the data context's implementation backlog.
2. **Schema language host**: do we author `glibre-foryc` ourselves
   atop Apache Fory's reflection API, or shell out to Fory's bundled
   compiler and post-process? Decision deferred to first
   prototype-spike under the data epic.
3. **Reserved-tag enforcement**: machine-checked in `glibre-foryc`,
   or convention only? Lean toward enforced; cost is one diff against
   the prior committed schema.
4. **Cross-dylib `std::span` of `std::byte`**: C++23 ABI for these is
   stable on the chosen libc++ version (clang on macOS), but document
   the libc++ minimum in the data SPEC §5.
5. **Determinism of float fields**: ban NaN payloads at deserialize
   time, or accept-and-canonicalize? Belongs in the determinism spike,
   not here.
