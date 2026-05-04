# PSO-Cache Detailed Design

> Detailed design for the `render` context's `PSOCache` aggregate
> (`specs/render/SPEC.md` §4.1.7, §5 `PSOCache` / `PSOKey` /
> `PSOHandle`, §7.1.2 `PSOCacheRecord`, §8.3.2 hot-reload handoff,
> §9.5 heap row, the PSO-cache arms of §10 — `PsoCompileFailed`,
> `ShaderModuleLoadFailed`, `BarrierViolation` aliasing).
>
> All conclusions re-derived; harmonius prior art
> (`/Users/cjhowe/Code/harmonius/docs/design/rendering/pipeline-state-cache.md`,
> `pipeline-state-cache-test-cases.md`) cited as research input only
> (PHILOSOPHY §"How harmonius is used"). `harmonius` is
> incoherent prior art; its requirements are mined, its design is
> not preserved.

Refs: spike #766 — `[SPIKE] design-render-pso-cache-detailed`. Parent
sub-epic #759. Sibling task-breakdown spike blocked-by this
deliverable.

## 1. Purpose

`PSOCache` is the single aggregate in the `render` context permitted
to **store, look up, lazily build, and evict resident
`MTL::RenderPipelineState` / `MTL::ComputePipelineState` handles**
keyed by `PSOKey = (shader_hash, state_hash)`. Its one responsibility
is mapping a composite key — the cook-time shader-bytecode hash from
`shader` plus a deterministic render-state-fingerprint hash composed
inside render — onto a resident pipeline-state object, idempotently,
with at-most-one build per missing key. It serves the per-draw lookup
on the hot path, runs the cold lazy-build path on first miss, drives
LRU eviction against the §9.5 64 MiB sub-budget, and is the on-process
cache that the §7.1.2 `PSOCacheRecord` archive warms at startup.

What `PSOCache` explicitly **refuses** to own:

- **Shader source compilation.** Slang → AIR / metallib bytecode is
  `shader`'s job (specs/shader/SPEC.md §4.3 / §4.6, spike #749 / #755).
  The cache consumes `shader_hash` as an opaque 8-byte content-address
  and the matching `MTL::Library` resident bytes as a borrowed handle;
  it never invokes a shader compiler. A miss whose `shader_hash` has
  no resident library returns `render::Error::ShaderModuleLoadFailed`,
  not `PipelineCompileFailed` (§10).
- **Descriptor-layout derivation.** Slang reflection → 4-frequency
  argument-buffer layout is `shader`'s `DescriptorLayout` (spike
  #753); the layout is embedded inside `ShaderArtifact` and is part of
  what `shader_hash` content-addresses. `PSOCache` consumes
  `MTL::ArgumentEncoder` handles produced upstream and records them in
  the `Bindings` frequency-group binder (§4.1.4 `ArgumentBuffer`
  cache, owned by `resources/argument_buffer.cpp`, not by the cache).
