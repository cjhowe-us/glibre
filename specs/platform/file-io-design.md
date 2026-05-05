# File-IO Detailed Design

> Detailed design for the `platform` context's file-io aggregate
> (`FileIo` / `IoToken` / `OpenMode` / `Stat` / `DirEntry` / sandbox
> roots, SPEC §4.6).
> Refines §5.11 / §6.5 / §8.1 / §9.1 / §10.3.6 of `specs/platform/SPEC.md`
> and the engine-wide `reviews/decisions/{error-model, perf-budget,
> plugin-abi, hot-reload-protocol, frame-phases, fory-codegen}.md`
> records.
> All conclusions re-derived; harmonius prior art (`harmonius/docs/
> requirements/platform/filesystem.md`, R-14.6.1 .. R-14.6.11) cited as
> research input only.

Refs: spike #721 — `[SPIKE] design-platform-file-io-detailed`. Parent
#714. Sibling task-breakdown spike blocked-by this deliverable.

## 1. Purpose

`FileIo` is the single component in `glibre-platform` permitted to call
the OS file-IO syscalls (`open` / `read` / `pread` / `pwrite` / `fsync`
/ `rename` / `unlink` / `stat` / `readdir`) and to spawn the bounded
worker thread pool that backs the async surface. Its one responsibility
is **admitting a path-keyed byte transfer across the OS seam**: take a
`CanonicalPath` rooted under a registered sandbox root, perform a
synchronous read / write / stat / list / remove on the caller thread or
queue an async equivalent onto the bounded pool, and return either a
`Result<T>` or a single-consumer `IoToken`. On any refusal the file
system, the in-flight ledger, and the sandbox-root registry are left
byte-equal to the pre-call snapshot.

What the aggregate explicitly refuses to own:

- **Asset deserialization** — owned by `data` (`specs/data/SPEC.md`
  §4 / §7); `FileIo` returns inert byte spans, never typed
  aggregates. `data::deserialize<T>` calls `FileIo::read_all` /
  `read_async` and does its own decoding.
- **Shader cache hashing / reload** — owned by `shader`
  (`specs/shader/SPEC.md`); the shader context calls `FileIo`'s read
  surface and computes its own blake3 over the bytes returned.
- **Filesystem watching** — owned by `FileWatcher` (SPEC §4.3); the
  brief and #719 keep this entirely separate. `FileIo` has no
  subscription state, no debounce, no FSEvents stream. A consumer who
  wants change notifications subscribes to `FileWatcher` and reads
  through `FileIo`.
