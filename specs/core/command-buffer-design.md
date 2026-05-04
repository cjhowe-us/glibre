# Command-Buffer Detailed Design

> Detailed design for the `core` context's command-buffer aggregate
> (`CommandBuffer`, SPEC §4.8 / §5.10 / §6.3). Refines those sections
> against the engine-wide decision records (`error-model.md`,
> `frame-phases.md`, `perf-budget.md`, `hot-reload-protocol.md`,
> `plugin-abi.md`, `fory-codegen.md`).
> All conclusions re-derived; harmonius prior art
> (`harmonius/docs/design/core-runtime/ecs.md` §"Command Buffer" and
> §"ParallelCommandWriter", R-1.1.32 / R-1.1.32a / R-1.1.33) cited
> only as research input.

Refs: spike #710 — `[SPIKE] design-core-command-buffer-detailed`.
Parent sub-epic #699. Sibling task-breakdown spike blocked-by this
deliverable.

## 1. Purpose

`CommandBuffer` is the smallest deterministic primitive for **deferred
ECS structural mutation** in glibre. While a system body executes, it
records spawn / despawn / add / remove / set intents into its
per-system buffer; the buffer is **not** queryable by the recording
system; at the end of the system's owning phase, the schedule replays
every recorded command into `World` in a fixed deterministic order.
This decouples "what a system wants to mutate" from "when other
systems can observe the mutation", which is the seam parallel system
execution (post-MVP) hangs on, and which the deterministic-replay
contract (PHILOSOPHY §7) rests on in MVP.

What the buffer explicitly refuses to own:

- **Schedule decisions.** When a buffer flushes, in what order the
  systems in a phase run, and which phase boundary is "the" sync
  point — owned by `Schedule` / `FrameLoop` (SPEC §4.4, §6.4 / §6.5;
  `schedule-frame-design.md` §3.5).
- **Entity ownership / archetype migration policy.** The buffer
  **describes** mutations; `World` (SPEC §4.1, §6.2) executes the
  archetype edits. The buffer never reaches into archetype storage.
- **Cross-context event routing.** Typed channels, capture/bubble
  propagation, observer cascades, and cross-world bridges are the
  `events` plugin's job (SPEC §3.3, R-1.5.* refusals; harmonius
  `events-plugins.md`). The command buffer is mutation-only.
- **Reactive queries / observer dispatch.** Component-lifecycle hooks
  (R-1.1.30) and event-channel observers (R-1.1.30a) are the
  `events` plugin's, not core. The buffer's flush does not invoke
  observer callbacks; if a plugin wants observer behaviour, it
  registers a system in the next phase that reads the post-flush
  state.
- **Hot-reload state preservation.** The buffer is per-frame
  transient; its contents do not cross a hot-reload swap. See §7
  (Persistence + ABI) and §8 (Hot-Reload).

The SRP boundary is sharp: if the **command record encoding**, the
**deterministic merge order**, the **flush trigger point**, the
**arena allocation rule**, or the **failure / overflow contract**
change, this design changes. Anything else — schedule shape,
archetype storage, plugin loading — is out of scope.

## 2. Requirements Coverage

Mapping of harmonius requirements (`R-1.1.32`, `R-1.1.32a`,
`R-1.1.33`, plus the cross-cutting "deferred structural change" rules
from `harmonius/docs/design/core-runtime/ecs.md` §"Command Buffer")
to glibre MVP coverage / refusal. Every entry is independently
re-derived; harmonius is research input only.