- **Render-graph encoding.** Pass declaration, alias planning, barrier
  emission, queue assignment all live in the graph aggregates
  (§4.1.2–§4.1.5, spike #760). `PSOCache::get` returns a `PSOHandle`;
  the consumer (`Pass::execute`) is responsible for binding it on the
  matching `MTL::CommandEncoder`. The cache never touches an encoder.
- **The on-disk shader archive.** `<library_root>/shader/...` is owned
  by `shader::ShaderCache` (spike #755). Render's `<pso-archive>`
  (§7.1.2) is **disjoint** — it persists Metal's pipeline-compiler
  output (`MTL::BinaryArchive` blobs), not shader source bytecode.
- **Capability detection.** Whether the device supports mesh shaders,
  ray query, hardware ray tracing, or a specific argument-buffer tier
  is owned by `MetalDevice::capabilities()` (§4.1.6) backed by the
  per-host `CapabilityMask` (§7.1.3). The cache reads `CapabilitySet`
  to gate compute-vs-render PSO build paths but does not probe.
- **GPU memory accounting beyond its own row.** The 64 MiB §9.5 PSO
  cache row covers the resident `MTL::PipelineState` objects, the
  resident `MTL::BinaryArchive` page cache, and the per-pass
  binding-table prebuilds. Heap-allocator rules belong to
  `MetalDevice` (§4.1.6) and `glibre::PerContextAllocator`
  (`reviews/decisions/perf-budget.md` §"Allocator Rules"); the cache
  reports residency, it does not allocate heaps.
- **Hot-reload of plugin code.** The §8 protocol drives plugin reload;
  the cache's `MetalDevice`-owned residency outlives the plugin image
  by SPEC §8.3.2. The cache participates only via
  `invalidate_by_shader_hash` (called by `shader`'s reload hook) and
  `pin` (called by render's `glibre_plugin_register`).
- **Obj-C++.** All Metal interaction is via `metal-cpp`. No `.mm`
  files, no Objective-C selectors. PHILOSOPHY "Don'ts".

If the composite-key composition rule, the LRU eviction policy, the
build path's miss handling, the `MTL::BinaryArchive` persistence
seam, the hot-reload invalidation contract, or the closed list of
failure-mode arms changes, this design changes. Anything else is out
of scope.

## 2. Requirements Coverage

Mapping of harmonius requirements (mined from
`docs/design/rendering/pipeline-state-cache.md` and
`pipeline-state-cache-test-cases.md`) to MVP refusal-or-coverage.
Every entry is independently re-derived; harmonius is research input
only (PHILOSOPHY §"How harmonius is used").

| Harmonius source                                                                                                       | Glibre disposition (MVP)              | Coverage site                                                                                                                                |
|------------------------------------------------------------------------------------------------------------------------|----------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------|
| `pipeline-state-cache.md` R-2.3.9.1 — memory-resident PSO cache keyed by `PsoKey`; lookup O(log n)                     | **Covered (refined to O(1) average)**  | §3 below: open-addressed hash table on `PSOKey`; harmonius's "sorted vec" choice is rejected — a draw-path lookup is keyed by random hashes. |
| `pipeline-state-cache.md` R-2.3.9.2 — `PsoKey` = hash(device id, driver version, shader variant)                       | **Refused as written / re-cast**       | §3.2: in-memory `PSOKey = (shader_hash, state_hash)` only. Device + driver fingerprint move to the **on-disk** archive (§7), not the runtime key — the live cache is per-process / per-device by construction. |
| `pipeline-state-cache.md` R-2.3.9.3 — disk layout versioned by `CacheFormatVersion` + device fingerprint directory     | **Covered**                            | §7 below: directory tree mirrors SPEC §7.1.2 (`<gpu-id>/<glibre-version>/<host-id>/`); whole-archive invalidation, not field migration.       |
| `pipeline-state-cache.md` R-2.3.9.4 — automatic invalidation on device change, driver upgrade, shader recompile        | **Covered**                            | §8 below: provenance gating on `(gpu_id, metal_feature_set, os_build_hash, glibre_types_abi_hash)`; per-shader invalidation by `shader_hash`.|
| `pipeline-state-cache.md` R-2.3.9.5 — LRU GC with configurable on-disk cap (default 512 MiB)                           | **Covered (in-memory) / capped (disk)**| §9 below: in-memory LRU under the 64 MiB §9.5 row. Disk cap is **256 MiB** (SPEC §7.1.2 invariant 4), not 512 MiB; harmonius's number was for a different aggregate. |
| `pipeline-state-cache.md` R-2.3.9.6 — corrupted entries are isolated; cache reopens clean                              | **Covered (whole-archive)**            | §7 below: SPEC §7.1.2 invariant 1 — a single `archive_blob_blake3` mismatch silently discards that record; manifest mismatch wipes the directory. No tombstone list. |
| `pipeline-state-cache.md` R-2.3.9.7 — hot-reload sends `Invalidate(PsoKey)` to the render thread                       | **Covered (re-cast on shader_hash)**   | §8 below: invalidation is keyed on `shader_hash`, not `PSOKey` — one shader edit invalidates the entire `(shader_hash, *)` slice in one call.|
| `pipeline-state-cache.md` R-2.3.9.8 — descriptor layout inferred from DXIL / SPIR-V reflection once, cached            | **Refused (shader's job)**             | `shader::DescriptorLayout` (spike #753) owns this. The cache embeds nothing layout-shaped; it pins `MTL::ArgumentEncoder` handles by reference. SPEC §3 refusal restated. |
| `pipeline-state-cache-test-cases.md` — corruption recovery, GC, cold-start latency, hot-reload latency                 | **Covered**                            | §11 test plan below maps each scenario to a Catch2 test under `tests/render/pso_cache/`.                                                       |
| `pipeline-state-cache.md` "Per-backend serialization API" — D3D12 / Metal / Vulkan branches                            | **Refused (Metal-only)**                | MVP is Metal 4 only (PHILOSOPHY tech stack). The §6 `IBackendArchive` seam is named so post-MVP `D3D12PipelineLibrary` / `VkPipelineCache` slot in without churning the public surface; the implementation has one body. |
| `pipeline-state-cache.md` mermaid sequence — Editor → HotReloadManager → RenderThread → PsoCache                       | **Refused (no editor in MVP)**         | §8 below: invalidation source is `shader::ShaderCache::invalidate_by_source_hash` plus `core::HotReloadCompleted` event — no editor-specific path. The cache never observes a "manager" intermediate. |

Glibre-native requirements added beyond harmonius:

- **Composite key includes a render-state fingerprint.** Harmonius's
  `signature_hash` collapses the GPU root signature; on Metal there is
  no root signature — the equivalent is a **deterministic blake3 over
  the non-shader half of `MTL::RenderPipelineDescriptor` /
  `MTL::ComputePipelineDescriptor`** (vertex layout, blend state,
  depth-stencil state, MRT formats, sample count, raster state, tile
  size, mesh-shader threadgroup mesh size). §3.2 specifies the field
  list and the canonicalisation rule. SPEC §4.2 invariant 3.
- **Descriptor-layout-hash is a third key component, not a second.**
  SPEC §4.1.7 invariant 1 names the cache key as
  `(shader_hash, state_hash)` — but the descriptor-layout that
  `shader` emits is part of what `shader_hash` content-addresses
  (specs/shader/SPEC.md §4.6 invariant 1: the artifact hash composes
  source + permutation + flags + target into one BLAKE3 over which
  `DescriptorLayout` is embedded). The descriptor-layout hash
  therefore enters `PSOKey` **transitively through `shader_hash`**;
  the cache never recomputes it. The spike-issue brief's "shader-hash
  + descriptor-layout + render-state fingerprint" reads to a §3.2
  composite of two flat 64-bit fields, where the descriptor-layout
  contribution is folded into `shader_hash` upstream.
- **`std::expected<T, glibre::Error>` at every fallible boundary.**
  Per `reviews/decisions/error-model.md`. `render::Error` is the §10
  closed sum; `PSOCache::get` returns `Result<PSOHandle>`,
  `PSOCache::pin` returns `Result<PSOHandle>`,
  `PSOCache::warm` returns `Result<void>`.
- **No exceptions across the plugin ABI.** The cache itself is
  `render`-plugin-internal; cross-plugin consumers reach it only
  through middleman `data` types
  (`reviews/decisions/plugin-abi.md`). Internal Metal calls that
  surface NSError through metal-cpp wrappers are translated to
  `render::Error` at first ingress.
- **`MTL::BinaryArchive` is the MVP persistence path.** Harmonius
  named the API but did not pin it to MVP; this design declares the
  archive **MVP-in-scope** for the cold-start warmer and **strictly
  optional** for the lazy-build path (§7). The cold-start warmer
  loads device-compiled archives; the steady-state hot path never
  touches the disk.
- **Lookup-miss is success-in-disguise on the warm path, error on
  the unknown-shader path.** §4 splits the public surface so
  `get(PSOKey)` returns `Result<PSOHandle>` whose
  `unexpected{PsoCompileFailed}` arm is reserved for a build-time
  failure. A miss whose `shader_hash` is not resident returns
  `unexpected{ShaderModuleLoadFailed}` — there is no "soft miss"
  category in render (unlike `shader::ShaderCache`'s `nullptr`
  return). This matches SPEC §10's per-variant semantics.
- **Per-shader_hash invalidation, not per-PSOKey.** Harmonius's
  `invalidate(PsoKey)` requires the caller to enumerate every state
  fingerprint that paired with the changed shader; the production
  paths cannot do that. §8 adopts `invalidate_by_shader_hash(u64)`
  matching SPEC §4.1.7 invariant 4 and the §5 header stub.
- **Two-level caching: live `MTL::PipelineState` + `MTL::BinaryArchive`
  page cache.** Harmonius treated the disk blob as cold-only; on
  Metal the binary archive can be queried for a partial hit (the
  archive contains the pipeline-compiled bytecode but not the
  resident `MTL::PipelineState` object — instantiating the latter
  from the former is microseconds, vs. hundreds of milliseconds from
  shader bytecode). §3.5 specifies the three-tier (live → archive →
  shader-bytecode-rebuild) lookup explicitly.

## 3. Detailed Model

### 3.1 Aggregate composition

```text
PSOCache (entity, MetalDevice-owned, §4.1.7)
├── MetalDevice* const                        device_           // borrowed; non-owning. Lifetime > cache.
├── const CapabilitySet                       capabilities_     // snapshotted at construct
├── eastl::hash_map<PSOKey,                   live_             // hot lookup table
│                   eastl::shared_ptr<Entry>,
│                   PSOKeyHash>
├── eastl::intrusive_list<Entry>              lru_              // tail = most-recent
├── std::shared_mutex                         table_mutex_      // see §6
├── eastl::hash_map<PSOKey, BuildBarrier>     in_flight_        // one-shot per missing key
├── std::mutex                                build_mutex_      // guards in_flight_ map
├── BinaryArchiveSet                          archives_         // §3.5; MTL::BinaryArchive borrows
├── ShaderModuleResolver                      shaders_          // §3.5; borrows from shader::ShaderCache
├── std::atomic<std::size_t>                  live_bytes_       // residency accounting
├── const std::size_t                         budget_bytes_     // 64 MiB (SPEC §9.5 row)
└── DiagnosticOverlay*                        overlay_          // §4.1 SPEC; eviction reporting

PSOCache::Entry (intrusive list node, ref-counted)
├── const PSOKey                              key
├── PipelineKind                              kind              // Render | Compute | Tile (post-MVP)
├── eastl::variant<                           handle            // resident object (metal-cpp)
│       NS::SharedPtr<MTL::RenderPipelineState>,
│       NS::SharedPtr<MTL::ComputePipelineState>>
├── std::size_t                               size_bytes        // approx. driver-reported
├── std::atomic<std::uint32_t>                pin_count         // see §6
├── std::atomic<std::uint64_t>                last_used_tick    // §3.6 LRU
└── intrusive_list_hook                       lru_hook
```

Every field is private; the §4 surface exposes only typed accessors.
`Entry` is heap-allocated under `ContextTag::render` (the cache's
allocations all carry that tag, per `perf-budget.md` Allocator Rule
1; the live `MTL::PipelineState` GPU bytes count under render's 512
MiB cell per Allocator Rule 5). The `intrusive_list_hook` is part of
`Entry` so eviction touches one allocation per drop, not two.

### 3.2 Composite key composition (`PSOKey`)

The runtime lookup key is the §5 SPEC stub verbatim:

```cpp
struct PSOKey {
    std::uint64_t shader_hash = 0u;   // from shader::ShaderArtifact::hash, truncated
    std::uint64_t state_hash  = 0u;   // composed inside render
};
```

#### 3.2.1 `shader_hash`

`shader_hash` is the high 8 bytes of `shader::ShaderHash` (the 32-byte
BLAKE3 over `(source_hash, permutation_key, flags_hash, target)` per
`specs/shader/shader-cache-design.md` §3.2). The truncation collapses
a 256-bit identity into a 64-bit lookup token; the 64-bit space is
sized for at most 2^32 distinct shader artifacts in MVP scale (~2^32
permutations across the cooked archive), giving a birthday-paradox
collision floor at 2^16 entries — comfortably above the §9 working
set ceiling (~512 resident pipelines at MVP). Collision check is
performed at insert: when two distinct `shader::ShaderHash` values
truncate to the same `u64`, the insert is rejected with
`PsoCompileFailed` (with `detail="shader_hash_collision"` for the
log) and the cooker is required to re-permute one of the offending
artifacts. Probability under MVP scale is ~1 in 2^32; the failure is
a build-time bug, not a runtime category.

`shader_hash` arrives at `PSOCache::get` already-canonicalised by
`shader`. The cache never re-derives it.

The descriptor-layout contribution is **transitively** present in
`shader_hash`: `shader::DescriptorLayout` (spike #753) is embedded by
value inside `shader::ShaderArtifact` (`specs/shader/shader-cache-design.md`
§3.4), and the artifact hash composes over the embedded layout. Two
artifacts with identical bytecode but different descriptor layouts
have different `shader_hash`es; the cache key therefore distinguishes
them without a separate field. SPEC §4.1.7 invariant 1's
`(shader_hash, state_hash)` is the complete identity.

#### 3.2.2 `state_hash`

`state_hash` is render's deterministic 64-bit truncation of a
BLAKE3-hashed canonical encoding of every non-shader field of the
`MTL::RenderPipelineDescriptor` or `MTL::ComputePipelineDescriptor`
that the Metal pipeline compiler folds into the pipeline binary.
Composition (one byte stream, fixed field order, no padding):

```text
state_blake3 := BLAKE3(
    u8  pipeline_kind                       // Render=0, Compute=1, Tile=2 (post-MVP)
  ‖ if Render:
      u8  raster_sample_count               // 1, 2, 4, 8
      u8  alpha_to_coverage                 // bool packed
      u8  alpha_to_one                      // bool packed
      u8  rasterization_enabled             // bool packed
      u8  input_primitive_topology_class    // PrimitiveTopologyClass enum ordinal
      u8  raster_max_amplification_count_pow2  // mesh-shader path only; 0 on vertex path

      // Vertex layout — sorted by buffer index, then attribute index.
      u8  vertex_buffer_count
      for each buffer:
        u8  buffer_index
        u8  step_function                  // PerVertex / PerInstance / PerPatch / PerPatchControlPoint
        u32 step_rate
        u32 stride
      u8  vertex_attribute_count
      for each attribute:
        u8  attribute_index
        u8  buffer_index
        u8  format                         // VertexFormat enum ordinal
        u32 offset

      // Color attachments — fixed-length 8 entries; absent slots are zeroed.
      for k in 0..8:
        u8  pixel_format                   // PixelFormat enum ordinal; 0 = Invalid
        u8  write_mask                     // ColorWriteMask bits
        u8  blending_enabled               // bool packed
        u8  rgb_blend_op                   // BlendOperation
        u8  alpha_blend_op
        u8  source_rgb_blend_factor
        u8  destination_rgb_blend_factor
        u8  source_alpha_blend_factor
        u8  destination_alpha_blend_factor

      u8  depth_pixel_format                // PixelFormat ordinal
      u8  stencil_pixel_format
      // Depth-stencil descriptor (separate Metal object, hashed inline)
      u8  depth_compare_function
      u8  depth_write_enabled
      u8  front_stencil_compare_function
      u8  front_stencil_failure_op
      u8  front_depth_failure_op
      u8  front_depth_stencil_pass_op
      u8  front_read_mask
      u8  front_write_mask
      u8  back_stencil_compare_function
      u8  back_stencil_failure_op
      u8  back_depth_failure_op
      u8  back_depth_stencil_pass_op
      u8  back_read_mask
      u8  back_write_mask

      u8  support_indirect_command_buffers  // bool packed
      u8  max_vertex_amplification_count

  ‖ if Compute:
      u32 max_total_threads_per_threadgroup
      u8  thread_group_size_is_multiple_of_thread_execution_width  // bool packed
      u8  support_indirect_command_buffers                          // bool packed
)
```

`state_hash := truncate_u64(state_blake3)`.

Canonicalisation rules:

1. **Field order is fixed.** Adding a new descriptor field that
   participates in the pipeline binary requires this design to bump
   and `state_hash` to be recomputed; existing keys break by design.
   Render's PSO-archive directory is invalidated wholesale on this
   change per SPEC §7.2.2.
2. **Default fields are encoded.** A descriptor whose `alpha_blend_op`
   defaults to `Add` is encoded with `Add`'s ordinal, not omitted —
   two descriptors that differ only in *whether they explicitly set
   the default* hash equal.
3. **Vertex-buffer / vertex-attribute lists are sorted** by
   `(buffer_index)` and `(attribute_index)` ascending before hashing,
   so two descriptors with the same logical layout but different
   declaration order hash equal.
4. **Color-attachment array is fixed-length 8** (Metal's hardware
   maximum on Apple Silicon). Slots beyond the active count are
   encoded with `pixel_format=Invalid`, `write_mask=0`, all blend
   fields zero — the encoding is constant-shape regardless of MRT
   count.
5. **Mesh-shader path adds `max_amplification_count_pow2`** because
   Metal folds it into the pipeline binary; vertex path encodes 0.
6. **Tile-shader pipelines (post-MVP)** add their own kind ordinal
   and a separate field block; the §3.2 grammar is forward-compatible
   under this rule.

The `state_hash` function is implemented in
`render/src/pso_cache/state_hash.cpp` and is **the** seam through
which any future descriptor-field addition flows. SPEC §4.2 invariant
3: identical keys must map to byte-equal pipeline bytecode. Render
never invents a state_hash that does not include all PSO-relevant
Metal pipeline descriptor fields.

The 64-bit truncation's collision floor is 2^16 simultaneous resident
pipelines; the §9.5 64 MiB cap admits ~512 entries at MVP scale, well
under the floor. A collision is detected at insert (§3.5 step 6
below) and refused with `PsoCompileFailed` /
`detail="state_hash_collision"`; resolution path is to bump the
`state_hash` function (this design's amendment surface).

### 3.3 Entry kinds

Three pipeline kinds in MVP, two of which are wired:

| Kind     | Metal type                                | Build path                                                                                            | MVP status |
|----------|-------------------------------------------|--------------------------------------------------------------------------------------------------------|------------|
| Render   | `MTL::RenderPipelineState`                | `MTL::Device::newRenderPipelineStateWithDescriptor(...)`; mesh-shader and vertex paths use this kind. | MVP        |
| Compute  | `MTL::ComputePipelineState`               | `MTL::Device::newComputePipelineStateWithDescriptor(...)`; cluster cull + RT passes use this.         | MVP        |
| Tile     | `MTL::RenderPipelineState` (tile variant) | `MTL::Device::newRenderPipelineStateWithTileDescriptor(...)`. Reserved for post-MVP forward-tile.     | post-MVP   |

The entry's `kind` field is part of `Entry` but **not** part of
`PSOKey` — `state_hash` already disambiguates render-vs-compute
descriptors via the `pipeline_kind` byte at offset 0. The field is
stored on `Entry` so the eviction path (§3.6) can call the matching
metal-cpp release.

### 3.4 Resident-bytes accounting

Metal does not directly report `MTL::PipelineState` residency size
(metal-cpp returns no `imageDataSize()` on these objects). The cache
estimates per-entry size as:

```text
size_bytes = base_overhead
           + bytecode_size_estimate
           + descriptor_table_size

base_overhead             = 4 KiB    // Metal driver overhead per pipeline
bytecode_size_estimate    = MTL::Library::data().length() reported by shader::ShaderCache
                            for the resident `(shader_hash)`'s bytecode portion
                            actually referenced by the pipeline (vertex + fragment / compute)
descriptor_table_size     = encoder_argument_count * 64                // Metal argument-buffer entries
```

The estimate is conservative; the actual driver footprint is observed
by `MTL::ResidencySet` reports (§4.1.6) and reconciled against
`live_bytes_` once per second by a debug-only cross-check that
asserts `|estimated - reported| ≤ 10%`. A drift beyond this triggers
a `warn` log, not an error — driver allocations are outside the
cache's control. The §9.5 64 MiB row uses the estimate; the gate
asserts against the estimate (§9 below).

### 3.5 Lookup path (`PSOCache::get`)

The hot path is one `live_` table read; the cold path is a three-tier
build (live → binary archive → shader-bytecode-rebuild) executed
under a per-key one-shot guard (§6 concurrency).

```text
get(key) :
    1. (HOT) shared_lock(table_mutex_).
       entry := live_.find(key)
    2. if entry exists:
           entry->last_used_tick = tick_counter_.fetch_add(1)
           // re-link to LRU tail under the lock or via a relaxed atomic
           // promotion bit; see §3.6.
           pin := entry->pin_count.fetch_add(1)
           release shared_lock
           return PSOHandle{entry.get()}            // O(1) hot path

    3. release shared_lock
       // (COLD) miss path; serialise per key.
       {
         lock_guard build_lock(build_mutex_);
         existing := in_flight_.find(key)
         if existing exists:
             // another thread is building; we wait on its barrier.
             release build_mutex_
             existing.wait()
             // wake; retry from step 1 — winner has populated `live_`.
             goto 1
         barrier := in_flight_.emplace(key, BuildBarrier{}).first
         release build_mutex_
       }

    4. // We hold the one-shot for `key`. Resolve the inputs.
       library := shaders_.resolve(key.shader_hash)
       if library is empty:
           in_flight_.erase(key); barrier.notify_all()
           return std::unexpected{render::Error::ShaderModuleLoadFailed}

    5. // Construct the descriptor matching `state_hash`.
       desc := state_descriptor_table_.find(key.state_hash)
       if desc is empty:
           // The caller pinned a state_hash render itself does not
           // know how to descriptor-build. This is a contract breach
           // by the upstream pass-registry; surfaces as a fatal arm.
           in_flight_.erase(key); barrier.notify_all()
           return std::unexpected{render::Error::PsoCompileFailed}
                  // detail="unknown_state_hash"

    6. // Build the PSO. Try the binary archive first.
       desc.set_binary_archives(archives_.borrow_for(key))
       NS::Error* err = nullptr;
       if desc.kind == Render:
           pso := device_->newRenderPipelineState(
               desc, MTL::PipelineOptionFailOnBinaryArchiveMiss, &err)
           if pso == nullptr or err != nullptr:
               // Archive miss. Re-issue without the fail-on-miss flag.
               err = nullptr;
               pso := device_->newRenderPipelineState(desc, &err)
       else if desc.kind == Compute:
           pso := device_->newComputePipelineState(
               desc, MTL::PipelineOptionFailOnBinaryArchiveMiss, &err)
           if pso == nullptr or err != nullptr:
               err = nullptr;
               pso := device_->newComputePipelineState(desc, &err)

    7. if pso == nullptr or err != nullptr:
           record err->localizedDescription() into ErrorContext::detail
           in_flight_.erase(key); barrier.notify_all()
           return std::unexpected{render::Error::PsoCompileFailed}

    8. // Insert into live_ + LRU under exclusive lock.
       size := estimate_size(pso, desc)              // §3.4
       evict_if_needed(size)                         // §3.6
       entry := make_shared<Entry>(key, kind, pso, size)
       {
         exclusive_lock(table_mutex_);
         live_.emplace(key, entry)
         lru_.push_back(*entry)
         live_bytes_.fetch_add(size)
       }

    9. // If the build was a binary-archive miss, persist the new
       //    archive entry asynchronously so the next process inherits.
       if archive_miss:
           archives_.add_to_writable(pso, key)       // §7

   10. {
         lock_guard build_lock(build_mutex_);
         in_flight_.erase(key);
         barrier.notify_all();                       // wake step 3 waiters
       }

   11. return PSOHandle{entry.get()}
```

Step 6 attempts the archive-backed build first (`FailOnBinaryArchiveMiss`)
because the archive build path is microseconds (Metal validates the
descriptor + signature, then loads the precompiled bytecode); the
fallback build path is hundreds of milliseconds (Metal's pipeline
compiler runs end-to-end). The two-attempt sequence costs one extra
descriptor validation on a pure miss and pays for itself any time the
on-disk archive is warm.

Step 8 acquires the exclusive table lock — never held across the
build itself (§6 invariant). The `evict_if_needed` call may release
ownership of LRU-tail entries whose `pin_count == 0`; no PSO live in
the current frame is ever evicted because every per-pass binding
table holds a pin (§6.2).

### 3.6 Eviction (LRU)

Eviction policy: **LRU** with a hard byte budget (§9.5 row =
64 MiB). The choice of straight LRU (not 2Q as in
`shader::ShaderCache`) reflects render's workload: PSO lookups are
**not scan-heavy** — every frame's draw list is dominated by a
working set of ~100–200 distinct `PSOKey`s (per-pass shaders × 4–8
material classes), and the working set turns over only on view /
quality-tier changes. Under steady-state S1 the hot set fits the
budget; eviction triggers only on tier transitions or post-MVP
visibility-buffer growth.

```text
evict_if_needed(incoming_bytes) :
    while live_bytes_.load() + incoming_bytes > budget_bytes_:
        victim := lru_.front()                      // least-recent
        if victim.pin_count.load() != 0:
            // Pinned — current frame still using it. Walk forward.
            // In practice the working set is much smaller than the
            // budget; this loop terminates after at most ~10 hops.
            move victim to tail (re-link); restart loop.
            // If every entry is pinned, return without eviction —
            // the caller proceeds and live_bytes_ overshoots until
            // a pin drops. Overshoot logs a warn once per frame.
            break_on_full_loop_around → log_warn
        live_.erase(victim.key)
        lru_.pop_front()
        live_bytes_.fetch_sub(victim.size_bytes)
        overlay_->record_eviction(victim.key)        // DiagnosticOverlay
        // shared_ptr ref-count drops to zero when last pin releases.
        // The MTL::PipelineState's ref-count drops to zero when the
        // shared_ptr drops — metal-cpp releases the GPU object then.
```

Eviction promotion (LRU touch) on hot hits uses a **relaxed atomic
"touched" bit** plus a periodic re-link sweep — re-linking on every
hit would acquire the exclusive lock and serialise the draw path. On
each eviction call the sweep walks `lru_` once and pushes touched
entries to the tail; a hit between sweeps marks the bit relaxed. The
sweep is amortised against the eviction rate, which is rare in
steady-state.

A pass that requests an evicted PSO re-promotes it via the standard
`get` path (§3.5), which walks the cold tier (binary archive →
rebuild). SPEC §4.1.7 invariant 3.

### 3.7 Pinning

`PSOHandle` is a non-owning handle (`Entry*`) that carries an
implicit pin: every `get` increments the entry's `pin_count`, and
the matching `release(handle)` (or RAII `PinnedPSO` wrapper)
decrements it. The frame loop's contract is:

1. **Phase 6 (`graph/builder.cpp`)** issues `pin(PSOKey)` for every
   pass's pipeline at graph-build time. Pins acquired in phase 6 are
   tracked in the `ExecutionPlan` (§4.1.5) and held until the plan is
   discarded.
2. **Phase 7 (`Pass::execute`)** calls `get(PSOKey)` only when the
   plan was rebuilt this frame (rare); steady-state hits the plan's
   prebuilt handle.
3. **Phase 7 retire** drops every pin held by the discarded plan in a
   single batch on `RenderFrame` retire. The drop is a relaxed atomic
   sub on each pinned entry; no map traversal.

Eviction skips pinned entries. The cache never observes a use-after-
eviction because no pass can record against a pipeline whose
`PSOHandle` it does not hold, and every held handle is pinned.

### 3.8 `pin` and `unpin` (cross-aggregate seam)

The §5 SPEC stub names two further entry points the spike-issue
brief implies:

```cpp
class PSOCache {
public:
    [[nodiscard]] Result<PSOHandle> pin(PSOKey) noexcept;        // SPEC §8.3.2
    void                            unpin(PSOHandle) noexcept;
    // ... existing get/warm/invalidate/evict_lru
};
```

`pin(key)` is `get(key)` with two behavioural differences:

1. The pin survives `RenderFrame` retire — explicit `unpin` is
   required.
2. Used by `glibre_plugin_register` (SPEC §8.3.2 step 1) to walk the
   new plugin's static `(pass_class → PSOKey)` table and warm the
   per-pass binding-table prebuilds. The hot-reload path's bounded
   compile rate is enforced by the warmer's per-tick budget (the
   register call may issue at most `RenderSettings.warm_per_tick`
   misses; further misses stall to the next tick — this is where
   §8.3.2 step 1's "rate-limited by the warmer's per-tick budget"
   lives).

`unpin` decrements the entry's pin_count atomically; once it reaches
zero the entry becomes eviction-eligible.

### 3.9 What the cache does NOT do

Re-stating §1's refusals against the §3 layout to make the boundaries
mechanically inspectable in code review:

- **No descriptor-builder.** The `state_descriptor_table_` map at §3.5
  step 5 is populated by upstream pass-registry code — each `Pass`
  registers its descriptor under a `state_hash` at register-time.
  The cache stores the inverse mapping for cold-path lookup; it never
  authors a descriptor.
- **No shader resolver.** `shaders_.resolve(shader_hash)` walks
  `shader::ShaderCache::get(ShaderHash)` (#755) under a borrowed
  reference; the cache neither owns nor decodes the result.
- **No archive owner.** `archives_` borrows `MTL::BinaryArchive`
  handles from the on-disk warmer (§7); the warmer owns the file
  layout and the eviction-on-disk policy.
- **No allocator.** All allocations route through
  `glibre::PerContextAllocator` with `ContextTag::render`.
- **No `MTL::Device` constructor.** The cache borrows the device from
  `MetalDevice` (§4.1.6); device lifecycle is the device aggregate's.

## 4. Public Surface

The §5 surface in `specs/render/SPEC.md` is authoritative. This
section restates the subset owned by `PSOCache` with per-method
behaviour annotations and pre-/post-conditions, and it adds the four
SPEC §8.3.2 / §3.8-implied entry points that §5 currently names but
does not fully document.

### 4.1 Types (locked from SPEC §5)

```cpp
namespace glibre::render {

// SPEC §5 — already in the header stub.
struct PSOKey {
    std::uint64_t shader_hash = 0u;
    std::uint64_t state_hash  = 0u;

    [[nodiscard]] friend constexpr bool
        operator==(PSOKey, PSOKey) noexcept = default;
};

struct PSOKeyHash {
    [[nodiscard]] constexpr std::size_t operator()(PSOKey k) const noexcept {
        return std::rotl(k.shader_hash, 21) ^ k.state_hash;
    }
};

class PSOCache {
public:
    // Hot-path lookup; lazy-builds on miss. Holds a pin on the
    // returned handle until release_pin() (RAII via PinnedPSO).
    [[nodiscard]] Result<PSOHandle> get(PSOKey) noexcept;

    // Long-lived pin (survives RenderFrame retire). Used by
    // glibre_plugin_register (SPEC §8.3.2) and by the startup warmer.
    [[nodiscard]] Result<PSOHandle> pin(PSOKey) noexcept;
    void                            unpin(PSOHandle) noexcept;

    // Bulk warm. Builds every key in `keys`; stops at the first
    // ShaderModuleLoadFailed / PsoCompileFailed and returns. The
    // partial set of warmed keys remains live.
    [[nodiscard]] Result<void>
        warm(eastl::span<const PSOKey> keys) noexcept;

    // Drop every entry whose key.shader_hash matches.
    // Returns the count dropped. Called by `shader`'s reload hook.
    std::size_t
        invalidate_by_shader_hash(std::uint64_t shader_hash) noexcept;

    // Force LRU drain to `target_size` bytes. Honours pin_count;
    // overshoot logged but never refused.
    void
        evict_lru(std::size_t target_size) noexcept;

    // Diagnostic accessors — read-only, lock-free.
    [[nodiscard]] std::size_t live_bytes()  const noexcept;
    [[nodiscard]] std::size_t entry_count() const noexcept;

protected:
    PSOCache() noexcept = default;
    ~PSOCache() = default;
    PSOCache(const PSOCache&)            = delete;
    PSOCache& operator=(const PSOCache&) = delete;
};

}  // namespace glibre::render
```

`pin` / `unpin` / `invalidate_by_shader_hash` / `entry_count` /
`live_bytes` are §5 amendments this design declares; they land in the
follow-up plan that lifts spike findings into SPEC text. The spike
issue brief expressly names `MTLBinaryArchive` persistence (§7) and
`invalidate_by_shader_hash` (§8) as in-scope deliverables.

### 4.2 Surface rules

- **`std::expected<T, glibre::Error>` at every fallible boundary.**
  Per `reviews/decisions/error-model.md`. `render::Error` is the §10
  closed sum; the cache returns the four arms named in §10 below.
- **No exceptions cross the public surface.** The aggregate compiles
  with `-fno-exceptions` (per error-model.md). metal-cpp's
  `NS::Error*` outputs are translated to `render::Error` at the
  `newRenderPipelineState` / `newComputePipelineState` boundary.
- **No exceptions cross the plugin ABI.** `PSOCache` is
  render-plugin-internal; cross-plugin consumers reach it through
  `MetalDevice::pso_cache()` returning a reference owned by the
  device singleton, which is itself a render-plugin-internal type
  (`reviews/decisions/plugin-abi.md`). No middleman type leaks the
  cache across the ABI.
- **Borrow rules.** `PSOHandle` is non-owning; valid for as long as
  the holder's pin is live (RAII `PinnedPSO` wrapper or explicit
  `unpin`). A `PSOHandle` whose entry has been evicted is detected
  on dereference (the entry's `shared_ptr` count is checked); a
  stale handle returns `unexpected{PsoCompileFailed}` from any
  encoder bind path. In practice no holder sees an evicted handle
  because every holder holds a pin.
- **Lookup-miss is not silent.** Unlike `shader::ShaderCache::get`,
  `PSOCache::get` does not have a "soft miss" return shape — every
  outcome is either `PSOHandle` or `unexpected{render::Error}`. A
  draw path that asks for an unknown PSO is a contract bug, not a
  fallback path.

### 4.3 Per-method contracts

- **`get(key) noexcept -> Result<PSOHandle>`** — hot path; one
  shared-locked hash-map probe in steady state. Cold path runs the
  §3.5 build under a per-key one-shot guard. Pre-conditions: `key`'s
  `shader_hash` is registered with `shaders_` (i.e. the shader is
  resident); `key`'s `state_hash` was previously registered by an
  upstream pass against the descriptor table. Failure modes:
  `ShaderModuleLoadFailed`, `PsoCompileFailed`. Wall-time bound:
  §9.

- **`pin(key) noexcept -> Result<PSOHandle>`** — same as `get` but
  the returned handle's pin survives `RenderFrame` retire; explicit
  `unpin` required. Used by `glibre_plugin_register` per SPEC §8.3.2
  to populate per-pass binding tables that outlive a single frame.
  The new-plugin pass-registry walk yields one `pin` call per
  `(pass_class, PSOKey)`; misses run the same §3.5 build path. The
  warmer's per-tick budget (`RenderSettings.warm_per_tick`) caps the
  number of cold builds the register call issues; excess misses
  return `unexpected{PsoCompileFailed}` /
  `detail="warm_budget_exhausted"` and the loader logs but proceeds —
  the next tick re-attempts the pin (the new plugin's
  binding-table prebuild is incremental).

- **`unpin(handle) noexcept -> void`** — relaxed atomic decrement.
  No-op on zero (debug-build assert; release-build silent). Once a
  pin reaches zero, the entry is eviction-eligible at the next
  `evict_if_needed` call.

- **`warm(keys) noexcept -> Result<void>`** — bulk pre-build of a
  span of PSOKeys. Used at process start by the §7 warmer to pre-
  fault the MVP material set. Iterates `keys` in order, calling the
  internal build path (§3.5 steps 4–10). On the first failure,
  returns the failure immediately — partially-warmed entries remain
  live (idempotent against re-warming). The caller (warmer) is
  responsible for retry / fallback policy.

- **`invalidate_by_shader_hash(shader_hash) noexcept -> std::size_t`**
  — under exclusive lock, walks `live_` and drops every entry whose
  `key.shader_hash == shader_hash`. Returns the count dropped. Pinned
  entries are dropped from `live_` and `lru_` but their `Entry`
  storage survives via the held `shared_ptr` until the last pin
  releases — Metal will then release the underlying `MTL::PipelineState`.
  Concurrent pins on the dropped key are safe: the `shared_ptr`
  reference keeps the object alive; the next `get(key)` lookup
  builds a fresh pipeline because `live_.find(key)` misses. SPEC
  §4.1.7 invariant 4. Called by the SPEC §8.5 observer-bus subscriber
  that listens for `shader::ShaderCacheInvalidated` events from
  spike #755.

- **`evict_lru(target_size) noexcept -> void`** — public form of
  `evict_if_needed` (§3.6). Used by debug tooling and by the
  §10 `lower-tier` recovery path (when render's tier drops, the
  entries built for the higher tier become cold and are explicitly
  drained ahead of the next frame's build wave).

- **`live_bytes() const noexcept`** / **`entry_count() const
  noexcept`** — relaxed atomic loads; no lock acquired. Used by
  §9.6 budget gate and `DiagnosticOverlay`.

## 5. Hot / Cold Path Split

The lookup is sharply tiered. Hot is one shared-locked hashmap
probe, returning a borrowed handle in nanoseconds. Cold runs the
§3.5 build with a per-key one-shot, runs in microseconds (archive
hit) to milliseconds (archive miss + driver compile).

| Path                       | Operations                                              | Lock posture                              | Wall-time (M1, S1)   | When it runs                                            |
|----------------------------|---------------------------------------------------------|-------------------------------------------|----------------------|---------------------------------------------------------|
| **Hot** — live hit         | `live_.find` + `pin_count` atomic add + `last_used_tick`| `shared_lock(table_mutex_)`, no exclusive | < 100 ns p99         | every `get` / `pin` whose key is resident                |
| **Cold A** — archive hit   | descriptor build + Metal archive-backed pipeline build  | one-shot via `in_flight_`, no table lock  | 50–500 µs            | first `get` after process start; first `pin` post-reload |
| **Cold B** — archive miss  | descriptor build + driver pipeline-compiler build       | one-shot via `in_flight_`, no table lock  | 100–500 ms           | first `get` for a never-seen `(shader_hash, state_hash)` |
| **Insert** — (after build) | `live_.emplace` + `lru_.push_back` + `live_bytes_` add | `unique_lock(table_mutex_)`               | 1–5 µs               | exactly once per built entry                              |
| **Eviction**               | LRU walk + `live_.erase` + `lru_.pop_front`             | `unique_lock(table_mutex_)`               | 1–10 µs per victim   | when `live_bytes_ + incoming > budget_bytes_`            |

Steady-state every-frame draw paths execute exclusively the **Hot**
row. The first frame after process start runs **Cold A** for every
warmed key (warmer span; §7) and **Cold B** for any miss (logged).
Hot-reload of `shader` triggers an `invalidate_by_shader_hash` (one
exclusive lock), then **Cold A/B** for the affected keys on next use.
The plug-and-play §10 `lower-tier` recovery drains via `evict_lru`
and rebuilds the new tier's keys on next use.

The hot-path budget contributes to phase 7's 1.0 ms CPU ceiling; see
§9 for the gate. Cold paths are amortised against process start /
hot-reload events and are not gated against the per-frame budget —
they live in `MetalDevice::warm` / hot-reload register, both of which
are bounded by their own decision records.

## 6. Concurrency

The cache is touched from multiple threads in phase 7 (§6.3 SPEC):
the **graph builder thread** for `pin` (graph-build), the **per-pass
encoder workers** for `get` (binding table refresh, rare), and the
**driver thread** for retire-time `unpin` batches. Phase 6 is
single-threaded against the cache (`extract.cpp` does not touch it).
Hot-reload register runs on the **loader thread** under exclusive
phase 8 ownership.

### 6.1 Locking strategy

- **`std::shared_mutex table_mutex_`** guards the `live_` table and
  `lru_` list. Acquired in shared mode for hot lookups; in exclusive
  mode for inserts, evictions, and `invalidate_by_shader_hash`. The
  exclusive critical section is bounded to a few hashmap operations
  and an LRU re-link; it never spans a Metal pipeline build.
- **`std::mutex build_mutex_`** guards the `in_flight_` map. Held
  only across the per-key barrier insert / wait-handoff; never held
  across the build itself.
- **Per-key one-shot.** `in_flight_[key]` is a `BuildBarrier` (a
  `std::atomic_flag` plus a `std::condition_variable_any` to wake
  waiters). The first thread to miss `live_` for `key` inserts the
  barrier under `build_mutex_` and runs the build outside any
  cache-side lock. Concurrent threads finding an existing barrier
  drop `build_mutex_` and wait on the cv. On completion, the builder
  inserts the entry under `table_mutex_` (exclusive) and notifies all
  waiters — they retry their lookup and find the live entry.
- **`std::atomic` for pin_count, last_used_tick, live_bytes_,
  tick_counter_.** All relaxed except where ordering is required:
  `pin_count` increments use `acquire` on read-side, `release` on
  decrement.

### 6.2 Pin lifetime & eviction safety

The §3.7 pin-and-frame-loop contract guarantees that no entry the
current frame is using has `pin_count == 0`. The §3.6 evict_if_needed
walk skips pinned entries; if every entry is pinned (working set ≥
budget), eviction is a no-op and `live_bytes_` overshoots until a pin
drops. Overshoot is logged as a §10 `warn` once per frame and triggers
the §10 `ResourceResidencyExceeded` arm only when the cumulative
overshoot exceeds 25% of `budget_bytes_` (heuristic — overshoot below
that floor is normal during view transitions and self-corrects).

A pin held across frames by the new-plugin register path (§3.8) is
explicit; the loader holds them until phase 8 exit, then releases the
old plugin's pins as part of the §8 hot-reload protocol.

### 6.3 Build outside lock

The cold build (§3.5 step 6) runs without holding `table_mutex_` —
the per-key one-shot serialises threads that miss the same key, and
parallel misses on different keys build in parallel (Metal's pipeline
compiler is thread-safe and parallelisable). The 3-worker phase-7
encoder pool can therefore drive three parallel cold builds against
three distinct `state_hash`es without serialising on the cache's own
mutex.

This matters for cold-start: the warmer's `warm(keys)` call is itself
multi-threaded (it dispatches per-key builds across the encoder pool)
and the cache's lock posture supports it without thread-storms on the
table.

### 6.4 Hot-reload concurrency

`invalidate_by_shader_hash` runs under exclusive `table_mutex_` and
exclusive phase-8 ownership of the `MetalDevice` (per SPEC §8 / §6.3
"no system body runs in phase 8"). No other thread can be in `get` /
`pin` during the call; the lock is therefore ceremonial in this
context but kept on the function for callers outside phase 8 (the
shader-cache invalidation observer can fire during phase 1 in
post-MVP editor mode — see §8.6).

## 7. Persistence + ABI

PSO cache persistence is **MVP-in-scope** via `MTL::BinaryArchive`,
backed by the SPEC §7.1.2 `PSOCacheRecord` schema. The on-disk
artefact persists Metal's pipeline-compiler output (post-bytecode-
compilation), not shader source — the disjoint roles of `shader::
ShaderCache` (compiled shader bytecode) and `render::PSOCache`
(device-compiled pipeline state) are reflected in two separate
on-disk archives, each owned by its context.

### 7.1 Disk layout

Per SPEC §7.1.2, the archive root is:

```
<user-data>/render/pso-archive/<host-id>/<glibre-version>/<gpu-id>/
├── manifest.fory                  // PSOCacheManifest (§7.2)
├── archive_<shard_index>.metalar  // MTL::BinaryArchive blob (Apple's `.metalar` extension)
└── ...
```

**Sharding** is post-MVP — MVP ships **one** `archive_0.metalar` per
directory. The `<gpu-id>/<glibre-version>/<host-id>` triple keys
device + driver + engine-version isolation, exactly as harmonius
required and as SPEC §7.1.2 invariants 2–3 enforce.

### 7.2 Manifest schema

The `PSOCacheRecord` schema is fully specified in SPEC §7.1.2; this
design adds no fields. The manifest the warmer reads is a thin
wrapper over an array of `PSOCacheRecord` — one entry per
`(shader_hash, state_hash)` archived. Author and consumer:

- **Writer.** The cache appends to the manifest at clean shutdown
  (`MetalDevice::shutdown` calls `PSOCache::flush_to_disk()` which
  serialises every `live_` entry plus every entry built since the
  last successful warm). Crash-during-write leaves a `.tmp` orphan;
  the next startup deletes orphans before reading the manifest.
- **Reader.** The startup warmer (§3.8 `pin` callers, plus the
  init-time bulk warm) loads the archive into `MTL::BinaryArchive`
  handles via `MTL::Device::newBinaryArchive(descriptor)` then
  `archive.deserializeFromURL(...)`, validates each
  `archive_blob_blake3` self-check, gates on the four-tuple
  provenance fields (SPEC §7.1.2 invariant 2: `(gpu_id,
  metal_feature_set, os_build_hash, glibre_types_abi_hash)` must all
  match the host), and presents the resulting handles to the cache
  via `archives_.add_readable(archive_handle, key_set)`.

### 7.3 Self-authenticating payload

SPEC §7.1.2 invariant 1: `archive_blob_blake3` is the first 8 bytes
of `BLAKE3(archive_blob)`. Mismatch on read silently discards that
record (steady-state condition after a crash mid-write); the cache
proceeds with an empty entry for that key and rebuilds on demand.
Corruption is **not** an error — it is a lookup miss against the
warm path. Render's §10 closed sum has no `ArchiveCorrupt` arm; the
recovery is the plain rebuild path of §3.5 Cold B.

### 7.4 Provenance gating (whole-archive invalidation)

SPEC §7.1.2 invariants 2–3 + §7.2.2 require **whole-archive
invalidation**, never field migration. The warmer's startup logic:

```text
warm_from_disk() :
    1. open manifest.fory; if absent → empty cache, return.
    2. validate manifest's glibre_types_abi_hash == host's
       glibre_types_abi_hash.
       Mismatch → rmtree(<pso-archive-dir>); return.
    3. for each PSOCacheRecord r in manifest:
        3a. if r.gpu_id != host.gpu_id: skip
        3b. if r.metal_feature_set != host.metal_feature_set: skip
        3c. if r.os_build_hash != host.os_build_hash: skip
        3d. if blake3(r.archive_blob).first_8 != r.archive_blob_blake3: skip
        3e. add r.archive_blob to a freshly-deserialised
            MTL::BinaryArchive; record (r.shader_hash, r.state_hash)
            in archives_.readable_keys
    4. if any record was kept, the next get() / pin() in the
       hot-reload of the matching key hits Cold A (archive backed).
       Otherwise the next miss hits Cold B (full build).
```

Steps 3a–3d gate per-record so a partially-stale archive (one record
referencing a removed GPU under a `<gpu-id>/<glibre-version>` whose
other records are still valid) drops only the offending records. The
manifest-level gate (step 2) is the only whole-directory wipe, and
it fires on the major axis (`glibre_types_abi_hash` change) that
SPEC §7.2.2 names.

### 7.5 ABI seam (`IBackendArchive`)

The cache talks to the archive via a one-method interface so the
post-MVP D3D12 / Vulkan ports plug in without churning the cache:

```cpp
namespace glibre::render {

class IBackendArchive {
public:
    virtual ~IBackendArchive() = default;

    // Add this archive's contents as a candidate during pipeline build.
    // Returns the metal-cpp-side MTL::BinaryArchive* for descriptor-
    // attachment (Render: setBinaryArchives:; Compute: setBinaryArchives:).
    [[nodiscard]] virtual NS::SharedPtr<MTL::BinaryArchive>
        borrow_for(PSOKey) const noexcept = 0;

    // Add a freshly-built PSO to the writable set; the writer is
    // backed by a per-process scratch MTL::BinaryArchive that is
    // serialised at PSOCache::flush_to_disk() time.
    [[nodiscard]] virtual Result<void>
        add_to_writable(NS::SharedPtr<MTL::PipelineState>, PSOKey) noexcept = 0;
};

}  // namespace glibre::render
```

MVP implementation: `MetalBinaryArchiveSet` under
`pso_cache/binary_archive.cpp`. Post-MVP `D3D12PipelineLibrarySet`
slots into the same trait without any change to `PSOCache`.

### 7.6 What is NOT persisted

- **Live `MTL::PipelineState` objects.** They are device-resident GPU
  objects; their bytes have no on-disk shape. The archive persists
  the **bytecode** that constructs them, not the objects.
- **Pin counts, LRU order, last_used_tick.** Per-process state.
- **`state_descriptor_table_`.** Populated by upstream pass-registry
  code at register-time; reconstituted from the new plugin's static
  table on every process start / hot-reload.
- **`shaders_` resolver state.** Borrows from `shader::ShaderCache`;
  nothing to persist.

## 8. Hot-Reload Integration

Two hot-reload axes drive the cache: **shader edits** (a `.slang` file
changed; `shader::ShaderCache` produced a new `ShaderHash` for some
permutations) and **plugin reloads** (a render dylib was swapped at
phase 8). Each axis hits the cache via a different entry point and a
different lock posture.

### 8.1 Shader edit → `invalidate_by_shader_hash`

In editor / dev-loop builds, `shader::ShaderCache::invalidate_by_source_hash`
(spike #755) returns the set of `ShaderHash`es whose source changed.
The §8.5 observer bus delivers a `shader::ShaderCacheInvalidated`
event whose payload is that span. Render subscribes from
`MetalDevice::register_observers()`. The subscriber:

```text
on_shader_cache_invalidated(eastl::span<const shader::ShaderHash> changed) :
    for h in changed:
        u64 truncated = truncate_u64(h)
        n := pso_cache.invalidate_by_shader_hash(truncated)
        log_info("pso cache: dropped {} entries on shader_hash 0x{:016x}", n, truncated)
```

Per SPEC §4.1.7 invariant 4, dropped entries linger via held pins
until in-flight frames retire; old `MTL::PipelineState`s are released
when the last pin drops, which happens at the next `RenderFrame`
retire (≤ 1 frame later). The next `get` / `pin` for the same key
hits the cold path and rebuilds against the new bytecode resident at
`shader::ShaderCache::get(new_hash)`.

The §8.5 observer bus is the **only** interface between
`shader::ShaderCache` and `render::PSOCache`. Neither aggregate calls
the other directly; both share the `core::HotReloadEvent` middleman
type per `reviews/decisions/hot-reload-protocol.md` §"Observer
Notification".

### 8.2 Plugin reload → `pin` walk

SPEC §8.3.2 specifies the render-side `glibre_plugin_register` walk:
the new plugin's static `(pass_class, PSOKey)` table is iterated, and
each entry triggers a `PSOCache::pin(PSOKey)` call. The cache itself
**survives the plugin swap** (it is owned by `MetalDevice`, not by
the plugin image), so the live entries are preserved verbatim across
the swap. The new plugin's binding tables fix up against the same
resident pipelines.

The bounded-compile rate (§4.3 `pin` contract) caps misses per tick.
The post-reload register call is therefore O(passes) atomic-store
fix-ups + O(distinct PSOKeys) cache-pin calls, identical to the SPEC
§8.3.2 budget claim.

### 8.3 Disk archive interaction across reloads

The on-disk archive is loaded **once** at process start (SPEC §7.1.2
invariant 4); a hot-reload inherits the warmed cache. New PSOKeys
introduced by the new plugin (passes the old plugin did not
register) hit Cold B unless the archive happens to contain them — a
subsequent clean shutdown will append the newly-built entries so the
next process boots with them warm.

PSOKeys that no longer have a living `(pass_class)` consumer (passes
the new plugin dropped) are not actively pruned; LRU drains them at
the first eviction wave. They remain on disk until the next clean
shutdown's flush step writes a manifest whose entries reflect only
the post-reload live set; on-disk entries with no living manifest
counterpart are pruned by the next LRU-on-disk pass (the cooker tool
`glibre-render-pso compact`).

### 8.4 Refusal cases

SPEC §8.4 enumerates four render-specific hot-reload refusal causes;
the PSO cache participates in two:

1. **`(state_hash) collides but `shader_hash` differs in cooked
   archive vs. new plugin's expected hash`.** SPEC §8.4 row 3. The
   `pin` walk's first cache hit on the colliding key surfaces a
   stored `shader_hash` that does not match the requested key — the
   §3.5 step 6 path's `add_to_writable` invariant ("`(shader_hash,
   state_hash)` map injectively to bytecode") breaks. The cache
   returns `unexpected{render::Error::PsoCompileFailed}` /
   `detail="shader_hash_mismatch_at_pin"`; the loader rolls up to
   `core::Error::PluginInitFailed` per §8.4 row 3.
2. **Capability-set narrowing** (SPEC §8.4 row 2) is detected
   *before* any cache call (the pass-registry walk's capability
   predicate fires first). The cache is not on the path.

### 8.5 GPU-fault restart

SPEC §10.4 enumerates the `GpuFault` recovery: render publishes a
self-`HotReloadRequest` and the same dylib is re-loaded at phase 8.
The cache survives — the persistent-resource set named in SPEC §8.2
includes the PSO archive and the live PSO table. The `pin` walk in
the re-register call rebuilds the binding tables against the
unchanged pipelines. The fault is therefore expressed as a
state-resync, not a cache rebuild — fault-restart cost is the
register-walk cost, not a re-warm.

### 8.6 Editor-driven targeted reload (post-MVP)

`reviews/decisions/hot-reload-protocol.md` Open Question 5 asks
about an editor-driven shader-only reload path that does not touch
the plugin protocol. The `invalidate_by_shader_hash` entry point is
already the seam such a path would use; this design names it
explicitly so the post-MVP plan does not need to amend the cache
surface to land that feature. Lock posture: the editor calls into
the cache from phase 1 (input-equivalent), so the exclusive
`table_mutex_` holds against any phase-7 hot-path read — the
`shared_mutex` choice in §6 is what makes this safe.

## 9. Performance

Render's row from `reviews/decisions/perf-budget.md` is **0.10 ms CPU
sim + 1.40 ms CPU submit + 8.0 ms GPU + 512 MiB heap**. SPEC §9 owns
the per-phase breakdown. This design's contribution — the PSO cache
slice — is:

### 9.1 Hot-path budget (per draw)

| Operation                                                          | p50         | p99       | Budget                                                                                |
|--------------------------------------------------------------------|-------------|-----------|---------------------------------------------------------------------------------------|
| `PSOCache::get(key)` — live hit                                    | ~30 ns      | < 100 ns  | Folded into phase-7 "per-pass `execute()` recording" 0.50 ms ceiling (SPEC §9.3 row). |
| `PSOCache::pin(key)` — live hit (in `graph/builder.cpp` register)  | ~50 ns      | < 200 ns  | Folded into phase-7 "graph/builder.cpp register" 0.10 ms (SPEC §9.3 row).             |
| `unpin(handle)` — relaxed atomic decrement                         | ~5 ns       | < 20 ns   | Folded into phase-7 "RenderFrame retire + transient pool recycle" 0.05 ms.            |

The hot path is sub-100 ns per draw. With ~3k draws per frame at S1,
PSO-cache hot-path cost is ~300 µs at the upper bound — a fraction of
the 0.5 ms encoder-record budget. The gate (§9.3 below) asserts only
the aggregate phase-7 ceiling; per-draw asserts are debug-build only.

### 9.2 Cold-path budget (build)

| Operation                                                          | p50            | p99            | Budget                                                                              |
|--------------------------------------------------------------------|----------------|----------------|-------------------------------------------------------------------------------------|
| Cold A — archive-backed pipeline build                             | 100–200 µs     | 500 µs         | Off the per-frame critical path. Bounded by `RenderSettings.warm_per_tick` per tick.|
| Cold B — driver pipeline-compiler build                            | 100–300 ms     | 500 ms         | Init-time / hot-reload only. Never runs on a steady-state per-frame path.           |
| `warm(keys)` — bulk warm, MVP material set (~256 keys)             | 1.5 s (cold)   | 3 s            | Process-start; budget vs. `core::ApplicationStart` time (out of MVP gate scope).    |
| `invalidate_by_shader_hash` — drop N entries                       | 10 µs / entry  | 50 µs / entry  | Editor / dev-loop only; runs in phase 1, well outside any frame-time budget.        |

Cold paths never fire on a frame's critical path under steady-state
S1. The §10 `PsoCompileFailed` arm is the only category that can
trigger a Cold B mid-frame, and it surfaces as `lower-tier` recovery
(§10.3) — the failing draw is dropped, the previous frame is
re-presented, and the next frame's lower-tier descriptor either hits
the cache or rebuilds against a smaller pipeline.

### 9.3 Heap budget

SPEC §9.5 row "PSO cache" = **64 MiB**, decomposed:

| Sub-row                                  | Cap     | Notes                                                                                                                  |
|-------------------------------------------|---------|------------------------------------------------------------------------------------------------------------------------|
| Resident `MTL::PipelineState` objects     | 32 MiB  | ~512 entries × ~64 KiB driver footprint. M1 driver-side overhead per pipeline is the dominant term.                    |
| `MTL::BinaryArchive` page cache           | 16 MiB  | Page-resident portion of the on-disk archive. Metal pages in on demand; the cap is a soft ceiling enforced by the kernel.|
| Per-pass binding-table prebuilds          | 8 MiB   | `MTL::ArgumentEncoder` allocations + the `state_descriptor_table_` map.                                                |
| `live_` + `lru_` + `in_flight_` overhead  | 8 MiB   | Hashmap buckets + intrusive-list nodes + atomic-flag slots. Tagged `ContextTag::render`.                                |
| **Subtotal**                              | **64 MiB** | Sum equals SPEC §9.5 row.                                                                                              |

The four sub-rows are exhaustive and additive; any new memory
category at MVP must dock into one. The `live_bytes_` gauge measures
the first row only (resident pipeline state); the remaining three
rows are tracked by `glibre::PerContextAllocator` under
`ContextTag::render`. The §9.6 gate asserts each row's ceiling under
`GLIBRE_ALLOC_STRICT=1` per `perf-budget.md` Allocator Rule 2.

### 9.4 CI gate hooks

Per `perf-budget.md` §"CI Gate Spec":

1. **Per-context unit perf tests.** Catch2 `BENCHMARK` blocks under
   `tests/render/pso_cache/bench_get.cpp` and `bench_invalidate.cpp`
   assert hot-path `get` p99 < 100 ns and `invalidate_by_shader_hash`
   per-entry p99 < 50 µs against a fixture-built S1 cache (~256
   resident entries).
2. **End-to-end frame timing.** The S1 nightly e2e captures phase-7
   CPU wall-clock; the existing SPEC §9.3 gate asserts the 1.0 ms
   ceiling. PSO-cache contribution is folded in, not separately
   asserted, because the per-pass record cost subsumes it.
3. **Heap ceiling enforcement.** The diagnostic build's
   `ContextTag::render` gauge asserts the 64 MiB §9.5 row in addition
   to the 512 MiB cell. The cache's `live_bytes()` accessor feeds the
   gauge.
4. **Headroom regression alarm.** Standard `perf:headroom-low`
   tripwire applies. Any p50 within 0.5 ms of the phase-7 ceiling
   triggers the warning per `perf-budget.md` CI Gate item 5.

## 10. Failure Modes

The cache contributes failure paths to four arms of SPEC §10's
closed sum. Adding or removing an arm here is a `render::Error` ABI
bump per `reviews/decisions/error-model.md` §"Composition Rules"
item 5.

| §10 arm                          | Cache trigger site                                                                                                                                                                                                                            | Recovery (§10.2)        | Severity | Notes                                                                                                                                                                |
|----------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|--------------------------|----------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `ShaderModuleLoadFailed`         | §3.5 step 4 — `shaders_.resolve(shader_hash)` returns empty (the requested shader is not resident in `shader::ShaderCache`).                                                                                                                  | `abort-engine` (init) / `lower-tier` (post-init) | `error`  | A miss against the resident shader set is a contract bug at init (the warmer pre-faults the MVP material set). Post-init, lower-tier demotes the offending pass.    |
| `PsoCompileFailed`               | §3.5 step 7 — `newRenderPipelineState` / `newComputePipelineState` returns null / non-null `NS::Error`. Also: §3.2 `shader_hash` collision (rare; build-time bug); §3.2 `state_hash` collision (rare; design-time bug). Also: SPEC §8.4 row 3 hot-reload mismatch. | `lower-tier`            | `warn`   | The `NS::Error.localizedDescription()` is captured into `ErrorContext::detail` for log forensics. The lower tier's pass predicate selects a different PSO (e.g. drops TAA → FXAA → Off). |
| `ResourceResidencyExceeded`      | §6.2 — every entry pinned, working set ≥ budget, cumulative overshoot > 25% of `budget_bytes_`.                                                                                                                                                | `lower-tier`            | `warn`   | The per-tier descriptor table is smaller; lower tier thins the resident set under the cap.                                                                          |
| `BinaryArchiveLoadFailed` *(implied; folds under `PsoCompileFailed`)* | §7.4 — `MTL::BinaryArchive::deserializeFromURL` fails on a bytewise-valid archive (bad signature, version mismatch the manifest didn't catch).                                                                                                | `lower-tier` (degrade)   | `warn`   | The archive is dropped from `archives_`; subsequent builds fall through to Cold B. Not a separate §10 arm — folded under `PsoCompileFailed` per §10.1's eighteen-variant cap. The detail string `"binary_archive_load_failed"` distinguishes in logs. |

The cache does **not** contribute to `MetalDeviceUnavailable`,
`SwapchainAcquireFailed`, `BarrierViolation`, `GraphCycle`,
`MeshletCullDispatchFailed`, `BLASBuildFailed`, `TLASBuildFailed`,
`FrameSubmitFailed`, `PresentTimeout`, `GpuTimeout`, or `GpuFault` —
those are the responsibility of other aggregates. The cache's role
in `GpuFault` recovery is passive (§8.5): it survives the restart
and re-binds against the re-registered plugin.

### 10.1 Per-arm detail-string registry

The `ErrorContext::detail` field carries a stable string for each
distinct failure site so the structured log is grep-able:

| Detail string                          | Site                                                | Arm                          |
|----------------------------------------|-----------------------------------------------------|------------------------------|
| `"shader_unresolved"`                  | §3.5 step 4                                         | `ShaderModuleLoadFailed`     |
| `"unknown_state_hash"`                 | §3.5 step 5                                         | `PsoCompileFailed`           |
| `"metal_compile_failed"`               | §3.5 step 7 (Metal NS::Error returned)              | `PsoCompileFailed`           |
| `"shader_hash_collision"`              | §3.2.1 collision detection at insert                | `PsoCompileFailed`           |
| `"state_hash_collision"`               | §3.2.2 collision detection at insert                | `PsoCompileFailed`           |
| `"shader_hash_mismatch_at_pin"`        | §3.5 step 6 (§8.4 row 3 reload refusal)             | `PsoCompileFailed`           |
| `"warm_budget_exhausted"`              | §4.3 `pin` rate limit                               | `PsoCompileFailed`           |
| `"binary_archive_load_failed"`         | §7.4 step 3e degraded-archive case                  | `PsoCompileFailed`           |
| `"residency_overshoot"`                | §6.2 over-25% overshoot                             | `ResourceResidencyExceeded`  |

The registry is exhaustive against §3 / §4 / §6 / §7; adding a new
detail string requires an amendment here.

## 11. Test Plan

Tests live under `tests/render/pso_cache/` (Catch2). Two tiers:
**unit** (no Metal device; mocks the build path) and **integration**
(real Metal device under the `tests/render/fixture/MetalFixture`
harness, gated `#ifdef GLIBRE_HAS_METAL_DEVICE` for headless CI).

### 11.1 Unit (mocked build)

Fixture: `MockBuilder` implements `IBackendArchive` plus a fake
`device_->newRenderPipelineState` that returns a stub `MTL::PipelineState*`
or a controlled error.

| Test name                                     | Scenario                                                                                                                                                                                              | Asserts                                                                                                                            |
|-----------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------|
| `key_compose_render`                          | A canonical `MTL::RenderPipelineDescriptor` produces a deterministic `state_hash` independent of field-set order; permuting the vertex-attribute declaration order produces the same hash.            | §3.2.2 rule 3 (sort).                                                                                                              |
| `key_compose_compute`                         | Compute descriptor's `max_total_threads_per_threadgroup` participates in `state_hash`; two descriptors differing only in that field hash differently.                                                  | §3.2.2 compute branch.                                                                                                             |
| `key_compose_default_explicit`                | A descriptor with default `alpha_blend_op=Add` hashes equal to one that explicitly sets `Add`.                                                                                                         | §3.2.2 rule 2 (defaults encoded).                                                                                                  |
| `get_miss_then_hit`                           | First `get(key)` triggers `MockBuilder::compile`; second `get(key)` returns the cached entry with `pin_count==2`. Assert build was invoked exactly once.                                              | §3.5 + §3.7.                                                                                                                       |
| `get_concurrent_miss_one_shot`                | Two threads call `get(key)` simultaneously; assert `MockBuilder::compile` invoked exactly once and both threads receive the same `PSOHandle`.                                                          | §6.1 + §6.3.                                                                                                                        |
| `get_unknown_shader_hash`                     | `shaders_.resolve` returns empty for the requested key; assert `unexpected{ShaderModuleLoadFailed}` and that no `live_` entry was created.                                                             | §10 row `ShaderModuleLoadFailed`.                                                                                                  |
| `get_unknown_state_hash`                      | `state_descriptor_table_` has no entry for `key.state_hash`; assert `unexpected{PsoCompileFailed}`, detail `"unknown_state_hash"`.                                                                       | §10 detail registry.                                                                                                                |
| `get_metal_compile_error`                     | `MockBuilder::compile` returns an `NS::Error*` non-null; assert `unexpected{PsoCompileFailed}` with detail `"metal_compile_failed"` and that the localised description is captured into `detail`.       | §3.5 step 7 + logging contract.                                                                                                    |
| `eviction_lru_unpinned_drops`                 | Insert N entries up to `budget_bytes_`; insert one more; assert the least-recent unpinned entry was dropped and `live_bytes_` is below the budget.                                                     | §3.6.                                                                                                                                |
| `eviction_skips_pinned`                       | Pin every entry; insert one more; assert no eviction (overshoot allowed) and a `warn` log fires once.                                                                                                  | §6.2.                                                                                                                                |
| `eviction_residency_exceeded_at_25pct`        | Pin every entry; cumulative overshoot crosses 25% of `budget_bytes_`; assert `unexpected{ResourceResidencyExceeded}` from the next `get` that triggers `evict_if_needed`.                              | §10 row `ResourceResidencyExceeded`.                                                                                              |
| `invalidate_drops_all_matching_shader_hash`   | Insert 4 entries with two distinct `shader_hash` values; call `invalidate_by_shader_hash(h_a)`; assert exactly the matching entries dropped and the unmatched entries remain.                          | §4.3 `invalidate_by_shader_hash` + §8.1.                                                                                          |
| `invalidate_with_pinned_keeps_alive`          | Pin a `(h_a, s_x)` entry, call `invalidate_by_shader_hash(h_a)`, assert the entry is removed from `live_` but the pinned `PSOHandle` still dereferences a live `MTL::PipelineState`.                   | §4.3 contract on pinned drop.                                                                                                      |
| `pin_unpin_lifecycle`                         | `pin(key)`; `unpin(handle)`; assert `pin_count` returns to 0 and the entry becomes eviction-eligible.                                                                                                  | §3.7 + §3.8.                                                                                                                        |
| `warm_partial_failure`                        | `warm({k1, k2_bad, k3})` — `k2_bad` has unknown `shader_hash`; assert `unexpected` and that `k1` is live, `k2`/`k3` are not.                                                                            | §4.3 `warm` contract.                                                                                                              |
| `state_hash_collision_at_insert`              | Force two distinct descriptors to collide in `state_hash` (debug-only mock); assert second insert is rejected with `PsoCompileFailed` / detail `"state_hash_collision"`.                               | §3.2.2 + §10.1.                                                                                                                    |
| `shader_hash_collision_at_insert`             | Force two distinct `shader::ShaderHash`es to truncate to the same `u64`; assert second insert is rejected with detail `"shader_hash_collision"`.                                                       | §3.2.1 + §10.1.                                                                                                                    |
| `live_bytes_accounting`                       | Insert and drop entries; assert `live_bytes()` is monotonically consistent (insert raises, drop lowers, sum-zero on full drain).                                                                       | §3.4.                                                                                                                                |
| `concurrent_get_and_invalidate`               | One thread loops `get(k_a)`; second thread calls `invalidate_by_shader_hash(h_a)` mid-loop; assert no use-after-free, no torn `PSOHandle`, and that the post-invalidate `get` rebuilds via cold path.   | §6.1 + §6.4.                                                                                                                        |

### 11.2 Integration (real Metal device)

Fixture: `MetalFixture` opens `MTLCreateSystemDefaultDevice()` and a
small set of `.metallib` test artifacts cooked into the test binary
(via `tests/render/test_shaders/`). Skipped on hosts without a Metal
device (CI matrix includes one M1 / M2 macOS runner).

| Test name                                     | Scenario                                                                                                                                                                                                                                                  | Asserts                                                                                                                          |
|-----------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------|
| `metal_get_or_build_render`                   | Build a real `MTL::RenderPipelineState` for a trivial vertex+fragment pair; assert handle non-null and a subsequent draw against a tiny offscreen target produces the expected pixel.                                                                       | End-to-end §3.5 happy path.                                                                                                       |
| `metal_get_or_build_compute`                  | Build a real `MTL::ComputePipelineState`; dispatch a 1×1×1 compute that writes a sentinel; assert sentinel readback.                                                                                                                                       | End-to-end §3.5 happy path.                                                                                                       |
| `metal_archive_warm_then_hit`                 | Run a build, flush archive to a tmp path, drop the cache, re-open against that path, assert second `get` hits Cold A (timed `< 1 ms p99`).                                                                                                                  | §7 warmer + Cold A budget.                                                                                                        |
| `metal_archive_corrupt_record_silently_skipped` | Pre-write a manifest entry with an `archive_blob` whose blake3 mismatches the stored `archive_blob_blake3`; assert warmer skips that record (no error surfaced) and the cache rebuilds via Cold B.                                                          | SPEC §7.1.2 invariant 1 + §7.3.                                                                                                   |
| `metal_archive_provenance_mismatch_wipes_dir` | Pre-write a manifest with `glibre_types_abi_hash` differing from the host; assert the warmer `rmtree`s the directory and an empty cache results.                                                                                                              | SPEC §7.1.2 invariant 2 + §7.4 step 2.                                                                                            |
| `metal_compile_failed_lower_tier`             | Force a malformed descriptor (e.g. invalid combination of MRT formats); assert `PsoCompileFailed` and that the §10 `lower-tier` recovery is observable (the test fixture sets a callback that asserts the recovery action).                                  | §10 row `PsoCompileFailed` + §10.2 ladder.                                                                                        |
| `metal_warm_under_budget`                     | `warm` 256 keys; assert no entry is dropped during warm (all fit the 64 MiB budget) and total wall-time is dominated by archive-deserialise rather than driver-compile when the archive is warm.                                                              | §9.3 row "PSO cache" + §9.2 Cold A.                                                                                              |
| `metal_warm_over_budget_evicts`               | `warm` 1024 keys against a budget shrunk to 16 MiB; assert eviction runs during warm, oldest entries are dropped, and the final `live_bytes()` is at the budget.                                                                                              | §3.6 under stress.                                                                                                                |
| `metal_invalidate_round_trip`                 | Build a PSO; invalidate by `shader_hash`; rebuild via the same key against a freshly-resident shader (simulating a hot-reload); assert handle differs from the first build (new `MTL::PipelineState*`) and an offscreen draw produces the expected pixel.   | §8.1 round-trip.                                                                                                                  |
| `metal_concurrent_warm`                       | `warm({256 distinct keys})` from three encoder-pool worker threads simultaneously; assert each key is built exactly once and no race surfaces in the resulting `live_` table.                                                                                | §6.3 parallel build.                                                                                                                |

### 11.3 Hot-reload integration test

A scripted fixture under `tests/render/pso_cache/hot_reload.cpp`
exercises the full cross-cache invalidation path:

```text
1. Build a cooked archive containing a known (shader_hash_a, state_hash_x) entry.
2. Open MetalDevice; warm; assert hit on (shader_hash_a, state_hash_x).
3. Simulate a shader-source edit by injecting a shader::ShaderCacheInvalidated
   event onto the observer bus with payload {shader_hash_a}.
4. Assert `pso_cache.invalidate_by_shader_hash(truncate_u64(shader_hash_a))`
   was called and the entry was dropped from `live_`.
5. Pin (shader_hash_a_new, state_hash_x) — a new shader hash with the same
   state — and assert a fresh build runs (Cold B).
6. Assert the prior (shader_hash_a) entry is no longer reachable.
```

This test composes the §8 invariants with §3.5 / §3.6; failure surfaces
both as a unit-test failure and as a captured trace in the §6 SPEC
trace-replay framework.

### 11.4 Coverage

Every public method in §4.1 has at least one test row above; every
detail string in §10.1 has a triggering test row; every Cold A / Cold
B branch in §3.5 has at least one row. The §11 acceptance criterion
("each invariant maps to a Catch2 test by name") is satisfied by the
table; the follow-up plan that lifts spike findings into SPEC §11
will mirror the names listed here.

## 12. Open Questions

- [OPEN] **Mesh-shader threadgroup mesh size in `state_hash`.** Metal
  4 mesh-shader pipelines fold the threadgroup mesh size into the
  pipeline binary on some Apple Silicon families and not others.
  §3.2.2 encodes `max_amplification_count_pow2` but not the
  threadgroup mesh size; verify on M1/M2/M3 whether the absence
  yields a real crash on non-folding families before the §6.5
  mesh-shader path lands. Resolution gate: spike opened against the
  mesh-shader pass implementation plan.

- [OPEN] **`MTL::BinaryArchive` sharding for the archive.** MVP ships
  a single `archive_0.metalar` per directory. At scale, a single
  archive blob risks long page-in latency on cold start. Sharding
  by `shader_hash`'s top byte (256 shards) is a likely future
  optimisation; defer to the post-MVP profiling spike that measures
  cold-start wall-time on a populated archive.

- [OPEN] **Shipping vs editor archive partition.** Harmonius's
  `$CACHE/editor/` vs `$CACHE/game/` split exists to prevent the two
  processes from racing on the same blob. Glibre's MVP has only one
  consumer per host (one game process), but a future editor-plus-
  player workflow will need the same isolation. Open until the
  editor's render-host model is decided in the editor spec spike.

- [OPEN] **`live_bytes_` vs Metal's `MTLResidencySet` reported
  bytes.** §3.4's drift-tolerance check is debug-only; release
  builds trust the estimate. Whether the gate (§9.4) should switch
  to the `MTLResidencySet` measurement once it is wired, and what
  the precision tradeoff is, is decided when the residency-set seam
  lands.

- [OPEN] **Tile-shader pipelines (post-MVP).** §3.3 names the kind;
  §3.2 reserves the encoding slot. Whether tile pipelines belong in
  the same `PSOCache` or in a separate `TileShaderCache` aggregate
  is a §1 SRP question (they have a different "reason to change"
  cadence — tile-based deferred is a per-platform algorithm choice,
  not a per-material PSO swap). Defer to the post-MVP forward-tile
  spec spike.

- [OPEN] **Shader-hash truncation collision policy.** §3.2.1
  currently rejects the second-inserted artifact at insert time.
  This is fine for build-time errors but is not exercised in CI
  beyond the unit test. Whether we want a CI step that exhaustively
  probes the cooked-shader-set's truncated-hash uniqueness against
  the §3.2 64-bit space — or accept the 1-in-2^32 risk — is open.
  Resolution gate: the cook-time CI workflow plan.

- [OPEN] **`warm_per_tick` default.** §3.8 / §4.3 cap the per-tick
  miss rate during hot-reload register; the default value is set in
  `RenderSettings`. The number that balances "register-call wall
  time bounded by phase 8 budget (~0.4 ms one-shot, `perf-budget.md`)"
  against "binding-table prebuild lag" is empirical. Defer to the
  hot-reload integration test that lands with the loader.

- [OPEN] **Cooker tool `glibre-render-pso compact` ownership.** §8.3
  refers to a tool that prunes orphan archive entries. Whether this
  lives under `tools/glibre-render-pso/` (a new tool) or is folded
  into `tools/glibre-cook/` (existing) is a §6.1 module-layout
  question. Defer to the tooling spec spike.
