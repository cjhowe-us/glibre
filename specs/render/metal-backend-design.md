# Metal-Backend Detailed Design

> Detailed design for the `render` context's **metal-backend** aggregate
> (SPEC §4.1.6 `MetalDevice` / `MetalQueue` / `MetalCommandBuffer`,
> §5 `DeviceDesc` / `submit_frame` / `PresentFence`). Refines
> `specs/render/SPEC.md` §4.1.6 and the harmonius-mined ingredients of
> §3.2 collapse 1 (single Metal 4 backend) and §3.3 (window/surface /
> pacing routed to `platform`).
> All conclusions re-derived; harmonius prior art
> (`/Users/cjhowe/Code/harmonius/docs/requirements/rendering/gpu-abstraction-layer.md`,
> in particular R-2.1.1, R-2.1.2, R-2.1.3, R-2.1.4, R-2.1.7, R-2.1.8,
> R-2.1.9, R-2.1.13, R-2.1.14, R-2.1.15, R-2.1.18) cited as research
> input only.

Refs: spike #762 — `[SPIKE] design-render-metal-backend-detailed`.
Parent sub-epic #759 (`[SUB-EPIC] Detailed Designs — render`). Sibling
task-breakdown spike blocked-by this deliverable.

## 1. Purpose

The metal-backend aggregate is the single seam in the `render` context
permitted to talk to `metal-cpp`. Its one responsibility is **owning
the Metal 4 device + per-role command queue trio + the per-frame
command-buffer / drawable / present cycle** — i.e. acquiring an
`MTL::Device` once at process start, vending three `MTL::CommandQueue`
instances (graphics / compute / copy), attaching a `CAMetalLayer`
surface previously created by `platform`, allocating frame-scoped
`MTL::CommandBuffer` instances on each tick, acquiring the next
`CA::MetalDrawable`, committing buffers in queue dependency order, and
signalling the `PresentFence` that wakes `platform`'s phase 9. Every
`metal-cpp` symbol the render dylib touches lives behind this seam —
no other render module forward-declares `MTL::*` types and no engine
code outside the render dylib links `metal-cpp` at all
(PHILOSOPHY §"No Obj-C++ in engine code").

What the metal-backend explicitly refuses to own:

- **The render graph.** `RenderGraph`, `GraphBuilder`, and the topo /
  alias / barrier / queue-assignment compile pipeline (SPEC §4.1.2,
  §4.1.5) live in `render/src/graph/`. The backend executes whatever
  command stream the plan emits; it never inspects pass topology.
  Detailed design: spike #760.
- **Pass bodies.** `gbuffer`, `lighting`, `shadow_rt`, `present`, …
  (SPEC §6.2.2) are pure consumers of the `MetalCommandBuffer`
  reference handed to their `execute()` lambda. Their detailed design
  is spike #764.
- **The PSO cache.** `PSOCache` (SPEC §4.1.7) is owned by the
  `MetalDevice` accessor (`MetalDevice::pso_cache()`) but its
  contents — `MTL::RenderPipelineState` / `MTL::ComputePipelineState`
  residency, the `(shader_hash, state_hash)` keying, the on-disk
  `MTL::BinaryArchive` warmer — are detailed in spike #766. The
  backend exposes the cache pointer; it does not author entries.
- **Shader binding / argument buffers.** Slang→AIR/metallib
  compilation belongs to `shader` (SPEC §3.3, sibling
  `specs/shader/SPEC.md`); Metal 4 argument-buffer organisation is the
  shader-backend's responsibility (#757) and lands in
  `render/src/resources/argument_buffer.cpp`.
