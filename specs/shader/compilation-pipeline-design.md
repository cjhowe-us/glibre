# Compilation-Pipeline Detailed Design

> Detailed design for the `shader` context's `CompilationPipeline`
> aggregate (`specs/shader/SPEC.md` §4.3, §4.7, §5, §6.2). Refines the
> SPEC's pinned contract that one Slang translation unit plus one
> `PermutationKey` plus one `CompileTarget` flow through one `slangc`
> subprocess invocation and return one sealed `ShaderArtifact`. All
> conclusions independently re-derived; harmonius `docs/design/
> rendering/render-pipeline.md` § "Shader Compilation Pipeline" cited as
> research input only.

Refs: spike #749 — `[SPIKE] design-shader-compilation-pipeline-detailed`.
Parent #744. Sibling task-breakdown spike blocked-by this deliverable.

## 1. Purpose

`CompilationPipeline` is the single component in the `shader` context
permitted to spawn the `glibre-shadercc` driver subprocess and to
translate its outputs into a sealed `ShaderArtifact`. Its one
responsibility is **driving one offline compile job**: take an already-
opened `ShaderSource`, an already-resolved `PermutationKey`, and a
selected `CompileTarget`; build the canonicalized argv; spawn the
driver; capture stdout (length-prefixed bytecode + reflection bytes)
and stderr (structured `shader::Error` JSON); and return either a
sealed `ShaderArtifact` (bytecode + `ReflectionBlob` +
`DescriptorLayout` paired by construction) or a typed `shader::Error`
(SPEC §4.3, §10).

What `CompilationPipeline` explicitly refuses to own:

- **Source ingestion** — `ShaderSource::open`, include-graph closure,
  preprocessor expansion, entry-point scan, and total-hash composition
  belong to the §4.1 aggregate (sibling `shader-source-design.md`,
  spike #745). The pipeline accepts an already-validated
  `PreprocessedSource`.
- **Permutation enumeration / encoding** — `PermutationKey::to_bytes`,
  `PermutationKey::from_bytes`, `PermutationIndex` ordinal arithmetic,
  cross-product walks, and project-pruning belong to the §4.2 aggregate
  (sibling `permutation-key-design.md`, spike #747). The pipeline
  accepts an already-resolved key value.
- **Reflection ingestion** — translating slangc's native reflection
  record into the canonical `ReflectionBlob` (`reflection/
  slangc_reflection_ingester.cpp`, frequency-tagging) belongs to the
  §4.4 aggregate (sibling `reflection-blob-design.md`, spike #751).
  The pipeline only forwards the raw reflection payload from the
  driver to the ingester and pairs the result with the bytecode.
- **Descriptor-layout derivation** — projecting `ReflectionBlob` onto
  the four-frequency tables and the `RootSignatureSchema` belongs to
  the §4.5 aggregate (sibling `descriptor-layout-design.md`, spike
  #753). The pipeline calls `DescriptorLayout::derive` exactly once
  per artifact and embeds the result.
- **Cache lookup / insertion** — keyed put / get against the CAS, the
  manifest, and the cooked `ShaderLibrary` belong to §4.6 (sibling
  `shader-cache-design.md`, spike #755). The pipeline never touches
  the CAS; the cooker is the sole `insert` site.
- **Backend selection** — `IShaderBackend` instance choice and
  capability negotiation belong to the §4.7 plugin trait dispatcher
  (sibling spike #757). The pipeline accepts an already-bound
  `IShaderBackend&` and never selects between backends.
- **Source-watcher signaling** — filesystem events from the platform
  watcher (#719) reach the pipeline only as already-decoded
  recompile requests; the pipeline neither registers nor unregisters
  watches.
- **Render-graph topology, PSO objects, GPU resource allocation** —
  refused (`specs/shader/SPEC.md` §3 refusals 1, 4, 5).
- **Runtime compilation in shipping** — the entire
  `CompilationPipeline` translation unit is excluded from the shipping
  link target (`specs/shader/SPEC.md` §4.3 invariant 3, §4.8 invariant
  3, §6.5). Shipping has no `compile()` virtual at the ABI surface.

The pipeline's SRP boundary is sharp: if the driver argv schema, the
stdout / stderr framing, the per-job synchronization model, the
exit-code → `shader::Error` mapping, the per-job cache-key composition,
or the in-process pairing of bytecode + reflection + descriptor layout
changes, this design changes. Anything else is out of scope.

## 2. Requirements Coverage

Mapping of harmonius `docs/design/rendering/render-pipeline.md` §
"Shader Compilation Pipeline" + `docs/requirements/rendering/
gpu-abstraction-layer.md` R-2.1.16/.17/.18 to glibre MVP refusal-or-
coverage. Every entry independently re-derived; harmonius is research
input only.

| Harmonius element | Glibre disposition (MVP) | Coverage site |
|-------------------|--------------------------|---------------|
| `HLSL → dxc → DXIL → metal-shaderconverter → metallib` transpile chain | **Refused — collapsed.** | One subprocess hop per artifact (`Slang → slangc → metallib` MVP, `Slang → slangc → DXIL` post-MVP). SPEC §3 collapse 1; this design §3.4. |
| `ShaderCompiler::compile(desc) → ShaderCompileResult` (in-process trait) | **Partial — covered by `IShaderBackend::compile` only.** | Slangc subprocess invoked behind the trait via the `glibre-shadercc` driver; never linked in-process (SPEC §3 collapse 3, §4.3 invariant 1). This design §3.5 + §3.6. |
| `ShaderTarget { Dxil, SpirV, MetalLib }` | **Partial — `MetalLib` MVP, `DXIL` post-MVP, `SpirV` deferred.** | `CompileTarget` enum (SPEC §5). Pipeline rejects `DXIL` until post-MVP support lands (`Error::UnsupportedTarget`). SPIR-V removed from the enum entirely (SPEC §11 acceptance criteria — #327 closed). |
| `metal-shaderconverter` as a second subprocess hop | **Refused.** | Slangc emits `metallib` directly from Slang; no transpile chain (SPEC §4.3 invariant 2). |
| Reflection extracted from a separate cross-reflection library | **Refused.** | slangc's native reflection API emits the reflection record paired with the bytecode in the *same* subprocess invocation (SPEC §3 collapse 4, §4.4 invariant 1). The pipeline forwards the paired payload to the §4.4 ingester. |
| Runtime `OnDemandCompiler` | **Refused for shipping.** | `CompilationPipeline` is `#if !GLIBRE_SHIPPING`-guarded out of the plugin (SPEC §4.3 invariant 3, §4.8 invariant 3). Editor / cooker is the only caller. |
| GR-2 / GR-4 in-process linkage | **Refused.** | slangc is invoked exclusively via the `glibre-shadercc` driver subprocess (SPEC §3 collapse 3, §6.2). |
| R-2.1.18 structured errors at every public boundary | **Covered.** | `std::expected<ShaderArtifact, shader::Error>`. Driver stderr is a stable JSON envelope mapped onto the closed §10 enum. This design §3.7 + §10. |

Glibre-native requirements added beyond harmonius:

- **One artifact = one sealed value.** The pipeline's success return is
  a single `ShaderArtifact` whose `bytecode` + `reflection` +
  `descriptor_layout` fields are all populated by construction. No
  partially-constructed artifact escapes the pipeline (SPEC §4.4
  invariant 1; this design §3.8).
- **Per-job determinism.** Two pipeline invocations with byte-equal
  `(PreprocessedSource, PermutationKey, CompileTarget, canonical
  flags)` produce structurally-equal `ShaderArtifact` values modulo
  the subprocess non-determinism that the canonicalization layer
  elides (SPEC §4.7 invariant 3; this design §3.7).
- **Refusal at every step is logged once and leaves no on-disk
  state.** The pipeline never writes to the CAS; failure leaves the
  cache byte-equal to the pre-call snapshot (`error-model.md`).
  This design §10.
- **Hot path is empty.** Shipping has no compile path; editor / cook
  is the sole hot path, and even there the pipeline is a cold-only
  surface (PHILOSOPHY §6, §8). This design §5 + §9.
- **Bounded concurrency via worker pool.** Multiple jobs run in
  parallel; one job's state is self-contained (SPEC §4.3
  "Stateless"). This design §6.

## 3. Detailed Model

### 3.1 Aggregate composition

```text
CompilationPipeline (root, owned by editor / cooker)
├── const IShaderBackend&      backend_   (ref; selected by §4.7 dispatcher)
├── ArgvBuilder                argv_      (canonicalizer; deterministic)
├── DriverSpawner              spawner_   (subprocess launcher — fork/exec wrapper)
├── PayloadDecoder             decoder_   (length-prefixed stdout framer)
├── ErrorEnvelopeReader        errors_    (stderr JSON → shader::Error)
├── const reflection::Ingester& ingester_ (ref; §4.4 ingester)
├── DescriptorLayout::derive   derive_    (free function; §4.5)
└── LogSink&                   log_       (spdlog-backed)
```

Each compile job is a stack-local `Job` value (§3.2); the pipeline
itself is a thin orchestrator with no per-job state. One pipeline
instance per editor / cooker process suffices; pipelines are not
shared across `World`s but several pipelines may coexist in different
threads (§6).

### 3.2 `Job` (per-invocation value object)

```cpp
struct Job {
    const ShaderSource&  source;          // §4.1, already opened + validated
    const PermutationKey key;             // §4.2, already resolved
    const CompileTarget  target;          // §5 enum
    CanonicalFlags       flags;           // §3.3, derived from (key, target)
    ShaderHash           cache_key;       // §3.7, BLAKE3-composed
    eastl::string        argv_canonical;  // §3.4, sorted/deduped argv
};
```

A `Job` is immutable after construction; field order is fixed (any
re-order changes the cache key and is therefore a spec amendment, not
an in-place edit). Construction is `Job::build(source, key, target,
backend.flag_template())` which fills `flags`, `argv_canonical`, and
`cache_key` deterministically.

### 3.3 `CanonicalFlags`

The driver's argv is composed of three layers:

| Layer | Source | Examples |
|-------|--------|----------|
| Backend-pinned flags | `IShaderBackend::flag_template()` (slangc flag set) | `-O3`, `-Qstrip_debug`, `-fvk-use-dx-position-w`, fixed Slang stage flags |
| Target-derived flags | `CompileTarget` enum mapping | `--target=metallib` (MVP) or `--target=dxil` (post-MVP) |
| Permutation-derived defines | `PermutationKey` projection | `-DGLIBRE_SHADING_MODEL=2`, `-DGLIBRE_FEATURE_BITS=0x...`, `-DGLIBRE_RENDER_PATH=1`, `-DGLIBRE_LOD_TIER=2` |

`CanonicalFlags` carries the merged set as a sorted `eastl::vector<eastl::string>`:

```cpp
struct CanonicalFlags {
    eastl::vector<eastl::string> flags;   // sorted ascending, deduped
    ShaderHash                   hash;    // BLAKE3 of concatenated bytes (§7.3 cache rule)
};
```

**Canonicalization rule (deterministic).** `flags` is built by:

1. Concatenating the three layers in fixed order.
2. Sorting ascending (lexicographic) — `eastl::sort`.
3. Deduplicating adjacent equals — `eastl::unique`.
4. Hashing the joined-by-`\0` byte stream via BLAKE3 → `flags.hash`.

Two semantically-equal flag sets always produce byte-equal `flags`
and byte-equal `hash` (SPEC §4.3 invariant 4).

**Define injection from key.** The four-axis projection is a pure
function: `to_defines(PermutationKey) → eastl::array<eastl::string, 4>`.
Implemented in `permutation/` (sibling design); the pipeline calls
through the public surface and never re-decodes axes. The four
defines map to slangc preprocessor inputs, so the entire permutation
state space is a single expansion of one Slang translation unit at
slangc time — there is no pre-expansion or `#ifdef`-rewriting outside
slangc.

### 3.4 Driver argv schema

The pipeline never spawns slangc directly. It spawns
`glibre-shadercc` (lives at `tools/shadercc/`, SPEC §6.2), which in
turn spawns slangc with the pinned reproducibility flags. The driver
binary path is resolved once at pipeline construction via
`IShaderBackend::driver_path()` and is held constant for the
pipeline's lifetime.

Driver argv shape (locked by SPEC §6.2; this section transcribes it
for the pipeline implementer):

```text
glibre-shadercc
    --schema-version=1
    --source-path=<absolute path to .slang file>
    --include-root=<absolute project source root>
    --target=<metallib|dxil>
    --shader-hash-stamp=<64-hex-char ShaderHash from §3.7>
    --flag <flag>     (repeated, sorted ascending)
    --flag <flag>
    ...
    --output-stdout-length-prefixed
    --error-envelope=json
```

The `--shader-hash-stamp` argument is **input only**: the driver
echoes it back in the stderr envelope on failure but does not derive
any output bytes from it. Its purpose is to let the editor / cooker
trace which job caused which log line without parsing argv.

`argv_canonical` is the joined-by-`\n` representation of the above
list with no quoting (the driver argv is a `posix_spawn` argv array
on macOS, not a shell string). The plugin canonicalizes once; the
driver re-canonicalizes once (sort + dedupe) for defense-in-depth and
fails with `Error::CompilerInvocationFailed` if the canonical form
differs.

### 3.5 Subprocess lifecycle (one job)

Each `Job` runs through eight stages, in order:

```text
1. compose_argv         (§3.3 + §3.4)            — pure function
2. compose_cache_key    (§3.7)                   — pure function
3. spawn_driver         posix_spawn → pid        — system call
4. write_source_stdin   piped preprocessed bytes — non-blocking write loop
5. await_exit           kqueue + timeout         — bounded wait
6. capture_stdout       length-prefixed framer   — non-blocking read loop
7. capture_stderr       JSON envelope reader     — non-blocking read loop
8. assemble_artifact    decode + reflect + derive (§3.8)
```

**Stage 3 (spawn).** `posix_spawn` with stdin / stdout / stderr piped;
file actions close every other inherited descriptor; the spawn
attribute sets `POSIX_SPAWN_SETSIGMASK` to block `SIGCHLD` until
`await_exit` registers it. Failure (driver missing, executable bit
unset, sandbox-profile rejection) → `Error::CompilerInvocationFailed`
(SPEC §10.2 row).

**Stage 4 (stdin).** The pipeline writes the
`PreprocessedSource.bytes` onto the driver's stdin in a non-blocking
loop (`EAGAIN` → poll). The driver reads stdin to EOF before invoking
slangc; this avoids leaking the source path through the filesystem
and lets the sandbox keep read-deny on the project root for slangc's
own descriptor table. EOF is signalled by closing the parent's write
end after the last byte.

**Stage 5 (await).** A `kqueue` `EVFILT_PROC` watch on the spawned pid
plus a `EVFILT_TIMER` of `kCompileWallClockBudget` (§9). Whichever
fires first wins:

| Event | Action |
|-------|--------|
| `EVFILT_PROC` (NOTE_EXIT) | proceed to stage 6, capture exit status |
| `EVFILT_TIMER` | `kill(pid, SIGTERM)` then `SIGKILL` after 100 ms grace; return `Error::CompilerTimedOut` |

**Stages 6 + 7 (capture).** stdout and stderr are drained in
parallel via `kqueue` `EVFILT_READ` on both file descriptors. stdout
is length-prefixed: `[u32 le bytecode_len][bytecode][u32 le
reflection_len][reflection]`. stderr is one JSON object per
invocation (driver always emits, even on success — empty `errors[]`
on success). Both descriptors must EOF before the artifact is
considered complete; a half-closed stdout or stderr after exit-zero is
`Error::MetalLibEmitFailed`.

**Stage 8 (assemble).** See §3.8.

### 3.6 stderr error envelope

The driver emits a stable JSON envelope on stderr in every case:

```json
{
  "schema_version": 1,
  "shader_hash_stamp": "<64-hex>",
  "exit_code": 0,
  "errors": [
    {
      "kind": "<closed_shader_Error_enum_name>",
      "stage": "<frontend|backend|reflection|driver>",
      "slang_diagnostic": {
        "file": "<project-relative-path>",
        "line": 42,
        "column": 17,
        "severity": "<error|warning>",
        "message": "<slang diagnostic verbatim>"
      }
    }
  ]
}
```

`ErrorEnvelopeReader` parses this with a hand-written zero-copy
reader (no JSON library on the hot path). On parse failure of the
envelope itself the pipeline returns
`Error::CompilerInvocationFailed` (envelope corruption is treated as
a driver protocol violation, not a Slang diagnostic).

The `kind` field maps onto the closed §5 `shader::Error` enum via a
static table; unknown `kind` values map onto `CompilerExitNonZero`
(closest-fit `refuse` arm) and the unknown name is logged at `warn`
for the spike's deliverable list to absorb in a future amendment.

### 3.7 Cache-key composition

```cpp
ShaderHash compose_cache_key(
    const ShaderSource&  source,
    const PermutationKey key,
    const CompileTarget  target,
    const CanonicalFlags& flags) {
  Blake3Hasher h;
  h.update(source.preprocessed().total_hash.bytes);    // 32 B
  h.update(key.to_bytes());                            //  6 B
  h.update(flags.hash.bytes);                          // 32 B
  std::byte t = static_cast<std::byte>(target);
  h.update(eastl::span(&t, 1));                        //  1 B
  return ShaderHash{h.finalize()};
}
```

This is the §2 / §4.6 invariant 1 BLAKE3 composition rendered as
code. The composition order is locked: `source_hash || key.to_bytes()
|| flags_hash || u8(target)`. Any re-order would invalidate every
existing cooked archive.

The cache key is computed *before* the subprocess spawns. The cooker
checks `cas_store.has(cache_key)` and skips the spawn entirely on hit
(SPEC §6.4 step 2e). For the editor / hot-reload path the cache check
is the cooker's responsibility (§8); the pipeline always spawns when
called.

### 3.8 Artifact assembly

On stage 8 the pipeline holds: `bytecode_bytes`, `reflection_bytes`,
`exit_code == 0`, `errors[].size() == 0`.

```text
1. ingester_.ingest(reflection_bytes) → ReflectionBlob
   - errors → Error::ReflectionExtractionFailed
   - (frequency-tagging refusals → DescriptorFrequencyAmbiguous /
     DescriptorFrequencyMissing per §4.4 inv 3)
2. DescriptorLayout::derive(reflection_blob) → DescriptorLayout
   - errors → DescriptorFrequencyAmbiguous / DescriptorFrequencyMissing
3. ShaderArtifact{
       key                 = job.key,
       target              = job.target,
       hash                = job.cache_key,
       bytecode            = std::move(bytecode_bytes),  // 0-copy move
       reflection          = std::move(reflection_blob),
       descriptor_layout   = std::move(descriptor_layout),
   }
```

The artifact is **sealed**: no field is mutable after assembly, the
type has no public mutator, and the value is returned by-move out of
`compile()`. Ownership of the bytecode buffer is the caller's
(cooker / editor); the pipeline retains no copy.

If stage 1 or 2 fails the pipeline returns the typed error and the
in-flight `bytecode_bytes` is dropped. The driver subprocess has
already exited at this point; there is no resource leak and no
half-published artifact.

### 3.9 Lifetime + ownership diagram

```text
caller (cooker / editor) ──► CompilationPipeline::compile(job)
                                     │
                                     ▼
                          spawn ─► glibre-shadercc ─► slangc
                                     │
                                     ▼
                          stdout (bytecode + reflection bytes)
                          stderr (envelope JSON)
                          exit code
                                     │
                                     ▼
                          assemble ─► ShaderArtifact ──► caller
```

The pipeline owns the subprocess pid + pipes for the duration of one
`compile()` call and not one byte longer; on return the artifact is
the caller's, and on error nothing escapes (`std::expected` carries
only the error enumerator).

## 4. Public Surface

### 4.1 Types (locked from SPEC §5)

The pipeline does not introduce new public types; it consumes and
produces the §5 `ShaderArtifact`, `ShaderSource`, `PermutationKey`,
`CompileTarget`, `ReflectionBlob`, `DescriptorLayout`, and
`ShaderHash` declarations verbatim. The `Job` value object (§3.2) is
internal — the pipeline's public ABI surface is exactly the §5
`IShaderBackend::compile` virtual.

### 4.2 `IShaderBackend::compile` contract

```cpp
#if !GLIBRE_SHIPPING
class SlangBackend final : public IShaderBackend {
public:
    std::expected<ShaderArtifact, Error>
    compile(const ShaderSource& source,
            const PermutationKey& key,
            CompileTarget target) override;

    // ... reflect, link, capabilities (SPEC §4.7) — not pipeline's surface.
};
#endif
```

**Preconditions.**

- `source` is the result of a successful `ShaderSource::open(...)`
  call (§4.1 sibling design); `source.preprocessed().total_hash` is
  populated.
- `key.is_well_formed()` is `true` (§4.2 sibling design).
- `target` is one of the supported `CompileTarget` enumerators for
  the active `IShaderBackend` capabilities. `target == DXIL` before
  post-MVP support lands returns `Error::UnsupportedTarget`.
- The thread is not the render-driver thread (`frame-phases.md` —
  driver thread runs frame phases 1–9; pipeline runs offline / in a
  worker pool).

**Postconditions on success.**

- The returned `ShaderArtifact` has `key == job.key`,
  `target == job.target`, `hash == job.cache_key`, non-empty
  `bytecode`, fully-populated `reflection`, fully-derived
  `descriptor_layout`.
- No CAS state was mutated (the cooker is the sole writer per SPEC
  §4.6 invariant 1).
- One subprocess was spawned and one subprocess exited; no orphan
  pids, no leaked file descriptors.

**Postconditions on failure.**

- The returned `std::expected<...>` carries one `shader::Error`
  enumerator from §10.
- No partial `ShaderArtifact` is observable (no half-populated
  fields, no mutated globals).
- One log line is emitted at `error` (driver / spawn failures) or
  `warn` (slang diagnostic) per `error-model.md` logging rules.
- All subprocess resources (pid waited on, fds closed) are reaped.

**Thread-safety.** `compile` is a member function on a stateless
backend. Concurrent calls with disjoint `Job` arguments are safe.
Concurrent calls with overlapping arguments are also safe (each call
owns its own subprocess pipes), at the cost of duplicating one
subprocess invocation; deduplication is the cooker's responsibility
(SPEC §6.4 step 2e).

### 4.3 No new ABI surface

The `compile()` virtual is `#if !GLIBRE_SHIPPING`-guarded at the
public header (SPEC §5). The shipping plugin's vtable does not contain
the slot; the dynamic linker cannot resolve the symbol; and the
`backend/` directory is excluded from the shipping link target (SPEC
§6.5). This is the §10.3 blanket rule rendered as an ABI fact.

The driver binary `glibre-shadercc` is itself outside the plugin's
ABI surface — it is a `tools/` artifact (`specs/tools/SPEC.md`) that
the plugin spawns, never a symbol the plugin links to. Slangc version
pinning therefore does not cross the plugin ABI seam (PHILOSOPHY §1).

## 5. Hot / Cold Path Split

The pipeline is **cold-only**. It never executes during a frame
(phases 1–9 of `frame-phases.md`). It never executes in shipping.

| Path | Surface | When | Cost |
|------|---------|------|------|
| Build-time cook | `ShaderCache::cook()` walks the resolved permutation set; calls `compile()` for each cache miss (SPEC §6.4 step 2f) | offline build / CI | bounded by the cooker's wall-clock budget (§9.2); orthogonal to the per-frame budget |
| Editor on-save | filesystem watcher (#719) → `inject_source_diff` (SPEC §8.6) → `recompile_affected` → `compile()` for the affected `(PermutationKey, target)` set | dev / editor build | per-job sub-second target (§9.1); off the render-driver thread |
| E2E test | direct call from `tests/shader/integration/` against a stub or real driver | CI | bounded by the per-job budget (§9.1) |

There is no hot path. The shipping `shader.dylib` does not link the
pipeline TU; runtime calls land on the read-only cache path
(`ShaderCache::Library::get`) which never invokes the pipeline.

The hot-reload barrier (`reviews/decisions/hot-reload-protocol.md`)
schedules a recompile at `Phase::HotReload` (frame-phase 10 — between
frames). The pipeline is the body of that recompile; its cost is
bounded by §9.1 wall-clock, not by phase 1–9 frame-budget cells.

## 6. Concurrency

### 6.1 One job is self-contained

A single `Job` runs on a single thread for the duration of one
`compile()` call. The job owns its subprocess pid, its three pipe
file descriptors, and its stack-local buffers. Nothing is shared with
other in-flight jobs except the (immutable) `IShaderBackend&` and
the (logger) `LogSink&`. There is no cross-job mutex on the pipeline's
hot path.

### 6.2 Worker pool scales horizontally

The cooker (`cache/cooker.cpp`) and the editor's recompile dispatcher
each maintain an `eastl::vector<std::thread>` worker pool sized to
`std::thread::hardware_concurrency()` (M1 baseline: 4 firestorm + 4
icestorm = 8 cores). Each worker:

```cpp
while (auto job = job_queue.pop()) {
    auto result = pipeline.compile(job->source, job->key, job->target);
    if (result) {
        cas_store.insert(*result);   // cooker only; editor publishes via §8.5 event
    } else {
        log_compile_failure(*job, result.error());
    }
}
```

The job queue is an MPSC ring buffer with bounded capacity (256 jobs
on the M1 baseline). Producers block on full; the cooker is the sole
producer in the cook path, and the editor's recompile dispatcher is
the sole producer in the editor path. Workers compete on `pop()`
through a single mutex protecting the queue head; the contention is
nominal because each pop is followed by a long subprocess wait.

### 6.3 Subprocess parallelism budget

slangc invocations are independent processes and parallelize across
cores subject to host pressure. The driver does not pin to a CPU; the
kernel scheduler distributes. On the M1 baseline an 8-wide worker
pool sustains ~6.5 effective slangc instances after kernel +
filesystem overhead (measured in spike #710's harness; recorded here
as a non-binding planning datum).

### 6.4 Re-entrancy

The pipeline is **not** re-entrant: a worker that has spawned a
subprocess for `Job A` must not call `compile` again before the
spawn completes. This is enforced by structure (each worker is a
single thread running one job at a time) — there is no callback
mechanism that would re-enter `compile`.

### 6.5 No cross-thread shared state

The pipeline's own data members are either `const&` (backend, ingester,
log sink) or function-local (per `compile` call). No mutable state is
held across calls. Multi-pipeline configurations (e.g. one pipeline
per worker thread) are valid and incur no extra synchronization.

### 6.6 Frame-phase ownership

The pipeline owns no frame phase. `frame-phases.md` Phase 10
(HotReload) is owned by `HotReloadBarrier`, which calls into the
pipeline as a coroutine-style step but does not delegate phase
ownership. Driver-thread invariants (frame-phases.md §"Driver Thread
Invariants") forbid the pipeline from running on the driver thread;
the editor's recompile dispatcher owns its own thread pool, off-driver.

## 7. Persistence + ABI

### 7.1 The pipeline persists nothing

`CompilationPipeline` is a transient orchestrator. It owns no on-disk
state; the only persistent records in the `shader` context are the
§7 SPEC schemas (`ShaderArtifactRecord`, `ShaderCacheManifest`,
`ReflectionRecord`), all owned by the §4.6 cache aggregate (sibling
`shader-cache-design.md`). The pipeline reads no Fory blobs and writes
none.

The pipeline's *output* — `ShaderArtifact` value — is what the cooker
inserts into the CAS via `cas_store.insert(ShaderArtifactRecord{...})`.
The mapping from `ShaderArtifact` (in-memory, §5) to
`ShaderArtifactRecord` (Fory, SPEC §7.2) is the data middleman's
codegen surface (`reviews/decisions/fory-codegen.md`); the pipeline
never serializes either form.

### 7.2 Cache-key ABI

The `ShaderHash` returned by §3.7 is the cross-aggregate ABI that
binds the pipeline to the cache. Its byte composition is locked
(SPEC §2 / §4.6 invariant 1; this design §3.7):

```text
ShaderHash = BLAKE3( source_hash[32]
                  || key.to_bytes()[6]
                  || flags_hash[32]
                  || u8(target) )
```

Any change to this composition is a manifest-incompatible change:
existing cooked archives become unreadable. The composition is
therefore frozen for v1; future axes ride inside `key.to_bytes()` or
`flags_hash` (additive) and never alter the hash recipe.

### 7.3 Plugin-ABI surface

The pipeline contributes nothing to the `glibre-types.dylib` ABI.
Its only public symbol is `IShaderBackend::compile`, which is the
trait-level virtual from SPEC §5 (already enumerated in the
manifest's `systems` / `passes` lists is **false** — the trait is
private to the `shader` plugin and does not enter the `core`
registries; `reviews/decisions/plugin-abi.md`).

The driver schema version (§3.4 `--schema-version=1`) is part of the
`shader` plugin's contract with `tools/shadercc/` but not of any
plugin's ABI seam. Bumping it is a coordinated change between this
plugin and the driver, gated by integration tests.

### 7.4 No middleman growth

The pipeline does not add types to `glibre-types.dylib`. The
data-middleman boundary (`reviews/decisions/plugin-abi.md`,
`fory-codegen.md`) sees only the §7 SPEC schemas owned by the cache
aggregate.

## 8. Hot-Reload Integration

### 8.1 Trigger

The pipeline is invoked from exactly one hot-reload trigger: a
`shader` source change (SPEC §8.1). The platform file watcher
(`specs/platform/SPEC.md` / spike #719) raises a content-change event;
the editor's re-key dispatcher computes the affected
`(SourceId, PermutationKey, CompileTarget)` set; for each affected
tuple the dispatcher submits a `Job` to the worker pool (§6.2);
each worker calls `pipeline.compile(...)`.

The pipeline does not subscribe to the watcher directly. The watcher
→ dispatcher → pipeline chain is (intentionally) one-way; the
pipeline observes neither the source nor the watcher.

### 8.2 What survives across a recompile

- **The pipeline instance.** Stateless (§6.5) — the same instance
  serves both pre- and post-recompile jobs.
- **The driver subprocess binary.** `glibre-shadercc` is not part of
  any reload event; it lives at a fixed path under `tools/`.
- **Backend instance.** `IShaderBackend` is held by the plugin loader
  outside the pipeline's lifetime.
- **Concurrent jobs already in flight.** They run to completion
  against the *prior* source bytes — the source-snapshot semantics
  are the pipeline's not the dispatcher's: every `compile` call
  reads `source.preprocessed().bytes` once at stage 4 and never
  re-reads. A subsequent edit cannot tear an in-flight job.

### 8.3 What gets re-derived

For each affected `(PermutationKey, CompileTarget)` tuple, a fresh
`Job` runs end-to-end:

1. `compose_argv` re-runs against the new `PreprocessedSource`.
2. `compose_cache_key` produces a new `ShaderHash` (different
   `source_hash`, same `key.to_bytes()`, same `flags_hash`, same
   `target`).
3. The driver subprocess emits new bytecode + new reflection.
4. The new `ShaderArtifact` is inserted into the CAS at the new hash
   (idempotent — SPEC §4.6 invariant 1; the prior artifact at the
   prior hash remains until cooker GC).
5. The editor publishes `ShaderArtifactReplaced` (SPEC §8.5) with
   `affected_old_hashes` / `affected_new_hashes` lists. `render` is
   the sole subscriber relevant to PSO invalidation.

The pipeline contributes step 3 only. Steps 1, 2, 4, 5 are owned by
the cooker / editor / dispatcher.

### 8.4 Refusal cases (pipeline-side)

The pipeline refuses (returns a typed error; emits no artifact; the
prior cache state remains live) in exactly the cases enumerated in
SPEC §10.2 that map onto pipeline failures:

| Trigger | Error arm | §10.1 severity |
|---------|-----------|----------------|
| driver missing / executable bit / sandbox reject | `CompilerInvocationFailed` | refuse |
| driver / slangc exit nonzero (frontend or backend) | `CompilerExitNonZero` | refuse |
| driver wall-clock timer fired | `CompilerTimedOut` | refuse |
| target not yet supported by backend | `UnsupportedTarget` | refuse |
| stdout missing or truncated metallib | `MetalLibEmitFailed` | refuse |
| reflection ingester rejected payload | `ReflectionExtractionFailed` | refuse |
| frequency-tagger ambiguity | `DescriptorFrequencyAmbiguous` | refuse |
| frequency-tagger gap | `DescriptorFrequencyMissing` | refuse |
| any code path in `GLIBRE_SHIPPING==1` | `ShippingCompilationAttempted` | fatal |

In every refusal the pipeline emits one structured `error` log (or
`warn` for slang diagnostics) per `error-model.md`, drops every
in-flight buffer, and returns. The cache is not touched. The §8.5
observer event is *not* published in this case (publication is the
dispatcher's responsibility on a successful recompile of the entire
affected set; partial success → no event — SPEC §8.4 close).

### 8.5 No `migrate` function

The pipeline produces immutable artifacts; there is no across-swap
state to migrate. The `migrate_<Type>_vN_to_vNplus1` family
(`hot-reload-protocol.md` §"Migrate Function Contract") applies only
to persistent on-disk records, which the pipeline does not own (§7.1).
A schema bump on `ShaderArtifactRecord` / `ReflectionRecord` /
`DescriptorLayoutRecord` invokes the §4.6 cache aggregate's
migration path; the pipeline simply produces freshly-typed
`ShaderArtifact` values that match the host's middleman ABI by
construction.

### 8.6 Test hooks

The SPEC §8.6 `inject_source_diff` and `recompile_affected` test
hooks are owned by the dispatcher / cache aggregate respectively.
The pipeline exposes no test-only entry points beyond the standard
`compile()` virtual; tests drive it directly with hand-built
`ShaderSource` and `PermutationKey` values (§11.1).

## 9. Performance

### 9.1 Per-job wall-clock budget (cold path)

The pipeline's per-`compile()` wall-clock budget is **soft-bounded at
sub-second** for a typical MVP shader (≤ 4 entry points, ≤ 500 lines
of Slang, ≤ 64 binding slots). Composition:

| Stage | Budget | Notes |
|-------|--------|-------|
| `compose_argv` (§3.3 + §3.4) | ≤ 50 µs | pure CPU; `eastl::sort` on ≤ 16 strings |
| `compose_cache_key` (§3.7) | ≤ 30 µs | one BLAKE3 of 71 bytes |
| `posix_spawn` + driver bring-up | ≤ 50 ms | dominated by driver startup |
| slangc compile (Slang → metallib, MVP) | ≤ 800 ms | **the dominant cost**; varies with shader complexity |
| stdout / stderr capture | ≤ 5 ms | length-prefixed framer + JSON envelope |
| reflection ingest + descriptor derive | ≤ 5 ms | sibling §4.4 / §4.5 |
| **Per-job total** | **≤ 900 ms typical, ≤ 2.0 s ceiling** | wall-clock |

The 2.0 s ceiling is the per-job timer fired by `EVFILT_TIMER`
(§3.5). Anything beyond is `Error::CompilerTimedOut`. The 900 ms
typical is an aspirational target tracked by the
`shader-cook-time-budget` spike (SPEC §9.2); it is not a CI gate on
this aggregate.

### 9.2 Per-cook total budget

The cooker walks the resolved permutation set (SPEC §6.4 step 1).
For an MVP project size of ~10 k artifacts and an 8-wide worker pool
on M1, total cook wall-clock is bounded by:

```text
total ≈ ceil(artifacts / pool_size) × per_job_typical
      ≈ ceil(10000 / 8) × 0.9 s
      ≈ ~1100 s typical
      ≈ ~18 min
```

This is offline build cost and is **not** part of the per-frame
budget (`reviews/decisions/perf-budget.md`). The
`shader-cook-time-budget` spike owns the contract; this design
records only the per-job constraint that contributes to it.

### 9.3 In-flight memory ceiling

A single `Job` holds, at peak, the following allocations (all
`ContextTag::shader`):

| Buffer | Typical | Ceiling | Tag |
|--------|---------|---------|-----|
| `argv_canonical` | ≤ 1 KiB | 4 KiB | `shader.compile.argv` |
| stdin write buffer (preprocessed source) | ≤ 64 KiB | 1 MiB | `shader.compile.stdin` |
| stdout capture (bytecode bytes) | ≤ 256 KiB | 4 MiB | `shader.compile.stdout` |
| stderr capture (envelope JSON) | ≤ 4 KiB | 64 KiB | `shader.compile.stderr` |
| reflection bytes (parsed by ingester) | ≤ 64 KiB | 1 MiB | `shader.compile.reflection` |
| **Per-job ceiling** | | **~6 MiB** | |

For the 8-wide worker pool the cooker peaks at ~48 MiB across all
in-flight jobs. This is **not** counted against the 32 MiB shipping
heap ceiling (SPEC §9.1) — the pipeline is shipping-excluded. It
falls under the editor / cooker's own perf-budget cell
(`reviews/decisions/perf-budget.md` "tools" row, allocator rule §4
transient arena).

The pipeline drains every per-job allocation by `compile()` return.
Strict mode (`GLIBRE_ALLOC_STRICT=1`) asserts zero `shader.compile.*`
live bytes between jobs; a leak is `core::Error::OutOfBudget{detail
="leak"}`.

### 9.4 Allocator rules

- All pipeline allocations route through `glibre::PerContextAllocator`
  with `ContextTag::shader` (per `perf-budget.md` Allocator Rules).
- Per-job buffers use a thread-local arena (`shader.compile.arena`)
  that is reset between jobs — zero free-list churn.
- The reflection ingester and descriptor-layout deriver own their
  own pools (sibling designs); the pipeline forwards spans, never
  copies.

### 9.5 No frame-budget contribution

The pipeline contributes **zero** to the engine-wide per-frame budget
(`perf-budget.md` `shader` row). It does not run on the driver
thread (`frame-phases.md` Driver Thread Invariants). It does not run
in shipping (SPEC §4.3 invariant 3). It does not run during
phase 1–9 (`frame-phases.md`).

Editor / hot-reload recompiles run during Phase 10 (HotReload, between
frames) on the dispatcher's worker pool, not the driver thread; they
contribute to the editor's own budget cell, not to `shader`'s 0.00 ms
cell.

### 9.6 CI gate

Pipeline-specific CI assertions complement SPEC §9.6:

1. **Per-job wall-clock ≤ 2.0 s** under the smoke test's typical
   shader fixture set; failures fail the PR.
2. **Zero `shader.compile.*` live bytes between jobs**
   (`GLIBRE_ALLOC_STRICT=1` replay).
3. **Subprocess reaped by `compile()` return** — no zombie pids,
   no leaked file descriptors (asserted via `lsof` snapshot diff in
   `tests/shader/integration/`).
4. **Determinism** — running the same job twice in sequence produces
   structurally-equal `ShaderArtifact` values modulo the embedded
   `bytecode` (subprocess timestamp non-determinism is elided by the
   driver's pinned reproducibility flags; SPEC §6.2).

## 10. Failure Modes

### 10.1 Pipeline-emitted `shader::Error` arms

The pipeline can emit any of the following enumerators from SPEC §5
/ §10.2. Any other arm in §5 is emitted by a sibling aggregate
(ingester, cache, source) and surfaces *through* the pipeline only
when the pipeline calls into that sibling.

| Enumerator | Trigger | Recovery | Severity |
|------------|---------|----------|----------|
| `EntryPointMissing` | The requested job names a non-existent entry point on the source. | refuse compile; pipeline propagates from `IShaderBackend::compile` argument check before spawn. | refuse |
| `EntryPointStageAmbiguous` | Source carries an entry point with zero or multiple `[shader(...)]` tags (caught by §4.1, surfaced through pipeline). | refuse compile; defer to source aggregate's resolution. | refuse |
| `PermutationKeyOutOfRange` | `key.is_well_formed()` is `false` at job construction. | refuse compile; codegen-table drift between caller and pipeline. | fatal |
| `CompilerInvocationFailed` | `posix_spawn` failed; sandbox profile rejected; driver binary missing or not executable; envelope parse failed (driver protocol violation). | refuse compile; no retry; surface to caller for human triage. | refuse |
| `CompilerExitNonZero` | Driver exited non-zero; `errors[]` non-empty; slang frontend or backend diagnostic. | refuse compile; forward `errors[]` JSON verbatim to editor / cooker logs; prior CAS entry remains live for that key. | refuse |
| `CompilerTimedOut` | `EVFILT_TIMER` fired before `EVFILT_PROC` (NOTE_EXIT). | refuse compile; **no in-process retry**; the cooker / editor may re-queue at its layer. | refuse |
| `UnsupportedTarget` | `CompileTarget == DXIL` before post-MVP support; or backend's `capabilities()` does not advertise the target. | refuse compile; surfaced as a permutation-enumeration bug. | refuse |
| `MetalLibEmitFailed` | stdout missing / truncated bytecode segment after exit-zero; or driver-side bytecode validation rejected the metallib container. | refuse compile; treat as driver protocol violation. | refuse |
| `ReflectionExtractionFailed` | The `reflection/` ingester (§3.8 step 1) rejected the reflection payload. | refuse publish (artifact never reaches CAS); prior live artifact unchanged. | refuse |
| `DescriptorFrequencyAmbiguous` | The frequency tagger found a binding with multiple frequency annotations (SPEC §4.4 inv 3). | refuse publish. | refuse |
| `DescriptorFrequencyMissing` | The frequency tagger found a binding with no frequency annotation. | refuse publish. | refuse |
| `CapabilityNotSupported` | A permutation requires a capability the backend does not advertise (`IShaderBackend::capabilities()`). | refuse compile; defense-in-depth check — the offline enumerator should have refused first. | refuse |
| `ShippingCompilationAttempted` | A `GLIBRE_SHIPPING==1` build entered `compile()` (only reachable via mistakenly-enabled test harness; the symbol is normally absent at link time). | refuse and abort the calling thread; structured log with `(SourceId, PermutationKey, CompileTarget)`. | fatal |

The pipeline does **not** emit `SourceNotFound`, `SourceParseFailed`,
`IncludeEscape`, `IncludeCycle`, `LinkFailed`,
`SpecializationConstantMissing`, `CacheLookupMiss`, `CacheCorrupt`,
`CacheIntegrity`, or `CacheReadOnlyViolation` directly. Those arms
belong to `ShaderSource`, `IShaderBackend::link`, or the cache
aggregate respectively; the pipeline can propagate them only when a
caller composes pipeline + sibling operations and a sibling fails.

### 10.2 Refusal vs publish — the subprocess seam

Every pipeline error from §10.1 is **`refuse`** severity except
`PermutationKeyOutOfRange` and `ShippingCompilationAttempted`
(both `fatal`). The §10.1 contract per `shader/SPEC.md` §10.1 binds
the severity to a cache-state outcome:

- `refuse` arms → in-flight artifact dropped; **CAS unchanged**;
  prior `ShaderHash` continues to resolve through
  `ShaderCache::get`. The dispatcher does not publish a
  `ShaderArtifactReplaced` event for the failed permutation (SPEC
  §8.4 close).
- `fatal` arms → callable-thread abort + log; no recovery; the
  caller must rebuild the cook. The §4.6 invariant on read-only
  shipping cache is preserved by construction (no insert path was
  taken).

### 10.3 Subprocess-reaping discipline

Every `compile()` return path — success or failure — must have
`waitpid`'d the spawned pid and closed the three pipe descriptors.
The pipeline owns this discipline through RAII helpers:

```cpp
class SubprocessHandle {
public:
    ~SubprocessHandle() {
        if (pid_ != -1) {
            kill(pid_, SIGKILL);  // belt-and-suspenders
            int status;
            ::waitpid(pid_, &status, 0);
        }
        close_if_open(stdin_fd_);
        close_if_open(stdout_fd_);
        close_if_open(stderr_fd_);
    }
private:
    pid_t pid_{-1};
    int   stdin_fd_{-1};
    int   stdout_fd_{-1};
    int   stderr_fd_{-1};
};
```

The destructor is the pipeline's last line of defense against zombie
pids and leaked fds. Every error path that returns out of `compile`
runs this destructor by stack-unwinding through `std::expected`'s
construction.

### 10.4 Logging

Per `error-model.md`:

- `error` severity: `CompilerInvocationFailed`,
  `CompilerTimedOut`, `MetalLibEmitFailed`, `CacheReadOnlyViolation`
  (if the pipeline is mistakenly invoked against a read-only cache),
  `ShippingCompilationAttempted` (`fatal` — also aborts the thread).
- `warn` severity: `CompilerExitNonZero` (Slang diagnostic — author
  bug, not engine bug), `EntryPointMissing`,
  `EntryPointStageAmbiguous`.
- `info` severity: successful compile (suppressed in release-tooling
  builds; `--verbose` flag re-enables).

Every log line carries the structured `ErrorContext` per
`error-model.md`: `(SourceId, PermutationKey.to_bytes() hex,
CompileTarget, ShaderHash hex)`. The
`shader_hash_stamp` from §3.4 is emitted in both the log and the
driver's stderr envelope, giving editor / CI a single token to
correlate by.

### 10.5 Cross-references

- Closed enum source-of-truth: `specs/shader/SPEC.md` §5 / §10.2.
- Hot-reload refusal projection: `specs/shader/SPEC.md` §8.4
  (refusal cases 1–4); this design §8.4.
- Driver protocol: `specs/shader/SPEC.md` §6.2; this design §3.4–§3.6.
- Engine-wide error policy: `reviews/decisions/error-model.md`.
- Shipping refusal blanket rule: `specs/shader/SPEC.md` §10.3.

## 11. Test Plan

All tests are Catch2; sources under `tests/shader/`. Each `type:plan`
issue this design feeds owns one or more named tests below; the spec
acceptance criteria (`specs/shader/SPEC.md` §11) lists the
user-story-level tests this aggregate is on the hook for (#330
metallib emit; #325 dxil emit, post-MVP).

### 11.1 Unit tests (mocked driver, `tests/shader/compile/`)

The driver subprocess is replaced with a programmable mock
(`MockDriverSpawner`) whose stdout / stderr / exit are controlled by
the test. This isolates the pipeline's eight stages (§3.5) from
slangc's own behavior.

| Test name | Scope | Asserts |
|-----------|-------|---------|
| `compilation_pipeline_argv_is_canonical_sorted_deduped` | §3.3 + §3.4 | Two `compile()` calls with semantically-equal flag sets produce byte-equal `argv_canonical`. |
| `compilation_pipeline_argv_includes_target_metallib_for_metal` | §3.4 | `--target=metallib` present when `target == MetalLib`. |
| `compilation_pipeline_argv_rejects_dxil_pre_post_mvp` | §3.4 | `target == DXIL` returns `Error::UnsupportedTarget` if the active backend's `capabilities()` does not yet advertise it. |
| `compilation_pipeline_define_injection_from_permutation_key` | §3.3 | Defines reflect all four axes; bit-stable across hosts. |
| `compilation_pipeline_cache_key_is_blake3_of_locked_composition` | §3.7 | `compose_cache_key` hash equals the spec recipe; recipe order is locked. |
| `compilation_pipeline_subprocess_lifecycle_reaps_pid` | §3.5 + §10.3 | After every error path the pid is `waitpid`'d and the three fds closed. |
| `compilation_pipeline_timeout_kills_subprocess_and_returns_timed_out` | §3.5 stage 5 | `EVFILT_TIMER` fires; `kill(SIGTERM)` then `SIGKILL`; `Error::CompilerTimedOut` returned. |
| `compilation_pipeline_invocation_failure_returns_compiler_invocation_failed` | §3.5 stage 3 | `posix_spawn` rejected → `Error::CompilerInvocationFailed`. |
| `compilation_pipeline_exit_nonzero_with_envelope_returns_compiler_exit_non_zero` | §3.6 | Stderr envelope `errors[].kind = "CompilerExitNonZero"` propagates through. |
| `compilation_pipeline_exit_nonzero_with_unknown_kind_falls_back` | §3.6 | Unknown `kind` maps to `CompilerExitNonZero` and warns. |
| `compilation_pipeline_truncated_stdout_returns_metallib_emit_failed` | §3.5 stage 6 | Length-prefix mismatch → `Error::MetalLibEmitFailed`. |
| `compilation_pipeline_envelope_parse_failure_returns_compiler_invocation_failed` | §3.6 | Malformed JSON envelope → driver protocol violation. |
| `compilation_pipeline_passes_reflection_bytes_to_ingester` | §3.8 step 1 | Mock ingester observes the exact reflection bytes from stdout. |
| `compilation_pipeline_seals_artifact_with_paired_reflection_and_layout` | §3.8 | Returned artifact's `reflection` and `descriptor_layout` are paired (same artifact, no field swap). |
| `compilation_pipeline_propagates_reflection_extraction_failed` | §3.8 step 1 | Mock ingester returns `ReflectionExtractionFailed`; pipeline does not insert anywhere. |
| `compilation_pipeline_propagates_descriptor_frequency_ambiguous` | §3.8 step 2 | `DescriptorLayout::derive` rejection → ambiguous arm; no artifact escapes. |
| `compilation_pipeline_no_state_leaks_between_jobs` | §6.5 | Run 100 jobs sequentially; assert zero `shader.compile.*` live bytes between calls. |
| `compilation_pipeline_concurrent_disjoint_jobs_run_independently` | §6.1 + §6.2 | 8 worker threads; 100 jobs each; all pids reaped; all artifacts byte-stable for fixed inputs. |

### 11.2 Integration tests (real driver, `tests/shader/integration/`)

Real `glibre-shadercc` + real slangc, against fixture Slang files
under `tests/shader/fixtures/`. These tests gate on the driver
binary being built (CMake `find_program(slangc)`-equivalent).

| Test name | Scope | Asserts |
|-----------|-------|---------|
| `compilation_pipeline_emits_metallib_via_subprocess` | end-to-end MVP target | A trivial Slang vertex+pixel pair compiles to a non-empty metallib; reflection has the expected entry-point list; descriptor layout partitions bindings by frequency. **(Spec §11 acceptance criterion #330.)** |
| `compilation_pipeline_emits_dxil_via_subprocess` | end-to-end post-MVP target | Same, with `--target=dxil`; gated on post-MVP feature flag. **(Spec §11 acceptance criterion #325; deferred until DXIL lands.)** |
| `compilation_pipeline_returns_compiler_exit_non_zero_for_slang_syntax_error` | error path | Fixture with deliberate Slang syntax error; assert `Error::CompilerExitNonZero`; assert log line carries the slang diagnostic verbatim. |
| `compilation_pipeline_returns_unsupported_target_when_backend_lacks_capability` | capability gate | A backend capabilities-stub claiming no DXIL support refuses `target == DXIL` with `Error::UnsupportedTarget`. |
| `compilation_pipeline_artifact_is_deterministic_across_two_runs` | §9.6 determinism | Run the same job twice; assert structurally-equal `ShaderArtifact` (modulo platform-stamp bytes elided by driver reproducibility flags). |
| `compilation_pipeline_concurrent_jobs_against_real_driver` | worker pool stress | 8 workers, 50 jobs against real slangc; assert all complete within §9.1 ceiling; no zombie pids; no fd leaks (lsof diff). |
| `compilation_pipeline_recompile_loop_publishes_new_hash` | hot-reload | Two compiles of "the same" source after an in-place byte mutation produce different `ShaderHash` values; the second insertion is idempotent on its own hash; the dispatcher (test scaffold) publishes `ShaderArtifactReplaced`. |

### 11.3 Performance microbenchmarks (`tests/shader/perf/`)

| Benchmark | Asserts |
|-----------|---------|
| `compilation_pipeline_per_job_typical_under_1s` | typical fixture compile ≤ 900 ms wall-clock on M1 baseline (non-binding aspirational; tracked under `shader-cook-time-budget`). |
| `compilation_pipeline_per_job_ceiling_under_2s` | adversarial fixture compile fails-or-completes within 2.0 s; never exceeds the timer. |
| `compilation_pipeline_strict_alloc_zero_leak` | `GLIBRE_ALLOC_STRICT=1` → 1000 jobs sequential → zero residual `shader.compile.*` bytes. |

### 11.4 E2E coverage

The acceptance-criterion spec tests (`shader/SPEC.md` §11) for #330
and #325 cover the pipeline through the cooker / editor; the spike
delivers no E2E test of its own (per CLAUDE.md "Tests by type" —
spike → deliverable doc). Once the user-story leaves run, their
E2E traces will exercise this design.

## 12. Open Questions

- [OPEN] **Driver-protocol envelope schema versioning.** §3.4 pins
  `--schema-version=1`; bumping it is a coordinated change between
  this plugin and `tools/shadercc/`. The owning record for the
  bump-handshake (which side amends first, which CI gate enforces
  the lockstep) is undecided; tracked as a follow-up under sub-epic
  #70. Owner: `shader` plugin lead. Resolution gate: first time the
  schema needs to grow (e.g. when DXIL lands and the envelope adds a
  `target_specific` field).
- [OPEN] **Worker-pool sizing override for editor vs cooker.** §6.2
  defaults the pool to `hardware_concurrency()`. The editor's
  on-save recompile may want a smaller pool (1–2 workers) so the
  user's interactive frame budget is not stolen by background
  slangc invocations. Owner: editor / `tools` context. Resolution
  gate: when the editor's first usability test surfaces a stutter
  during on-save recompile under contention.
- [OPEN] **Subprocess sandbox profile for non-macOS dev hosts.** SPEC
  §6.2 names `sandbox-exec` (macOS) and "platform-equivalent seccomp
  filter on Linux dev hosts". The exact Linux profile (allowed
  syscalls, seccomp-bpf bytecode) is not pinned; the MVP target is
  macOS and the pipeline's `posix_spawn` path is host-portable, but
  Linux dev hosts will need a concrete profile before they can run
  the cook. Owner: `platform` context (sibling spike). Resolution
  gate: first PR that adds a Linux dev-host CI runner.
- [OPEN] **Re-queue policy for `CompilerTimedOut`.** §10.1 forbids
  in-process retry, deferring to the cooker / editor caller. The
  caller's policy (re-queue once with a longer timeout? blame the
  shader and surface to the author? both?) is not yet specified.
  Owner: cooker design (sibling spike for `cache/cooker.cpp`).
  Resolution gate: first time a real shader hits the 2.0 s ceiling
  in a CI cook.
- [OPEN] **Driver argv shape for the post-MVP DXIL target.** §3.4
  pins MVP metallib argv. The DXIL flag set (root-signature flags,
  shader-model selector, validation toggles) will introduce a small
  family of additional flags; whether they ride inside
  `IShaderBackend::flag_template()` (transparent to this design) or
  require a new argv schema version (§3.4 `--schema-version=2`) is
  undecided. Owner: post-MVP DXIL plan (sub-epic TBD). Resolution
  gate: when the DXIL spike opens.