- **Path canonicalization beyond root-prefix admission** — the
  `CanonicalPath` value object is owned by `platform/detail/path/`
  (SPEC §6.1); `FileIo` takes already-canonical paths only (§4.6
  inv #1) and adds the additional sandbox-root precondition.
- **Asset-bundle / pak-file unpacking, content addressing, residency
  caches** — owned by `content` (per SPEC §1's refusal list).
- **Mid-frame I/O on the driver thread** — refused by §4.6 inv #2
  (`FileIo` never blocks the main thread); the synchronous primitives
  are off-main-thread-only and asserted in debug builds.

The aggregate's SRP boundary is sharp: if the OS file-IO syscall set
admitted, the `FileIoConfig` shape, the worker-pool topology, the
atomic-write protocol, the path-root admission rule, the per-token slot
discipline, or the queue-saturation refusal contract changes, this
design changes. Anything else is out of scope.

**`OpenMode` encoding.** `OpenMode` is declared in SPEC §5.11 and
named in the preamble above, but no method in this design's API takes
an `OpenMode` argument. The operation-named API encodes mode
implicitly: `read_all` / `read_async` imply read + sequential-access;
`write_atomic` / `write_atomic_async` imply create-or-replace + write.
The `OpenMode` enum is retained in SPEC §5.11 for future API
extensions (e.g. an `open_with_mode` surface allowing append or
exclusive-create). Its introduction before a second consumer requires
it is tagged in §12 [OPEN] #9.

## 2. Requirements Coverage

Mapping of harmonius requirements
(`R-14.6.*`, `harmonius/docs/requirements/platform/filesystem.md`) to
glibre MVP refusal-or-coverage. Every entry is independently
re-derived; harmonius's Tokio + Rust shape is rejected wholesale and
each underlying requirement re-evaluated against the C++23 / SDL3 /
POSIX seam.

| Harmonius req                       | Glibre disposition (MVP)            | Coverage site                                                                                   |
|-------------------------------------|-------------------------------------|-------------------------------------------------------------------------------------------------|
| R-14.6.1 async open/read/write, sequential + random-access, banning stdlib I/O | **Partial — sync + bounded async, no random offset (MVP)** | Sync (§4.1) + bounded async (§4.2) below; `pread` / `pwrite` deferred behind `read_all` / `write_atomic` until a second consumer needs offset I/O (§12 [OPEN] #1). `std::fstream` banned by §6.9. |
| R-14.6.2 async create / delete / recursive mkdir / deferred-to-close delete / batch delete | **Partial — single-shot remove + atomic write covers MVP**  | `FileIo::remove` (§4.1.5); `write_atomic` synthesizes "create or overwrite" (§4.1.2). Recursive mkdir, batch delete, deferred-to-close delete deferred (§12 [OPEN] #2); not on the MVP critical path (no peer context asks). |
| R-14.6.3 async stat (size, timestamps, permissions, type) + batch stat | **Partial — single-path stat covered, batch deferred**       | `FileIo::stat_path` returns the §5.11 `Stat` struct. Batch-stat deferred until `content`'s residency sweep needs it (§12 [OPEN] #3); MVP loops in caller. |
| R-14.6.4 async list_dir, incremental yield, depth limits, glob filtering | **Partial — flat list with caller-supplied buffer, no depth/glob** | `FileIo::list_dir(path, span<DirEntry>)` (§4.1.4). Incremental, depth, and glob deferred to the consumer (`tools` / `content`); §4.6 inv #5 keeps the aggregate's surface to one snapshot per call. |
| R-14.6.5 .. R-14.6.6, R-14.6.8, R-14.6.9 file watching, debounce, BLAKE3 dedup, typed event stream, hash cache | **Refused (routed to `FileWatcher`, SPEC §4.3 / #719)**                | Out of `FileIo`'s SRP. The watcher aggregate owns subscription, dedup, hash cache; `FileIo` is the byte-stream the watcher's consumers read through. |
| R-14.6.7 canonical-path resolution across platforms (relative, symlinks, junctions, drive letters, UNC, long paths, case rules) | **Covered (delegated to `CanonicalPath`)**                  | `CanonicalPath::from_absolute` (SPEC §5.2); `FileIo` enforces canonicalization-at-the-seam by accepting only `CanonicalPath` (§4.6 inv #1). This design adds the **sandbox-root admission** layer on top (§3.2). |
| R-14.6.10 ≥ 80 % of raw disk bandwidth                                    | **Covered (deferred budget)**                                          | Throughput budget (§9.1); CI gate fixture (§11.3) measures sustained sequential I/O against `dd`-equivalent baseline on macOS APFS. |
| R-14.6.11 Tokio-on-Linux + threaded fallback                              | **Refused (Linux is post-MVP)**                                        | macOS-first per PHILOSOPHY §1 / SPEC §1; libdispatch is not used either (§3.4 below). The bounded-pool design ports cleanly to Linux when that work lands. |

Coverage rule: every harmonius requirement above either lands in this
design or is refused with a one-line rationale. No silent drops.

Glibre-native requirements added beyond harmonius:

- **Sandbox-root admission** — every `FileIo` operation accepts only
  paths whose canonical form has one of the registered roots as a
  prefix. The brief calls these `project` / `user` / `cache`. This
  closes the implicit "any absolute path" hole the harmonius
  filesystem requirement left open and makes editor- / runtime-side
  jail discipline a property of the aggregate, not the caller. Detail
  in §3.2.
- **Atomic write protocol** — §4.6 inv #4 names this; this design
  pins the `<target>.tmp.<pid>.<rand>` + `fsync(file)` + `rename` +
  `fsync(parent_dir)` sequence as the only write protocol the
  aggregate exposes. There is no non-atomic public write API. Detail
  in §4.1.2.
- **Bounded async budget enforced as refusal, not back-pressure** —
  §4.6 inv #6 names the rule; this design pins the saturation
  semantics: once `io_thread_budget * queue_depth` slots are in
  flight, the next `read_async` / `write_atomic_async` returns
  `IoFailure { OsCode { ENOBUFS } }` with diagnostic prefix
  `"out-of-budget"` (§10.3.6 already specifies this). No queueing,
  no allocation, no callback — back off in the caller.
- **Single-consumer `IoToken` with two-phase abandon** — this design
  pins the slot lifetime: `~IoToken()` stamps "abandoned"; the worker
  delivers the result then returns the slot. A caller that drops
  tokens without polling exhausts the budget — by design.

## 3. Detailed Model

### 3.1 Aggregate composition

```text
FileIo (root, owned by platform)
├── PathRoots                  roots_       (sandbox-root registry; §3.2)
├── eastl::array<Worker, N>    workers_     (N = FileIoConfig.io_thread_budget; §3.4)
├── SpscRing<Request>          requests_    (engine → workers; bounded; §3.5)
├── eastl::array<Slot, M>      slots_       (M = N * queue_depth; §3.6)
├── SlotFreeList               free_slots_  (lock-free LIFO; §3.6)
├── ReadAllArena               read_arena_  (per-call buffer for sync read_all; §6.9)
└── PlatformAllocator&         alloc_       (sub-arena handle, ContextTag::platform)
```

The aggregate owns no OS file handles across calls — each operation
opens, reads/writes, closes within a single transaction. This is the
load-bearing simplification: there is no `OpenFile` entity, no
descriptor table, no fd lifetime management. POSIX file descriptors are
scoped to one `Request` or one synchronous call.

`FileIo` is owned by the platform plugin's startup composer; one
instance per process (sized by `FileIoConfig`).

### 3.2 `PathRoots` (sandbox-root registry, sub-aggregate)

The brief introduces three named roots: `project`, `user`, `cache`.
Each is a `CanonicalPath` registered at `FileIo::create` time; all
subsequent operations admit only paths whose canonical form has one of
the registered roots as a byte-prefix.

```cpp
// platform/src/fileio/roots.hpp — internal.
enum class RootId : std::uint8_t {
    Project = 0,   // game / editor working tree (read-mostly)
    User    = 1,   // per-user save / preferences (read-write)
    Cache   = 2,   // derived / regeneratable artifacts (read-write, prunable)
};

struct PathRoots {
    eastl::array<CanonicalPath, 3>  roots;   // dense; index = RootId
    eastl::array<bool, 3>           writable;// project=false, user=true, cache=true
};
```

Rules:

- **Roots are registered once at construction and frozen.** Adding /
  removing a root after `FileIo::create` is refused (the platform
  plugin's startup composer is the only caller of root registration).
  This is what makes root admission a `O(3)` byte-prefix scan with
  zero allocation per call.
- **`project` is read-only at runtime.** Every `write_atomic*` /
  `remove` against a path whose admitted root is `project` returns
  `PermissionDenied` with diagnostic prefix `"project-readonly"`. The
  editor's "save scene" path goes to `user/` or `cache/` per the
  `tools` SPEC, never overwrites the source tree. Tests, dev builds,
  and CI cooks may construct a `FileIo` with `project` writable
  (build-time flag, not API toggle); production runtime never.
- **Path admission is byte-prefix on the canonical form.** A path
  passes admission iff there exists a `RootId r` such that
  `canonical_path.view().starts_with(roots[r].view())` AND the next
  byte after the prefix is `'/'` or end-of-string (so `/User/foo`
  does not pass under root `/User/foobar`). The check is a tight loop
  over three roots; sub-microsecond.
- **Symlinks are resolved by `CanonicalPath::from_absolute` before
  the prefix check.** A symlink under `project/` pointing at
  `/etc/passwd` canonicalizes to `/etc/passwd` and fails admission.
  This is the sandbox's load-bearing rule: the prefix check happens
  on the canonicalized form, not on the lexical path.
- **No `..` / `.` in admitted paths.** `CanonicalPath::from_absolute`
  already rejects non-canonical input (SPEC §5.2); this design adds
  no separate normalization pass. If `from_absolute` accepted it,
  it is canonical.
- **Refusal arm.** Admission failure returns `PermissionDenied` with
  diagnostic prefix `"path-outside-root"`. The brief's
  `PathOutsideRoot` is the documented prefix; promoting it to a
  first-class §4.7 arm is gated by a second consumer needing typed
  dispatch (§12 [OPEN] #4 — same Occam's-razor logic as the
  `SurfaceLost` / `WatcherUnavailable` carve-outs).

The three names are compiled into the platform plugin's text segment
as the `RootId` enum; the `PathRoots` struct's `roots[]` array is
populated by the engine's startup composer from the `Process::cwd()` /
`Process::executable_path()` / `~/Library/Application Support/<game>` /
`~/Library/Caches/<game>` resolution table that the launcher hands
in. The aggregate does not hard-code the OS-specific user / cache
paths; it consumes whatever absolute paths the composer provides.

### 3.3 `IoToken` (entity)

```cpp
// platform/src/fileio/token.hpp — internal projection of §5.11 IoToken.
struct Slot {
    std::atomic<IoToken::State>          state;        // InFlight | Ready | Cancelled
    std::atomic<bool>                    abandoned;    // ~IoToken set this
    eastl::span<const std::byte>         result_bytes; // filled before state=Ready (release)
    Result<void>                         err;          // failure code if state=Ready and err is unexpected
    Request                              req;          // bound at enqueue, read by worker
    const char*                          prefix;       // async-path prefix transport (§3.9); nullptr = no prefix
    std::uint32_t                        next_free;    // intrusive free-list link
};

// IoToken::Impl is just &Slot; the public IoToken from §5.11 is a
// move-only handle that holds slot index + free-list backref.
// Note: the `prefix` field is written by the worker (after translator runs,
// before state=Ready release-store) and read by the consumer (after observing
// state==Ready acquire-load). The release/acquire on `state` provides the
// necessary ordering; no separate synchronization on `prefix` is needed.
```

Token lifetime invariants (extending §4.6 inv #3):

- **Single-consumer.** Only the holder of the `IoToken` may call
  `poll` / `wait_for` / `cancel` / `take_result`. Copying is forbidden;
  moving transfers ownership; dropping cancels the operation
  (best-effort) and stamps `abandoned`.
- **Two-phase abandon.** `~IoToken()` does NOT free the slot. It
  stamps `abandoned = true` (release) and calls `cancel()` (which
  stamps `state = Cancelled` if not yet `Ready`). The worker thread,
  on completion, observes `abandoned` and returns the slot to
  `free_slots_`. This is the only safe order: the worker may be
  mid-operation when the token destructor runs, and freeing the slot
  underneath an in-progress write would corrupt the next caller.
- **Slot count is bounded.** `M = N * queue_depth` (default
  `2 * 16 = 32` slots); abandoned-but-not-yet-released slots count
  against the budget. A caller that drops `IoToken`s at full rate
  exhausts the pool — this is by design; the saturation refusal arm
  (§10.3) tells the caller to back off.
- **`take_result` is single-shot.** First call after `state == Ready`
  returns the bytes (or the error); second call returns
  `AlreadyExists` per §5.11. The bytes' lifetime is the slot's
  lifetime, which the next `take_result` or `~IoToken` ends — callers
  that need to keep the bytes copy out before either.

### 3.4 Worker pool topology

The aggregate spins **`io_thread_budget` workers** (default 2, MVP
ceiling 8) at `FileIo::create`. Each worker is a `std::thread` named
`"glibre-fileio-N"` whose body is:

```text
worker_loop:
    while (!stopping):
        slot = requests_.pop_blocking()        # SPSC dequeue (§3.5)
        if slot.state == Cancelled:
            release(slot)
            continue
        do_work(slot)                          # POSIX call; §3.7
        slot.state = Ready (release)
        if slot.abandoned:
            release(slot)                      # §3.3 two-phase abandon
```

The pool is **plain `std::thread`**, not libdispatch / GCD. Reasoning:

- libdispatch's queue API is callback-shaped; §4.6 inv #7 bans
  callbacks crossing the boundary. Wrapping libdispatch into a
  poll-only surface re-introduces the kernel wait we deliberately
  avoid (§6.5 SPEC: `wait_for` uses sleep with backoff, not futex /
  mutex, to keep the slot one cache line and the API kernel-free).
- libdispatch on macOS has a system-wide concurrency budget the
  engine cannot constrain. Bounding our own worker count is what
  §4.6 inv #6 demands.
- Cross-platform: the same `std::thread` body runs on Linux and
  Windows when those ports land. libdispatch ports do not.
- macOS-26 / Apple Silicon `std::thread` is `pthread`-backed; spawn
  cost is amortized over the process lifetime (workers spin once at
  `FileIo::create`).

Pool sizing knob: `FileIoConfig::io_thread_budget` (SPEC §5.11). MVP
default of 2 covers single-asset reads + a streaming worker without
serializing. The 8-worker ceiling is a pragmatic upper bound (8 cores
on M1; more workers contend for the disk and inflate latency); larger
configurations refuse at `create` with `Unsupported`.

### 3.5 Request queue

The ring type depends on the worker count `N`.

**N == 1 (MVP default).** A bounded SPSC ring (`detail/spsc_ring.hpp`)
sized to `M = N * queue_depth` entries. One producer (engine driver
thread) and one consumer (worker 0). This is the same primitive used
by `event/` and `watcher/`. Worker 0 is both dispatcher and executor.
Saturation (`head == tail + capacity`) returns `IoFailure` with
diagnostic prefix `"out-of-budget"` (§10.3.6 / SPEC §4.6 inv #6) —
never grows.

```text
engine driver thread
        │ push(Request)          (one producer)
        ▼
   spsc_ring (M slots, detail/spsc_ring.hpp)
        │ pop(Request)           (one consumer)
        ▼
   worker[0]
```

**N > 1 (post-MVP, disabled for MVP).** The SPSC ring cannot serve
multiple consumers safely — N workers CAS'ing the same head pointer
would be a data race. For N > 1 the ring is replaced by a separate
MPSC ring (`detail/mpsc_ring.hpp`) with a per-consumer head index
and a single shared tail, allowing N consumers to pop without
contention on a single head. The `event/` / `watcher/` SPSC ring is
**not** shared on this path; `file-io`'s N > 1 ring is a distinct
instance with the wider consumer contract. The exact ring contract,
work-stealing protocol, and fairness guarantees for the N > 1 path
are captured in §12 [OPEN] #11.

```text
engine driver thread
        │ push(Request)          (one producer)
        ▼
   mpsc_ring (M slots, detail/mpsc_ring.hpp)
        │ pop(Request)           (N consumers, each with own head index)
        ▼
worker[0] worker[1] ... worker[N-1]
```

For MVP (`N = 2` default), only the SPSC ring is used (two workers
do not require MPSC because the dispatch model assigns one ring to
worker 0 and worker 1 blocks on a semaphore from worker 0 — the
full MPSC promotion is post-MVP). Saturation is computed the same
way regardless of ring type.

A request is a fixed-size POD:

```cpp
struct Request {
    enum class Op : std::uint8_t {
        ReadAll,
        WriteAtomic,
    };
    Op                            op;
    std::uint32_t                 slot_index;     // back-ref into slots_
    CanonicalPath                 path;           // string_view backed by interner arena
    eastl::span<const std::byte>  bytes_in;       // Write only; nullptr for Read
};
```

`Request::bytes_in` is **caller-owned** for the duration of the
operation. The caller MUST keep the buffer live until
`IoToken::poll() != InFlight` — the worker reads from the caller's
memory directly, no copy on enqueue. Dropping the buffer mid-write
is a contract violation; `~IoToken()`'s `cancel()` does NOT drain the
buffer requirement (the worker may still be reading). This is what
makes write enqueue allocation-free; callers who need a quick exit
copy the bytes into the `cache` root and pass that span (which lives
as long as the file).

### 3.6 Slot pool + free-list

`slots_` is a `eastl::array<Slot, M>` allocated once at `FileIo::create`
in the platform sub-arena (§9 below). `free_slots_` is a lock-free
LIFO populated at construction with every slot index `0..M`.

- `read_async` / `write_atomic_async` pop the head of `free_slots_`;
  empty → return `out-of-budget`.
- The worker, after delivering or observing abandoned, pushes the
  slot back. A treiber-stack with a versioned head pointer suffices
  for MVP ABA-safety; `eastl::atomic` wraps the CAS.
- `Slot::next_free` is the intrusive link; no separate free-list
  storage.

Slot count `M = N * queue_depth` with default `queue_depth = 16` →
`M = 32` slots at `N = 2`. `Slot` size is one cache line (64 B on M1):
`atomic<State>` (1 B) + `atomic<bool>` (1 B) + 6 B pad +
`span` (16 B) + `Result<void>` (~16 B) + `Request` (~24 B). Pool
footprint is `64 * 32 = 2 KiB` resident at default config; bounded by
the §9 8 MiB sub-arena.

### 3.7 Synchronous primitives — POSIX direct

The §5.11 `read_all` / `write_atomic` / `stat_path` / `list_dir` /
`remove` calls are thin POSIX wrappers, **off-main-thread asserted in
debug** (§4.6 inv #2 + §6.5 SPEC). Each call's body:

#### 3.7.1 `read_all(CanonicalPath p)`

1. Admit `p` against `roots_` (§3.2). Refusal →
   `PermissionDenied { "path-outside-root" }`.
2. `fd = open(p.view(), O_RDONLY | O_CLOEXEC)`. On `errno`, translate
   per §10.2.1 of SPEC; close `fd` if non-negative; return.
3. `fstat(fd, &st)`. On `EINTR` retry once; otherwise translate.
4. `read_arena_.allocate(st.st_size)` — recycled per call per §6.9 of
   SPEC. Out-of-arena → `Unsupported { "read-too-large" }` (the arena
   is sized at config time; a single `read_all` ≥ 5 MiB asks more
   than the arena can hold; the operator should use `read_async` or
   raise the arena ceiling at startup).
5. Loop `read(fd, buf+off, st.st_size-off)` until EOF or short-read
   accumulates; `EINTR` retried; partial read followed by EOF is a
   short file (returns the partial bytes; not an error).
6. `close(fd)`. Return the span.

#### 3.7.2 `write_atomic(CanonicalPath p, span bytes)`

1. Admit `p` against `roots_` (§3.2); writability check (§3.2 rules:
   project = read-only). Refusal → `PermissionDenied` with prefix
   `"path-outside-root"` or `"project-readonly"`.
2. Build temp path `t = p.view() + ".tmp." + pid + "." + rand`. The
   temp path is constructed on the worker's stack (no allocation; the
   buffer is `eastl::array<char, PATH_MAX>`).
3. `fd = open(t, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644)`. If
   `EEXIST` (vanishingly rare; another temp from same pid + rand),
   pick a new `rand` and retry once; second collision →
   `IoFailure { OsCode { EEXIST } }`.
4. Loop `write(fd, bytes+off, bytes.size-off)`; `EINTR` retried;
   short-write loops back. On `ENOSPC` → `IoFailure { OsCode { ENOSPC } }`,
   `unlink(t)`, `close(fd)`, abort.
5. `fsync(fd)` then `close(fd)`. On `fsync` failure → translate +
   unlink + abort.
6. `rename(t, p)` — the atomic-rename promise. On `errno` (e.g.
   `EXDEV` cross-device, `ENOENT` parent gone, `EROFS` read-only fs),
   translate, `unlink(t)`, abort.
7. `parent_fd = open(parent_dir(p), O_RDONLY | O_DIRECTORY |
   O_CLOEXEC)`; `fsync(parent_fd)`; `close(parent_fd)`. Parent fsync
   is what flushes the rename's directory entry to disk on APFS; per
   APFS docs the rename itself is atomic on a single volume but
   durability requires the parent fsync.

The atomic protocol's rationale (§4.6 inv #4): **callers that want
durability ask for it by name.** There is no non-atomic public write.
A caller that wants append-mode (e.g. log file) goes through the
`spdlog`-based logger, which owns its own file handles and is not a
`FileIo` consumer. This collapses two tempting requirements
(durability + low-latency-append) by routing the second to its own
specialist.

#### 3.7.3 `stat_path(CanonicalPath p) -> Stat`

Admit, `lstat(p, &st)`, translate. `Stat::is_symlink` is set from
`S_ISLNK(st.st_mode)`; we use `lstat` (not `stat`) so the symlink
itself is described, not its target. Callers that want the target
canonicalize the symlink's resolved form via `CanonicalPath::from_absolute`
and `stat_path` again.

#### 3.7.4 `list_dir(CanonicalPath p, span<DirEntry> out) -> size_t`

Admit, `opendir(p)`, loop `readdir_r`-equivalent (`readdir` on macOS
is thread-safe per POSIX-2008; we use it directly), fill `out` until
either dir is exhausted or `out.size()` written, `closedir`. Return
the count written. If the directory has more entries than `out` can
hold, the surplus is silently truncated; the caller is expected to
check `count == out.size()` and re-call with a larger buffer or
adopt a paginating wrapper. Glob filtering and recursion are caller-
side; this is one snapshot per call (§4.6 inv #5).

#### 3.7.5 `remove(CanonicalPath p)`

Admit, writability check, `unlink(p)` (or `rmdir` if `Stat::is_directory
== true`; resolved by an inline `lstat`). Translate `errno`.

### 3.8 Async primitives — bounded enqueue

`read_async(CanonicalPath p) -> Result<IoToken>` and
`write_atomic_async(CanonicalPath p, span bytes) -> Result<IoToken>`
share one body:

1. Admit `p`; on refusal, return the error **without** allocating a
   slot (cheap reject; refused paths do not consume budget).
2. For writes: writability check + caller-buffer-lifetime
   responsibility documented inline (§3.5).
3. Pop a slot from `free_slots_`. Empty →
   `IoFailure { OsCode { ENOBUFS } }` with diagnostic prefix
   `"out-of-budget"` (§10.3.6).
4. Build the `Request` directly into `slots_[i].req`; set
   `state = InFlight`, `abandoned = false`.
5. Push slot index onto `requests_` ring. The push is non-blocking
   and infallible because the ring has the same capacity as
   `free_slots_` — if we got a slot, the ring has room.
6. Return `IoToken{slot_index}`.

This makes `read_async` / `write_atomic_async` allocation-free on the
hot path. The only failure modes after admission are the two named
above (`out-of-budget` from slot pool exhaustion); ring saturation
cannot happen by construction.

### 3.9 Failure-translation seam

Every backend call funnels its error through the `detail/error/`
translator (SPEC §6.10): `posix.cpp` maps `errno` →
`platform::Error`. The mapping table (canonical, frozen at SPEC
§10.2.1):

| `errno`          | `platform::Error` arm  | Diagnostic prefix          |
|------------------|------------------------|----------------------------|
| `ENOENT`         | `NotFound`             | (none)                     |
| `EACCES`/`EPERM` | `PermissionDenied`     | (none)                     |
| `EEXIST`         | `AlreadyExists`        | (none)                     |
| `EINTR`          | `Interrupted`          | (none)                     |
| `ENOTSUP`/`EINVAL` | `Unsupported`        | (operation-specific)       |
| `ENOBUFS`        | `IoFailure`            | `"out-of-budget"`          |
| `ENOSPC`/`EIO`/  | `IoFailure`            | (none)                     |
| `EXDEV`/other    |                        |                            |

Sandbox-root admission failure (synthetic, not from `errno`):
`PermissionDenied` with prefix `"path-outside-root"`. Project
writability refusal: `PermissionDenied` with prefix
`"project-readonly"`. Read-arena overflow: `Unsupported` with prefix
`"read-too-large"`.

The diagnostic prefix is the §10.6 SPEC mechanism for telling adjacent
arms apart without growing the closed sum. Each prefix lives as a
`constinit const char*` literal in `fileio/error_prefixes.hpp` and is
checked via byte-equal comparison at the consumer (no allocation,
exactly the discipline §10.6 imposes for `"surface-lost"` /
`"watcher-unavailable"` / `"queue-full"`).

**Prefix transport by path:**

- **Sync path.** The translator runs on the caller thread immediately
  after the POSIX call returns. The TLS prefix (SPEC §3.3 of
  `platform-error-design.md`: `set_prefix` / `current_prefix` /
  `reset_prefix`) is written and read on the same thread before the
  `unexpected` is returned to the caller — no cross-thread transport
  needed.
- **Async path.** The worker thread runs the translator after
  `do_work(slot)` completes. Because the worker runs on its own
  thread, its TLS prefix is invisible to the consumer thread that
  reads `take_result()`. To bridge this: the `Slot` struct (§3.6)
  gains a `const char* prefix` field. After the translator runs and
  BEFORE the worker sets `state = Ready` (release), the worker
  captures `current_prefix()` into `slot.prefix`. The consumer reads
  `slot.prefix` (acquire-ordered by the `state` transition) and
  compares pointer-equal against the `prefix::file_io_*` literals
  from `fileio/error_prefixes.hpp`. This is safe because all prefixes
  are `constinit const char*` literals with program lifetime — pointer
  equality is valid across threads.

  See §12 [OPEN] #10 for the implementation gate on this transport.

## 4. Public Surface

The §5.11 stub from `specs/platform/SPEC.md` is authoritative. This
section restates it with the design's per-method behaviour annotations
and adds the `PathRoots` admission surface.

### 4.1 Per-method contract (synchronous)

Quoted from `specs/platform/platform.hpp`:

```cpp
class FileIo {
public:
    [[nodiscard]] static auto create(FileIoConfig = {}) noexcept -> Result<FileIo>;
    /* ... */

    [[nodiscard]] auto read_all(CanonicalPath) noexcept                                    -> Result<eastl::span<const std::byte>>;
    [[nodiscard]] auto write_atomic(CanonicalPath, eastl::span<const std::byte>) noexcept  -> Result<void>;
    [[nodiscard]] auto stat_path(CanonicalPath) noexcept                                   -> Result<Stat>;
    [[nodiscard]] auto list_dir(CanonicalPath, eastl::span<DirEntry> out) noexcept         -> Result<std::size_t>;
    [[nodiscard]] auto remove(CanonicalPath) noexcept                                      -> Result<void>;

    [[nodiscard]] auto read_async(CanonicalPath) noexcept                                       -> Result<IoToken>;
    [[nodiscard]] auto write_atomic_async(CanonicalPath, eastl::span<const std::byte>) noexcept -> Result<IoToken>;
};
```

Per-method contract:

- **`create(FileIoConfig)`** — allocates the platform sub-arena (§9),
  spins `io_thread_budget` workers (§3.4), constructs `slots_` (§3.6)
  and `requests_` (§3.5). The `FileIoConfig` is extended to also
  carry the three `CanonicalPath` roots (§3.2) — the public surface
  in SPEC §5.11 lists `io_thread_budget` only; this design adds
  three `CanonicalPath` fields without changing the constructor's
  return shape:

  ```cpp
  struct FileIoConfig {
      std::uint8_t   io_thread_budget{2};
      CanonicalPath  project_root{};
      CanonicalPath  user_root{};
      CanonicalPath  cache_root{};
      // Optional: read-arena ceiling and queue-depth knobs.
      std::uint32_t  read_arena_bytes{5 * 1024 * 1024};   // §9
      std::uint8_t   queue_depth{16};                     // §3.5
  };
  ```

  Default-constructed `CanonicalPath` is treated as "this root is
  not registered"; admission against an unregistered root always
  refuses with `path-outside-root`. The startup composer is
  expected to register at least `project` and `user`; `cache` is
  allowed to be absent (engine-only builds without a cache dir).
  **Refusal arms.** `Unsupported` (config invalid:
  `io_thread_budget == 0` or `> 8`, or all three roots are empty);
  `IoFailure` (worker thread spawn failed). Saturation surfaces as
  `core::Error::OutOfBudget` at the core seam, not as a
  `platform::Error` arm — see §10.2 for the routing.

- **`read_all(p)`** — synchronous read; off-main-thread asserted in
  debug (§4.6 inv #2). Flow per §3.7.1. Returns a borrowed span;
  lifetime is valid until the NEXT call to `read_all` ON THE SAME
  THREAD (§6.9 SPEC: the `read_arena_` is per-`FileIo` instance but
  recycled per call; concurrent `read_all` calls on different threads
  are not supported — the synchronous surface is off-main-thread-only
  and the arena is not designed for concurrent recycling). Callers
  that need persistence copy out. Async path workers allocate from
  per-worker arenas (`workerN_arena_`), not from the caller's
  `read_arena_`; async-completion buffers have lifetime governed by
  the consumer's `take_result()` reclaim per Slot, see §3.6.

- **`write_atomic(p, bytes)`** — synchronous atomic write per
  §3.7.2. Flow goes through the temp + rename + parent-fsync
  protocol; no other write API exists. Same off-main-thread
  assertion.

- **`stat_path(p) -> Stat`** — synchronous `lstat`; sandbox-admitted.

- **`list_dir(p, out) -> count`** — flat enumeration into the caller's
  span; does not recurse; does not glob. Caller-paginated.

- **`remove(p)`** — synchronous unlink/rmdir; sandbox-admitted +
  writability-checked.

- **`read_async(p) -> IoToken`** — flow per §3.8. Allocation-free
  past `create`; refuses on `out-of-budget`. The returned token's
  `take_result()` yields the bytes when `state == Ready`.

- **`write_atomic_async(p, bytes) -> IoToken`** — same flow; the
  caller-supplied span MUST outlive the token's terminal state.
  `take_result()` returns an empty span on success (writes have no
  body); the `Result<void>` wrap is encoded in the token's `err`
  field, surfaced via `take_result`. A success token returns
  `Result<eastl::span<const std::byte>>{}` with a zero-sized span;
  the §5.11 surface already returns `span<const byte>` from
  `take_result` so this is shape-stable.

### 4.2 `IoToken` operations (locked from §5.11)

```cpp
class IoToken {
public:
    [[nodiscard]] auto poll() noexcept                          -> State;
    [[nodiscard]] auto wait_for(Duration) noexcept              -> State;
    auto cancel() noexcept                                      -> void;
    [[nodiscard]] auto take_result() noexcept                   -> Result<eastl::span<const std::byte>>;
    /* destructor + move ctor / move assign per §5.11 */
};
```

- **`poll()`** — single relaxed `atomic<State>::load` followed by an
  acquire fence on the `Ready` transition (§6.5 SPEC). No syscall, no
  allocation, single cache-line read.
- **`wait_for(Duration)`** — `std::this_thread::sleep_for` with
  exponential backoff capped at the supplied duration (§6.5 SPEC: no
  futex / mutex / condvar). Returns `Ready` / `Cancelled` / `InFlight`
  (timeout). The deliberate avoidance of a kernel-wait keeps the slot
  one cache line and the API kernel-free.
- **`cancel()`** — flips slot to `Cancelled` (release). Best-effort:
  if the worker has already begun work, the operation runs to
  completion and the result is delivered as `Cancelled` rather than
  `Ready` (the caller's `take_result` then returns `Interrupted`).
- **`take_result()`** — first call after `Ready` returns the bytes
  (or the failure); calling before `Ready` returns `Interrupted`;
  calling twice returns `AlreadyExists`. Single-shot.
- **`~IoToken()`** — two-phase abandon (§3.3): stamp `abandoned`,
  call `cancel`. Slot is released by the worker on completion check.

### 4.3 Public ABI surface — POD spans only

Per PHILOSOPHY §11 + plugin-abi.md §"Surface rules", the public ABI
surface that crosses the plugin seam is POD-typed: bytes flow as
`eastl::span<const std::byte>`, paths as `CanonicalPath` (which is a
single `string_view` field — POD-equivalent), tokens as opaque
move-only handles holding a `Slot*`.

There is no `extern "C"` plugin entry-point owned by `FileIo` itself;
the aggregate is consumed by the platform plugin's own `register`
(which captures a reference into the plugin's per-system context). A
peer plugin reaches `FileIo` through `glibre::platform::FileIo&`
exposed via `PluginContext::platform()`.

`noexcept` on every public function. Every fallible call returns
`Result<T>`. No exceptions cross the boundary (§4.7 inv #2 SPEC); the
single Objective-C++ bridge is in `surface/` not `fileio/`, so there
is no `NSException` ingress here. POSIX errors fail through return
codes only.

## 5. Hot / Cold Path Split

`FileIo` is exclusively a **cold path** with respect to the engine's
per-frame budget: every per-frame access on the driver thread is
either zero (idle) or one SPSC dequeue (`IoToken::poll()` on a
completed token). Heavy work is off-thread.

| Path  | Trigger                                  | Frequency                                  | Budget                                                            |
|-------|------------------------------------------|--------------------------------------------|-------------------------------------------------------------------|
| Hot   | `IoToken::poll()` on driver thread        | Per pending completion (≤ tens / second)   | <0.001 ms / call (single relaxed atomic load + acquire fence)     |
| Hot   | No-token frame                            | Every frame                                | 0.000 ms (no work; the poll-call site is gated by token presence) |
| Cold  | `read_async` / `write_atomic_async` enqueue | Per asset stream / save                  | <0.005 ms / call (slot pop + ring push; allocation-free)          |
| Cold  | Synchronous `read_all` / `write_atomic`   | Boot / cook / tools                        | OS-bound; off-main-thread; not in steady-state frame budget       |
| Cold  | Worker `do_work(slot)`                    | Per request                                | OS-bound; on dedicated worker; never on driver thread             |

Hot-path invariants (the aggregate's contribution to the every-frame
budget, lining up with §9.1 of SPEC):

- The driver thread never runs a syscall through `FileIo`. The
  off-main-thread debug assert in `read_all` / `write_atomic` /
  `stat_path` / `list_dir` / `remove` enforces this; calling them
  from the steady-state frame is a SPEC violation (§9.1 SPEC).
- `IoToken::poll()` is one relaxed `atomic::load` + acquire fence on
  the slot's `state`. No memory traffic on idle frames (the caller's
  poll-call site is gated by a `token.has_value()` check that is
  itself a register read).
- The MPSC request ring's producer side (engine) is touched only at
  `read_async` / `write_atomic_async` — never on idle frames.

Cold-path invariants:

- `open` / `read` / `pwrite` / `fsync` / `rename` / `unlink` / `stat`
  / `readdir` are called only on cold paths (synchronous calls from
  off-main-thread, or async-worker call sites on dedicated workers).
- `dlopen` / Fory / SDL3 / metal-cpp are never reached from
  `fileio/` — the module's external-surface footprint is POSIX +
  blake3 (only inside `CanonicalPath` interner) + platform's
  internal sub-arena.
- The slot pool's free-list and the request ring's CAS ops happen on
  cold paths only; the driver thread's `read_async` / `poll` only do
  loads / one CAS pair per call, all bounded.

The cold/hot split is what makes the aggregate's §9.1 cell entry
("0.000 ms (sim) + 0.000 ms (submit)") honest: the per-frame driver-
thread cost is the `poll` of one cache line per pending completion,
which sits in the §9.1 reserved tail. Plugin- / asset-count growth
inflates the cold-path duration linearly; the hot path stays
sub-microsecond.

## 6. Concurrency

The aggregate runs across **three thread classes**:

1. The **driver thread** (engine main thread): calls `read_async` /
   `write_atomic_async` / `IoToken::poll` / `IoToken::cancel` /
   `IoToken::take_result` / `~IoToken`. Never calls the synchronous
   primitives (asserted in debug).
2. **Off-main task threads** (tools / cook / boot): call the
   synchronous primitives. Asserted **not** to be the driver thread
   via the TLS sentinel from `pthread_self()` (§6.5 SPEC).
3. The **worker pool** (`io_thread_budget` threads, default 2): owns
   `do_work(slot)`. Never observable to peer plugins; lifetime is
   `FileIo`'s.

### 6.1 Threading rules

1. **Synchronous primitives are off-main-thread only.** Debug builds
   assert `pthread_self() != main_thread_sentinel`. Release builds
   skip the assert (the violation is documented as undefined behavior
   for budget purposes; §9 of SPEC pins the contract in the perf
   budget).

2. **Async primitives are driver-thread-callable.** `read_async` /
   `write_atomic_async` are designed for the driver thread (asset
   streaming kicks off a frame's reads from `cull-extract`, polls
   completions next frame). Allocation-free past `create`; bounded
   by `out-of-budget` refusal.

3. **`IoToken` is single-consumer.** No two threads may call any
   token method concurrently. Move semantics enforce this at the
   type system; passing a token through an MPSC channel is allowed
   (the channel serializes), but two threads reading the same token
   is a contract violation (no diagnostic; the slot's atomic state
   is correct under any single-reader access pattern).

4. **The slot's `state` transition is the synchronization point.**
   Worker writes `state = Ready` (release) AFTER writing
   `result_bytes`; reader observes `state == Ready` (acquire) BEFORE
   reading `result_bytes`. Standard release/acquire publishing.

5. **The slot's `abandoned` flag is one-shot.** Set by
   `~IoToken()` (release). Worker reads on completion (acquire);
   only one transition direction (false → true), no ABA.

6. **No mutexes inside `fileio/`.** The free-list (§3.6) is a
   treiber-stack; for N == 1 the request ring is `detail/spsc_ring.hpp`
   (same primitive as `event/` / `watcher/`); for N > 1 the ring is
   a separate `detail/mpsc_ring.hpp` instance (§3.5); the worker
   dispatch is per-worker atomic head indices.

### 6.2 Hot-reload barrier interaction

`FileIo` is a peer aggregate within the platform plugin; phase 8
behaviour follows SPEC §8.1 / §8.3:

- **Peer plugin reload**: `FileIo` is opaque-pass-through. In-flight
  tokens held by the peer plugin remain valid across the peer's
  swap because `IoToken::Impl` holds a slot pointer that is owned by
  `FileIo`, not the peer plugin. The peer plugin's `glibre_plugin_drain`
  is responsible for either (a) draining its tokens to terminal state
  before drain returns, or (b) handing them off to a middleman-typed
  ECS component so the post-swap `register` finds them. (b) is
  uncommon; (a) is the normal path. Tokens dropped during drain go
  through the two-phase abandon (§3.3) — the worker continues to run
  and frees the slot itself.

- **Platform self-reload** (§8.3 SPEC): `FileIo`'s contribution to
  `glibre_plugin_drain` is to **drain every in-flight `IoToken` to
  its terminal state** by joining the worker pool and tearing it
  down. The `FileIoSurvival` middleman type (§8.5 SPEC carries
  `FileIoConfig` as the only surviving bytes) lets `Q::register`
  re-spin the pool with the same budget. No `IoToken` survives
  platform self-reload; consumers re-issue requests against the
  re-spun pool. (This matches §8.1 SPEC: "no surviving plugin-side
  bytes; OS file handles are scoped to single tokens.")

The chosen drain policy is **drain-to-terminal**, not refuse-on-
in-flight, for two reasons:

1. **Refusal would race with normal completion.** A token's worker
   may finish during the same phase 8 the loader runs in;
   distinguishing "in flight" from "just completed" requires an
   acquire-load-then-recheck loop that adds no value. Joining the
   worker pool synchronously is simpler and correct.

2. **Bounded cost.** `io_thread_budget * queue_depth = 32` slots in
   flight max; each `do_work` finishes in at most one OS read /
   write (bounded by file size; the synchronous primitives' budget
   covers this). Worst-case drain time is one POSIX `read` of the
   largest in-flight file; for MVP-scale files (≤16 MiB scenes,
   ≤2 MiB shaders) this is sub-millisecond on APFS. Bounded fits
   inside the 0.40 ms hot-reload-frame budget (perf-budget.md
   "hot-reload frame").

3. **Cancellation is lossy without drain.** `IoToken::cancel`
   stamps `Cancelled`, but if the worker has already begun, the
   operation runs to completion. Drain-to-terminal makes the post-
   swap state observable (every token is `Ready` or `Cancelled`,
   never `InFlight`); refuse-on-in-flight leaves `InFlight` tokens
   visible to the post-swap world, which is harder to reason about.

If a future profile shows drain time blowing the 0.40 ms budget on
a typical reload (e.g. 16 MiB scene reads in flight), the policy
escalates to the §8.4 P1-style refusal: "mid-frame I/O drop —
retry next frame". The refusal arm `core::Error::HotReloadRefused`
already exists (hot-reload-protocol.md §Refusal Cases); the platform
maps it from `glibre_plugin_drain`'s `unexpected(Unsupported)`
return per SPEC §8.4. **MVP picks drain-to-terminal**; the refusal
escalation is a §12 [OPEN] item gated on observed timing.

### 6.3 Determinism

The aggregate is **non-deterministic by construction**. File reads
return whatever the disk has; writes are visible to subsequent reads
on the same path (same volume, same fs); rename ordering is the OS's.
Determinism is the consumer's responsibility — the `data` context's
golden round-trips snapshot the bytes that go in, not the OS path
that produced them.

This is consistent with `frame-phases.md` rationale: phase-1 (input)
captures non-deterministic OS state into the deterministic ECS; the
simulation half (phases 2–5) is byte-equal because its inputs are
already in the world. `FileIo` reads happen off the simulation path
(boot, cook, hot-reload, async streaming), so non-determinism in the
file system does not leak into world state until a consumer copies
bytes in — and that copy is the consumer's seam, not `FileIo`'s.

## 7. Persistence + ABI

### 7.1 Nothing serialized

`FileIo` ships **zero `.fory` schemas**. Per SPEC §7.1 "platform's net
contribution to `host_glibre_types_abi_hash` remains zero", the
aggregate's surfaces are purely in-memory:

- `CanonicalPath` is a `string_view` over an interner arena — no
  on-disk representation; the canonical form is recomputed at
  `from_absolute` time.
- `Stat` / `DirEntry` are point-in-time value snapshots — no
  persistence.
- `IoToken` / `IoToken::State` / `OpenMode` are in-process
  enumerations.
- `FileIoConfig` is configuration; if the platform plugin ever needs
  to survive its own swap with config preserved (§8.5 SPEC names
  `FileIoSurvival` for this), the schema lives under
  `data/schemas/platform/` and is added to the middleman; today
  this is a forward declaration only (§8.5 SPEC closing paragraph).

The aggregate's bytes that cross dylib boundaries are POD spans of
`std::byte` returned from `read_all` / `take_result`. No struct
layout, no enum value, no token field is part of any persistent
schema. Schema evolution in the `data` / `content` consumers is
their problem; `FileIo` is contract-stable as long as the §5.11 / §3
surface is stable.

### 7.2 ABI hash contribution

`FileIo`'s contribution to `glibre_types_abi_hash()` is **zero**.
The aggregate compiles into the platform plugin's `.dylib`; its types
do not appear in `glibre-types.dylib`. A change to `FileIoConfig`'s
shape, the `Slot` struct, the `Request` struct, or any internal type
does NOT bump the hash — these are platform-plugin-private and rebuilt
when the platform plugin rebuilds.

The two cross-plugin types `FileIo` does touch — `CanonicalPath` and
`eastl::span<const std::byte>` — are POD-equivalents whose layout is
governed by their owning context (`platform/detail/path/` for
`CanonicalPath`, EASTL upstream for `span`). Both are part of the
public surface SPEC §5.11 declares, and both are stable across this
design's evolution.

### 7.3 ABI stability rules (transcribed from PHILOSOPHY §11)

1. Public ABI surfaces never expose `std::` containers or `eastl::`
   containers — only POD spans / handles / `string_view`. `FileIo`
   complies: `eastl::span<const std::byte>` and `CanonicalPath`
   (single-`string_view` POD) are the only types that cross.
2. `noexcept` on every public function. Every fallible function
   returns `Result<T>`.
3. The platform plugin's loader behaviour is governed by
   `plugin-abi.md` §"Loader Sequence"; `FileIo`'s public types are
   loaded as part of the platform plugin's `.dylib` text segment,
   not as a separate ABI seam.

## 8. Hot-Reload Integration

The aggregate integrates with `HotReloadBarrier` (SPEC §8 +
hot-reload-protocol.md) in two roles, both already named in the
parent SPEC and refined here:

### 8.1 Peer-plugin reload (passive)

When some peer plugin (e.g. `render`, `physics`) reloads, the
platform plugin (and `FileIo` within it) is the **outgoing-side
opaque pass-through** described in SPEC §8.1. The peer plugin's
`drain → swap → migrate → resume` sequence does not touch
`FileIo`'s state — `slots_`, `requests_`, `workers_` all continue
running. The peer plugin's outgoing code's `IoToken` instances
either:

- (a) are drained to terminal state by the peer's own `glibre_plugin_drain`
  (typical pattern; the peer awaits its outstanding `IoToken`s
  before returning from drain), or
- (b) are abandoned in the peer's destructors, in which case the
  two-phase abandon (§3.3) kicks in: the slot stays allocated until
  the worker delivers, then is released. Slot leak is bounded by
  `io_thread_budget * queue_depth` and self-heals.

`FileIo` makes **no contribution** to peer-plugin reload. The §8.1
SPEC table "Survives peer-plugin swap? Yes — pass-through" stands.

### 8.2 Platform self-reload (active drain)

When the platform `.dylib` itself is the outgoing plugin (SPEC §8.3),
`FileIo`'s contribution to `glibre_plugin_drain` is in step 2 of the
SPEC's drain sequence: **"drain every in-flight `IoToken` to its
terminal state"**.

Drain procedure (deterministic; runs on the loader thread during
phase 8):

1. **Stop accepting new requests.** Set `requests_.closed = true`
   (an atomic bool checked at every `read_async` / `write_atomic_async`
   entry); subsequent calls return
   `IoFailure { OsCode { ECONNRESET } }` with diagnostic prefix
   `"shutting-down"`.
2. **Wake every worker.** Each worker is in either
   (a) `requests_.pop_blocking()` — woken by the closure flag's
   release-store + a stub request that flips its check; (b)
   mid-`do_work(slot)` — runs to completion.
3. **Join every worker thread.** `workers_[i].join()` in order;
   each join takes at most one in-flight POSIX call's duration.
4. **Cancel any still-`InFlight` slots.** After joins return, every
   slot is in `Ready` or `Cancelled`. Walk `slots_[]` once and stamp
   any straggler to `Cancelled` (defensive; no slot should still be
   `InFlight` after worker join).
5. **Capture `FileIoConfig` into the `FileIoSurvival` middleman
   type** (SPEC §8.5 #4). `roots_` is captured byte-for-byte (three
   `CanonicalPath` strings); `io_thread_budget`, `queue_depth`,
   `read_arena_bytes` captured as scalars.
6. **Release `slots_`, `requests_`, `read_arena_`** back to the
   platform sub-arena. The sub-arena itself survives (it is owned by
   the loader's allocator infrastructure, not by `FileIo`).
7. Return `Result<void>{}` from the drain.

Per the perf-budget.md "hot-reload frame" budget: drain time ≤ 0.40 ms.
Step 3 (worker join) is the only bounded-but-non-zero contributor;
under MVP-scale workloads (≤ 32 in-flight slots, ≤ 16 MiB largest
file) the join completes in ~50 µs on APFS — well inside the budget.

If the budget is blown in profiling — e.g. an experimental "stream
50 MB texture into cache" pattern lands and creates a 200 µs read in
flight at every reload — the escalation is the §8.4 SPEC P1-style
refusal: `glibre_plugin_drain` returns
`unexpected(Unsupported { "io-drain-overrun" })`; the loader maps to
`core::Error::HotReloadRefused`; the operator retries on the next
frame boundary. The escalation is gated on observed timing (§12
[OPEN] #5).

`Q::glibre_plugin_register` then re-spins:

1. Read `FileIoSurvival` from the middleman.
2. Call `FileIo::create(config_from_survival)`. Workers spin, slots
   allocate, ring constructs.
3. Re-publish `FileIo&` through `PluginContext::platform()` for peer
   consumers to re-acquire.

No `IoToken` survives platform self-reload (per SPEC §8.1 row); peer
consumers re-issue any in-flight requests against the new `FileIo`
instance after the `HotReloadCompleted` event fires.

### 8.3 Refusal cases (cross-reference)

The full §10.3.6 table from `specs/platform/SPEC.md` is the closed
enumeration of `FileIo` refusals. The hot-reload-relevant subset:

- `IoFailure { OsCode { ECONNRESET } }` with prefix `"shutting-down"`
  — emitted during platform self-reload after step 1 of drain. Peer
  callers receive this on `read_async` / `write_atomic_async` if they
  attempt a request after drain has begun. The recovery contract is
  "wait for the `HotReloadCompleted` event; re-issue against the new
  `FileIo&`". This is logged at `info` (not `warn`); it is an
  expected transient state, not a failure.
- `core::Error::HotReloadRefused` (cause: `Unsupported { "io-drain-overrun" }`)
  — emitted only when the drain budget is blown (§12 [OPEN] #5).
  Falls under the umbrella `HotReload` arm; logged at `warn` per
  hot-reload-protocol.md.

Both refusals preserve the prior platform plugin's `FileIo` as live.
This is PHILOSOPHY §9 + §8 jointly: refuse on overrun + only swap
at frame boundary = stale-but-working over half-loaded.

## 9. Performance

The aggregate's contribution to per-frame and per-load budgets,
locked against perf-budget.md "platform" row + SPEC §9.1.

### 9.1 Per-frame (steady-state, hot path)

| Cell                     | Budget        | Source                                     |
|--------------------------|---------------|--------------------------------------------|
| `platform` CPU sim        | 0.20 ms total | perf-budget.md row `platform`              |
|   of which `FileIo` driver-thread | 0.000 ms steady-state | SPEC §9.1 "FileIo" row     |
|   absorbed into reserved tail | up to 0.001 ms / pending poll | SPEC §9.1 reserved tail |
| `platform` CPU submit     | 0.05 ms       | perf-budget.md row `platform`              |
|   of which `FileIo`       | 0.000 ms      | no submit-phase work                       |
| `platform` heap (FileIo's share) | 8 MiB / 16 MiB | SPEC §9.2 sub-arena table          |

The driver-thread cost on idle frames is **zero**: the engine's
asset-streaming layer wraps `IoToken::poll` in a `if (token)` check
that is a register read. Frames with a pending completion incur one
relaxed atomic load + one acquire fence + one branch — measured at
~3 ns on M1 firestorm; absorbed into the 0.099 ms reserved tail of
the SPEC §9.1 table.

### 9.2 Per-call cost (cold path, off-main-thread)

| Step                              | Budget         | Notes                                                              |
|-----------------------------------|----------------|--------------------------------------------------------------------|
| Sandbox-root admission            | <0.001 ms      | Three `string_view` byte-prefix compares; SIMD-bounded.            |
| `read_all` (1 MiB file, APFS)     | ~1.5 ms        | OS-bound; one `open` + one `read` + one `close`; fits the 80% disk-bandwidth target (R-14.6.10). |
| `write_atomic` (1 MiB file, APFS) | ~3 ms          | OS-bound; temp + write + fsync + rename + parent-fsync; durability cost is the second `fsync`. |
| `stat_path`                       | <0.05 ms       | `lstat` on warm cache.                                             |
| `list_dir` (256 entries)          | <0.5 ms        | `opendir` + 256 × `readdir` + `closedir`; OS-bound.                |
| `remove`                          | <0.05 ms       | `unlink` / `rmdir`.                                                |
| `read_async` enqueue              | <0.005 ms      | Slot pop + ring push; no syscall, no allocation.                   |
| `write_atomic_async` enqueue      | <0.005 ms      | Same shape as `read_async`.                                        |
| `IoToken::poll`                   | <0.001 ms      | One relaxed atomic load + acquire fence.                           |
| `IoToken::wait_for(d)`            | bounded by `d` | Sleep with exponential backoff; not a kernel wait.                 |
| Worker `do_work(slot)` (read)     | OS-bound       | Same as `read_all` body.                                           |
| Worker `do_work(slot)` (write)    | OS-bound       | Same as `write_atomic` body.                                       |

### 9.3 Throughput target (R-14.6.10)

The harmonius requirement R-14.6.10 demands ≥ 80 % of raw disk
bandwidth. APFS sequential-read throughput on M1 is ~3 GB/s (NVMe
SSD); 80 % is ~2.4 GB/s. The bounded-pool design hits this on
sequential reads ≥ 1 MiB because:

- `read_all` is one syscall after `open`; no Tokio-equivalent
  scheduling overhead.
- `read_async` adds one ring enqueue + one worker `do_work` body;
  the worker's `read` is the same syscall.
- The 16 KiB syscall-buffer-equivalent (POSIX's internal granule on
  APFS) is not exposed; reads return the whole file as one span.

The CI gate fixture (§11.3) measures sustained sequential I/O against
`dd if=/dev/zero of=/path bs=1m count=1024`-equivalent baseline on the
macos-26-m1 runner. PR fails if sustained throughput drops below
80 % of the baseline measured the same session.

### 9.4 Per-context cell impact

`FileIo`'s heap accounting against the SPEC §9.2 8 MiB `FileIo`
sub-arena (transcribed verbatim — this design adds no new sub-arena):

- `slots_` × M: each ~64 B (one cache line). `M = 32` at default
  config → 2 KiB resident.
- `requests_` ring: same M slots × `sizeof(Request) ≈ 24 B` = 768 B.
- `read_arena_`: 5 MiB ceiling (recycled per `read_all` call per
  §6.9 SPEC). The single largest sub-arena consumer.
- Worker thread stacks: 2 × 256 KiB = 512 KiB (default macOS
  pthread stack).
- `roots_` storage: three `CanonicalPath` strings interned in the
  platform path arena — typically <1 KiB total.
- Slack: ~2 MiB inside the 8 MiB ceiling.

Total resident: ~5.7 MiB at default config, well inside the 8 MiB
sub-arena and 16 MiB platform cell. Increasing `io_thread_budget` to
the ceiling (8) consumes 8 × 256 KiB = 2 MiB more stack + 4 × the
slot pool (≈ 8 KiB); still inside the cell.

## 10. Failure Modes

The §10.3.6 failure-mode table in `specs/platform/SPEC.md` is
authoritative. This section enumerates the **file-io-aggregate-specific**
rows and adds per-arm operator-action notes. The brief's named arms
(`PathOutsideRoot`, `NotFound`, `PermissionDenied`, `IoError`,
`AsyncQueueFull`) map to the existing closed sum + diagnostic
prefixes (§4.7 closed-sum invariant); no new arms are added.

### 10.1 Aggregate-emitted `platform::Error` arms

| Brief name           | Surface arm + prefix                                | Step (§3)   | Recovery   | Operator action                                                                                  |
|----------------------|-----------------------------------------------------|-------------|------------|--------------------------------------------------------------------------------------------------|
| `PathOutsideRoot`    | `PermissionDenied` + `"path-outside-root"`          | 3.7.1–3.7.5 admit | Refuse | The path is not under any registered root. Caller must use a path under `project` / `user` / `cache`. |
| (project read-only)  | `PermissionDenied` + `"project-readonly"`           | 3.7.2 / 3.7.5 | Refuse  | Production runtime cannot write into `project/`. Save into `user/` or `cache/`.                   |
| `NotFound`           | `NotFound` (no prefix)                              | 3.7.* admit + ENOENT | Refuse | Path does not exist. Caller may stat-then-decide, or rely on `write_atomic` semantics.   |
| `PermissionDenied`   | `PermissionDenied` (no prefix)                      | 3.7.* + EACCES/EPERM | Refuse | OS denied; check filesystem ACLs / TCC grants on macOS.                                  |
| `IoError`            | `IoFailure` (no prefix) + `OsCode { errno }`        | 3.7.* + other errno | Refuse | OS-level error; raw `errno` in the OsCode payload. Operator reads the code.              |
| `AsyncQueueFull`     | `IoFailure` + `"out-of-budget"` + `OsCode { ENOBUFS }` | 3.8 / 3.6 | Refuse  | Slot pool exhausted. Caller backs off; either drain in-flight tokens or batch fewer requests. |
| (read arena overflow)| `Unsupported` + `"read-too-large"`                  | 3.7.1 step 4 | Refuse  | File exceeds `read_arena_bytes`. Use `read_async` (worker has its own buffer) or raise the arena ceiling at startup. |
| (interrupt)          | `Interrupted` (no prefix)                           | 3.7.* + EINTR retried | Refuse | Signal arrived mid-call; routine on SIGINT. Caller may retry. (Once-retry is internal.)  |
| (drain in progress)  | `IoFailure` + `"shutting-down"` + `OsCode { ECONNRESET }` | 3.8 / 8.2 step 1 | Refuse  | Platform self-reload is draining. Caller waits for `HotReloadCompleted`; re-issues then. |
| (config invalid)     | `Unsupported` (no prefix)                           | `create`    | Refuse     | `io_thread_budget == 0`, `> 8`, or no roots registered.                                          |
| `OsCode`             | `OsCode` (raw fallback)                             | translator-miss | Refuse | The translator did not recognize the `errno`. File a bug; the table grows.                    |

Refuse vs Rollback distinction (per SPEC §10.4 / §10.5 patterns):

- **Refuse only.** Every `FileIo` arm is a refusal; the file system
  is left byte-equal (synchronous calls do not start the operation
  if admission fails; async calls do not allocate a slot if admission
  fails; ring saturation does not consume budget).
- The lone semi-rollback case is `write_atomic` mid-operation
  failure (e.g. `ENOSPC` after partial write): the temp file is
  unlinked before returning, so the target path is unchanged. This is
  not "rollback" in the registry-mutation sense — it is the protocol
  itself preserving atomicity.

### 10.2 What `FileIo` does NOT raise

Out-of-scope (raised by other aggregates and propagated through
`FileIo` callers):

- `core::Error::OutOfBudget` — raised by the per-context allocator
  if the `platform` cell is exhausted at `FileIo::create`.
  Propagated; not constructed inside `fileio/`.
- `core::Error::HotReloadRefused` — raised by the loader if drain
  overrun triggers the §12 [OPEN] #5 refusal. The platform plugin's
  `glibre_plugin_drain` returns `unexpected(Unsupported)` and the
  loader wraps it.
- `data::Error` / `content::Error` / `shader::Error` — raised by
  consumers parsing the bytes `FileIo` returns. The byte stream is
  inert past the seam.

### 10.3 Logging severity (consolidated, transcribed from SPEC §10.6)

| Arm                                    | Default severity | Notes                                                |
|----------------------------------------|------------------|------------------------------------------------------|
| `NotFound`                             | `info`           | Caller's domain may escalate                         |
| `PermissionDenied` no prefix           | `error`          | Almost always operator-actionable                    |
| `PermissionDenied` `"path-outside-root"` | `error`        | Programmer error or sandbox breach attempt           |
| `PermissionDenied` `"project-readonly"`| `warn`           | Caller bug in production; CI promotes to error       |
| `AlreadyExists`                        | `warn`           | Routine on `take_result` double-call; CI gates       |
| `Interrupted`                          | `debug`          | Routine on SIGINT                                    |
| `Unsupported` no prefix                | `warn`           | Capability / config gap signal                       |
| `Unsupported` `"read-too-large"`       | `warn`           | Caller-actionable: switch to async or raise ceiling  |
| `Unsupported` `"io-drain-overrun"`     | `warn`           | Hot-reload backpressure; retry next frame            |
| `IoFailure` `"out-of-budget"`          | `warn`           | I/O pool saturation; backpressure signal             |
| `IoFailure` `"shutting-down"`          | `info`           | Expected transient during platform self-reload       |
| `IoFailure` other                      | `error`          | Default for unclassified I/O failure                 |
| `OsCode`                               | `error`          | Refuse to silence the unclassified                   |

Logging is the *handler's* responsibility per error-model.md
§"Logging / Telemetry" rule 1; `FileIo` constructs the typed arm +
TLS-buffered prefix and returns. The engine-wide log helper does the
formatting and dispatches to `spdlog`.

### 10.4 Abort cases (process termination)

Reserved for `FileIo` contract violations the design's invariants
cannot tolerate (mirrors SPEC §4.6 inv set):

- A worker thread observes `slot.state == InFlight` AND
  `slot.abandoned == true` AND fails to release the slot after
  `do_work` returns. This means the two-phase abandon discipline
  itself is broken; the slot leak is unbounded. Emit
  `glibre::log_error(err, error)` and `std::terminate`.
- `pthread_create` returns non-zero at `FileIo::create` AFTER the
  first worker started successfully (partial pool spawn). Roll back
  by joining started workers; if join fails, terminate. Pure
  partial-pool failure is a `Result<void>` `Unsupported` return at
  `create` time.

These mirror the plugin-loader's "abort on contract violation" cases
(plugin-loader-design.md §10.3): silent recovery would mask
determinism-snapshot bugs and hot-reload integrity bugs.

## 11. Test Plan

### 11.1 Unit tests (Catch2, `engine/platform/test/fileio/`)

Each row of the §10.1 failure-mode table maps to one or more unit
tests. The harness uses **fixture-built sandbox roots** under
`tmp/` per test (deterministic; `tmp/test-NNN/{project,user,cache}`
populated with known files):

| Test name                                          | Drives arm / behavior              | Fixture                                                    |
|----------------------------------------------------|------------------------------------|------------------------------------------------------------|
| `roots.admission_byte_prefix`                      | `path-outside-root`                | Path one byte off project root → refuses.                  |
| `roots.admission_symlink_to_outside`               | `path-outside-root`                | Symlink under project pointing at `/etc/passwd`.           |
| `roots.admission_dot_dot_rejected_by_canonical`    | `path-outside-root`                | `from_absolute("/proj/../etc/passwd")` rejected upstream.  |
| `roots.project_readonly_in_production`             | `project-readonly`                 | `write_atomic` into project root → refuses.                |
| `roots.user_writable`                              | (positive)                         | `write_atomic` into user root succeeds.                    |
| `roots.cache_writable`                             | (positive)                         | `write_atomic` into cache root succeeds.                   |
| `path.normalization_dot_segments_rejected`         | (positive — at canonical seam)      | `CanonicalPath::from_absolute` rejects `.` / `..`.         |
| `path.normalization_trailing_slash_rejected`       | (positive)                         | Same; trailing `/` not canonical.                          |
| `path.normalization_double_slash_rejected`         | (positive)                         | `//foo/bar` not canonical.                                  |
| `sync.read_all_returns_full_bytes`                 | (positive)                         | Read 1 MiB known file; assert byte-equal.                   |
| `sync.read_all_short_file_no_error`                | (positive)                         | 13-byte file; assert returned span has 13 bytes.            |
| `sync.read_all_arena_overflow_returns_arm`         | `read-too-large`                   | File > 5 MiB; assert `Unsupported`.                         |
| `sync.read_all_not_found_returns_arm`              | `NotFound`                         | Missing file under user root.                               |
| `sync.read_all_permission_denied_returns_arm`      | `PermissionDenied`                 | `chmod 000` file under user root.                           |
| `sync.read_all_off_main_assert_in_debug`           | (debug-only)                       | Call from main thread → `assert` fires.                     |
| `sync.write_atomic_overwrites_existing`            | (positive)                         | Overwrite existing file; assert new bytes, no temp residue. |
| `sync.write_atomic_creates_new`                    | (positive)                         | Write to non-existent path; assert exists.                  |
| `sync.write_atomic_temp_cleanup_on_failure`        | (positive — atomicity)              | Inject `ENOSPC` mid-write; assert temp file unlinked.       |
| `sync.write_atomic_parent_fsync_called`            | (positive — durability)             | Strace-equivalent; assert `fsync` on parent dir issued.     |
| `sync.stat_path_returns_size_and_mtime`            | (positive)                         | Known file; assert `Stat` fields.                           |
| `sync.list_dir_truncates_on_small_buffer`          | (positive)                         | 100-entry dir, 32-slot span; assert count == 32 and entries are first 32 in OS order. |
| `sync.remove_unlinks_file`                         | (positive)                         | Existing file → `remove` → `stat_path` returns `NotFound`.  |
| `async.read_async_round_trip`                      | (positive)                         | `read_async` → poll loop → `take_result`; bytes match.     |
| `async.write_atomic_async_round_trip`              | (positive)                         | Async write → poll → assert file on disk byte-equal.       |
| `async.token_poll_no_alloc`                        | (positive — perf)                   | 10 000 polls; assert tagged-allocator counter == 0.        |
| `async.token_take_result_twice_returns_arm`        | `AlreadyExists`                    | Two-shot `take_result`; second returns `AlreadyExists`.    |
| `async.token_take_result_before_ready_returns_arm` | `Interrupted`                      | Poll says `InFlight`; `take_result` → `Interrupted`.       |
| `async.queue_saturation_returns_arm`               | `out-of-budget`                    | Saturate `M = 32` slots; 33rd `read_async` → `IoFailure` w/ prefix. |
| `async.cancel_before_worker_starts`                | (positive — cancellation)           | `cancel` before dequeue; assert `Cancelled` state.          |
| `async.cancel_during_work_runs_to_completion`      | (positive — best-effort cancel)     | `cancel` mid-work; assert worker delivers `Ready` (per §4.6 inv #3). |
| `async.token_two_phase_abandon`                    | (positive — slot lifecycle)         | Drop token mid-work; assert worker releases slot.          |
| `async.token_drop_at_full_pool_recovers`           | (positive)                         | Saturate, drop all tokens, assert pool re-fills after worker drains. |
| `pool.worker_count_zero_refuses_at_create`         | `Unsupported`                      | `FileIoConfig.io_thread_budget = 0` → `create` returns `Unsupported`. |
| `pool.worker_count_above_ceiling_refuses`          | `Unsupported`                      | `io_thread_budget = 9` → `Unsupported`.                    |
| `error.errno_translation_table`                    | (positive — translation)            | Force each `errno` row of §3.9 table; assert exact arm + prefix. |

### 11.2 Integration tests (Catch2, `engine/platform/test/integration/`)

Drives the full failure-mode + lifecycle coverage:

- `integration.fileio_lifecycle` — `create` → 16 in-flight async
  reads → drain via natural completion → re-issue → `destroy`;
  assert all complete cleanly, slot pool returns to full free count.
- `integration.fileio_full_failure_table` — drive every arm in §10.1
  in sequence; assert prior tokens remain valid after each refusal.
- `integration.sync_off_main_thread` — spawn a worker thread, run
  `read_all` / `write_atomic` / `stat_path` / `list_dir` / `remove`
  in sequence on the same path; assert bytes round-trip.
- `integration.async_streaming_pattern` — emulate asset streaming: 8
  concurrent `read_async` of 1 MiB files, poll on driver thread for 30
  frames, assert all complete with byte-equal payloads.
- `integration.platform_self_reload_drain` — start 16 in-flight
  reads, trigger platform self-reload via the e2e hook, assert (a)
  drain completes within 0.40 ms, (b) `FileIoSurvival` carries
  matching config to the rebuilt instance, (c) post-reload the new
  pool starts at full budget and accepts new requests.
- `integration.peer_plugin_reload_pass_through` — start a `read_async`
  from peer plugin P, reload P, assert the worker delivers and the
  slot is released cleanly via two-phase abandon.

### 11.3 E2E + performance microbenchmarks

E2E traces (`tests/e2e/platform/fileio/`):

- `bad-root` (path admission refusal) — golden `.glibre-trace`
  records the refusal arm + prefix.
- `saturation` (drives `out-of-budget`) — golden records the arm at
  the 33rd request.
- `self-reload-drain` — golden records `HotReloadCompleted` event
  and post-reload throughput resumes.

Catch2 `BENCHMARK` blocks (`engine/platform/test/perf/fileio_bench.cpp`):

- `bench.read_all_1mib_apfs` — asserts ≥ 80 % of `dd`-baseline
  throughput (R-14.6.10).
- `bench.write_atomic_1mib_apfs` — asserts the durability cost
  (parent fsync) is ≤ 1.5x the `dd`-baseline write time.
- `bench.read_async_enqueue` — asserts <0.005 ms (§9.2).
- `bench.token_poll_idle` — asserts <0.001 ms (§9.2).
- `bench.platform_self_reload_drain` — asserts ≤ 0.40 ms with 32
  slots in flight, ≤ 16 MiB files (§9.4).

CI gates per perf-budget.md §"CI Gate Spec" #1 and SPEC §9.4: any
benchmark exceeding its budget fails the PR. The throughput baseline
`dd`-equivalent is computed in the same CI session and stored as a
session-local reference; cross-runner drift is absorbed into the
gate's tolerance band.

## 12. Open Questions

- **[OPEN] Random-access `pread` / `pwrite` (R-14.6.1 partial)**:
  the MVP surface offers whole-file `read_all` and whole-file
  `write_atomic` only. Adding offset I/O is a one-method addition
  (`pread(path, offset, span<byte> out) -> Result<size_t>`); the
  internal worker can call `pread64` directly. Defer until the
  first peer context (likely `content`'s streaming residency cache)
  asks. Track in `task-breakdown-platform-file-io-detailed`.

- **[OPEN] Recursive mkdir / batch delete / deferred-to-close
  delete (R-14.6.2 partial)**: the MVP surface offers single-shot
  `remove`. Recursive directory creation is implicit in
  `write_atomic`'s parent-existence requirement (the temp + rename
  protocol fails if the parent does not exist). Whether to add an
  explicit `mkdir_p` API or require the caller to walk parents is
  the open question; lean toward `mkdir_p` once the editor's "save
  scene to a fresh subdir" use case lands. Same gate as random-
  access I/O.

- **[OPEN] Batch stat (R-14.6.3 partial)**: a `stat_paths(span<CanonicalPath>,
  span<Stat> out)` could amortize one open + close per call into a
  single batched syscall sequence. Worth it only if `content`'s
  residency-sweep profile shows N × `stat_path` is a hotspot. Defer
  to that profile.

- **[OPEN] `PathOutsideRoot` first-class arm**: the brief names
  `PathOutsideRoot` as an error; this design implements it as
  `PermissionDenied` + `"path-outside-root"` prefix per §4.7
  Occam's-razor closed-sum invariant. Promotion to a typed arm is
  gated on a second consumer needing typed dispatch (mirrors
  `SurfaceLost` / `WatcherUnavailable` carve-outs in SPEC §10.4 /
  §10.5). Frozen until that consumer appears.

- **[OPEN] Hot-reload drain overrun escalation (§6.2 / §8.2)**: MVP
  picks drain-to-terminal and absorbs worker-join cost into the
  0.40 ms hot-reload-frame budget. If profiling shows the budget is
  blown under a real workload (e.g. 16 MiB scene reads in flight at
  every reload), the escalation is the §8.4 P1-style refusal:
  `glibre_plugin_drain` returns `Unsupported { "io-drain-overrun" }`,
  loader maps to `core::Error::HotReloadRefused`. Track the timing
  in the platform CI gate; reopen if a regression appears.

- **[OPEN] `wait_for` backoff vs futex**: `IoToken::wait_for` uses
  `std::this_thread::sleep_for` with exponential backoff (§6.5
  SPEC). If a profile shows the backoff is wasting CPU (e.g. tools'
  cook job calls `wait_for(5s)` with a tight backoff), promote to a
  futex / `std::atomic::wait` for the long-wait case. Decision
  deferred to the first observed profile that justifies the
  complexity; the slot-fits-one-cache-line invariant must survive
  any change.

- **[OPEN] `read_arena` ownership for async path**: `read_async`
  workers need their own buffers (the synchronous `read_arena_` is
  driver-thread-only-recycled, so worker reads must not share it).
  Workers allocate from per-worker arenas (`workerN_arena_`) as
  described in §4.1 (`read_all`) and §3.6 (Slot reclaim). The
  natural choice is one arena per worker of size
  `read_arena_bytes / N`, but this couples slot size to read size
  in a way the synchronous arena does not. The first implementation
  PR picks the explicit policy; this design permits any policy that
  respects the §9 8 MiB sub-arena ceiling. Per-worker arena
  ownership stays strictly within the worker — the consumer's
  `take_result()` reclaim (Slot → free-list) is the hand-off point.

- **[OPEN] `list_dir` glob / recursion**: the MVP surface accepts a
  flat single-directory enumeration. The editor's content-tree
  refresh is the likely first consumer to want both glob and depth
  limits. Defer the API extension to the editor / content seam
  spike that re-derives `FileWatcher`'s subscription discipline;
  same gate.

- **[OPEN] Linux / Windows port semantics**: this design is
  POSIX-direct. Windows has no `fsync(parent_dir)` equivalent
  (`FlushFileBuffers` is per-handle); `MoveFileEx` provides the
  rename-replacing-existing semantics, but the durability story
  differs. Out of MVP per PHILOSOPHY §1; the §3 surface is
  host-agnostic, only the §3.7 bodies change. Track at the first
  Windows-port spike.

- **[OPEN] `OpenMode` API extension** [NON-BLOCKING]: `OpenMode`
  (SPEC §5.11) is reserved for future API surfaces such as
  `open_with_mode(path, OpenMode)`. No existing consumer requires
  it; the enum is retained so the API slot is named. Promote to a
  concrete method once a second consumer demonstrates the need (same
  Occam's-razor gate as `PathOutsideRoot` arm, §12 [OPEN] #4).

- **[OPEN] Async-path prefix transport — Slot gains `const char* prefix`**
  [BLOCKING IMPLEMENTATION]: the `Slot` struct (§3.6) gains a
  `const char* prefix` field (already reflected in §3.6 and §3.9).
  The implementation PR must: (a) define the canonical set of
  `prefix::file_io_*` literals in `fileio/error_prefixes.hpp`;
  (b) have the worker capture `current_prefix()` into `slot.prefix`
  AFTER the translator runs and BEFORE the `state = Ready`
  release-store; (c) have the consumer read `slot.prefix` after
  observing `state == Ready` (acquire-load). Gate the implementation
  PR on the canonical literal set being established first.

- **[OPEN] N > 1 MPSC ring for multi-worker file-IO** [NON-BLOCKING]:
  The N > 1 worker path (§3.5) requires a separate MPSC ring
  (`detail/mpsc_ring.hpp`) because SPSC cannot safely serve multiple
  consumer threads. The exact ring contract (per-consumer head index,
  shared tail, ABA hazards), work-stealing protocol, and fairness
  guarantees are deferred to a dedicated spike. File-IO N > 1
  enablement is post-MVP per PHILOSOPHY §5 / plans/mvp.md; the MVP
  `N = 2` path uses a SPSC ring with dispatcher semantics (§3.5).
  Track at `[SPIKE] iterate-platform-mpsc-ring`.