- **Surface + window lifecycle.** The `CAMetalLayer*` is created by
  `platform` and exposed through `Window::surface()` as a
  `metal_cpp::MTL::Layer*` (`specs/platform/SPEC.md` §4.1, §6.2,
  spike #715). The backend **attaches** to that pointer; it never
  creates it, never resizes it without a platform event, never
  destroys it. Frame pacing (`CAMetalDisplayLink`) is `platform`'s
  phase 9.
- **Resource heaps + transient pool.** `MTL::Heap` allocation policy,
  alias planning, and the 256 MiB transient pool (SPEC §4.1.4, §9.5)
  are owned by `render/src/resources/`; the backend hands out the
  device pointer the heap allocator wraps but does not size or shape
  the pool.
- **Cross-backend abstraction.** No `IDevice` / `ICommandBuffer`
  vtable — Metal 4 is the only shipping backend (SPEC §3.2 collapse 1,
  §6.6). A future Vulkan or D3D12 plugin would fork this design into
  its own `vk-backend` / `d3d-backend` aggregate.

If the way `MTL::Device` is acquired, the way the three queues are
constructed, the way a `CAMetalLayer` is attached, the way
`nextDrawable` is requested, the way a command buffer's commit + fence
emission works, or the way `metal-cpp` C++ ownership maps to Metal's
NS retain/release changes, this design changes. Anything else is out
of scope.

## 2. Requirements Coverage

Mapping of harmonius requirements (`gpu-abstraction-layer.md`) to MVP
refusal-or-coverage. Every entry is independently re-derived;
harmonius is research input only (PHILOSOPHY §"How harmonius is used").

| Harmonius source                                                                                                              | Glibre disposition (MVP)                                | Coverage site                                                                                                                                                              |
|-------------------------------------------------------------------------------------------------------------------------------|---------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| R-2.1.1 — top-level trait with associated types for device / cmd-buf / PSO / handles, generics for static dispatch             | **Refused (over-abstraction)**                          | §3.1 below: single concrete `MetalDevice` / `MetalQueue` / `MetalCommandBuffer`. SPEC §3.2 collapse 1 + §6.6 reject the trait until a second concrete backend exists.       |
| R-2.1.2 — command-buffer abstraction supporting graphics / compute / copy with type-safe binding                              | **Covered (queue-typed CB)**                            | §3.3: `MetalCommandBuffer::queue_role()` + queue-purity invariant (SPEC §4.1.6 invariant 4); type-safe binding is the shader-backend's responsibility (#757), out of scope. |
| R-2.1.3 — unified PSO validated at creation, zero validation overhead at encode                                                | **Refused (split aggregate)**                           | PSO lifecycle is `PSOCache` (#766); the backend exposes it but does not author it. Encode-time zero-cost is preserved by §3.4 record path.                                |
| R-2.1.4 — Metal backend in Rust via `objc2-metal`, no C++/Swift in FFI                                                         | **Refused (re-derived)**                                | Glibre is C++23/26. The C++ analogue is `metal-cpp` (Apple-published, header-only). NO Obj-C++ allowed in engine code (PHILOSOPHY §"Don'ts"). §3.2 isolates the dependency. |
| R-2.1.5 / R-2.1.6 — D3D12 + Vulkan backends                                                                                    | **Refused (out of scope MVP)**                          | SPEC §3.2 collapse 1 + §6.6: Metal 4 is the only shipping backend; second-backend work is a future plugin spike, not a retrofit.                                            |
| R-2.1.7 — GPU heap sub-allocator < 64 OS allocations / frame, page-aligned on Metal                                             | **Covered (delegated)**                                 | §3.2 below: backend owns the `MTL::Device` reference fed to `render/src/resources/transient_pool.cpp` (spike outside this aggregate). Page-alignment is `MTL::Heap`-native.   |
| R-2.1.8 — CPU-side shadow state per CB filtering redundant transitions                                                         | **Covered (encoder cursor)**                            | §3.4: `MetalCommandBuffer` carries an encoder cursor + binding shadow that the recording side updates. SPEC §4.1.6 invariant 3 enforces single-encoder discipline.        |
| R-2.1.9 — automatic barrier batch / merge / split                                                                              | **Refused (graph layer owns)**                          | Barrier emission is `RenderGraph`'s compile step (SPEC §4.1.2, §4.1.5; spike #760). The backend executes plan-emitted barriers verbatim through the encoder.               |
| R-2.1.10 — GPU work graphs (D3D12 native + Metal/Vulkan emulation)                                                             | **Refused (post-MVP)**                                  | No work-graph use in MVP. Hybrid-RT compute path uses standard ray query (SPEC §6.4), not work graphs.                                                                     |
| R-2.1.11 — feature-emulation layer at device-creation, no runtime branching                                                    | **Covered (capability gating)**                         | §3.1 captures `Capability` bits at `MetalDevice::create`; `RenderGraph` predicates branch at compile time (SPEC §4.1.2 invariant 3), never inside passes.                  |
| R-2.1.12 — per-pass GPU timestamps with one-frame deferred readback                                                            | **Covered (delegated)**                                 | §3.4 shows the `MTLCounterSampleBuffer` ring attached to each command buffer; the readback fixture is SPEC §9.6.1 and stays in `render/src/graph/diagnostic.cpp`.           |
| R-2.1.13 — generational `Handle<T>` for all GPU resources, no raw pointers                                                     | **Covered (engine-wide)**                               | `Handle<Tag>` is the SPEC §5 mechanism; the backend does **not** vend raw `MTL::*` pointers across its public surface. §4 below states the invariant.                       |
| R-2.1.14 — abstraction adds < 5% CPU overhead vs. raw                                                                          | **Covered (perf budget)**                               | §9: backend's CPU contribution to phase 7 is bounded by SPEC §9.3 (0.05 ms submit + 0.05 ms retire = 0.10 ms within the 1.0 ms phase ceiling).                              |
| R-2.1.15 — sub-allocator O(1) amortised; state tracker ≤ 64 KB / CB                                                            | **Covered (CB cursor cap)**                             | §9.2 below: each `MetalCommandBuffer` carries ≤ 4 KiB of cursor + shadow state. Sub-allocator perf is the resources sub-system's contract.                                 |
| R-2.1.16 — descriptor frequency groups (per-frame / per-pass / per-material / per-draw)                                        | **Refused (shader-backend)**                            | Argument-buffer organisation lives in `render/src/resources/argument_buffer.cpp`; the metal-backend exposes only the `MTL::CommandBuffer*` argument-binding API.            |
| R-2.1.17 — offline shader compile via dxc / metal-shaderconverter                                                              | **Refused (out of scope here)**                         | Owned by `shader` (sibling SPEC); backend reads metallib blobs at PSO compile time only, never at frame time.                                                              |
| R-2.1.18 — structured errors for device / alloc / PSO / graph                                                                  | **Covered**                                             | §10: every backend entry returns `std::expected<T, glibre::Error>` over `render::Error` per `reviews/decisions/error-model.md`. PSO errors route via `PSOCache` (#766).      |

Glibre-native requirements added beyond harmonius:

- **`metal-cpp` only — never Obj-C++ in engine code.** PHILOSOPHY
  §"Don'ts" hard rule. Re-derived: Obj-C++ in plugin TUs would force
  every consumer of the render header to compile against Apple
  toolchain extensions, which collides with `clang++ -std=c++23
  -fsyntax-only -Wall -Wextra -Wpedantic` (SPEC §5 conformance) and
  with the `-fno-exceptions` compile contract
  (`reviews/decisions/error-model.md`). The `metal-cpp` headers are
  pure C++23 wrappers over Objective-C runtime calls and stay inside
  the render dylib's `metal/` directory.
- **Headless support at device creation.** `DeviceDesc::headless = true`
  is a first-class entry point: tests, the cooker, and CI fixtures
  build a `MetalDevice` without ever calling `attach_surface`. SPEC §5
  doesn't enumerate this bool today; §3.1 below promotes it to a
  first-class field that lands as part of the next ABI bump.
- **One device per process; no second-instance escape hatch.** SPEC
  §4.1.6 invariant 1. Re-derived: `MTLCreateSystemDefaultDevice` is
  cheap on retry but `MTL::Heap` residency sets are device-keyed; two
  devices would duplicate the 512 MiB heap budget. The backend's
  `create()` returns `unique_ptr` and the engine registry holds it
  for process lifetime.
- **Surface attach is decoupled from device creation.** A
  `MetalDevice` may exist without a surface (headless / cook /
  multi-window-pre-attach window). `attach_surface(MTL::Layer*)` is a
  separate cold-path call that binds the layer to the device, sets
  pixel format / drawable count / colorspace, and registers the
  resize callback `platform` invokes.
- **Drawable acquire is per-frame and may fail benignly.**
  `nextDrawable` returning `nil` (or its `metal-cpp` analogue
  `CA::MetalLayer::nextDrawable() == nullptr`) maps to
  `Error::SwapchainAcquireFailed` and triggers
  `abort-frame` per SPEC §10.3 — the previous frame is re-presented.
- **Submit and present-fence emission are explicit and ordered.** A
  single `submit_frame` call per `RenderFrame` performs (a) commit on
  every queue used by the plan, (b) signal of the `PresentFence`, in
  that order, with no implicit GPU wait. SPEC §5
  `submit_frame(MetalDevice&, const RenderFrame&) → Result<PresentFence>`
  is the bound surface.

## 3. Detailed Model

### 3.1 Aggregate composition

```text
MetalDevice (engine-singleton, value-shaped owner)
├── MTL::Device*              device_                  // owned, NS_RETAIN'd
├── CapabilitySet             capabilities_            // probed at create
├── eastl::array<MetalQueue,3> queues_                  // graphics / compute / copy
├── PSOCache                  pso_cache_               // §4.1.7, owned but author #766
├── TransientPool             transient_pool_          // §4.1.4, owned but author elsewhere
├── ResidencyAttachment       residency_set_           // MTL4ResidencySet wrapper
├── SurfaceAttachment         surface_                 // optional; null until attach
└── DeviceDesc                desc_                    // immutable post-create

MetalQueue (per-role, child of MetalDevice)
├── MTL::CommandQueue*        queue_                   // owned, role-tagged
├── Queue                     role_                    // Graphics | Compute | Copy
└── std::atomic<uint64_t>     submit_counter_          // monotonic per-queue submit id

MetalCommandBuffer (frame-scoped, child of MetalQueue)
├── MTL::CommandBuffer*       buffer_                  // owned, queue-tagged
├── MetalQueue*               owning_queue_            // back-pointer
├── EncoderCursor             cursor_                  // active encoder + binding shadow (≤ 4 KiB)
├── DebugMarkerStack          markers_                 // push/pop for diagnostics
└── std::uint64_t             expected_submit_id_      // matches owning_queue_->submit_counter_

SurfaceAttachment (optional, child of MetalDevice)
├── CA::MetalLayer*           layer_                   // borrowed from platform; NOT owned
├── MTL::PixelFormat          color_format_            // chosen at attach
├── std::uint8_t              max_drawables_           // 2 or 3 (frames-in-flight)
├── std::atomic<std::uint64_t> drawable_size_           // packed (width_f32 | height_f32<<32); updated on platform resize; read lock-free in phase 7
└── ResizeCallback            on_resize_               // platform invokes from phase 1

PresentFence (POD, returned by submit_frame)
└── std::uint64_t             value                    // monotonically increasing per-queue
```

The aggregate is a strict tree: `MetalDevice` owns the queues and the
optional surface; queues are non-movable, non-copyable, and outlive
every command buffer they vend; a `MetalCommandBuffer` is a
single-frame value and is committed-or-discarded inside one phase 7
tick (SPEC §6.2.2 step 3).

`MetalDevice` and `MetalQueue` explicitly delete all copy and move
constructors and assignment operators:

```cpp
MetalDevice(const MetalDevice&)                      = delete;
MetalDevice& operator=(const MetalDevice&)           = delete;
MetalDevice(MetalDevice&&)                           = delete;
MetalDevice& operator=(MetalDevice&&)                = delete;

MetalQueue(const MetalQueue&)                        = delete;
MetalQueue& operator=(const MetalQueue&)             = delete;
MetalQueue(MetalQueue&&)                             = delete;
MetalQueue& operator=(MetalQueue&&)                  = delete;

MetalCommandBuffer(const MetalCommandBuffer&)        = delete;
MetalCommandBuffer& operator=(const MetalCommandBuffer&) = delete;
MetalCommandBuffer(MetalCommandBuffer&&)             = delete;
MetalCommandBuffer& operator=(MetalCommandBuffer&&)  = delete;
```

`MetalCommandBuffer` owns a `MTL::CommandBuffer*` that must not alias; the
explicit `= delete` declarations above enforce the same non-copyable,
non-movable contract as `MetalDevice` and `MetalQueue`. These deletes are the
enforcement mechanism for the "engine-singleton" and "thread-affine"
contracts stated in §6.1 and §6.3; any `std::move` or copy attempt on
these types is a compile error with a clear diagnostic.

### 3.2 `MetalDevice::create` — cold-path device acquisition

```text
Result<unique_ptr<MetalDevice>> create(const DeviceDesc&) noexcept:

  1. Pick MTL::Device:
     - if desc.prefer_low_power and a low-power device exists in
       MTL::CopyAllDevices(), pick that one
     - else MTL::CreateSystemDefaultDevice()
     - on null → return unexpected{render::Error::DeviceUnsupported}
       (no Metal device on this host)
  2. Probe Capability bits against the MTL::Device:
     - MeshShaders        ← supportsFamily(MTLGPUFamilyApple7) AND
                            supportsFamily(MTLGPUFamilyMetal3)
     - RayQuery           ← supportsRaytracing
     - HardwareRayTrace   ← supportsRaytracing AND
                            supportsFunctionPointers
     - BindlessResources  ← argumentBuffersSupport >= Tier2
     - ResidencySets      ← available on MacOS 26+ baseline (always
                            true for MVP host; left as a probe for
                            forward compat)
     - TimestampQueries   ← supportsCounterSampling(.atStageBoundary)
     - HdrPresent         ← system maximum EDR head-room > 1.0
     - DolbyVision        ← os/system probe (out-of-MVP gate)
     - VariableRate       ← supportsRasterizationRateMap
     - MetalFx            ← MetalFX framework available + tier matches
  3. Construct MetalQueue × 3:
     - graphics_queue ← device->newCommandQueue() with maxCommandBufferCount = 64
     - compute_queue  ← idem
     - copy_queue     ← idem with maxCommandBufferCount = 16
     - tag each with its Queue role
     - on any null → release predecessors and return
       unexpected{render::Error::DeviceUnsupported}
  4. Construct PSOCache and TransientPool (handles only; their
     contents are populated by their owning aggregates' init).
  5. Construct ResidencyAttachment (MTL4ResidencySet wrapper);
     attach the heaps the resources sub-system has registered so
     far (zero on cold start; populated by post-init register).
  6. Surface remains absent (surface_ = nullptr). Headless mode
     (desc.headless = true) skips even the surface storage.
  7. Wrap (device, queues, pso_cache, transient_pool, residency,
     desc) in a unique_ptr<MetalDevice> via a private factory; the
     ctor is protected to enforce engine-singleton creation through
     the registry.
```

`MetalDevice::create` is **idempotent across hot-reload** — the engine
calls it exactly once per process, the resulting `unique_ptr` lives in
the `core` registry, and reload traffic in §8 reuses the existing
device (no second create). Failure paths return early, release any
NS-retained pointers, and never partially construct.

### 3.3 `MetalDevice::attach_surface` — bind the platform `CAMetalLayer`

```text
Result<void> attach_surface(MTL::Layer* layer,
                            SurfaceAttachDesc desc) noexcept:

  1. Reject if surface_ is non-null:
       return unexpected{render::Error::ResourceImportRefused}
       (one surface per device in MVP; multi-surface is post-MVP).
  2. Reject if layer is null:
       return unexpected{render::Error::ResourceImportRefused}
       (null layer is a caller-precondition violation; no frame is
       running yet, so re-present is meaningless — recovery is
       abort-engine, matching the double-attach case in step 1).
  3. Configure the layer (caller-vended; mutated under platform's
     contract that it survives until detach):
       - layer->setDevice(device_)
       - layer->setPixelFormat(desc.color_format)         // bgra8Unorm_sRGB or rgba16Float for HDR
       - layer->setColorspace(desc.colorspace)            // sRGB / Display P3 / Rec.2100 PQ
       - layer->setMaximumDrawableCount(desc.max_drawables) // 2 (low-latency) or 3 (smooth)
       - layer->setDisplaySyncEnabled(true)               // VSync; pacing owned by platform phase 9
       - layer->setFramebufferOnly(true)                  // present-only target; sampling forbidden
  4. Initialise drawable_size_ from layer->drawableSize().
  5. Register on_resize_ callback. platform invokes this from phase 1
     (input pump) when SDL3 reports a window resize (specs/platform/SPEC.md
     §4.1 surface contract); the callback updates drawable_size_ and
     publishes a render::Error::SwapchainOutOfDate via the
     DiagnosticOverlay (no immediate refusal — the next phase-6 plan
     compile re-evaluates view extents).
  6. surface_ ← non-null SurfaceAttachment.
  7. Return success.
```

`attach_surface` is the **sole writer** of `surface_`. Detach
(`detach_surface`) is a symmetric cold-path call invoked by the
process-shutdown sequence and is the **only** writer of `nullptr` to
`surface_`. Mid-frame surface mutation is forbidden; the platform
resize callback only updates the size field, and any pixel-format /
colorspace change goes through detach + attach at phase 8.

### 3.4 Per-frame submit cycle — `submit_frame` hot path

This is the only public free function in the metal-backend's surface;
SPEC §5 lists it adjacent to the `MetalDevice` class:

```text
Result<PresentFence> submit_frame(MetalDevice& device,
                                  const RenderFrame& frame) noexcept:

  1. Driver thread enters phase 7 (frame-phases.md row 7). Pre-conditions:
     - frame is immutable (SPEC §4.1.1 invariant 1)
     - device.surface_ may be null (headless) — gate present accordingly
     - the ExecutionPlan corresponding to this frame has been compiled
       by graph/compile.cpp on the graph builder thread (SPEC §6.2.2
       step 2). The plan reference is the only piece of state the
       backend needs from the graph layer.

  2. Acquire one MTL::CommandBuffer per used queue:
     - for q in {Graphics, Compute, Copy} that the plan touches:
         cb_q ← device.queue(q).acquire_command_buffer()
         on failure → cleanup acquired siblings; return
                       unexpected{render::Error::QueueSubmitFailed}
     - tag each CB with its queue role; allocate one debug-marker
       stack per CB for §6 concurrency ordering.

  3. Record:
     - the graph's ExecutionPlan::record_into(cb_q) is called once
       per used queue, on per-queue worker threads (SPEC §6.3
       three-thread topology). Each call walks the plan's queue
       partition and invokes Pass::execute(cb_q, bindings) for every
       pass on that queue, in plan order.
     - per-pass execute() lambdas open / close encoders against the
       cursor; the cursor enforces the single-encoder-at-a-time
       invariant (SPEC §4.1.6 invariant 3) by ending the previous
       encoder and emitting the plan-required barrier when the
       lambda asks for a different encoder type.
     - Cross-queue fences emitted by the plan (graph/compile.cpp,
       SPEC §6.2.2 step 2) are encoded as MTL4 fences via
       metal/fence.cpp — backend-internal, not part of the public
       surface.

  4. Acquire the next drawable (only if the present pass is in the plan
     AND device.surface_ is non-null):
     - drawable ← device.surface_->layer->nextDrawable()
     - if drawable is null:
         return unexpected{render::Error::SwapchainAcquireFailed}
         (the previous frame is re-presented; SPEC §10.3 abort-frame).
     - Hand drawable to passes/present.cpp via the bindings table;
       it sets the present pass's color attachment to drawable.texture.

  5. Schedule the present:
     - cb_graphics->presentDrawable(drawable)  -- Metal's CB-side
       present scheduler. The drawable is presented when the GPU
       finishes executing this CB. Pacing is platform's phase 9.

  6. Commit in queue dependency order (compute first if compute writes
     graphics-queue inputs without a graphics-side wait; the plan's
     queue assignment fixes the order):
     - device.queue(Compute).submit(cb_compute)
     - device.queue(Graphics).submit(cb_graphics)
     - device.queue(Copy).submit(cb_copy)    -- copy is independent in MVP
     - on any submit failure → return
       unexpected{render::Error::QueueSubmitFailed}
     MetalQueue::submit(MetalCommandBuffer&) asserts the CB's queue-role
     tag matches the queue before calling cb->commit() internally. This
     keeps the "Queue purity" invariant (SPEC §4.1.6 invariant 4) inside
     MetalQueue's SRP boundary rather than in submit_frame.

  7. Emit PresentFence:
     - fence_value ← device.queue(Graphics).next_submit_id()
     - return PresentFence{ fence_value }
     MetalQueue::next_submit_id() returns the counter value incremented
     by the preceding submit() call (memory_order_acq_rel per §6.2).
     submit_frame never touches submit_counter_ directly.

  8. Driver thread exits phase 7. The CBs are owned by Metal until
     completion; their lifetimes are managed by the metal-cpp
     auto-release pool inside the per-frame arena (the arena drains
     at phase 9 retire, after platform consumes the fence).
```

Key invariants enforced inside `submit_frame`:

- **No GPU wait on the driver thread.** Phase 7 must return as soon as
  the CBs are enqueued; the `PresentFence` value is the **CPU-visible
  submit-id**, not a GPU completion fence. `platform`'s phase 9 owns
  the GPU-completion wait for pacing.
- **Queue purity.** A CB recorded on `Graphics` cannot be committed to
  `Compute` (SPEC §4.1.6 invariant 4). The acquire path tags each CB
  with its queue role; the commit path asserts the tag matches the
  queue.
- **Single drawable acquisition per frame.** `nextDrawable` is called
  exactly once per `submit_frame`; multi-view fan-out (SPEC #401)
  composes into a single present pass over a single drawable.
- **Failure leaves the device intact.** Any error variant rolls back
  acquired CBs (`metal-cpp` releases them via auto-release-pool drain
  at scope exit) and leaves `device_` / `queues_` / `surface_`
  untouched. The next frame's `submit_frame` call sees a clean
  precondition.

### 3.5 Encoder cursor + debug marker stack

Inside the `MetalCommandBuffer`, the encoder cursor is a discriminated
union over `{None, RenderEncoder*, ComputeEncoder*, BlitEncoder*,
AccelStructEncoder*}` plus a small (≤ 4 KiB) shadow of the most-
recently-bound `MTL::ArgumentBuffer*` per descriptor-frequency group
(R-2.1.8 + R-2.1.16). When a pass's `execute()` lambda asks for an
encoder of a different kind, the cursor:

1. Calls `endEncoding()` on the current encoder (no-op when None).
2. Emits any plan-required barrier (`MTL4` fence + memory barrier
   constructed by `graph/compile.cpp`).
3. Opens the new encoder via `cb->renderCommandEncoderWithDescriptor(...)`
   / `cb->computeCommandEncoder()` / `cb->blitCommandEncoder()` /
   `cb->accelerationStructureCommandEncoder()`.
4. Restores any per-frequency argument-buffer bindings the new
   encoder needs from the shadow.

The debug-marker stack is a SPEC §4.1.6 obligation: `push_debug_group`
and `pop_debug_group` map to `MTL::CommandEncoder::pushDebugGroup` /
`popDebugGroup` on the active encoder, falling back to
`cb->pushDebugGroup` / `popDebugGroup` when no encoder is open. The
stack is bounded to depth 16; overflow returns
`render::Error::PassUnsupportedConfig` (caller bug) rather than
silently truncating.

## 4. Public Surface

### 4.1 Types (locked from SPEC §5)

The metal-backend contributes three opaque classes, two POD structs,
and one free function to the SPEC §5 public header:

| Symbol                | Kind            | SPEC §5 line | Notes                                                                                  |
|-----------------------|-----------------|--------------|----------------------------------------------------------------------------------------|
| `DeviceDesc`          | `struct` (POD)  | 1252         | `prefer_low_power : bool`, `tier : QualityTier`. **§3.1 ABI add:** `headless : bool = false`, `validation : bool = false`. |
| `MetalDevice`         | opaque class    | 1257         | `create(const DeviceDesc&)`, `capabilities()`, `queue(Queue)`, `pso_cache()`, `transient_pool()`. **§3.3 ABI add:** `attach_surface`, `detach_surface`. |
| `MetalQueue`          | opaque class    | 1275         | `acquire_command_buffer()`, `submit(MetalCommandBuffer&)`, `next_submit_id()`, `role()`. `submit()` asserts queue-role tag and increments `submit_counter_`; `next_submit_id()` returns the last-incremented value. |
| `MetalCommandBuffer`  | opaque class    | 1291         | `push_debug_group`, `pop_debug_group`, `queue_role()`. Internal cursor not exposed.    |
| `PresentFence`        | `struct` (POD)  | 1415         | `value : u64`. Returned by `submit_frame`.                                              |
| `submit_frame`        | free function   | 1419         | `(MetalDevice&, const RenderFrame&) → Result<PresentFence>`.                           |

The §3.1 / §3.3 ABI adds (`headless`, `validation`, `attach_surface`,
`detach_surface`) are landed in a single render-plugin ABI bump
alongside the metal-backend implementation; they are listed here so the
sibling task-breakdown spike can scope the surface diff. One new error
variant is required: `GpuFault` (SPEC §10.1 table row 20, marked "ABI
add"). All other backend failure modes map to existing §5 enumerators.
The `GpuFault` enumerator lands alongside the metal-backend
implementation in the same render-plugin ABI bump; the sibling
task-breakdown spike must scope this surface diff explicitly (see §8.5
and §10 for the trigger and recovery protocol, and §11.1
`gpu_fault_capture.cpp` + §11.4 story #398 for coverage).

### 4.2 New surface-attach descriptor

```cpp
struct SurfaceAttachDesc {
    // Caller-vended CAMetalLayer* (typed as void* at the platform seam,
    // up-cast to MTL::Layer* by metal/device.cpp).
    void*               layer            = nullptr;

    // Color format of the swapchain. Default sRGB BGRA8 for MVP.
    // HDR paths set rgba16Float + Rec.2100 PQ colorspace.
    std::uint32_t       color_format     = 0u;       // MTL::PixelFormat
    std::uint32_t       colorspace       = 0u;       // CG colorspace name id

    // Frames-in-flight target. 2 = low-latency; 3 = smooth pacing.
    std::uint8_t        max_drawables    = 3u;

    // True when the layer is HDR-capable AND RenderSettings.hdr_output.
    bool                hdr              = false;
};

class MetalDevice {
    // ... existing surface (SPEC §5) ...
    [[nodiscard]] Result<void> attach_surface(SurfaceAttachDesc) noexcept;
    [[nodiscard]] Result<void> detach_surface() noexcept;
};
```

The `void* layer` indirection — `CA::MetalLayer*` reaches the surface
through `platform`'s `Window::surface().raw_layer()` which returns
`void*` (`specs/platform/SPEC.md` §6 surface bridge contract). The
metal-backend up-casts inside `metal/device.cpp`; the public header
stays free of `metal-cpp` types so callers compile against
`render.hpp` without a `metal-cpp` include path.

### 4.3 No third-party access to `MTL::*` pointers

The header defines no accessor returning `MTL::Device*` /
`MTL::CommandQueue*` / `MTL::CommandBuffer*`. Pass `execute()`
lambdas receive a `MetalCommandBuffer&` whose only public surface is
the debug-marker stack and the `queue_role()` query — every encoder
operation is mediated through the cursor. This is the SPEC §6.6
cross-platform-readiness clause realised: replacing the metal-backend
with a `vk-backend` requires changing `metal/*` and `passes/*` only;
no graph or resource code links `metal-cpp`.

## 5. Hot / Cold Path Split

| Path | Frequency | Surface | Allowed work |
|------|-----------|---------|--------------|
| **Cold** — process init | once per process | `MetalDevice::create` | `MTLCreateSystemDefaultDevice`, three `newCommandQueue` calls, capability probe, `MTL4ResidencySet` construct, PSOCache + TransientPool wiring. May allocate freely against `ContextTag::render` cold-init bucket. |
| **Cold** — surface attach | once per window | `attach_surface`, `detach_surface` | `setDevice`, `setPixelFormat`, `setMaximumDrawableCount`, callback registration. Must complete before phase 7 of the next frame. |
| **Cold** — hot-reload swap | phase 8 | (no public call) | The `MetalDevice` survives swap unchanged (§8). Internal pointer fix-ups happen via `glibre_plugin_register`. |
| **Hot** — per-frame submit | once per frame (≤ 4 views) | `submit_frame` | One CB acquire per used queue; one ExecutionPlan recording per CB; one `nextDrawable`; one `presentDrawable`; one `commit` per CB; one fence increment. **No allocation, no NS-retain, no `MTL::Library` walk, no PSO compile.** |
| **Hot** — per-pass record | once per pass per frame | `MetalCommandBuffer::push_debug_group` / `pop_debug_group` | Encoder open/close routed via cursor; argument-buffer rebind on shadow miss. **No `MTL::Heap` allocation, no PSO miss-path.** |

The cold/hot split is the SPEC §9.3 ceiling realised: the hot path's
0.05 ms `metal/queue.cpp` slice (one `commit()` per queue + fence
emit) plus the 0.05 ms retire slice fit inside the 0.10 ms backend
budget. PSO compile, heap allocation, and `MTL::Library` parsing all
sit in cold paths; a hot-path miss is a typed-error refusal
(`Error::PipelineCacheMiss`, `Error::HeapOutOfMemory`,
`Error::TransientPoolExhausted`), never a hidden compile.

## 6. Concurrency

The backend integrates with SPEC §6.3's three-thread topology: one
graph-builder thread, three per-pass encoding workers (one per
queue), one render driver thread. Backend objects' thread-safety
contract:

### 6.1 `MetalDevice` is read-mostly + cold-path-write

- After `create()` returns, every member except `residency_set_` and
  `surface_` is immutable for the device's lifetime.
- `residency_set_` mutation happens only at cold-path register / drain
  boundaries (SPEC §8.3) under the loader's exclusive ownership.
- `surface_` mutation happens only at `attach_surface` / `detach_surface`
  call sites; concurrent reads from phase 7 are forbidden by the
  schedule.
- `drawable_size_` is a `std::atomic<std::uint64_t>` packing the two
  32-bit IEEE floats as `(width_bits | uint64_t(height_bits) << 32)`.
  The resize callback (phase 1) stores with `memory_order_release`; the
  phase-7 submit-frame reader loads with `memory_order_acquire`. This
  establishes the required happens-before: the phase-loop barrier between
  phase 1 and phase 7 is already a sequenced point, but the atomic also
  guards the rare case where the resize callback fires from a platform
  thread asynchronously before the barrier.
- All hot-path readers (graph builder, encoders, driver) treat
  `MetalDevice` as `const &`. No locks.

### 6.2 `MetalQueue` is thread-safe per Metal

Per Apple's documentation, `MTL::CommandQueue` is thread-safe for
concurrent `commandBuffer` allocation and `commit`. The wrapper
`MetalQueue` adds:

- `submit_counter_` is a `std::atomic<uint64_t>` updated under
  `memory_order_acq_rel` inside `submit()`. The companion
  `next_submit_id()` reads it with `memory_order_acquire` and returns
  the current value; callers (including `submit_frame` step 7) use
  this accessor rather than touching `submit_counter_` directly. The
  `PresentFence` consumer in `platform`'s phase 9 also reads via
  `memory_order_acquire`.
- `acquire_command_buffer()` is a single `device_->commandQueue()->commandBuffer()`
  call; lock-free.

The three queues never share a `MetalCommandBuffer`; each CB is
queue-tagged at acquire and queue-checked at commit (SPEC §4.1.6
invariant 4).

### 6.3 `MetalCommandBuffer` is single-thread per CB

Metal's contract: a `MTL::CommandBuffer` is **not thread-safe** for
concurrent encoding from multiple threads. The wrapper preserves this
by being thread-affine:

- A CB returned from `MetalQueue::acquire_command_buffer()` is owned
  by exactly one worker thread for its lifetime.
- The cursor + debug-marker stack are mutated only on that thread.
- Cross-thread handoff is forbidden until commit; the commit path
  transfers ownership back to the driver thread, which calls
  `commit()` and drops the wrapper.

Per-pass `execute()` lambdas inherit the thread affinity of the CB they
receive; they may not move the CB to a different thread.

### 6.4 Cross-queue ordering

Cross-queue dependencies are encoded as plan-emitted `MTL::SharedEvent`
fences (Metal 4 `MTL4Fence` where supported), constructed by
`graph/compile.cpp` and consumed by `metal/fence.cpp`. The backend
applies them mechanically — `cb_compute->encodeSignalEvent(event,
value)` / `cb_graphics->encodeWaitEvent(event, value)` — and never
schedules a queue commit before its dependencies' commits.

The plan also encodes the queue-commit order; the backend's commit
loop walks that order and never reorders. For MVP the order is fixed
(`Compute → Graphics → Copy`) but the design admits arbitrary plan-
provided orderings.

## 7. Persistence + ABI

### 7.1 The metal-backend persists nothing

`MetalDevice`, `MetalQueue`, `MetalCommandBuffer`, `SurfaceAttachment`,
`PresentFence`, and the encoder cursor have **no Fory schema** and
**no on-disk artefact**. They are pure runtime state.

The PSO archive (SPEC §7.1.2) is keyed by `(gpu_id, metal_feature_set,
os_build_hash)` which the backend probes at `create()`; the archive
itself is owned by `PSOCache` (#766) and is the only persistent
artefact in the device's neighbourhood.

`CapabilityMask` (SPEC §7.1.3) is owned by `platform`'s probe path; the
backend re-derives a live `CapabilitySet` on every `create()` from the
device handle, never reads the persisted mask.

### 7.2 ABI surface

The metal-backend exports its types through SPEC §5's plugin header.
Specifically:

- `DeviceDesc`, `SurfaceAttachDesc`, `PresentFence` are POD structs.
  Their layouts are part of the render-plugin ABI; field additions
  bump the ABI hash via `reviews/decisions/plugin-abi.md` rules.
- `MetalDevice`, `MetalQueue`, `MetalCommandBuffer` are forward-declared
  opaque classes (no member layout exposed). Adding a method bumps
  the ABI; changing a method signature bumps the ABI; field additions
  inside the class are ABI-neutral.
- `submit_frame` is a free function; signature changes bump the ABI.

No `MTL::*` types ever reach the ABI surface. The backend's symbols
link `metal-cpp` privately inside the dylib only.

### 7.3 Plugin manifest

Per `reviews/decisions/plugin-abi.md`, the render plugin's manifest
declares its exported aggregates and capability requirements. The
metal-backend contributes:

- Declared phase ownership: phase 7 entry / exit (the device is the
  recipient of `submit_frame` calls inside phase 7).
- Declared capability requirements: none at the manifest level.
  Capability gating is per-pass and queried at graph-build time, not
  at plugin load time.
- Declared persistent component types: none; the backend persists
  nothing (§7.1).

Manifest changes are an ABI bump.

## 8. Hot-Reload Integration

The metal-backend is **the most resilient piece of the render plugin
to hot-reload**. SPEC §8.2's survival inventory marks
`MetalDevice` and `MetalQueue` as "Yes — survive swap" because the
device handle is owned by the engine `Registry` (constructed once at
process start, vended to every plugin generation); the swap touches
host code only.

### 8.1 Drain — `glibre_plugin_drain`

The leaving plugin's drain body's responsibilities for the metal-
backend are minimal:

1. Wait for any in-flight `MetalCommandBuffer` references held by
   passes' `execute()` lambdas to be retired. Since drain runs at
   phase 8 entry (SPEC §8.1) and phase 7 has already returned, no
   recording is in progress. The drain step is a no-op for the
   backend.
2. Detach any debug-overlay listeners on the `MetalDevice`'s
   diagnostic ring (§3.3 of `glibre/render/graph/diagnostic.cpp`).
3. Leave `device_`, `queues_`, `pso_cache_`, `transient_pool_`,
   `residency_set_`, and `surface_` **untouched**. They survive.

### 8.2 Swap

The loader replaces the dylib's vtable / function pointers. The
`MetalDevice` instance lives behind a `unique_ptr` held by the
`core` registry; its bytes do not move. Pointers held by the
leaving plugin become invalid, but they are not dereferenced after
drain returns.

### 8.3 Migrate

No `migrate(...)` body for the metal-backend's types — they have no
Fory schema (§7.1). The migration round runs zero backend functions.

### 8.4 Resume — `glibre_plugin_register`

The arriving plugin's register body:

1. Looks up the surviving `MetalDevice*` via the registry: `auto*
   device = registry.get<MetalDevice>();`. Failure here is a loader
   contract violation (the registry is engine-managed, not plugin-
   managed); reachable only when the registry has been corrupted —
   wraps to `core::Error::PluginInitFailed`.
2. Re-registers pass classes against the surviving device's
   PSOCache (SPEC §8.3.2): walks the new plugin's `(pass_class,
   PSOKey)` table and calls `device->pso_cache().pin(key)` for each
   entry. The metal-backend's role here is to vend the cache pointer
   — the pin operation is the PSOCache's responsibility (#766).
3. Re-registers any heap regions the new plugin's resources sub-system
   declares against the residency attachment via
   `device->residency().attach(heap)`. Pre-existing registrations
   from the leaving plugin survive intact.
4. Does **not** call `attach_surface` again. The surface attachment
   from process start survives the swap; the new plugin's first
   `submit_frame` call sees the same `surface_->layer`.
5. Does **not** rebuild any command buffer. The next frame's phase 7
   will produce them fresh from the new plugin's pass bodies.

### 8.5 GPU-fault restart (SPEC §10.4) — special path

When `submit_frame` reports `GpuFault` (SPEC §10.3 `GpuFault` row),
the recovery is `hot-reload-restart` (SPEC §10.4). The metal-backend's
contribution to this path:

1. Capture the `MTL::CommandBufferError` payload synchronously inside
   `submit_frame` before returning the error. The encoder-info dump
   is a single Metal API call (`error.userInfo[
   MTLCommandBufferEncoderInfoErrorKey]`).
2. Surface the payload through `render_report_gpu_fault` (SPEC §10.4
   step 1), an internal entry the platform-side fence-wait calls.
3. Survive the subsequent reload-of-self exactly like any other
   reload (§8.1–§8.4 above): the device + queues survive, the
   capability set is **re-probed** against the current device because
   a thermal throttle or eGPU detach may have changed the available
   feature set. The re-probe runs inside the new plugin's
   `glibre_plugin_register`; the fix is sticky (SPEC §10.2 (2) "the
   bit only re-enables on hot-reload register if the new plugin
   redeclares the capability *and* the host still supports it").

### 8.6 Refusal cases

The metal-backend contributes no new umbrella refusal arm; SPEC §8.4's
existing render refusal table covers the metal-backend cases:

| Render refusal cause                                              | Backend trigger                                                                                                          |
|-------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------|
| `render::Error::CapabilityNotSupported`                           | New plugin declares a pass requiring a capability bit the live `CapabilitySet` doesn't carry (e.g. requires MeshShaders on a host that lost the bit during GPU-fault re-probe). |
| `render::Error::PipelineCompileFailed`                            | New plugin declares a `(state_hash, shader_hash)` collision against a surviving cache entry — the backend's role is only to expose the cache; the refusal is authored by PSOCache (#766). |
| `core::Error::HotReloadRefused` direct                            | Mid-frame reload requested. Loader-level refusal; the metal-backend never sees it.                                       |

## 9. Performance

This section refines SPEC §9's render-row budget for the metal-backend
slice. Every cell is a contractual ceiling per SPEC §9; the backend's
contribution is bounded.

### 9.1 Per-frame CPU budget (phase 7)

| Step                                                    | Budget    | Source                                                                                            |
|---------------------------------------------------------|-----------|---------------------------------------------------------------------------------------------------|
| `acquire_command_buffer` × (≤ 3 queues)                 | < 0.01 ms | One Metal API call per queue; lock-free CB allocation per Apple docs.                             |
| `nextDrawable` (graphics queue, present pass)           | < 0.01 ms | Drawable-pool fast path; the `nil`/timeout case is the SPEC §10.3 `SwapchainAcquireFailed` row.    |
| `commit()` × (≤ 3 CBs)                                  | 0.04 ms   | SPEC §9.3 row "metal/queue.cpp — submit + PresentFence signal" = 0.05 ms total (commits + fence). |
| `presentDrawable` (graphics CB)                         | < 0.01 ms | Single API call; deferred until GPU completion.                                                   |
| Fence increment (`std::atomic` + return)                | < 0.001 ms| One acquire-release atomic.                                                                       |
| **Backend driver-thread subtotal**                      | **0.05 ms** | Inside SPEC §9.3 row total of 1.0 ms phase-7 driver budget; the backend is < 5% of the row.       |

The encoder cursor's per-pass overhead (open / close + barrier emit)
is folded into SPEC §9.3's "Per-pass `execute()` recording" 0.5 ms
slot, not into the backend's 0.05 ms. Per-encoder cost on M1 is
< 1 µs amortised (R-2.1.14).

### 9.2 Memory budget

| Slot                                                    | Budget    | Source                                                                                            |
|---------------------------------------------------------|-----------|---------------------------------------------------------------------------------------------------|
| `MetalDevice` storage (handles, callbacks, cap mask)    | < 64 KiB  | One device per process; trivial.                                                                  |
| `MetalQueue` × 3 (handles + atomic counters)            | < 4 KiB   | Three queues, dozens of bytes each.                                                               |
| `MetalCommandBuffer` cursor + shadow per CB             | ≤ 4 KiB   | R-2.1.15 ceiling adopted.                                                                         |
| Debug-marker stack per CB                               | ≤ 1 KiB   | Bounded depth 16 × short string view.                                                             |
| `MTL4ResidencySet` membership bitset                    | folded into SPEC §9.5 row "GPU resource handles" (16 MiB bucket).                                                |
| **Total CPU-side residency**                            | < 256 KiB | A negligible fraction of the 512 MiB render heap ceiling. Tagged `ContextTag::render`.            |

### 9.3 GPU budget

The backend itself emits **no GPU work** beyond the `presentDrawable`
slot. SPEC §9.4's `passes/present.cpp` slice (0.5 ms on M1) covers the
swapchain blit + present scheduling; that slice is the present
pass's body, not the metal-backend's intrinsic cost.

### 9.4 Allocation rules

Per SPEC §9.5.1:

- The cold-path `create()` allocates against `ContextTag::render` and
  fits inside the 512 MiB ceiling at process start (sub-megabyte).
- The hot-path `submit_frame` performs **zero** heap allocations; CBs
  are vended from `MTL::CommandQueue`'s internal pool (Metal-managed,
  not glibre-managed), and the encoder cursor is reused frame-to-
  frame on a triple-buffered scratch.
- `attach_surface` allocates the `SurfaceAttachment` (~1 KiB) once.
- The transient pool's heap allocations (256 MiB ceiling, SPEC §9.5
  row 3) are owned by `render/src/resources/transient_pool.cpp`, not
  by the metal-backend; the backend only vends `device_` to the pool.

## 10. Failure Modes

Every metal-backend failure surface returns `std::expected<T, glibre::Error>`
over the SPEC §10.1 closed-sum `render::Error`. The backend contributes
the following triggers; the recovery / severity / capability-fallback /
test-fixture columns are SPEC §10.3's authoritative rows.

| Variant                       | Backend trigger                                                                                                                     | SPEC §10.3 row                          |
|-------------------------------|-------------------------------------------------------------------------------------------------------------------------------------|-----------------------------------------|
| `DeviceUnsupported` (a.k.a. `MetalDeviceUnavailable`) | `MTLCreateSystemDefaultDevice` returns null at `create()`, or `newCommandQueue` returns null for any of the three roles.                                       | row 1 (`abort-engine`).                 |
| `DeviceLost`                  | `MTL::CommandBuffer.status == .error` with `.deviceRemoved` reason observed by `submit_frame` on commit (rare; eGPU detach, kext crash).                                       | row 1 (`abort-engine`).                 |
| `SwapchainAcquireFailed`      | `nextDrawable` returns null inside `submit_frame` after the platform-defined acquire timeout (SPEC §9.4 budget = 1.5 ms). | row 2 (`abort-frame`).                  |
| `SwapchainOutOfDate`          | Resize callback fires from `platform` phase 1 between frames; the next `submit_frame` re-checks `drawable_size_` against the plan's expected view extent and returns this variant if the plan was compiled for the old size.                  | (folded into SPEC §10 follow-up — re-plan on next frame). |
| `QueueSubmitFailed` (a.k.a. `FrameSubmitFailed`) | `MTL::CommandQueue.commit` returns failure (transient driver error, queue overflow); not a device loss.                                                                          | row 15 (`abort-frame`).                 |
| `PresentFailed`               | `presentDrawable` rejected by Metal (drawable was already presented or texture wrong format).                                                                                    | (folded into row 15 / row 16).          |
| `FenceTimeout` (a.k.a. `PresentTimeout` / `GpuTimeout`) | The CPU-side `PresentFence` consumer in `platform` phase 9 fence-wait exceeds the 16.6 ms budget by > 2× without a GPU-side completion signal.                                | rows 16, 17 (`abort-frame` / `lower-tier`). |
| `GpuFault`                    | `MTL::CommandBuffer.status == .error` with `.faulted` reason; surfaced via the backend's diag-capture path (§8.5; SPEC §10.4).                                                  | row 18 (`hot-reload-restart`).          |
| `ResourceImportRefused`       | `attach_surface` called with a null `layer` (caller-precondition violation, abort-engine); or called twice without an intervening `detach_surface`; or surface attach attempted from a non-cold-path frame phase.  | (no recovery — caller bug; `abort-engine`). |
| `CapabilityNotSupported`      | `attach_surface` requested HDR colorspace on a host without the `HdrPresent` capability bit; or `create()` succeeded but a downstream pass requires a missing capability bit.    | rows 13, 14 (`lower-tier` / `disable-feature`). |

The backend never fabricates a variant outside this list; every
trigger maps to a single arm. The `render::Error` enum is closed
(SPEC §10), and adding a new variant is a render-plugin ABI bump.

### 10.1 Logging

Each backend failure routes through `glibre::log_error(Error)` per
`reviews/decisions/error-model.md` §"Logging / Telemetry". The
structured fields are:

- `plugin_fqn = "glibre.render"`
- `aggregate = "metal-backend"`
- `phase = <init|attach|frame-7|frame-9>`
- `metal_status = <integer if MTLCommandBufferError, else 0>`
- `gpu_id = <MTLDevice.registryID>`
- `inner = <render::Error enumerator>`

Failures at `create()` log at `error`; failures at `submit_frame`
log at `warn` (the recovery is per-frame, the previous frame is
re-presented); `GpuFault` logs at `error` because it triggers the
hot-reload-restart path.

## 11. Test Plan

Every test file lives under `tests/render/metal/`. Tests are Catch2
fixtures per `reviews/decisions/error-model.md` and CLAUDE.md "Tests
by type — `plan` → unit tests".

### 11.1 Unit tests (mocked metal-cpp lifecycle)

Mock target: a thin `MetalCpp` test fake that replaces
`MTL::CreateSystemDefaultDevice`, `newCommandQueue`, `commandBuffer`,
`commit`, `nextDrawable`, `presentDrawable`, and the
`MTLCommandBufferError` payload with controllable test doubles. The
fake lives under `tests/render/metal/fake/`; it is the only test
infrastructure that depends on `metal-cpp` headers.

| File                                                | Asserts                                                                                                            |
|-----------------------------------------------------|--------------------------------------------------------------------------------------------------------------------|
| `device_create_happy.cpp`                           | `MetalDevice::create({})` returns a non-null `unique_ptr` on a fake that vends a healthy device + 3 queues.        |
| `device_create_no_device.cpp`                       | Returns `unexpected{DeviceUnsupported}` when the fake's device factory returns null.                               |
| `device_create_queue_null.cpp`                      | Returns `unexpected{DeviceUnsupported}` when the fake fails any of the three `newCommandQueue` calls; pre-acquired predecessors are released. |
| `device_create_capabilities_probed.cpp`             | Fake reports `Apple7` + `Metal3` + raytracing; the resulting `CapabilitySet` carries `MeshShaders | RayQuery | HardwareRayTrace`. |
| `device_headless.cpp`                               | `DeviceDesc{ .headless = true }` constructs a device with `surface_ == nullptr`; `submit_frame` skips the present pass. |
| `surface_attach_happy.cpp`                          | `attach_surface(SurfaceAttachDesc{ .layer = &fake_layer, ... })` calls `setDevice`, `setPixelFormat`, `setMaximumDrawableCount` on the fake; `surface_` becomes non-null. |
| `surface_attach_null_layer.cpp`                     | Returns `unexpected{ResourceImportRefused}` for a null `layer` (caller-precondition violation; recovery abort-engine, not abort-frame). |
| `surface_attach_double.cpp`                         | A second `attach_surface` without intervening detach returns `unexpected{ResourceImportRefused}`.                  |
| `surface_resize_callback.cpp`                       | The callback registered at attach is invoked when the fake reports a resize; `drawable_size_` stores with `memory_order_release` and a concurrent phase-7 reader load with `memory_order_acquire` observes the new dimensions without a data race.  |
| `submit_frame_happy.cpp`                            | One CB acquired per used queue; `record_into` called per CB; `nextDrawable` called once; `presentDrawable` called once; commits in plan order; fence increments by 1. |
| `submit_frame_drawable_null.cpp`                    | `nextDrawable` returning null returns `unexpected{SwapchainAcquireFailed}`; no `commit` issued.                    |
| `submit_frame_commit_fail.cpp`                      | A commit failure returns `unexpected{QueueSubmitFailed}`; subsequent commits are skipped; sibling CBs released.    |
| `submit_frame_queue_purity.cpp`                     | A test that records a CB on the graphics queue and attempts to commit it via `MetalQueue::submit` on the compute queue triggers the debug-build assertion (and returns the typed error in release-build). |
| `command_buffer_debug_markers.cpp`                  | `push_debug_group` / `pop_debug_group` map to the active encoder; depth 16 is enforced; overflow returns `PassUnsupportedConfig`. |
| `command_buffer_encoder_cursor.cpp`                 | Switching encoder kind ends the previous encoder and emits the plan-required barrier; argument-buffer shadow restores bindings. |
| `command_buffer_thread_affinity.cpp`                | Recording from a non-owning thread triggers the debug-build assertion. (Documents R-2.1.14's contract.)            |
| `device_lost_path.cpp`                              | A fake CB that resolves to `.error` with `.deviceRemoved` causes `submit_frame` to return `unexpected{DeviceLost}`. |
| `gpu_fault_capture.cpp`                             | A fake CB that resolves to `.error` with `.faulted` triggers the diag-capture entry; the fault payload is forwarded via `render_report_gpu_fault`. |
| `present_fence_monotonic.cpp`                       | Across N successful submits, the returned `PresentFence::value` strictly increases by 1 per submit; concurrent reads see a consistent value. |
| `present_fence_timeout.cpp`                         | A fake where the platform's phase-9 fence-wait callback fires with an elapsed time exceeding 2× the 16.6 ms budget ceiling returns `unexpected{FenceTimeout}` (SPEC §10.3 rows 16/17); distinct from the drawable-nil path (`SwapchainAcquireFailed`) exercised by `submit_frame_drawable_null.cpp`. |

### 11.2 Integration tests (real device under platform fixture)

These run only on macOS hosts under a `[require:metal]` Catch2 tag and
are gated out of the Linux/Windows CI matrices. The fixture builds a
real `MetalDevice` against the host's `MTLCreateSystemDefaultDevice`
and a hidden `CAMetalLayer` constructed by the platform test harness
(`tests/platform/fixtures/HiddenWindow.hpp`).

| File                                                | Asserts                                                                                                            |
|-----------------------------------------------------|--------------------------------------------------------------------------------------------------------------------|
| `integration/device_create_real.cpp`                | `MetalDevice::create({})` succeeds against the host device; capabilities reported match `system_profiler SPDisplaysDataType` output. |
| `integration/surface_attach_real.cpp`               | `attach_surface` against a real hidden `CAMetalLayer`; `nextDrawable` returns a non-null drawable.                  |
| `integration/submit_frame_real_no_pass.cpp`         | A minimal plan with only a clear-color present pass commits successfully and presents one frame.                    |
| `integration/submit_frame_real_three_queues.cpp`    | A plan touching all three queues (compute clear + graphics present + copy noop) commits in plan order; cross-queue fence resolves. |
| `integration/headless_no_present.cpp`               | A headless device records and commits a compute-only plan; the present pass is gated out by the absent surface.    |

### 11.3 Performance microbenchmarks

`tests/render/perf/metal/` Catch2 `BENCHMARK` blocks against the S1
fixture (SPEC §9 baseline; one character + 200 props + 8 lights):

- `BENCHMARK("submit_frame backend slice, S1, p99")` ≤ 0.05 ms.
- `BENCHMARK("device create cold-start, S1")` ≤ 5 ms (cold-start
  bucket; not a per-frame ceiling but bounds startup latency).
- `BENCHMARK("attach_surface cold, S1")` ≤ 1 ms.

### 11.4 E2E coverage

The metal-backend's contribution to the SPEC §11 user-story matrix:

- #396 (`render: present pass acquires drawable and signals
  PresentFence`) — `submit_frame_happy.cpp` +
  `present_fence_monotonic.cpp` cover the unit half;
  `tests/e2e/render/present.glibre-trace` covers the E2E half.
- #397 (`render: PresentTimeout (>2× budget) triggers abort-frame`) —
  `present_fence_timeout.cpp` covers the unit half (see §11.1 below);
  the e2e trace inserts a deliberate GPU stall that exhausts the phase-9
  fence-wait deadline. Note: `submit_frame_drawable_null.cpp` tests the
  `SwapchainAcquireFailed` path (nextDrawable nil), which is a distinct
  failure from `FenceTimeout` (SPEC §10.3 rows 16/17 vs. row 2).
- #398 (`render: GpuFault triggers hot-reload-restart with diag
  capture`) — `gpu_fault_capture.cpp` covers the unit half; the e2e
  trace runs the full §10.4 protocol.
- #402 (`render: ResourceResidencyExceeded triggers lower-tier
  recovery`) — backend's `attach_surface` HDR-on-non-HDR-host path
  feeds into the resources sub-system test that closes #402.

## 12. Open Questions

- [OPEN] **Multi-window / multi-surface in MVP?** Today the design
  pins exactly one `SurfaceAttachment` per `MetalDevice`. The MVP
  scope (`reviews/decisions/perf-budget.md` S1 fixture) is single-
  window. A second editor-attached preview window would need either a
  second `attach_surface` call (rejected as ABI break) or a
  `MetalDevice::attach_surface(view_handle, desc)` overload. Owner:
  next render sub-epic; resolution gate: when the editor-preview
  story (#post-MVP) lands.
- [OPEN] **HDR auto-detect at attach.** The current design takes
  `SurfaceAttachDesc::hdr` as caller input. Auto-detecting HDR head-
  room from `NSScreen.maximumExtendedDynamicRangeColorComponentValue`
  inside `attach_surface` would simplify the callsite but couples the
  metal-backend to `AppKit` — currently a `platform`-only dependency.
  Owner: render context; resolution gate: when HDR output ships
  (post-MVP, currently no story).
- [OPEN] **`MTL4Compiler` async-PSO feedback channel.** Metal 4
  exposes a CB-completion-handler that reports per-pass GPU fault
  encoder info. The backend captures it on `GpuFault` but does not
  expose a streaming subscription for the editor's live profiler.
  Owner: tools context; resolution gate: when the editor profiler
  spike lands (post-MVP).
- [OPEN] **Fence semantics across backends.** `PresentFence` is a
  CPU-visible monotonic submit id. A future Vulkan / D3D12 backend
  would want a richer GPU-visible fence object. The MVP design picks
  the CPU-id form because `platform` phase 9 reads only the id; if a
  second backend lands, the fence type would graduate to a
  variant or a sum type. Owner: render + platform; resolution gate:
  second-backend spike (post-MVP).
- [OPEN] **Validation layer parity with debug builds.** Metal's
  shader validation + API validation are configurable per-process
  via `MTL_DEBUG_LAYER`. The backend's `DeviceDesc::validation` field
  (§4.1 ABI add) lets a build opt in, but the production toggle
  story (env var vs. compile flag vs. RenderSettings) is unresolved.
  Owner: render; resolution gate: first internal alpha (covered by
  the diagnostic-overlay story #399 implementation).