| Harmonius req                                                  | Glibre disposition (MVP) | Coverage site                                                                                |
|----------------------------------------------------------------|--------------------------|----------------------------------------------------------------------------------------------|
| R-1.1.32 deferred structural changes via command buffers       | **Covered**              | §3 (record model) + §3.6 (deterministic apply); SPEC §4.8.                                   |
| R-1.1.32a 100k-command flush ≤ 1 ms; ≤ 64 KiB typical per-system | **Covered (smaller cap, larger total)** | §9.1 — apply-phase ≤ 0.10 ms, per-system arena cap chosen to match MVP entity counts (§9.2); the `100k commands per frame` figure is harmonius-scale, glibre's MVP scenario S1 (`perf-budget.md` §"Justification") tops out closer to ~2k mutations/frame. |
| R-1.1.33 parallel command recording with sort keys             | **Refused (MVP) / Reframed (post-MVP)** | §6 — MVP runs all systems on the loop driver thread; the per-system buffer **is** the per-thread bucket. Parallel recording lands when per-system parallelism does (`SPIKE` #496); the "sort key" primitive is replaced with the **lexicographic system-FQN tiebreaker** locked in SPEC §4.8 invariant 2, which is deterministic across runs without authoring `u64` sort keys. |
| harmonius `Commands<'w>` / `EntityCommands<'a, 'w>` builder API | **Covered (flatter shape)** | §4 — glibre exposes `CommandBuffer&` directly (no scoped builder layer); the recorded entity returned by `spawn()` is immediately usable in subsequent `add_component` calls within the same buffer (§3.6 entity-id remapping). |
| harmonius `CommandBuffer::flush(&mut World)` triggers observers | **Refused**              | §1 — observer dispatch is `events`-plugin territory, not core. `flush` writes archetype state and returns; nothing else fires. Plugins observe via next-phase systems.                                |
| harmonius nested-command depth limit of 16 (observer recursion)| **Refused**              | §1 — no observer cascade; depth limit irrelevant. Deferred to `events` SPEC if/when cascades land.                                                                                                  |
| harmonius `ParallelCommandWriter` per-thread segments + sort   | **Refused (MVP)**        | §6.1 — single-threaded MVP; per-system buffers already bucket by-system, which is the same shape the post-MVP per-thread design will use. No sort-key authoring burden for plugin authors.        |
| harmonius `Commands::reserve` deferred entity ID               | **Covered (different mechanism)** | §3.6 — `spawn()` allocates a real `Entity` immediately from `World::reserve_entity_for_command_buffer` (a pre-flush reservation path on the entity allocator); the `Entity` is alive but in the empty archetype until apply makes its components real. No "deferred" id; the id is real, the components are deferred. This collapses the `Entity` / `EntityCommands` two-phase trick in harmonius to one primitive. |
| harmonius "overflow grows the command vec"                     | **Refused (refuse-on-overflow)** | §3.5 / §10 — append past the per-system arena cap returns `core::Error::CommandBufferOverflow` (SPEC §4.8 invariant 4). Growing under hot-path determinism would cost an allocator round-trip mid-system; refusing is the smallest deterministic choice.        |

Coverage rule: every harmonius clause above either lands here or is
refused with one-line rationale. No silent drops.

Glibre-native requirements added beyond harmonius:

- **Apply at exactly the schedule-defined sync point**, never
  mid-system, never mid-phase (`schedule-frame-design.md` §3.5;
  SPEC §4.8 invariant 5). This is the seam that makes parallel
  recording (post-MVP) determinism-preserving without a per-system
  fence.
- **Apply preserves byte-equal world snapshots across hosts and runs**
  (PHILOSOPHY §7). The merge order is data-independent: lexicographic
  system FQN, then in-buffer insertion index. No host-specific
  wallclock, no memory-address ordering.
- **Per-frame transient memory, no cross-frame retention** — the
  buffer's arena belongs to the per-frame transient pool
  (`perf-budget.md` Allocator Rule #4). It drains by phase 9 (present);
  any leak is `core::Error::OutOfBudget` with a `leak` detail string,
  not a `CommandBuffer` arm.
- **Refuse, do not swap, on hot-reload while recording.** A buffer
  must be drained pre-swap (§8). The hot-reload barrier owns the
  drain assertion; the command buffer is the asserted invariant.

## 3. Detailed Model

### 3.1 Aggregate composition

```text
CommandBuffer (per-system, per-frame value object; SPEC §4.8)
├── Header
│   ├── owning_system_id  : SystemId       (set by Schedule at registration; immutable)
│   ├── owning_system_fqn : eastl::string_view (borrowed from SystemDesc; lex tiebreaker)
│   ├── owning_phase      : Phase          (1..=9, captured at registration)
│   ├── arena             : eastl::pmr::monotonic_buffer_resource* (per-frame; §3.5)
│   └── arena_cap_bytes   : std::uint32_t  (the §9.2 per-system cap)
│
├── Records (append-only during the system body)
│   ├── records_          : eastl::pmr::vector<Command>  (32-byte fixed-size; §3.2)
│   └── byte_payload_     : eastl::pmr::deque<std::byte> (variable-sized component bodies; §3.4)
│
├── Reserved-entity ledger (entity-id remapping; §3.6)
│   └── reserved_         : eastl::pmr::vector<Entity>   (id allocated, archetype empty)
│
└── Counters
    ├── recorded_count_   : std::uint32_t
    └── overflow_seen_    : bool                       (latched on first refused append)
```

The `CommandBuffer` is owned by `Schedule` (one per registered system,
allocated at `Schedule::compile()` time; freed at
`Schedule::unregister_system()` or `Schedule::~Schedule()`). Each
system's `SystemContext::commands()` returns a reference to the
matching buffer for the duration of the system's body. The reference
is invalidated the instant the body returns; cached pointers are
dangling.

### 3.2 `Command` record (32-byte POD)

```cpp
// core/include/glibre/core/command.hpp — internal, not in §5 public ABI.

namespace glibre::core::detail {

enum class CommandKind : std::uint8_t {
    SpawnEmpty       = 0,  // reserve an Entity; no components; payload_offset unused
    Despawn          = 1,
    AddComponent     = 2,
    RemoveComponent  = 3,
    SetComponent     = 4,
};

struct Command {
    CommandKind     kind;            // 1 byte
    std::uint8_t    pad0_;           // 1 byte (zeroed)
    std::uint16_t   pad1_;           // 2 bytes (zeroed; reserved for post-MVP flags)
    std::uint32_t   insertion_index; // 4 bytes; monotonic in append order, used as tiebreaker
    Entity          target;          // 8 bytes; Entity{bits} from §5.2
    TypeId          type_id;         // 8 bytes; ignored for SpawnEmpty / Despawn
    std::uint32_t   payload_offset;  // 4 bytes; index into byte_payload_; 0 for non-payload kinds
    std::uint32_t   payload_size;    // 4 bytes; 0 for non-payload kinds
};
static_assert(sizeof(Command) == 32);
static_assert(std::is_trivially_copyable_v<Command>);

}  // namespace glibre::core::detail
```

Record-shape rules:

- **POD only.** Trivially copyable, trivially destructible, no
  inheritance, no virtuals. The post-MVP `glibre-foryc` parallel
  thunk emits a single `memcpy` to merge per-thread buckets;
  POD-ness is what makes that legal.
- **Fixed 32 bytes.** All five kinds inhabit the same shape. Padding
  fields are zeroed at write so two equally-shaped commands
  byte-compare equal across hosts (PHILOSOPHY §7 determinism).
- **Variable-sized payloads (component bytes for `AddComponent` /
  `SetComponent`) live in a parallel `byte_payload_` deque** keyed by
  `(payload_offset, payload_size)`. The payload bytes are copied at
  record time (§3.4) so the system body's local component value can
  go out of scope before flush. Copy size is bounded by the
  TypeRegistry's `size` for the named `TypeId` (SPEC §4.9 #3).

`payload_offset == 0` is the explicit sentinel "no payload"; the
payload deque reserves byte 0 as a never-used guard so this sentinel
is unambiguous.

### 3.3 Buffer lifetime

```text
Frame N timeline (per buffer):

Phase entry (Schedule::run_phase):
  arena.reset()                       ← drains last frame's records
  records_.clear() / byte_payload_.clear() / reserved_.clear()
  recorded_count_ = 0
  overflow_seen_  = false

System body (driver thread runs SystemFn):
  ctx.commands().spawn() / despawn() / add / remove / set
    → append Command + (optional) payload bytes
    → on cap exceeded: latch overflow_seen_, return std::unexpected{CommandBufferOverflow}

Phase exit (Schedule::run_phase, after every system in phase ran):
  for each system S in compiled_[p] (topological order):
    flush(S.commands(), world)        ← §3.6 deterministic apply
  // buffers are NOT cleared here; they are reused next frame, drained at next entry.

Phase 9 exit (per-frame transient pool drain):
  arena handle returns its slab to PerContextAllocator
  (perf-budget.md Allocator Rule #4 — transient arena exemption)
```

The arena is **never** retained across frames. Re-entry of the same
phase next frame begins with a fresh slab; the records vector and
payload deque are pmr-allocated against the arena and follow the
slab's lifetime.

### 3.4 Arena allocation strategy

Every `CommandBuffer` allocates exclusively through a per-system
`eastl::pmr::monotonic_buffer_resource` whose upstream is the
**per-frame transient arena** owned by `core` (`perf-budget.md`
Allocator Rule #4). The chain:

```
CommandBuffer.records_ ──┐
                         ├──→ pmr::monotonic_buffer (per-system slab)
CommandBuffer.byte_payload_ ─┤      ├── upstream: core's per-frame transient arena
CommandBuffer.reserved_  ──┘      ├── upstream: PerContextAllocator (ContextTag::core)
                                  └── ceiling: §9.2 per-system cap (1 KiB / 4 KiB / 16 KiB tiers)
```

Properties enforced:

1. **Single allocation per frame per buffer (steady-state).** The
   slab is sized to the buffer's tier (§9.2). The first append
   carves from the slab; subsequent appends are bump-allocations,
   no syscall, no allocator hit.
2. **Bounded.** Append fails with `CommandBufferOverflow` when the
   slab's high-water mark would exceed the tier cap (§3.5). The
   monotonic resource never grows — by design.
3. **No cross-buffer aliasing.** Each system gets its own slab. Two
   buffers cannot share underlying bytes. This is what makes the
   post-MVP per-thread parallel-record path safe without locks.
4. **Drained, not freed, between frames.** `arena.reset()` rewinds
   the slab to its base; the slab itself is retained (avoids a
   per-frame upstream allocation). The slab is freed only on
   `Schedule::unregister_system` or `Schedule::~Schedule`.

The per-frame transient arena exemption from `perf-budget.md` rule 4
is what lets the buffer's slabs sit outside the §9.3 `CommandBuffer
pool` 4 MiB residency cell — they count against the engine-wide
transient pool, not against the cell ceiling. The 4 MiB cell tracks
**per-buffer-vector overhead** (record vector capacity, payload deque
overhead, reserved-entity ledger) only.

### 3.5 Append (record) path

The hot path. Called from a system body, runs once per intended
mutation. Every append is `O(1)` amortized.

```cpp
// internal sketch — not in §5 public ABI.
Result<void>
CommandBuffer::append(detail::Command cmd, std::span<const std::byte> payload) noexcept {
    if (overflow_seen_) {
        return std::unexpected{
            glibre::Error{core::Error::CommandBufferOverflow, /* ctx */}};
    }

    const std::size_t need =
        sizeof(detail::Command) + payload.size_bytes();

    if (arena_.bytes_used() + need > arena_cap_bytes_) {
        overflow_seen_ = true;
        return std::unexpected{
            glibre::Error{core::Error::CommandBufferOverflow, /* ctx */}};
    }

    if (!payload.empty()) {
        cmd.payload_offset = static_cast<std::uint32_t>(byte_payload_.size());
        cmd.payload_size   = static_cast<std::uint32_t>(payload.size_bytes());
        byte_payload_.insert(byte_payload_.end(), payload.begin(), payload.end());
    }
    cmd.insertion_index = recorded_count_++;
    records_.push_back(cmd);
    return {};
}
```

Hot-path invariants:

- **No `World&` access during append.** The buffer never reads world
  state during recording — that is the read-during-write hazard the
  whole primitive exists to avoid (SPEC §4.8 invariant 3).
- **No allocator round-trip in steady state.** All vectors and the
  payload deque sit on the monotonic arena (§3.4). Bump-allocate or
  refuse.
- **No locking.** Each system's buffer is owned by exactly one thread
  (the loop driver thread in MVP; the system's executing worker
  post-MVP). No atomics, no fences in the append path.
- **Latching `overflow_seen_`.** Once an append is refused, every
  subsequent append on the same buffer in the same frame is also
  refused (cheaper than re-checking the cap, and forces the system
  to surface the failure rather than silently drop a tail of
  commands).

### 3.6 Apply (flush) path — deterministic merge

The cold-relative-to-record path (still per frame, runs once per
phase exit). The schedule drives apply; plugins do not call it.

Apply order, fixed by SPEC §4.8 invariant 2 (independently derived
from PHILOSOPHY §7 determinism):

1. **Outer key — lexicographic owning-system FQN.** All buffers in
   the phase being exited are sorted by their `owning_system_fqn`.
   Two systems with identical FQN are forbidden by `Schedule`
   (registration-time uniqueness check); the loader rejects two
   plugins claiming the same system FQN with `PluginNameCollision`.
2. **Inner key — `insertion_index` ascending.** Within one buffer,
   commands replay in append order. This is the index stamped at
   §3.5 append time.

In MVP, the schedule already runs systems in topological order and
each system's buffer is drained before the next system runs in the
*next* phase, but the **flush iteration itself** uses the lex-FQN
order, not topological order. This is deliberate:

- Topological order is a property of the access set, which can
  legitimately change across loads (a hot-reload that adds a system
  re-runs the topo sort). Determinism requires the apply order be
  **independent of ordering choices made elsewhere**.
- Lex-FQN is a pure function of the loaded plugin set — and the
  plugin set is captured by the ABI hash gate (`plugin-abi.md`).
  Same plugin set → same lex order, regardless of registration
  history.

**Apply algorithm** (per-phase exit):

```cpp
// core/src/world/command_buffer_pool.cpp — internal sketch.
Result<void>
CommandBufferPool::flush_phase(Phase p, World& world) noexcept {
    // Stable sort once per phase; cheap for MVP system counts (~tens).
    eastl::span<CommandBuffer*> buffers = pool_.buffers_in_phase(p);
    eastl::sort(buffers.begin(), buffers.end(),
        [](const CommandBuffer* a, const CommandBuffer* b) noexcept {
            return a->owning_system_fqn() < b->owning_system_fqn();
        });

    for (CommandBuffer* buf : buffers) {
        for (const detail::Command& cmd : buf->records_) {
            const auto r = apply_one(cmd, *buf, world);
            if (!r) {
                // §10: log once at the handling boundary, continue draining
                // (refusal of one command does not block siblings).
                glibre::log_error(r.error(), spdlog::level::warn);
            }
        }
        buf->reset_for_next_frame();   // §3.3 phase-entry rewind
    }
    return {};
}
```

`apply_one` switches on `Command::kind`:

- **`SpawnEmpty`**: the entity was already allocated at record time
  (see entity-id remapping below). Apply is a no-op archetype
  transition (the entity already sits in the empty archetype). This
  collapses the harmonius "reserve then insert" two-phase trick into
  one primitive: spawn returns a real `Entity` immediately.
- **`Despawn`**: `world.despawn(target)`. If `target` is already
  despawned (another system in the same phase did so), the call
  returns `EntityStale`; we **swallow `EntityStale` for despawn**
  (idempotent semantics) and otherwise propagate.
- **`AddComponent` / `SetComponent`**: read payload from
  `byte_payload_[payload_offset, payload_offset + payload_size)`,
  call `world.set_component(target, type_id, span)` (the typed
  surface lives plugin-side; ABI is byte-typed — see SPEC §5.5).
- **`RemoveComponent`**: `world.remove_component(target, type_id)`.

**Entity-id remapping for spawn-then-mutate.**

The hard case: a system spawns an entity and immediately records
component adds against it. The spawned entity must be a real,
nameable identity (not a placeholder index) so the second command
can reference it.

Solution (re-derived; harmonius uses a more elaborate
`Commands::spawn` returning an `EntityCommands` builder that defers
the entity-id allocation to flush time, which we reject as gratuitous
abstraction):

1. `CommandBuffer::spawn()` calls into a thin
   `World::reserve_entity_for_command_buffer()` API that immediately
   allocates a generational id from the entity allocator's free list
   AND stages the entity in the empty archetype atomically. The
   returned `Entity` is fully alive — it just has no components yet.
2. The caller may store the `Entity` in plain memory and reference
   it from subsequent `add_component` calls within the same buffer
   AND within other buffers in the same phase. Determinism is
   preserved because `World::reserve_entity_for_command_buffer` is
   serialized through the entity allocator's lock (single-threaded
   in MVP; CAS post-MVP) and produces a deterministic id sequence
   for a given (recording order × loaded-plugin set).
3. The buffer records `SpawnEmpty{target = E}` so the apply pass
   can post-condition-check `E` is alive (it is) and skip archetype
   work it does not need to do. The downstream `AddComponent{target =
   E, type_id = T}` apply rows do the real archetype migration.

This collapses harmonius's two-phase reservation (R-1.1.32 prose
"reserve an entity ID, immediately available for recording further
commands against ... not truly spawned until flush") to **one
primitive: the id is real, the components are deferred**. The user
visible behaviour is identical (queries do not see the entity's
components until apply); the internal mechanism is one allocator
hit, not two.

**Cross-buffer references.** Buffer A spawns `E`; buffer B in the
same phase references `E`. Determinism rule:

- A's `SpawnEmpty(E)` apply runs before B's `AddComponent(E, T)`
  apply iff A's owning-system FQN < B's lexicographically.
- If the order is reversed (B references `E` and B's FQN < A's),
  B's apply sees `world.is_alive(E) == true` (the entity allocator
  reserved it at record time, before any flush), but
  `world.has_component(E, T) == false` (A's component
  registrations have not applied yet because A flushes after B).
  This is the same observable state every host produces, so
  determinism holds — and is what matters.

**What is **not** supported in MVP:**

- "Within-buffer read of own deferred mutation" — explicitly forbidden
  by SPEC §4.8 invariant 3. A system reading the world after
  recording sees the **pre-record** state.
- "Record + apply in same phase" — apply is the phase-exit step;
  recording during apply is forbidden (the buffer pool detects this
  via a thread-local `is_applying_` flag and refuses the append with
  `CommandBufferOverflow`+"detail: recursive append during apply").

### 3.7 Why the per-system bucket shape

The harmonius design distinguishes "Commands" (a single buffer reused
across systems) from "ParallelCommandWriter" (per-thread segments
merged at flush). Glibre collapses these to **one primitive: per-
system buffer**. Justification:

- In MVP (single-threaded), per-system equals per-thread (the loop
  driver thread runs every system, but each system's
  `SystemContext::commands()` returns a different buffer).
- Post-MVP per-system parallelism (`SPIKE` #496) executes one system
  per worker; per-system buffer equals per-worker buffer for that
  scheduling slice. No re-sharding required.
- The `system_fqn` is already the determinism tiebreaker (SPEC §4.8
  invariant 2). Per-system bucketing makes that tiebreaker the
  outer merge key for free; harmonius's `u64 sort_key` becomes
  unnecessary plugin-author burden.

This is the Occam collapse referenced in SPEC §3.2: two harmonius
primitives (Commands + ParallelCommandWriter) become one glibre
primitive (`CommandBuffer`).

## 4. Public Surface

The §5.10 stub from `specs/core/SPEC.md` is authoritative. This
section restates it with per-method behaviour annotations.

### 4.1 `CommandBuffer` operations (locked from §5.10)

```cpp
// specs/core/SPEC.md §5.10 — public ABI; reproduced for cross-reference.
namespace glibre::core {

class CommandBuffer {
public:
    [[nodiscard]] Result<Entity> spawn() noexcept;
    [[nodiscard]] Result<void>   despawn(Entity e) noexcept;

    [[nodiscard]] Result<void>
    add_component(Entity e, TypeId t, std::span<const std::byte> bytes) noexcept;

    [[nodiscard]] Result<void>
    remove_component(Entity e, TypeId t) noexcept;

    [[nodiscard]] Result<void>
    set_component(Entity e, TypeId t, std::span<const std::byte> bytes) noexcept;

    // Schedule-only; not for plugin code.
    [[nodiscard]] Result<void> flush(World& world) noexcept;
    void                       clear() noexcept;

    [[nodiscard]] std::size_t  recorded_count() const noexcept;

protected:
    CommandBuffer() noexcept;
    ~CommandBuffer();
};

}  // namespace glibre::core
```

Per-method contract:

- **`spawn() -> Result<Entity>`** — calls
  `World::reserve_entity_for_command_buffer()` (an internal seam, not
  in the §5 public surface) which atomically allocates a generational
  id and stages the entity in the empty archetype. Records
  `SpawnEmpty{target = e}`. Returns the entity. Failure modes:
  `CommandBufferOverflow` (arena full), `OutOfBudget` (engine-wide
  allocator ceiling — propagated from `PerContextAllocator`).
- **`despawn(Entity e) -> Result<void>`** — records `Despawn{target =
  e}`. Validation deferred to apply (the entity may not yet exist
  if it was spawned by an earlier-FQN system in the same phase
  and despawn is recorded before apply runs). Failure mode:
  `CommandBufferOverflow`.
- **`add_component(...)`** / **`set_component(...)`** —
  `add_component` records `AddComponent{...}` (apply asserts the
  type is **not** yet on the entity); `set_component` records
  `SetComponent{...}` (apply tolerates either present or absent —
  the typed surface treats add+set as the same archetype migration
  in MVP per SPEC §5.5). Both copy the payload into
  `byte_payload_` synchronously (§3.4). Validation of `TypeId`
  membership happens at apply, not at record (the type registry is
  immutable-after-init per SPEC §4.9, so an unregistered type is
  always unregistered; record-time validation would just duplicate
  apply-time work). Failure modes: `CommandBufferOverflow`,
  `OutOfBudget`.
- **`remove_component(...)`** — records `RemoveComponent{...}`. Apply
  tolerates "type not present on entity" as idempotent (no error,
  no log). Failure mode: `CommandBufferOverflow`.
- **`flush(World&) -> Result<void>`** — schedule-internal; the
  `protected` constructor + the `Schedule` friendship means plugin
  code cannot call `flush` directly. Flushes are coordinated by
  `CommandBufferPool::flush_phase` (§3.6) which calls
  `flush(world)` per buffer in lex-FQN order. The public `flush`
  exists for the few unit tests that exercise apply correctness
  in isolation (§11.1); it is **idempotent** when the buffer is
  empty (no-op success) and **resettable** via a follow-up call
  to `clear()` if needed.
- **`clear()`** — resets the buffer to empty without flushing
  (`records_.clear()`, `byte_payload_.clear()`, `reserved_.clear()`,
  `overflow_seen_ = false`, `recorded_count_ = 0`). Used by the
  hot-reload barrier when refusing a swap discards in-flight
  recorded mutations (§8). Plugin code does not call `clear()`.
- **`recorded_count()`** — `O(1)` getter on the counter. Read-only,
  safe to call from any thread that owns the system body's
  execution slot.

### 4.2 `SystemContext::commands()` integration

```cpp
// SPEC §5.11 — per-system view.
namespace glibre::core {
class SystemContext {
public:
    [[nodiscard]] CommandBuffer& commands() noexcept;
    // ... world(), tick(), phase() ...
};
}  // namespace glibre::core
```

Plugin systems obtain their buffer via `ctx.commands()` once per
body and record into it for the duration of the body. The reference
is invalid after the body returns; cached pointers are dangling.

### 4.3 ABI surface

Public ABI: spans, opaque handles, `Result<T>`. Never `std::*` /
`eastl::*` containers across the boundary (PHILOSOPHY §11). The
record-and-payload internals are private; they never appear in the
public header.

The only types crossing the loader / plugin boundary in this
aggregate are the four already in §5: `Entity`, `TypeId`,
`std::span<const std::byte>`, `Result<T>`. The internal
`detail::Command` POD never leaves the dylib.

## 5. Hot / Cold Path Split

| Path | Trigger                                           | Frequency                            | Budget                                            |
|------|---------------------------------------------------|--------------------------------------|---------------------------------------------------|
| Hot  | `CommandBuffer::spawn` / `despawn` / `*_component`| Per system body, per intent          | <100 ns per append (§9.1)                         |
| Hot  | `CommandBuffer::recorded_count`                   | Diagnostic; rarely on critical path  | O(1)                                              |
| Cold | `CommandBufferPool::flush_phase`                  | Once per phase per frame             | <0.10 ms aggregate across all systems (§9.1)      |
| Cold | `CommandBuffer::clear`                            | Hot-reload refusal                   | O(1) — pointer rewinds, no allocator hit          |
| Cold | `CommandBuffer::flush(world)` (test-only)         | Unit-test fixtures                   | O(records); not in shipping path                  |

Hot-path invariants (record path):

- **Bump-allocate or refuse.** The slab never grows. Every append
  is a single comparison + `memcpy` (for payload) + `push_back`
  (POD record).
- **No World access.** The record path never reads / writes
  archetype storage. The only `World` interaction is the
  `World::reserve_entity_for_command_buffer` call inside `spawn()`,
  which hits the entity allocator (one CAS / lock, generationally
  ordered) — by design the only `World` syscall on the record path.
- **No locking.** Per-system ownership = single-thread access in
  MVP; per-worker post-MVP.
- **Refuse latching.** Once `overflow_seen_` is true, every append
  fails fast (no re-measurement of arena fill).

Cold-path invariants (apply path):

- **Lex-FQN sort once per phase exit.** O(K log K) for K systems in
  the phase; K bounded by ~tens in MVP. Sort is over pointers, no
  string copies.
- **In-buffer iteration is contiguous.** `records_` is a packed
  vector of 32-byte PODs; cache-friendly.
- **No allocations during apply.** Apply touches `World` only;
  archetype migration may allocate inside `World`, but the buffer
  itself is read-only during apply.

The split makes the `CommandBuffer` cell budget (§9.3 row "
`CommandBuffer pool` ~0.10 ms / 4 MiB") **proportional to mutation
count, not entity count**. A frame with zero mutations costs
zero time in apply (sort runs but the inner loops are empty).

## 6. Concurrency

The aggregate's concurrency model is locked by
`schedule-frame-design.md` §6 + `frame-phases.md` (single-threaded
MVP, per-system parallelism post-MVP via `SPIKE` #496). This
section transcribes the buffer's contribution.

### 6.1 MVP — single-threaded record, single-threaded apply

1. **One game-loop driver thread.** All system bodies run on it,
   serially in topological order within each phase. `CommandBuffer`
   appends therefore have a single writer; no atomics needed.
2. **Apply runs on the same thread** at phase exit, serially across
   buffers in lex-FQN order. No fences needed.
3. **Cross-thread access is forbidden.** Plugin systems may not
   spawn a worker that records into another system's buffer. The
   `CommandBuffer&` lifetime (`SystemContext::commands()` valid only
   for the body's duration) prevents this by construction; the type
   has no copy ctor and no thread-safe handle.

### 6.2 Post-MVP — per-system parallelism (lock-free record)

Locked by `SPIKE` #496 (`per-system-parallelism-seam-foryc-vs-
schedule`). The buffer's design is forward-compatible:

1. **Per-system buffer == per-worker bucket.** The same primitive
   used in MVP is the per-thread bucket post-MVP; no resharding,
   no merge step.
2. **Lock-free record.** Each worker writes to its own buffer.
   `arena_.bytes_used()` is read & written by exactly one thread;
   no atomic needed. The append path is unchanged from §3.5.
3. **Deterministic merge at apply phase.** The phase-exit fence
   (a single `std::atomic_thread_fence(memory_order_acq_rel)` in
   the parallel scheduler) makes every worker's writes visible to
   the apply thread. The apply thread (the loop driver) then runs
   the §3.6 lex-FQN sort + replay; result is byte-equal to the
   single-threaded MVP outcome, by construction.
4. **No sort keys at record time.** Harmonius's `u64 sort_key` per
   command is refused; the per-system bucket already encodes the
   outer key, and `insertion_index` already encodes the inner key.

The apply phase runs on the loop driver thread in both MVP and
post-MVP. Parallel apply is **not** considered in MVP; the
determinism of cross-buffer references (§3.6) is most easily
preserved by sequential apply, and apply's budget (§9.1) does not
demand parallelism.

### 6.3 Apply phase slot in the frame

The apply trigger point is the **phase-exit step of the system's
owning phase**, per `schedule-frame-design.md` §3.5 and SPEC §4.8
invariant 5. Concretely (transcribed from `frame-phases.md`):

| Phase #          | Owning context | Apply runs at end of phase iff... |
|------------------|----------------|-----------------------------------|
| 1 input          | platform       | any phase-1 system recorded into its buffer |
| 2 logic          | (deferred)     | likewise; deferred bodies are no-ops in MVP |
| 3 physics-fixed  | physics        | likewise                          |
| 4 animation      | (deferred)     | likewise                          |
| 5 transform      | core           | likewise                          |
| 6 cull-extract   | render         | likewise                          |
| 7 render-submit  | render         | render systems do **not** record buffer mutations into ECS structural changes (render is read-only over ECS post-extract); likewise the buffer is empty |
| 8 hot-reload     | core           | **assertion**: every buffer in the loaded set must be empty (drained) before the barrier executes its drain step; see §8 |
| 9 present        | platform       | likewise                          |

There is no "global sync point" — every phase exit is a sync point
for the systems registered in that phase. This is the load-bearing
property that makes per-system parallelism (post-MVP) compose with
deterministic apply: the parallel work is bracketed by a phase, the
apply is the phase's exit fence.

## 7. Persistence + ABI

### 7.1 Nothing crosses the ABI

The command buffer is **transient by contract**. It carries no
state across:

- frame boundaries (drained at phase-9 transient pool drain),
- plugin loads / unloads (the buffer is owned by `Schedule`, which
  the loader rebuilds; the buffer's contents at the moment of a
  load are flushed to the world by phase 9 of the same frame),
- hot-reload swaps (the barrier requires every buffer be drained
  pre-swap; see §8),
- process restarts.

There is therefore **no Fory schema, no `.fory` file, no entry in
`glibre-types.dylib`'s schema set**, and no contribution to
`glibre_types_abi_hash()`. The buffer's record format
(`detail::Command`) is private to `core`'s implementation; it never
crosses a dylib boundary.

This is the carrying-state-is-empty contract from SPEC §4.8: the
buffer is the smallest deterministic deferred-mutation primitive,
and its smallness includes "owns no surviving state".

### 7.2 No Fory schemas

Per `fory-codegen.md` §"What gets a schema" — Fory schemas exist
only for types that need on-disk persistence or cross-dylib
transport. `CommandBuffer` and `detail::Command` need neither.
Keeping them out of the middleman dylib also keeps the schema
churn rate low: changing the record's internal layout (e.g. adding
a flag bit in `pad1_`) does not bump `glibre_types_abi_hash()` and
therefore does not invalidate any loaded plugin.

### 7.3 Public ABI surface invariants

The §4.1 surface (`spawn` / `despawn` / `*_component` / `flush`
/ `clear` / `recorded_count`) is part of `glibre-core`'s C++ ABI
and is gated by `min_engine_version` per `plugin-abi.md` §"Versioning
Rules". Adding a new `kind` to `detail::Command` is internal
(no ABI bump); adding a new public method to `CommandBuffer` is
an additive C++ ABI change (`min_engine_version` minor-bump);
removing or renaming a public method is a major-version bump
(refused by every plugin built against the prior `min_engine_version`).

## 8. Hot-Reload

### 8.1 The drain assertion

`HotReloadBarrier::step` (SPEC §4.6 / §8.3-§8.6;
`hot-reload-barrier-design.md` §6.2) runs at phase 8. At that point,
every system body for the frame has already returned (phases 1-7
are complete) and **every command buffer must be drained** —
i.e., applied to the world by the phase exit of its owning phase
or, for buffers in phase 8 itself (none in MVP), explicitly cleared.

The barrier's debug-build assertion (`hot-reload-barrier-design.md`
§6.2 #3) reads:

```cpp
// Before Drain step entry:
GLIBRE_ASSERT(world.is_quiescent());
//             └─ implies: no live Query iterators,
//                no system body on stack,
//                AND no CommandBuffer mid-append (§4.8 invariant 1).
```

The "no CommandBuffer mid-append" sub-assertion is implemented as a
thread-local counter bumped by the schedule on system entry / exit
(`schedule-frame-design.md` §3.4) plus a buffer-pool sweep that
verifies every buffer's `records_` was applied (counted apply ops
== counted appends for the frame). In release builds the assertion
compiles out; the schedule-driven flush ordering is the contract,
the assertion is the verification.

### 8.2 What survives a swap (nothing)

Per §7.1, **no command-buffer state crosses a hot-reload swap**.
The barrier sequence (`hot-reload-protocol.md` §4):

1. **Drain (step 1)** — calls outgoing plugin's
   `glibre_plugin_drain`. The plugin must not record into any
   command buffer during drain. (The barrier holds exclusive access
   to the world; command buffers belong to systems, and no system
   runs during phase 8 — so drain has no buffer to record into. The
   drain function gets a `World&` directly, not a `SystemContext`.)
2. **Swap (step 2)** — `Schedule` is rebuilt. Old buffers (one per
   old system) are destroyed; new buffers (one per new system)
   are constructed. Buffer destruction is a single
   `arena_.release()` call returning the slab to the per-context
   allocator. No data migration.
3. **Migrate (step 3)** — touches archetype storage only; no
   buffer interaction.
4. **Resume (step 4)** — `glibre_plugin_register` rebuilds the new
   plugin's systems. Fresh buffers are allocated for the new
   systems; old buffers' slabs were already released in step 2.

If a refusal fires at any step, the barrier walks the rollback
discipline (`hot-reload-protocol.md` §"Failure & Rollback"): step
1 has no state to undo (drain is read-only over the world), step 2
rebuilds the prior `Schedule` (which re-allocates the prior
buffers, empty), step 3's migration arena is reset, step 4's
register is re-run for the prior plugin (idempotent per
`plugin-abi.md`).

The buffer's contribution to rollback is exactly: **be empty**.
Once the schedule is rebuilt, every buffer in the loaded set is
in its phase-entry initial state (§3.3); no rollback work specific
to the buffer is needed.

### 8.3 Drain protocol (cross-reference)

`hot-reload-barrier-design.md` is the authority for the drain
protocol. The buffer's three-clause contract with the barrier:

1. **Pre-barrier-entry**: every system in phases 1-7 of frame N
   has flushed its buffer at its phase exit (the schedule's
   responsibility). On entry to phase 8, the buffer pool's
   `is_quiescent()` returns true; the barrier's debug assertion
   does not fire.
2. **During barrier**: no system body runs; no buffer is appended
   to. The barrier may invoke `clear()` on a buffer if a refusal
   path needs to discard mid-flight reservations (an edge case:
   a phase-1 system spawned an entity that was reserved but the
   spawn-record was never flushed because the schedule rebuild
   destroyed the old buffer mid-flight — the entity allocator's
   reservation is rolled back via the entity allocator's
   own undo path, which is the entity allocator's responsibility,
   not the buffer's).
3. **Post-barrier-exit**: `Schedule::~Schedule` (or the rollback's
   re-construction) leaves the barrier with fresh empty buffers
   for the next frame. Phase 9 (present) sees buffers in the
   phase-entry initial state.

### 8.4 Refusal cases (cross-reference)

The buffer raises one `core::Error` arm:
`CommandBufferOverflow`. This arm is **not** a hot-reload arm;
it fires at record time (phases 1-7), is logged at `warn` per
SPEC §10.1, and the schedule continues. It does not cause a
barrier refusal.

If a buffer is in an overflow state at phase-8 entry (i.e., the
system that overflowed dropped a tail of its intended mutations),
the barrier still proceeds — overflow is a recorded-count vs
applied-count mismatch the buffer pool surfaces via a frame-stat
counter, not a barrier-refusal cause.

## 9. Performance

The buffer's contribution to per-frame budgets, locked against
`perf-budget.md` §"Per-Context Budget Table" + SPEC §9.3 (`core`
sub-budgets row "`CommandBuffer pool` ~0.10 ms / 4 MiB").

### 9.1 Per-frame (steady-state)

| Cell                                         | Budget                                           | Source                                                |
|----------------------------------------------|--------------------------------------------------|-------------------------------------------------------|
| Per-append cost (record path, hot)           | <100 ns                                          | bump-allocate + memcpy + push_back; no syscall        |
| Per-flush cost (single buffer, cold)         | ~0.5 µs / record + archetype-edit cost in `World`| `World::set_component` etc. dominate                 |
| Aggregate apply cost across all phases       | <0.10 ms                                         | SPEC §9.3 `CommandBuffer pool` cell                   |
| Phase-exit lex-FQN sort                      | <1 µs / phase                                    | K systems × 8B pointers; cache-resident               |
| Heap residency (bookkeeping)                 | <4 MiB                                           | SPEC §9.3 cell; arena slabs not counted (§3.4 rule 4) |

The 0.10 ms cell is **aggregate across every system in the frame**;
S1 fixture (`perf-budget.md` §"Justification") with ~2k entities
records an estimated <500 mutations/frame steady-state (most are
read-only systems), well inside the budget. The harmonius
benchmark (R-1.1.32a) of 100k commands/frame ≤ 1 ms is a stress
test, not a steady-state contract; glibre's MVP S1 is much smaller.

### 9.2 Per-system arena tier sizing

Each registered system's arena is allocated to a tier based on its
`SystemDesc.expected_command_density` hint (a new optional field
on `SystemDesc`, defaulting to `Tier::Small`). Tiers (locked here):

| Tier   | Arena cap | Records cap (approx, no payload) | Typical use                                           |
|--------|-----------|----------------------------------|-------------------------------------------------------|
| Small  | 1 KiB     | ~32 records                      | most systems; queries that occasionally spawn / despawn|
| Medium | 4 KiB     | ~128 records                     | gameplay logic spawning effects; AI decision systems  |
| Large  | 16 KiB    | ~512 records                     | bulk spawn events (level streaming, save load)        |

A system that exceeds its tier cap returns `CommandBufferOverflow`
and the operator can re-author with a higher tier (a manifest
declaration, no runtime negotiation). Tier choice is a SystemDesc
field — it does not require a manifest schema bump. The total
residency across MVP systems (~64 in the S1 fixture, mostly Small)
is <4 MiB, fitting the §9.3 cell.

The tiers are deliberately small: the design objective is "each
system records a handful of mutations per frame, not hundreds".
A system reaching the Large tier signals a code-smell (probably
a missing batch primitive); the tier ceiling is the pressure to
factor.

### 9.3 Apply-phase wall-time budget

Per `perf-budget.md` §"Pipelined Frame Timing", the `core` sim
budget is 0.40 ms; SPEC §9.3 splits that across the seven core
aggregates with `CommandBuffer pool` getting 0.10 ms (submit half,
because apply runs at phase exits across phases 1, 2, 3, 4, 5 sim
and phases 6, 7 submit). The submit-vs-sim split in §9.3 is a
documentation choice (the apply runs pretty evenly across the
frame); for budgeting purposes, the 0.10 ms is the **total apply
cost across all phases of one frame**.

`BENCHMARK_CELL` (SPEC §9.5) for the buffer:

```
core/command-buffer: per_system_arena_drain
  cpu_ceiling: 0.10 ms
  heap_ceiling: 4 MiB
  fixture: S1 (perf-budget.md §"Justification")
  asserts:
    1. aggregate apply across phases 1..9 <= 0.10 ms
    2. resident bookkeeping bytes <= 4 MiB
    3. arena slabs are not double-counted (transient pool exemption)
```

Per `perf-budget.md` §"CI Gate Spec" rule 1, CI fails on assertion
violation.

### 9.4 Heap residency

| Item                                          | Bytes                 |
|-----------------------------------------------|-----------------------|
| Per-buffer header (`CommandBuffer` object)    | ~96 B                 |
| `records_` vector (small-vector inline storage)| 64 B inline + heap   |
| `byte_payload_` deque overhead                | ~128 B                |
| `reserved_` vector (small-vector)             | 64 B inline + heap    |
| Pool index: `eastl::vector<CommandBuffer*>` per phase | 9 × ~64 B      |

For an MVP-scale 64-system loaded set: ~24 KiB of bookkeeping
across all buffers, leaving the bulk of the §9.3 4 MiB cell
unused (deliberate slack — the `Tier::Large` outliers may need
larger residency in concentrated scenarios).

## 10. Failure Modes

The §10.1 failure-mode table in SPEC is authoritative. The buffer
emits exactly one `core::Error` arm.

### 10.1 Buffer-emitted `core::Error` arms

| Arm                       | Trigger                                                                                                  | Recovery | Severity |
|---------------------------|----------------------------------------------------------------------------------------------------------|----------|----------|
| `CommandBufferOverflow`   | Append exceeds the per-system arena cap (§3.5). Latched on first refused append; subsequent fails free.  | Refuse   | `warn`   |

Operator action:

- Read the buffer's `recorded_count()` at the point of refusal.
- Inspect the system's `SystemDesc.expected_command_density` and
  bump the tier (Small → Medium, or Medium → Large).
- If already Large, factor the system to record fewer mutations
  per frame (e.g., emit one `BulkSpawn{count = N}` command via a
  not-yet-defined plugin extension, or split across frames).

### 10.2 Refused vs. dropped tail

When `CommandBufferOverflow` latches, the **tail of the system's
intended mutations is dropped**: the records made before overflow
are still applied at phase exit; the records that would have come
after are lost. This is deliberate (the smallest deterministic
choice) but visible: the system's per-frame mutation set is
truncated.

The frame-stat counter `core.command_buffer.overflow_count`
increments per overflow event; the editor's perf HUD surfaces it.
A non-zero count is a SPEC §10 violation in CI's strict mode
(asserting determinism preservation requires the system fit within
its declared tier). In shipping builds, the counter logs a
once-per-frame `warn` and continues.

### 10.3 What the buffer does NOT raise

Out-of-scope (raised by other aggregates and propagated through
the buffer):

- `EntityStale` — `World::despawn` raises this when a despawn
  apply hits an already-despawned entity. The buffer **swallows**
  `EntityStale` for despawn (idempotent semantics; §3.6) but
  propagates it for `add_component` / `remove_component` /
  `set_component` if the target is dead.
- `TypeUnregistered` — `World` raises this if `apply_one` calls
  `set_component(TypeId)` for an unregistered type. The buffer
  propagates.
- `OutOfBudget` — the per-context allocator raises this if the
  initial slab allocation (at `Schedule::compile`) would exceed
  `core`'s 64 MiB ceiling. The buffer surfaces it via
  `Schedule::compile`'s return value, not via any `CommandBuffer`
  method.

### 10.4 No abort cases

The buffer has no `Abort`-recovery rows. Every refusal is a
`Refuse` and leaves the world unchanged. This is the smallest
correctness contract: the buffer never terminates the process.

## 11. Test Plan

### 11.1 Unit tests (Catch2, `tests/core/command_buffer/`)

| Test name                                          | Covers                                       | Fixture                                      |
|----------------------------------------------------|----------------------------------------------|----------------------------------------------|
| `command_buffer.spawn_returns_alive_entity`        | §3.6 entity-id remapping                     | empty world                                  |
| `command_buffer.spawn_then_add_component_applies`  | §3.6 within-buffer reference                 | empty world                                  |
| `command_buffer.despawn_idempotent_under_double`   | §3.6 EntityStale-swallow                     | world with one entity                        |
| `command_buffer.add_component_payload_round_trip`  | §3.4 payload copy                            | TypeRegistry with one type                   |
| `command_buffer.remove_component_idempotent`       | §3.6 missing-type tolerance                  | world with one entity, no T                  |
| `command_buffer.set_component_overwrites`          | §3.6 set vs add semantics                    | world with one entity + T                    |
| `command_buffer.records_in_insertion_order`        | §3.6 inner key                               | record N then assert apply order             |
| `command_buffer.lex_fqn_outer_order`               | §3.6 outer key                               | two systems with deterministic-named bodies  |
| `command_buffer.overflow_returns_arm`              | §3.5, §10.1                                  | Tier::Small system recording 64 records      |
| `command_buffer.overflow_latches_subsequent_fails` | §3.5 latching                                | post-overflow append also refuses            |
| `command_buffer.overflow_does_not_drop_prefix`     | §10.2 prefix-applied                         | overflow at record N; assert N-1 applied     |
| `command_buffer.deterministic_apply_byte_equal`    | §3.6 cross-host determinism                  | golden snapshot via PHILOSOPHY §7 fixture    |
| `command_buffer.empty_flush_is_noop`               | §4.1 idempotence                             | empty buffer → flush → world unchanged       |
| `command_buffer.clear_after_record_resets`         | §4.1 clear semantics                         | record + clear + flush → no apply            |
| `command_buffer.recorded_count_matches`            | §4.1 counter accuracy                        | append N, assert recorded_count() == N       |
| `command_buffer.payload_offset_zero_is_sentinel`   | §3.2 sentinel encoding                       | record SpawnEmpty + AddComponent             |
| `command_buffer.command_record_is_pod_32_bytes`    | §3.2 invariant                               | static_assert + sizeof check                 |

### 11.2 Integration tests (Catch2, `tests/core/integration/`)

- `integration.command_buffer_phase_exit_flush` — register two
  systems in phase 5, both record mutations, run one frame; assert
  apply happens at phase-5 exit (queries inside phase 6 see the
  changes; queries inside phase 5 do not).
- `integration.cross_buffer_entity_reference` — system A spawns
  entity E and adds component T1; system B (same phase, lex-later
  FQN) adds T2 to E; assert E has both T1 and T2 after the phase.
- `integration.parallel_record_stress` (post-MVP guard, marked
  `[!benchmark][ci-skip]` until SPIKE #496 lands) — N systems
  record M mutations each on N workers; assert apply produces
  byte-equal world state across 100 runs.
- `integration.frame_stat_overflow_counter` — register a system
  whose body intentionally overflows; assert
  `core.command_buffer.overflow_count` increments by exactly one
  per overflowing frame.
- `integration.tier_sizing_residency` — load 64 systems with
  tier breakdown 32/24/8 (Small/Medium/Large); assert resident
  bookkeeping <4 MiB per §9.4.

### 11.3 Hot-reload integration tests (`tests/core/integration/hot_reload/`)

- `integration.hot_reload_drain_assertion_no_pending_buffer` —
  schedule a reload at frame N; in frame N's phase 1, a system
  records into its buffer; the buffer flushes at phase 1 exit
  (deterministic by §3.6); phase 8 entry's
  `world.is_quiescent()` assertion holds; reload proceeds.
- `integration.hot_reload_refusal_buffer_already_drained` —
  same setup, but `glibre_plugin_register` returns unexpected at
  step 4; rollback restores prior schedule; the prior plugin's
  buffers are re-created empty; next frame resumes recording.

### 11.4 Benchmark gates (Catch2 `BENCHMARK`, `tests/core/perf/`)

Per SPEC §9.5 + `perf-budget.md` §"CI Gate Spec":

- `bench.command_buffer.append_under_100ns` — single append on a
  Tier::Small buffer; assert <100 ns.
- `bench.command_buffer.aggregate_flush_under_0_10ms` — 64
  systems, S1 mutation density (~500 records/frame); assert
  apply phase aggregate <0.10 ms (the §9.5 `BENCHMARK_CELL`).
- `bench.command_buffer.lex_fqn_sort_overhead` — 64 buffers in
  one phase; sort cost <1 µs.

### 11.5 E2E coverage

E2E traces under `tests/e2e/core/command_buffer/`:

- `e2e.spawn_despawn_lifecycle` — golden trace driving the
  `[STORY] command-buffer-deferred-mutation` (#328) acceptance
  criterion: spawn-then-mutate behaviour byte-equal across runs.
- `e2e.deterministic_replay` — record traces over 600 frames
  with deliberate cross-buffer entity references; replay asserts
  byte-equal trace output (the PHILOSOPHY §7 obligation).

### 11.6 Coverage matrix

Every §10.1 row maps to at least one §11.1 unit test
(only `CommandBufferOverflow` here, mapped to three: returns,
latches, prefix-applied). Every §3.x invariant has at least one
test exercising it; every §4.1 method has at least one test
exercising the success and failure paths. The §11.4 benchmarks
back the §9.x perf cells.

## 12. Open Questions

- **[OPEN] `expected_command_density` field on `SystemDesc`** —
  this design adds the optional tier hint to `SystemDesc`. Locking
  the shape (enum vs `std::uint32_t` byte hint) is the first
  follow-up plan's call. The simplest is the `enum class Tier {
  Small, Medium, Large }` form; if a system author wants 32 KiB,
  the resolution is to factor the system, not to grow the enum.

- **[OPEN] Despawn idempotence vs error for cross-buffer races** —
  §3.6 specifies that `Despawn` apply swallows `EntityStale`
  (idempotent). An alternative is to **fail** the apply (the
  refused-tail semantics from §10.2) so an operator notices when
  two systems both intended to despawn the same entity. The
  idempotent choice is the smallest contract; revisit if a
  concrete user story emerges where the silent collision masks a
  bug. (Tracked alongside SPEC §12 open questions; not currently
  a blocker.)

- **[OPEN] Bulk-spawn primitive** — a system that needs to spawn
  hundreds of entities (level streaming) hits the `Tier::Large`
  ceiling fast. The right answer is probably a `BulkSpawn{count =
  N, archetype_hint = ...}` command kind that allocates ID space
  in one allocator hit; defer until the first concrete need
  (level-streaming spike). The current `kind` enum has a free
  slot for it.

- **[OPEN] Apply ordering of intra-buffer cross-entity
  references** — §3.6 says "in-buffer insertion order". A
  pathological case: buffer A despawns E1, then in the same buffer
  spawns E2 and adds a component referencing E1's archetype —
  apply runs A's records in order and the second record sees E1
  gone. The contract is correct (insertion order is deterministic);
  the question is whether tooling should warn on this pattern. The
  answer is "lint at codegen time when component bodies are
  generated"; tracked for the codegen plan, not this design.

- **[OPEN] Schedule's friend declaration of `CommandBuffer`** —
  `flush()` and `clear()` are public (`§4.1`) but only meant to
  be called by `Schedule` / the buffer pool / hot-reload-barrier
  test fixtures. The locked shape is "public method, callers
  outside the friend set will trip a `[[deprecated]]` warning by
  the implementation plan". A stricter alternative is `protected`
  + a `friend Schedule` declaration; pick during the plan that
  authors `core/src/world/command_buffer.cpp`.

- **[OPEN] Per-frame transient arena slab caching** — §3.4
  states slabs are retained across frames and rewound at phase
  entry. If a `Tier::Large` system records nothing for hundreds of
  frames, the 16 KiB slab is residency drift. Mitigation: shrink
  to Tier::Small after N idle frames. Cheap to add, no MVP user
  story demands it. Track with the perf-HUD plan when the
  residency counter surfaces.

- **[OPEN] Cross-world buffer routing** — multi-world support is
  refused in MVP (SPEC §3.3). When it lands (post-MVP), a buffer
  registered against world W1 must not be flushed against world
  W2. Current design has `flush(World&)` taking the world by
  reference, so the check is "the buffer's owning world == the
  passed world", a single pointer compare; the assert lives in
  `flush_phase`. Defer details to the multi-world spike.
