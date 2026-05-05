# platform — Detailed Design: file-watcher aggregate

> Detailed design for the `FileWatcher` / `FileEvent` / `WatchToken`
> aggregate declared in `specs/platform/SPEC.md` §4.3. Refines §4.3,
> §5.8, §6.4, §8.1 (peer-reload pass-through and platform-self-reload
> watcher row), §8.3 step 3 + step 4.3 (subscription capture and fresh-
> event replay), §8.5.2 (`WatcherSurvival` middleman), §8.6 (file-event
> consumer reseat), §9.1 (FileWatcher per-frame budget = 0), §9.2
> (4 MiB sub-arena), §10.3.3 (returnable arms by entry point) and §10.5
> (`watcher-unavailable` recovery shape). It does **not** redefine the
> §5 stub.
>
> Cites `reviews/decisions/error-model.md`,
> `reviews/decisions/frame-phases.md`,
> `reviews/decisions/perf-budget.md`,
> `reviews/decisions/plugin-abi.md`,
> `reviews/decisions/hot-reload-protocol.md`,
> `reviews/decisions/fory-codegen.md`. No public surface is introduced
> beyond the §5.8 stub locked in `specs/platform/SPEC.md`; deviations
> require an amendment spike, not an in-place edit.
>
> Harmonius prior art (`harmonius/docs/requirements/platform/
> filesystem.md`, `R-14.6.5` / `R-14.6.6` / `R-14.6.8` / `R-14.6.9`)
> cited as research input only — every conclusion below was
> re-derived per `PHILOSOPHY.md`.

Refs: spike #719 — `[SPIKE] design-platform-file-watcher-detailed`.
Parent: #714. Sibling task-breakdown spike blocked by this deliverable.

## 1. Purpose

The `file-watcher` aggregate is the single component in the platform
context permitted to register an `FSEventStreamRef` with macOS Core
Services and to drive its callback-thread → main-thread fan-out. Its
one responsibility is **owning the lifetime of recursive directory
subscriptions plus the debounced, deduplicated `FileEvent` stream
attached to them**: subscribe a `CanonicalPath` root, run an internal
I/O thread that feeds the FSEvents runloop, canonicalize raw paths,
collapse duplicates inside a per-root debounce window, reassemble
atomic renames, and vend the `FileEvent`s through a per-token SPSC
ring drained from the main thread by `FileWatcher::take_events`.

What this aggregate explicitly refuses to own:

- **Hot-reload barrier** — `core::HotReloadBarrier` (sibling spike
  #706, design `specs/core/hot-reload-barrier-design.md`) consumes
  `FileEvent`s through whatever consumer holds the `WatchToken`, but
  the watcher does not call into the loader; it has no knowledge of
  plugin dylibs, ABI hashes, or migration tables.
- **Plugin loader / `dlopen` mechanics** — `core::PluginLoader`
  (#704). The watcher emits filesystem events; the loader decides
  what (if anything) to reload.
- **Asset reload bodies** — owned by `content` (post-MVP) and by the
  consuming plugin's `glibre_plugin_register`. The watcher signals
  *that* a path changed; consumers decide *what to do*.
- **Polling fallback** — when `watcher-unavailable` recurs (SPEC §10.5
  step 4) the consumer (typically the editor / hot-reload coordinator)
  spins a `FileIo::stat_path` / `list_dir` shim. SPEC §10.5 step 5
  is explicit: "Building polling into the aggregate would re-merge
  the responsibilities §4.3 SRP keeps apart." The watcher refuses.
- **Window / event pump** — `Pump`, `EventQueue<InputEvent>`,
  `EventQueue<WindowEvent>` (sibling #717). `FileEvent` is *not*
  funnelled through `Pump::drain`; it has its own per-token drain
  (SPEC §5.8 `take_events`). The collapse rule is in §3.5 below.
- **`FileIo` blocking / async I/O** — sibling #721. The watcher does
  no file *content* reads; the BLAKE3 dedup mentioned in SPEC §6.4
  reads bytes from the OS only inside the I/O thread, never on the
  main thread, and never through `FileIo`. The two aggregates share
  the `CanonicalPath` value object and nothing else.
- **`Clock` / time** — sibling #723. The debounce window timestamps
  events with `Clock::now()`, but the `Clock` aggregate is taken by
  reference via the `create(Clock&)` parameter (SPEC §4.4 inv #5);
  the watcher does not own a time source. Two concrete callers in MVP:
  the editor hot-reload coordinator and the shipping runtime asset
  reload path. The SPEC §5.8 stub amendment (adding `Clock&` to
  `create`) must land before the first plan PR; see §12.
- **Persistence of the subscription list across process restart** —
  refused per SPEC §7.3 ("File-watcher canonical-path subscription
  list (cache)"). Subscriptions are reseated by the consumer on every
  startup; persisting them in platform would duplicate `content`'s
  authoritative ownership and break determinism (a re-run with a
  stale list emits a different event stream than a fresh run).
- **Platform-side error policy** — closed sum `platform::Error` is
  owned by §4.7 / §10. This design surfaces failures into pre-existing
  arms (`NotFound`, `PermissionDenied`, `Unsupported`,
  `IoFailure { OsCode }` with `"watcher-unavailable"` prefix); it does
  not invent new arms.
- **Obj-C / Obj-C++ glue** — CLAUDE.md "No Obj-C++ in engine code".
  The macOS FSEvents API is pure C (`FSEventStreamCreate`,
  `FSEventStreamScheduleWithRunLoop`, `CFRunLoopRun`,
  `FSEventStreamRelease` from `<CoreServices/CoreServices.h>`); SPEC
  §6.4 specifically calls out that the backend `.cpp` file uses the C
  API directly without falling into a `.mm`. This design preserves
  that rule.

The aggregate's SRP boundary is sharp: if the OS file-watch backend
shifts (FSEvents → kqueue / inotify / `ReadDirectoryChangesW`), if
the canonicalization rule changes (Apple ships a new normalisation
form, an APFS case-folding option flips), or if the rename-reassembly
heuristic mutates, this design changes. Anything else is out of scope.

## 2. Requirements coverage

Mapping of harmonius filesystem requirements
(`harmonius/docs/requirements/platform/filesystem.md`, R-14.6.5,
R-14.6.6, R-14.6.7, R-14.6.8, R-14.6.9) onto MVP coverage in this
aggregate. Every entry is independently re-derived; coverage sites
refer to sections of `specs/platform/SPEC.md` and to the design
sections below.

| Harmonius clause                                                                                  | Glibre disposition (MVP)                                                                                                                                                                                                                                                       |
|---------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **R-14.6.5** monitor for create / modify / delete / rename via platform-native APIs; debounce; recursive | **Covered.** SPEC §4.3 inv #2 (debounce + dedup) + §4.3 inv #3 (atomic rename) + §6.4 (FSEvents-via-C-API on macOS). Recursive subscription per `kFSEventStreamCreateFlagWatchRoot` + `kFSEventStreamEventIdSinceNow`. §3.3, §3.4, §3.5 below.                                |
| **R-14.6.6** content-hash dedup with BLAKE3                                                       | **Covered, scoped.** BLAKE3 is *internal* to the watcher's I/O thread; it never crosses the public surface. The hash collapses metadata-only events (`FSEventStreamEventFlagItemModified` without content change) within one debounce window per §4.3 inv #2. §3.6 below.       |
| **R-14.6.7** canonical absolute paths; macOS case-folding                                         | **Covered, collapsed.** `CanonicalPath` (SPEC §4.3 inv #6) is the only key shape that crosses the watcher's boundary. Canonicalisation happens once on the I/O thread before dedup (§3.4 step 3); the public API never accepts a raw path. §3.7 below.                          |
| **R-14.6.8** typed `FileEvent` with `FileEventKind` enum                                          | **Covered, collapsed.** `FileEvent` is a closed `eastl::variant<Created, Modified, Deleted, Renamed>` (SPEC §5.8). No `next() async` shape — glibre's frame-driven model uses non-blocking `take_events(token, span)`. §3.8 below.                                              |
| **R-14.6.9** in-memory cache of `(canonical_path → BLAKE3 hash)`                                  | **Covered, scoped.** The LRU is internal to the I/O thread (SPEC §6.4 last paragraph) and lives in the watcher's 4 MiB sub-arena. It is never public; consumers cannot insert or query. §3.6 + §9.2 below.                                                                       |
| **R-14.6.5** "configurable debounce interval"                                                     | **Partial — fixed default in MVP, knob deferred.** The debounce window is a build-time constant (default 100 ms, §3.6 below). Surfacing it as a runtime knob requires the editor / hot-reload coordinator spike — refused for MVP per Occam (no second consumer demands it). §12 holds the trigger. |
| Tokio async-channel delivery / async fn next()                                                    | **Refused (collapsed).** Per SPEC §3 collapse "kqueue / inotify / ReadDirectoryChangesW / FSEvents → `FileWatcher`": no Tokio, no async streams. The frame-driven `take_events(WatchToken, span)` is a non-blocking SPSC drain; backpressure surfaces as `IoFailure` with prefix `"watcher-unavailable"` only on FSEvents stream death, not on consumer slowness (the SPSC is bounded; overflow is documented in §10 below). |
| Network mount / sandbox sensitivity                                                               | **Covered as recovery.** SPEC §10.5 already enumerates volume-unmount, FSEvents service exhaustion, and TCC denial as `watcher-unavailable` triggers. This design discharges the aggregate side of that recovery (release stream, release token, no auto-re-arm). The consumer-side polling shim is refused per §1.                                       |
| Watch cancellation                                                                                | **Covered.** `FileWatcher::unwatch(WatchToken)` (SPEC §5.8) tears down the FSEvents stream and the per-token SPSC; subsequent `take_events` against the stale token returns `NotFound` (SPEC §10.3.3 row 3).                                                                    |

Glibre-native requirements added beyond harmonius:

- **Per-token SPSC bounded ring**, never grows. Sized at construction
  (default 4096 slots × 64 B = 256 KiB; sub-arena math in §9.2). The
  writer is the I/O thread; the reader is the main thread. SPSC, not
  MPSC, because the rename-reassembly step §3.5 requires the I/O
  thread to be the sole writer per token.
- **Watch token numeric identity is process-scoped, not reload-stable
  across platform self-reload.** SPEC §8.6 already locks this: peer
  plugin reload preserves `WatchToken`; platform-self-reload re-vends
  fresh tokens and consumers reseat. This design honours the rule
  without inventing reload-survivable token machinery.
- **Outstanding subscription counter for §8.3 step 3.** The aggregate
  exposes the in-memory subscription list as `eastl::span<const
  CanonicalPath>` to the loader during platform self-reload drain. No
  public getter; the loader gains access through a friend-restricted
  internal hook. §3.10 below.
- **Drain budget = 0 on the game-loop driver thread** (SPEC §9.1 row
  3). The aggregate may not run any work in phases 1–9 except a
  non-blocking SPSC dequeue inside `take_events`. The FSEvents
  callback runs on the watcher's own runloop thread and never on the
  main thread.

Coverage rule: every harmonius clause above either lands in this
design (with a coverage site) or is refused with a one-line rationale.
No silent drops.

## 3. Detailed model

### 3.1 Aggregate composition

```text
FileWatcher  (aggregate root, owned by platform)
├── std::thread              io_thread_              (one per FileWatcher; runs CFRunLoopRun)
├── std::atomic<bool>        stop_requested_         (set by ~FileWatcher; observed by io thread)
├── CFRunLoopRef             runloop_                (the io thread's CFRunLoop, opaque)
├── FSEventStreamRef         stream_                 (one stream per FileWatcher; multi-root)
├── eastl::vector<RootEntry> roots_                  (main-thread authoritative subscription list;
│                                                    guarded by cold_mutex_)
├── std::atomic<roots_snapshot_t*> roots_snapshot_  (lock-free read pointer for the I/O thread;
│                                                    written with memory_order_release on mutation,
│                                                    read with memory_order_acquire in FSEvents
│                                                    callback — see §3.x and §6.3)
├── eastl::vector<TokenRing> rings_                  (per-token SPSC ring of FileEvent)
├── DedupCache               dedup_                  (LRU keyed on CanonicalPath × content_hash)
├── RenameReassembler        renames_                (inode-id → pending Created/Deleted pair)
├── std::atomic<std::uint64_t> next_token_           (monotonic; never reused this process)
└── PerContextAllocator&     arena_                  (the platform sub-arena, §9.2)
```

The `FileWatcher` is the aggregate root. The §5.8 stub is the only
public surface; everything in the box above is private to the
implementation file group `engine/platform/src/watcher/`. SPEC §6.4
already names the file split (`watcher/backend.hpp`,
`watcher/backends/sdl3_fsevents.cpp` — though this design renames the
backend file to `watcher/backends/macos_fsevents.cpp` because the
MVP path uses Core Services directly, not SDL3 filesystem events;
SDL3 has no recursive-watch surface and the SPEC §6.4 paragraph
acknowledges the direct-FSEvents fallback as the load-bearing path).

`FileEvent` and `WatchToken` are value objects (SPEC §5.8). They cross
the boundary by value through `take_events`'s output span; consumers
treat them as PODs.

### 3.2 `WatchToken` lifetime

```cpp
struct WatchToken { std::uint64_t value{0}; constexpr bool operator==(const WatchToken&) const noexcept = default; };
```

- Vended monotonically by `FileWatcher::watch` from `next_token_`.
- Never reused inside one `FileWatcher` instance — even after
  `unwatch`. This is the rule that lets a consumer hold a `WatchToken`
  past its `unwatch`: subsequent `take_events(stale_token, …)` returns
  `NotFound` (SPEC §10.3.3) rather than silently aliasing onto a fresh
  subscription that happened to recycle the value.
- Numeric identity does **not** survive a `FileWatcher` instance
  swap. Platform self-reload re-vends a fresh `FileWatcher` (SPEC §8.6
  bullet 3); peer-plugin reload passes through the same `FileWatcher`
  and the same tokens (SPEC §8.1 row 3).
- `WatchToken{0}` is the sentinel "invalid"; the aggregate never
  vends `0`. The first valid token is `1`.

### 3.x `roots_snapshot_t` — lock-free read-side view for the I/O thread

`roots_snapshot_t` is a POD struct arena-allocated from the dedicated
`snapshot_pool_` sub-allocator (§9.2). It provides the FSEvents
callback with a stable, lock-free view of the current subscription
roots without the I/O thread ever taking `cold_mutex_`.

```cpp
// Internal to engine/platform/src/watcher/; never crosses the public ABI.
struct roots_snapshot_t {
    eastl::array<root_entry_t, 14> entries;  // 14-root bound matching §9.2 ring count
    uint8_t                        count;    // number of valid entries (0..14)
};
```

**Arena backing.** `snapshot_pool_` holds at most 3 live
`roots_snapshot_t` instances simultaneously (1 active + 2
quarantined for a 1-frame retire window). Each instance is
approximately 14 × `root_entry_t` size ≈ 14 × ~32 B = 448 B,
rounded up to 512 B per instance. Pool budget: 3 × 512 B =
~1.5 KiB — see §9.2 for the table row.

**Write side (main thread, inside `cold_mutex_`).** After any
`watch` / `unwatch` that mutates `roots_`:

1. Allocate a new `roots_snapshot_t` from `snapshot_pool_`.
2. Copy the current `roots_` vector into `entries` / `count`.
3. Store the pointer with `std::memory_order_release`:
   ```cpp
   roots_snapshot_.store(new_snap, std::memory_order_release);
   ```
4. Move the previously active snapshot to the quarantine list.
   After two full runloop iterations on the I/O thread, the
   quarantined snapshot is safe to reclaim back to
   `snapshot_pool_` (no callback can still be holding a reference
   into it after the next iteration completes).

**Read side (I/O thread, inside FSEvents callback).** Load the
pointer with `std::memory_order_acquire`:
```cpp
const roots_snapshot_t* snap =
    roots_snapshot_.load(std::memory_order_acquire);
```
The `memory_order_release` on the write side and the
`memory_order_acquire` on the read side establish a
happens-before edge: all mutations to the snapshot's `entries`
and `count` fields visible on the main thread at the time of
`store` are visible to the I/O thread after the `load`. No
further synchronisation is needed for reading the snapshot
members — they are plain struct fields behind the atomic pointer.

The callback never dereferences a stale pointer: the quarantine
window (two I/O-thread runloop iterations) guarantees the
previous snapshot outlives any concurrent callback invocation.
The `std::atomic<roots_snapshot_t*>` itself uses only `load` /
`store` (no compare-exchange); AArch64's release / acquire pair
compiles to a plain store + ISH barrier + plain load (the
compiler emits `stlr` / `ldar` on Apple Silicon), which is
zero-overhead on the hot callback path.

### 3.3 `FileWatcher::create(Clock& clock)`

Called once per consumer that needs an isolated subscription set;
typical MVP usage is one `FileWatcher` per editor or per content
plugin. The aggregate does *not* enforce singleton semantics — the
sub-arena (§9.2) is sized for one instance, but the contract permits
two if a consumer needs to isolate roots (e.g. user-content vs.
engine-installed assets). Steps:

1. **Allocate the per-instance state** from the platform `FileWatcher`
   sub-arena (§9.2, 4 MiB cell). Failure (sub-arena exhausted) →
   `IoFailure { OsCode { ENOBUFS } }` with prefix `"out-of-budget"`,
   discharged by `perf-budget.md` Allocator Rule #2.
2. **Spawn the I/O thread.** The thread function is
   `watcher::detail::io_thread_main(FileWatcher::Impl*)`. Inside it:
   a. Capture `CFRunLoopGetCurrent()` into `runloop_` (this thread's
      runloop; not the main thread's).
   b. `stream_` is null during startup (deferred creation). The
      first `watch()` call (§3.4 step 5) creates the stream once
      at least one root is registered. No empty `CFArrayRef` is
      ever passed to `FSEventStreamCreate` — `FSEventStreamCreate`
      rejects an empty path array on macOS 12+ and would return
      `NULL`, which `FSEventStreamScheduleWithRunLoop(nullptr, …)`
      would subsequently crash. The stream is reconstructed on
      every subsequent `watch` / `unwatch` that changes the root
      set (FSEvents requires recreate-on-change, §3.4 step 5 below).
   c. Enter `CFRunLoopRun()`. Returns when `CFRunLoopStop` is called
      from the destructor (§3.9).
   Failure (thread spawn / runloop init) → translate to `IoFailure`
   per SPEC §10.2.1 (`EAGAIN` on `pthread_create` saturation maps to
   `IoFailure { OsCode { errno } }` with prefix `"resource"`).
3. **Wait for the runloop to be live** using a `std::binary_semaphore`.
   The main thread allocates `std::binary_semaphore sem{0}` before
   spawning the I/O thread and passes its address into the thread
   function. The I/O thread stores `CFRunLoopGetCurrent()` into
   `runloop_` then calls `sem.release()`. The main thread calls
   `sem.try_acquire_for(5ms)`; on timeout, the thread is joined and
   `IoFailure { OsCode { 0 } }` with prefix `"watcher-unavailable"`
   is returned. The 5 ms bound is safe on loaded CI hosts where a
   ~64 µs bounded spin would expire prematurely.
4. **Return `FileWatcher`** by move. The aggregate's pimpl is the only
   heap reference the public `FileWatcher` holds.

`create` does **not** subscribe any roots; the FSEvents stream is
empty until the first `watch` call. This separation is what lets
`create` succeed on a system where TCC has not yet been granted: the
permission check happens at `watch` time, not at `create` time.

### 3.4 `FileWatcher::watch(CanonicalPath root)` — subscribe a root

Called on the main thread (no concurrent `watch` from the I/O thread;
the contract is documented in `take_events`'s sibling rule SPEC
§10.3.3 row 4 "off-main-thread call → `Unsupported`" — same rule
applies to `watch` and `unwatch`). Steps:

1. **Validate** the `root`. The constructor of `CanonicalPath` (SPEC
   §4.3 inv #6) already enforces UTF-8 + absoluteness; this function
   additionally enforces directory-ness via a single
   `lstat`-equivalent call (`stat` resolves through the symlink which
   we want — symlinked roots should follow). Failure: `Unsupported`
   on file (non-directory), `NotFound` on missing path,
   `PermissionDenied` on a readdir denial (the FSEvents stream-create
   call later would fail anyway; pre-checking here lets us return the
   precise arm).
2. **Allocate a new `WatchToken`** (`next_token_.fetch_add(1, ...)`).
3. **Allocate a per-token SPSC ring** in the sub-arena. Default size:
   4096 entries × 64 B = 256 KiB. Failure → `IoFailure` with
   `"out-of-budget"` prefix (§9.2). The ring's writer side is held by
   the I/O thread; the reader side is the main-thread caller of
   `take_events`.
4. **Append the root to `roots_`.** This is a vector push of a
   `RootEntry { token, canonical_root, ring_index, debounce_window,
   inode_for_renames }`.
5. **Recreate the `FSEventStreamRef` on the I/O thread.** FSEvents
   does not support adding paths to a live stream; the watcher must
   stop the existing stream, `FSEventStreamRelease` it, and create a
   new one with the union of all roots. The recreate is dispatched
   into the I/O thread's runloop via `CFRunLoopPerformBlock` +
   `CFRunLoopWakeUp`. The main thread does not block on the result of
   the recreate; the per-token ring is alive immediately so any
   consumer call to `take_events` returns 0 events until FSEvents
   delivers the first batch. The recreate code:
   ```text
   if (stream_) {
       FSEventStreamStop(stream_);
       FSEventStreamInvalidate(stream_);
       FSEventStreamRelease(stream_);
       stream_ = nullptr;
   }
   const CFArrayRef paths = make_cfarray_from_roots_(roots_);
   const FSEventStreamContext ctx = { 0, this, nullptr, nullptr, nullptr };
   stream_ = FSEventStreamCreate(
       /*allocator=*/ kCFAllocatorDefault,
       /*callback=*/  &watcher::detail::fsevents_callback,
       /*context=*/   &ctx,
       /*paths=*/     paths,
       /*sinceWhen=*/ kFSEventStreamEventIdSinceNow,
       /*latency=*/   debounce_seconds_,                       // §3.6
       /*flags=*/     kFSEventStreamCreateFlagFileEvents
                    | kFSEventStreamCreateFlagWatchRoot);
   FSEventStreamScheduleWithRunLoop(stream_, runloop_, kCFRunLoopDefaultMode);
   FSEventStreamStart(stream_);
   ```
   Flags: only `kFSEventStreamCreateFlagFileEvents` and
   `kFSEventStreamCreateFlagWatchRoot`. `kFSEventStreamCreateFlagNoDefer`
   is intentionally absent — the 100 ms `latency` argument owns
   coalescing of save→fsync→atomic-rename→unlink-old bursts. With
   `NoDefer` present, FSEvents would fire immediately on the first
   event in every burst, bypassing OS coalescing and making the
   software debounce (not the OS) the load-bearing collapse mechanism;
   BLAKE3 would then run per intermediate event rather than once on
   the post-coalesce event. Removing the flag restores the correct
   invariant: BLAKE3 fires only after the OS has coalesced the burst.

   FSEvents stream-create / start failure → `IoFailure { OsCode { 0 } }`
   with prefix `"watcher-unavailable"` (SPEC §10.5). On failure the
   token is **rolled back** (`roots_` entry removed; SPSC ring
   released back to the sub-arena), so the caller may retry without
   leaking a token slot.
6. **Return the `WatchToken`** to the main-thread caller.

Step 5 is the only OS-touching step; steps 1–4 are pure validation
and bookkeeping. The recreate is the load-bearing collapse: every
`watch` call walks all roots, which is `O(N_roots)` in path-array
construction. MVP root counts are ≤ 16 (one per editor content tree
plus a small handful of engine-asset trees); the cost is bounded.

### 3.5 `FileWatcher::unwatch(WatchToken)` — release a root

Called on the main thread. Steps:

1. **Look up the `RootEntry`** by token. Stale token →
   `NotFound` (SPEC §10.3.3). Idempotent in the sense that a second
   `unwatch` on the same token also returns `NotFound`; this is the
   shape `WatcherUnavailable` recovery (§10.5 step 2) relies on.
2. **Drop the entry from `roots_`** and mark its SPSC ring for
   teardown. The ring is not freed immediately because the I/O
   thread may still be writing into it; the I/O thread is signalled
   to drain its in-flight buffer for that token, then the ring is
   returned to the sub-arena.
3. **Recreate the `FSEventStreamRef`** with the new (smaller) root
   set, same dispatch shape as §3.4 step 5. If `roots_` becomes
   empty, the recreate releases the stream entirely (no
   zero-path stream is created — FSEvents rejects that).
4. **Return `void`** on success.

The teardown rules above guarantee SPEC §4.3 inv #4 ("`~FileWatcher`
tears down every native subscription it holds; no orphaned
[…] FSEvents stream survives"): every `unwatch` releases the OS
resource synchronously from the I/O thread's perspective; the
destructor (§3.9) calls `unwatch` for every remaining token before
joining the thread.

### 3.6 Debounce + dedup (the I/O-thread frontend)

The FSEvents callback runs on the watcher's I/O thread. The callback
signature is fixed by Core Services:

```c
void fsevents_callback(
    ConstFSEventStreamRef stream,
    void* clientCallBackInfo,
    size_t numEvents,
    void* eventPaths,            // const char* const* (kFSEventStreamCreateFlagFileEvents)
    const FSEventStreamEventFlags eventFlags[],
    const FSEventStreamEventId eventIds[]);
```

Inside the callback, on the I/O thread, the watcher does
**five** ordered steps, each documented inline against a SPEC
invariant:

1. **Path canonicalization** (SPEC §4.3 inv #1). For each raw path the
   callback reports, run it through the same `CanonicalPath`
   constructor that `watch` / `unwatch` use; failure (path went
   missing between FSEvents emission and our read — the directory
   was deleted in flight) results in the event being downgraded to
   `Deleted` against the most recent canonical form we held for that
   inode, or dropped if no inode mapping is known. No raw path
   escapes the callback.
2. **Per-root assignment**. Map the canonical path back to a
   `WatchToken` by longest-root-prefix match against `roots_`. An
   event under multiple overlapping roots is delivered once per
   covering root (this matches FSEvents' own redundancy and is the
   shape consumers expect; deduplicating across roots would lie
   about which subscription the event came from).
3. **Content-hash dedup** (SPEC §4.3 inv #2). The dedup LRU is keyed
   on `(canonical_path, content_hash)` where `content_hash` is the
   BLAKE3 of the file's contents at the moment of the event. If the
   same `(path, hash)` was emitted within the debounce window
   (default **100 ms** measured by `Clock::now()` per `RootEntry`),
   collapse the event onto the existing entry rather than emitting a
   new one. The hash is computed lazily: the LRU first checks for a
   `(path, *)` hit; if the cached hash matches a freshly-read
   content hash, dedup; otherwise re-hash and update. The default
   100 ms is the `latency` argument passed to `FSEventStreamCreate`
   (§3.4 step 5) plus a single-frame slack at 60 fps; this is what
   collapses editor "save → fsync → atomic-rename → unlink-old" three-
   event bursts into one `Modified`.
4. **Rename reassembly** (SPEC §4.3 inv #3). Inode-keyed pending
   table: when the FSEvents flags carry `kFSEventStreamEventFlagItemRenamed`
   (which the OS sets on both halves of a rename pair), the watcher
   buffers the first half (`Created` or `Deleted` candidate) for up
   to one debounce window. If the matching half arrives within the
   window with the same inode, both halves collapse into a single
   `FileEvent::Renamed { from, to }`. If the window expires without
   the match, the buffered half is emitted as-is (`Created` or
   `Deleted`) — the rename heuristic refuses to lie when the OS does
   not deliver the pair.
5. **SPSC enqueue**. The resulting `FileEvent` is pushed into the
   per-token ring (`rings_[root_entry.ring_index]`). The push is
   non-blocking; if the ring is full (consumer fell behind by more
   than 4096 events — see §10), the watcher drops *the oldest event*
   to make room and increments a per-token `std::uint64_t` overflow
   counter. `uint64_t` is chosen over `uint32_t` to eliminate silent
   wrap-around: at 4096 drops/s (a realistic storm rate) a 32-bit
   counter wraps in ~12 days, silently resetting to zero and breaking
   the "non-zero counter triggers a log" invariant. At `uint64_t`
   width no saturation is needed. The log fires on the first non-zero
   observation per token-lifetime; subsequent increments accumulate
   without log spam (rate-limited via the diagnostics channel).
   SPEC §4.3 does not currently mandate "never drop" for `FileEvent`
   (unlike SPEC §4.2 inv #3 for `WindowEvent`), and dropping oldest
   matches the "live-reload is best-effort under storm" intent of
   the consumer (the next `Created` event will resurface the file).
   Drops are loud-but-bounded: §10 below.

The five steps run inside one callback invocation. The callback never
allocates from the system heap — every push is into the
sub-arena-backed structures. The BLAKE3 work itself is O(file-size)
and is the only step that can block the FSEvents callback for a
non-trivial duration; the I/O thread's runloop tolerates this because
no other thread depends on it for hot-path work.

### 3.7 `CanonicalPath` integration

`CanonicalPath` (SPEC §5.7 / §4.3 inv #6) is shared with `FileIo`. The
watcher constructs `CanonicalPath` values in two places:

- **`watch(root)` entry point** — caller already supplies a
  `CanonicalPath`, so the type system enforces the contract; nothing
  to do in the aggregate beyond the directory-ness check (§3.4).
- **FSEvents callback (§3.6 step 1)** — the OS reports raw `const
  char*` UTF-8 paths. The callback re-invokes
  `CanonicalPath::from_absolute(eastl::string_view)`; if the OS path
  is non-canonical (lowercase variant on a case-insensitive volume,
  symlink path because the user watched a symlinked tree), the
  constructor canonicalises. Failure here is rare; the resulting
  `Unsupported` arm is logged once at `warn` per offending raw path
  (rate-limited inside the I/O thread to avoid spdlog flood under
  storm).

The interner mentioned in SPEC §9.2 row 3 is shared across the I/O
thread and the main thread. The reader (main thread, inside
`take_events`) reads `CanonicalPath` values copied out of the SPSC
ring; the interned string body lives in the watcher's sub-arena and
outlives any single event. SPSC-ring entries hold a `string_view` /
small handle into the interner, *not* an owning `eastl::string`; this
is what keeps the per-event memory ≤ 64 B.

### 3.8 `FileWatcher::take_events(WatchToken, eastl::span<FileEvent> out)`

The main-thread drain. Steps:

1. **Look up the per-token ring.** Stale token → `NotFound`. Off-main-
   thread call (debug assert; release path returns `Unsupported`).
2. **SPSC dequeue** up to `out.size()` events. Returns the count
   actually written. The dequeue is `O(N)` in slot count; the SPSC
   ring uses the same wait-free single-producer/single-consumer
   pattern as the sibling `EventQueue<T>` (§4.2), but per-token rather
   than global.
3. **Stamp diagnostics on overflow.** If the per-token overflow
   counter is non-zero, the call additionally logs (rate-limited; one
   `warn` per 1000 overflows per token) before returning. Overflow is
   not an error — the call still returns the number of valid events
   it wrote.
4. **Return the count** (`Result<std::size_t>`).

`take_events` is the only call that reads from the ring; the
single-consumer property is enforced by contract (one consumer
plugin per `WatchToken`). Multi-consumer fan-out is the consumer's
responsibility (a consumer that wants to share a watch among multiple
sub-systems builds its own broadcaster on top of one token).

### 3.9 `~FileWatcher` — destruction

Order matters. Steps:

1. **Set `stop_requested_ = true`** (sequentially-consistent store,
   read by I/O thread on every callback entry).
2. **Stop the FSEvents stream.** Dispatch to the I/O thread runloop:
   `FSEventStreamStop(stream_)` then `FSEventStreamInvalidate` then
   `FSEventStreamRelease`. After this returns, no further callbacks
   fire.
3. **Stop the I/O thread runloop** (`CFRunLoopStop(runloop_)`). The
   runloop returns from `CFRunLoopRun`; the thread function returns.
4. **Join the thread.** Bounded wait: 100 ms is the budget; if it
   exceeds, we log at `error` and `terminate` — this is the same
   shape as the §4.7 inv #2 "platform never auto-retries" rule; we
   do not let a hung I/O thread keep the process alive past the
   destructor.
5. **Release every per-token SPSC ring** to the sub-arena.
6. **Release the sub-arena allocation** for the pimpl. (The arena
   itself outlives the watcher; the watcher is just a participant.)

Steps 1–4 cover SPEC §4.3 inv #4. The destructor is `noexcept`; any
internal failure terminates per the engine's `-fno-exceptions` build
mode (`error-model.md` §"Decision" rule 3).

### 3.10 Loader hook for platform self-reload

SPEC §8.3 step 3 requires the loader to capture
`eastl::span<CanonicalPath>` of the current subscription roots before
a platform-self-reload swap. This design exposes that capture through
a friend-restricted internal hook in
`engine/platform/src/watcher/reload_hook.hpp`:

```cpp
namespace glibre::platform::watcher::detail {

// Loader-only. Returns a stable span of canonical roots for every
// live FileWatcher in the process; the span is valid until the next
// watch / unwatch on any of those watchers. Used by phase-8 drain to
// build the WatcherSurvival middleman (SPEC §8.5.2).
[[nodiscard]] auto capture_subscription_set(FileWatcher&) noexcept
    -> eastl::span<const CanonicalPath>;

}  // namespace glibre::platform::watcher::detail
```

The hook is friend-restricted to `glibre::core::HotReloadBarrier`
(see `specs/core/hot-reload-barrier-design.md` §6) and to the
loader's per-plugin drain dispatcher. No public C++ identifier
references it; no plugin has access. This is the §1 refusal "watcher
survives a swap" expressed structurally.

The matching restore path is consumer-side, not aggregate-side: SPEC
§8.6 bullet 3 makes the consumer responsible for re-issuing
`watch(root)` after platform self-reload. The aggregate replays
`Created` for every existing file under each re-subscribed root
because FSEvents itself does that on a fresh stream registration
(`kFSEventStreamEventIdSinceNow` against a directory that already has
files). The consumer's idempotent "load or refresh" handling
(SPEC §8.3 step 4.3) consumes that burst correctly; the dedup window
collapses identical content.

## 4. Public surface

The §5.8 public stub from `specs/platform/SPEC.md` is the only
boundary this design ships. Recapitulated for self-containment:

```cpp
// platform/include/glibre/platform/file_watcher.hpp
#pragma once
#include <cstdint>
#include <EASTL/span.h>
#include <EASTL/variant.h>
#include <glibre/error.hpp>
#include <glibre/platform/canonical_path.hpp>
#include <glibre/platform/clock.hpp>  // Clock& parameter of create(); use clock_fwd.hpp if clock-design §4 uses a forward-decl header

namespace glibre::platform {

struct WatchToken { std::uint64_t value{0};
                    constexpr bool operator==(const WatchToken&) const noexcept = default; };

namespace file_event {
struct Created  { CanonicalPath path; };
struct Modified { CanonicalPath path; };
struct Deleted  { CanonicalPath path; };
struct Renamed  { CanonicalPath from; CanonicalPath to; };
}  // namespace file_event

using FileEvent = eastl::variant<
    file_event::Created,
    file_event::Modified,
    file_event::Deleted,
    file_event::Renamed>;

class FileWatcher {
public:
    [[nodiscard]] static auto create(Clock& clock) noexcept -> Result<FileWatcher>;

    FileWatcher(FileWatcher&&) noexcept;
    FileWatcher& operator=(FileWatcher&&) noexcept;
    FileWatcher(const FileWatcher&)            = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;
    ~FileWatcher();

    [[nodiscard]] auto watch(CanonicalPath root) noexcept -> Result<WatchToken>;
    [[nodiscard]] auto unwatch(WatchToken)        noexcept -> Result<void>;

    [[nodiscard]] auto take_events(WatchToken, eastl::span<FileEvent> out) noexcept
        -> Result<std::size_t>;

private:
    FileWatcher() noexcept = default;
    struct Impl;
    Impl* impl_{nullptr};
};

}  // namespace glibre::platform
```

Surface invariants this design enforces beyond the SPEC §5.8 stub:

1. **No exceptions cross the boundary.** `noexcept` on every public
   method is binding. Internal allocation failure inside `Impl` is
   surfaced as `IoFailure { OsCode { ENOBUFS } }` per §10.
2. **No Obj-C / Obj-C++ types in the header.** `void*` is not exposed;
   `FSEventStreamRef`, `CFRunLoopRef`, `FSEventStreamEventFlags` and
   the rest of Core Services live in the implementation translation
   unit only. The header includes `<EASTL/variant.h>`, `<EASTL/span.h>`,
   `<cstdint>`, `<glibre/error.hpp>`, `<glibre/platform/canonical_path.hpp>`,
   and `<glibre/platform/clock.hpp>` — that is the closed list.
   (`Clock&` is a parameter of `create`; the header must pull in its
   definition. Verify the canonical header name against
   `specs/platform/clock-design.md §4` — if clock-design uses a
   forward-decl header `<glibre/platform/clock_fwd.hpp>`, substitute
   that instead and update the `#include` in the code snippet above.)
3. **`std::expected<T, glibre::Error>`** is the return type of every
   fallible function (`error-model.md` Decision rule 1). The aliased
   `Result<T>` is defined in `glibre/error.hpp`.
4. **Move-only.** Copy is deleted; assignment moves. The aggregate
   owns one OS resource set per instance and a copy would alias OS
   handles.
5. **`FileWatcher::Impl` is opaque** (PIMPL). The header carries the
   forward declaration and a single pointer; no plugin compiles
   against the implementation layout, which is what lets the
   `glibre-platform.dylib` self-reload without an ABI hash bump on
   every internal layout tweak (per `plugin-abi.md`).
6. **`WatchToken` is not serialisable.** No `to_bytes` / `from_bytes`
   / Fory schema. Persisting a watch token across runs is meaningless
   (the FSEvents stream is process-scoped) and SPEC §7.3 already
   refuses watch-list persistence; the type elides serialisation
   surface to make that refusal structural.

ABI shape: the public header compiles to one exported symbol per
public method (`_ZN6glibre8platform11FileWatcher6createERNS0_5ClockE`,
…) plus the move-constructor / move-assignment / destructor triple.
(Itanium ABI mangle for `glibre::platform::FileWatcher::create(
glibre::platform::Clock&)` — updated to reflect the `Clock&`
parameter added per §12 [BLOCKING]. Exact mangle confirmed against
the Itanium ABI encoding rules; verify with `c++filt` at codegen
time. If the clock-design spike determines `Clock` lives in a
different namespace, the mangle must be updated accordingly.)
None involves `eastl::*` template internals at the symbol level (the
opaque-pimpl pattern keeps `eastl::vector<…>` / `eastl::variant<…>`
out of the exported function signatures except as the by-value
`FileEvent` parameter, which is itself plain-trivial-layout).

## 5. Hot/cold path split

SPEC §6.4 already names the split implicitly; this section formalises
it.

**Hot path** (callable from any frame phase, ≤ 0.001 ms per call,
no allocation, no syscalls beyond the SPSC dequeue):

- `FileWatcher::take_events(WatchToken, eastl::span<FileEvent>)` —
  steady-state drain. Inner loop is the SPSC dequeue; the only
  branches are stale-token / off-main-thread guards.

**Cold path** (called rarely; allocation, syscalls, OS-level resource
juggling permitted):

- `FileWatcher::create(Clock&)` — once per consumer (one or two times
  per process in MVP).
- `FileWatcher::watch(CanonicalPath)` — once per subscribed root
  (≤ ~14 times per process). Triggers an FSEvents stream recreate.
- `FileWatcher::unwatch(WatchToken)` — once per token, on shutdown or
  when a subscription is no longer needed. Triggers a stream recreate.
- `~FileWatcher()` — once per instance, at process shutdown or
  platform self-reload drain.

**Off-main-thread** (executes inside the watcher's I/O thread, never
counted against any frame-phase budget):

- The FSEvents callback (`watcher::detail::fsevents_callback`).
- Path canonicalisation, BLAKE3 hashing, dedup-LRU updates,
  rename-reassembly buffering, SPSC enqueue.

The hot/cold split structurally enforces SPEC §9.1 row 3
("`FileWatcher` Hot-path cost on the driver thread is **0**"):
the only main-thread method that runs every frame is
`take_events`, and even that is invoked only by consumers that
actively want the events (the watcher does not have a "drain me each
frame" registration). Most frames see zero `take_events` calls; the
hot-reload coordinator typically calls it once per frame, and it's
typically a 0-event SPSC dequeue.

`watch` / `unwatch` are deliberately *not* hot-path: their cost is
amortised across the lifetime of a subscription. SPEC §9.1 budgets
them implicitly at 0 because they run during platform-cold setup
and shutdown only.

## 6. Concurrency

Two threads cooperate. No lock is taken on the hot path; one lock
guards the cold-path stream-recreate dance.

### 6.1 Threads

- **Main thread (game-loop driver).** Calls `create`, `watch`,
  `unwatch`, `take_events`, `~FileWatcher`. Reads from per-token
  SPSC rings.
- **Watcher I/O thread.** One per `FileWatcher` instance. Hosts a
  `CFRunLoop` and the `FSEventStreamRef`. Receives FSEvents
  callbacks, runs the §3.6 five-step pipeline, writes to per-token
  SPSC rings.

No third thread participates. The BLAKE3 work runs inline in the
FSEvents callback on the I/O thread; we deliberately do *not* offload
to a worker pool (which would re-introduce the FileIo dependency
§1 refuses) or to GCD (which would re-introduce Obj-C-style
dispatch we're keeping out of the engine).

### 6.2 Communication channels

- **Main → I/O: cold-path commands.** `watch` and `unwatch` post a
  block to the I/O thread runloop via `CFRunLoopPerformBlock` +
  `CFRunLoopWakeUp`. The block recreates the FSEvents stream with the
  new root set. The main thread *does not* block on the recreate
  result; the per-token SPSC ring is allocated synchronously on the
  main thread before the block runs, so the consumer has a valid
  ring before any event can flow.
- **I/O → Main: per-token SPSC rings.** Wait-free single-producer
  single-consumer rings. The producer is the I/O thread (the only
  thread that runs the §3.6 pipeline); the consumer is the main
  thread (the only thread that calls `take_events` per the contract).
  No multi-producer / multi-consumer variant exists.
- **Bidirectional shutdown signal.** `stop_requested_` is a
  `std::atomic<bool>` set by the destructor on the main thread and
  read by the I/O thread on every callback entry; combined with
  `CFRunLoopStop` (which wakes the runloop synchronously), this
  guarantees a bounded shutdown window (§3.9 step 4).

### 6.3 Synchronisation primitives

- **No mutex on the hot path.** SPSC rings synchronise via
  `std::atomic_ref<std::uint32_t>` head/tail counters with
  `memory_order_acquire` / `memory_order_release` pairs.
- **One `std::mutex` (`cold_mutex_`) for cold-path `roots_` mutation
  plus shadow-copy protocol for the I/O thread.** The FSEvents callback
  reads the lock-free snapshot pointer with
  `std::memory_order_acquire`:
  ```cpp
  const roots_snapshot_t* snap =
      roots_snapshot_.load(std::memory_order_acquire);
  ```
  It never takes `cold_mutex_`. The main thread (in `watch` /
  `unwatch`) takes `cold_mutex_`, builds a new `roots_snapshot_t`
  in `snapshot_pool_` (§3.x, §9.2), and stores the pointer with
  `std::memory_order_release`:
  ```cpp
  roots_snapshot_.store(new_snap, std::memory_order_release);
  ```
  The release/acquire pair is the happens-before edge that makes all
  `entries` / `count` writes visible to the callback thread after its
  `load` — no additional barriers are needed to read snapshot fields.
  The old snapshot enters a one-frame quarantine; after two I/O-thread
  runloop iterations it is safe to reclaim to `snapshot_pool_` (the
  callback cannot be reading a pointer it loaded before the store).
  The FSEvents stream recreate is dispatched via
  `CFRunLoopPerformBlock` / `CFRunLoopWakeUp` (off the I/O thread's
  hot path); worst-case callback blocking is zero — the callback
  always reads from the lock-free snapshot and never waits for
  `cold_mutex_`. `cold_mutex_` protects only the main-thread-side
  `roots_` vector and the `snapshot_pool_` allocations.
- **No spinlocks.** macOS scheduling does not give us reliable
  spinlock semantics outside the kernel; we use `std::mutex` and
  accept the rare cold-path contention.
- **No condition variables.** Shutdown uses `CFRunLoopStop` + thread
  join; SPSC backpressure is the drop-oldest rule from §3.6 step 5.

### 6.4 Frame-phase positioning

Per `frame-phases.md` and SPEC §9.1, the watcher's main-thread work
is invoked from **phase 8 (hot-reload barrier)** by the
`HotReloadBarrier` aggregate (or, post-MVP, by an editor-side content
coordinator from a phase 1 sub-system). The watcher itself is *not*
a phase owner; it does not appear in the frame-phases.md table. Its
I/O thread runs continuously, decoupled from frame phases entirely.

The "drain at frame boundary" property is therefore consumer-driven:
the consumer (most often `core::HotReloadBarrier`) pulls events at a
frame boundary; the watcher does not push. This matches SPEC §4.3
inv #5 "Watcher does not block the main thread".

### 6.5 Why no `Pump` integration

`Pump::drain` (§4.2) handles `InputEvent` / `WindowEvent` because
those events drive simulation/UI on the same frame they arrive.
`FileEvent` is fundamentally different: it triggers cold paths
(plugin reload, asset reload) that run at most once per frame and
have no per-frame-tick coupling. Routing `FileEvent` through `Pump`
would force every frame to walk a usually-empty queue and would
re-couple the watcher to SDL3's pump cadence. The per-token SPSC
drain is the SRP-faithful shape.

## 7. Persistence + ABI

Nothing in this aggregate is persisted. There is no `.fory` schema;
there are no on-disk files; there is no IPC.

| Aspect             | This aggregate                                                                                                                                                                                |
|--------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Fory schemas       | None (per SPEC §7.3 explicit refusal of the "subscription list cache" temptation).                                                                                                            |
| `data/schemas/`    | No files contributed.                                                                                                                                                                         |
| `glibre-types.dylib` contribution to `host_glibre_types_abi_hash` | **Zero.** Even the `WatcherSurvival` middleman type (SPEC §8.5.2) is forward-declared and absent from the MVP middleman set (SPEC §8.5 last paragraph: "Their addition […] would bump […] once when platform self-reload is first implemented; until then this section is a forward-declaration"). |
| Persisted caches   | None. The BLAKE3 dedup LRU is in-memory only; SPEC §7.3 refuses persistence.                                                                                                                  |
| Plugin ABI surface | The §5.8 stub is the entire ABI. Any internal layout change is opaque to plugins (PIMPL). A change to the public stub itself triggers `plugin-abi.md`'s "ABI hash" rebump, which propagates through the standard channel. |
| Cross-process      | None. All state is per-process; subscriptions are reseated on every startup (SPEC §7.3).                                                                                                      |

The net effect: a platform-only edit that touches *only* this
aggregate's implementation files cannot trigger a plugin-side ABI
rebuild. This is the load-bearing reason §7 is short.

## 8. Hot-reload

Two cases per the SPEC §8 split.

### 8.1 Peer-plugin reload (the common case)

A peer plugin (e.g. `content`, `tools`, the editor's hot-reload
coordinator) holds one or more `WatchToken`s and calls `take_events`
each frame. When the peer plugin reloads at phase 8:

1. **Drain step.** Outgoing peer plugin P captures its `WatchToken`s
   into the world's middleman storage (the consumer-side responsibility
   per `hot-reload-protocol.md` §"State Survival Rules").
2. **Swap step.** Loader replaces P's `.dylib` with Q's. The
   `FileWatcher` instance is *not* touched — it lives in platform's
   memory, not in P's; SPEC §8.1 row 3 documents this as an "opaque
   pass-through".
3. **Migrate step.** Empty for the watcher; SPEC §8.1 row 3.
4. **Resume step.** Q's `glibre_plugin_register` reads the captured
   tokens out of the middleman storage and resumes calling
   `take_events`. The token's numeric value is preserved (SPEC §8.2
   guarantee 1: "Every handle vended by platform […] is a numeric
   identifier owned by the platform aggregate that vended it"); Q
   sees the same SPSC ring P saw, with whatever events accumulated
   during the swap window now drained out.

No event is lost: events arriving during the swap window land in the
SPSC ring, the I/O thread keeps writing, and Q reads them once it
resumes. The SPSC ring sizing (4096 slots, §3.4) tolerates a swap
window up to ≈ 41 ms of 100-event-per-ms-burst storm — well past the
0.40 ms budget for phase 8 reload (`perf-budget.md` reload-frame).

The watcher *itself* survives the peer-plugin swap; subscribers
re-validate their tokens by calling `take_events` immediately after
register and verifying the result is not `NotFound`. A `NotFound`
return after a peer-plugin reload indicates a bug in the consumer
(it failed to reseat the token through the middleman) and is logged
at `error`.

### 8.2 Platform-self-reload

When platform itself is the outgoing plugin (SPEC §8.3), the
`FileWatcher` instance does **not** survive — its I/O thread, its
`FSEventStreamRef`, and its SPSC rings all live in platform's `.dylib`
memory which is being unmapped. The replacement protocol is
documented in SPEC §8.3 step 3 (drain) + step 4.3 (resume); this
design contributes the two specific aggregate-side hooks:

- **§3.10 capture hook** — exposed to the loader for snapshotting the
  subscription list into `WatcherSurvival`.
- **Subscriber re-validation contract** — peer plugins re-acquire the
  *new* `FileWatcher` handle from the platform API after register,
  and re-issue `watch(root)` for each captured root. Old `WatchToken`
  values are invalidated by construction (the new `FileWatcher`
  starts `next_token_` at 1 again; consumers receive new tokens as
  part of the reseat per SPEC §8.6 bullet 3).

Fresh-event replay: SPEC §8.3 step 4.3 already guarantees that each
re-subscribed root re-emits a `Created` for every existing file,
because FSEvents does this naturally on a fresh stream registration.
The dedup window collapses identical content; consumers idempotently
"load or refresh" against the burst. The watcher's only contribution
to the protocol is to honour FSEvents' default behaviour rather than
suppressing it.

### 8.3 Refusal cases

The watcher inherits the three universal refusal cases
(`hot-reload-protocol.md` §"Refusal Cases"):

- **ABI hash mismatch.** Detected at the loader, not in the watcher.
  No watcher-specific contribution.
- **Schema migration failure.** Vacuous — no `.fory` schema (§7).
- **Plugin init failure.** If platform's `Q::glibre_plugin_register`
  fails to re-create the `FileWatcher` (e.g. `pthread_create` failed,
  or FSEvents was unavailable system-wide), Q's register returns
  `unexpected(IoFailure { OsCode { 0 } })` with prefix
  `"watcher-unavailable"`; the loader maps to
  `core::Error::HotReloadRefused` and rolls back to P. The previous-
  good `FileWatcher` continues to run. SPEC §8.4 describes the
  rollback path.

### 8.4 No mid-frame swap concern

Unlike `Window` / `Surface` (SPEC §8.4 P1 "Mid-frame window drop"),
the watcher has no mid-frame refusal because:

- The watcher does no per-frame work on the main thread other than
  `take_events`. SPEC §9.1 budgets it at 0 ms.
- No GPU / render-side reference is ever held against a `WatchToken`.
- The phase 8 barrier runs after phase 7 (`render-submit`). By
  construction, no `take_events` call is in flight when phase 8
  starts — phase 8 *is* the typical site of the call, and only one
  thread (the loop driver) calls it. There is no "watcher drain in
  progress while another phase runs" state to refuse from.

## 9. Performance

### 9.1 Per-frame budget

SPEC §9.1 row 3 locks `FileWatcher` at **0.000 ms / 0.000 ms** per
frame (sim / submit). This design discharges that as follows:

- **Steady state.** The hot path is `take_events`. The SPSC dequeue
  is wait-free, branch-light, and runs in O(events-actually-drained).
  In typical MVP traffic — editor idle, a developer occasionally
  saving a file — the overwhelming majority of `take_events` calls
  return 0 events with cost < 0.001 ms (single atomic load on the
  ring head/tail pair). Storms of up to 100 events / frame still
  return inside the same precision band; the dominant cost is the
  per-event copy (~64 B) into the consumer's output span.
- **Cold-path calls.** `watch`, `unwatch`, `create`, `~FileWatcher`
  are not budgeted in SPEC §9.1 because they run during platform-
  cold setup / shutdown. A `watch` call that triggers an FSEvents
  recreate has a measured cost ≈ 0.5–2 ms on M1 (FSEvents service
  IPC + kernel registration). This cost lands on the *consumer's*
  init / shutdown frames; perf-budget locally defers to `core`'s
  reload-frame budget (0.40 ms phase 8) when the call site is
  hot-reload-driven, which in MVP it isn't (only editor-side reseats
  trigger `watch` post-boot, and those are bounded to ≤ 16 calls
  total).
- **Off-thread work.** The I/O thread runs the FSEvents callback,
  canonicalisation, BLAKE3, dedup, rename reassembly. None of this
  draws against any frame-phase budget. The CPU it consumes is
  bounded by the OS event rate (FSEvents emits at most ~10–100
  events per second under typical editor usage; bursts during
  `git checkout` of a large tree top out at ~10k events / second
  for ≤ 1 s, well within an entire OS thread's capacity even when
  BLAKE3-hashing every file).

### 9.2 Heap / sub-arena

SPEC §9.2 row 3 locks the watcher at **4 MiB** in the platform sub-
arena. This design partitions:

| Component                                | Bytes (approx) | Notes                                                                                                |
|------------------------------------------|---------------:|------------------------------------------------------------------------------------------------------|
| Per-instance pimpl (`FileWatcher::Impl`) | 4 KiB          | Vectors, atomics, the `RootEntry` table.                                                                                                      |
| `roots_` capacity                        | 8 KiB          | 14 `RootEntry` × ~512 B each (canonical root path + flags + ring index + small per-root metadata).                                            |
| Per-token SPSC rings                     | 3584 KiB       | **14 tokens × 256 KiB each = 3584 KiB = 3.5 MiB.** Dominant. Reduced from 16 to 14 to leave headroom for overhead components.               |
| `CanonicalPath` interner                 | 256 KiB        | Bounded; shared with the roots and with in-flight events. Old entries reclaimed when no live event references them.                            |
| BLAKE3 dedup LRU                         | 64 KiB         | 1024 `(canonical_path × 32 B hash)` slots; ring-replacement, no growth.                                                                      |
| Rename-reassembly buffer                 | 16 KiB         | At most ~64 inflight half-renames; bounded by debounce window × event rate.                                                                   |
| `snapshot_pool_` (`roots_snapshot_t` ×3) | ~2 KiB         | 1 active + 2 quarantined snapshots (§3.x). Each ~512 B (14 × `root_entry_t` ≈ 448 B, rounded). Absorbed within existing 256 KiB headroom.    |

Arithmetic: 4 + 8 + 3584 + 256 + 64 + 16 + 2 = 3934 KiB ≈ 3.84 MiB,
leaving ~254 KiB headroom within the 4 MiB sub-arena cell. The
`snapshot_pool_` addition (~2 KiB) is absorbed well within the
existing 256 KiB headroom; no arena budget amendment is required.
The headroom is intentional: it absorbs future overhead growth (e.g. a
widened dedup LRU, additional per-root metadata) without requiring an
arena budget amendment. The ring count was reduced from 16 to 14
(ring row previously read "16 × 256 KiB = 4 MiB exactly", leaving
zero room for interner + dedup + reassembly + pimpl overhead).
14 tokens covers all MVP consumer root-counts with margin (editor
content tree ≤ 10 roots + engine-asset trees ≤ 4; see §3.4 step 5).
The overflow behaviour for each sub-component is uniform: hitting the
bound returns `IoFailure { OsCode { ENOBUFS } }` with prefix
`"out-of-budget"` per `perf-budget.md` Allocator Rule #2 (strict
mode) or logs `warn`-once-per-frame (shipping mode, Rule #3).

### 9.3 Drain budget per frame

The consumer-side budget for `take_events` is **owned by the
consumer**, not by this aggregate. The hot-reload coordinator's
budget (`core::HotReloadBarrier`, phase 8, 0.40 ms reload-frame) is
the dominant consumer in MVP; it pays at most a 0-event SPSC
dequeue plus a small loop over any drained events. This design's
contribution is the structural guarantee that the SPSC dequeue is
O(N drained) with no surprise allocation, so the consumer's budget
math is honest.

### 9.4 BLAKE3 cost (off-thread, called out for awareness)

BLAKE3 on M1 is ~2 GiB/s single-threaded. A typical content-tree
edit hashes ≤ 10 MiB / second of touched bytes (humans don't save
more than a few hundred KiB per second sustained). Even pathological
`git checkout`-of-Linux-kernel-tier bursts (~1 GiB rehashed) take
~0.5 s wall on the I/O thread, during which the main thread is
unaffected because the I/O thread runs at a normal scheduler
priority and does not contend on shared locks.

If a future SPEC change wants to bound BLAKE3 work per second, the
knob is `kFSEventStreamCreateFlagFileEvents` (which fires more
events but at the file granularity we want) plus a per-second
rate-limiter on hash work in §3.6 step 3. That knob is not exposed
in MVP; SPEC §12 holds its trigger.

## 10. Failure modes

The watcher contributes to no new variants of `platform::Error` (SPEC
§4.7 closed-sum rule + §10.7 refusal "Hot-reload refusal […] Platform
contributes nothing new"). All failures route to existing arms.

### 10.1 Returnable arms by entry point (recap of SPEC §10.3.3)

| Entry point             | Returnable arms                                                                  | Trigger                                                                |
|-------------------------|----------------------------------------------------------------------------------|------------------------------------------------------------------------|
| `FileWatcher::create(Clock&)` | `IoFailure`, `PermissionDenied`                                             | FSEvents init failed; sandbox / TCC denial; thread spawn saturation     |
| `FileWatcher::watch`    | `Unsupported`, `NotFound`, `PermissionDenied`, `IoFailure` (prefix `"watcher-unavailable"`) | non-canonical / non-directory path; missing root; sandbox denial; FSEvents stream-create refused |
| `FileWatcher::unwatch`  | `NotFound`                                                                       | stale `WatchToken`                                                     |
| `FileWatcher::take_events` | `NotFound`, `Unsupported`                                                     | stale token; off-main-thread call                                      |

### 10.2 Specific named failure modes (per the spike brief)

The spike-issue task §10 calls out three named modes — `FSEventsStartFailed`,
`QueueOverflow`, `RootMissing`. These are *internal labels*; they map
to existing `platform::Error` arms as follows:

- **`FSEventsStartFailed`** → `IoFailure { OsCode { 0 } }` with TLS
  diagnostic prefix `"watcher-unavailable"`. Trigger: any of
  `FSEventStreamCreate`, `FSEventStreamScheduleWithRunLoop`, or
  `FSEventStreamStart` returning failure (FSEvents API uses Boolean
  returns + `kCFNotificationCenterEvent...` failures parsed into the
  `IoFailure` arm via the §10.2.3 NSException seam — although Core
  Services rarely raises `NSException`s and the more common shape is
  a Boolean false from `FSEventStreamStart`, which the bridge maps
  to the same arm with the explicit `"watcher-unavailable"` prefix).
  Caller follows SPEC §10.5 recovery: `unwatch`, wait one pump cycle,
  retry; on second failure, fall back to polling (consumer-side).
  Severity: `warn` (escalates to `error` when polling engages, per
  SPEC §10.6 row).
- **`QueueOverflow`** → SPSC ring at the per-token capacity. **Not
  surfaced as a `platform::Error` arm in MVP** — instead, the I/O
  thread drops the oldest event and increments a per-token overflow
  counter (§3.6 step 5). The next `take_events` call detects a
  non-zero counter and logs a rate-limited `warn`; the call's
  return is unaffected (it returns the count of valid events
  written). Rationale: SPEC §4.2 inv #3 "queues are bounded; full
  queue is fatal" explicitly applies to `EventQueue<T>` in the §4.2
  pump path (input / window events that sim cannot afford to lose),
  *not* to `FileEvent` (live-reload is best-effort under storm —
  the next `Created` event resurfaces the file). This is the one
  place this design diverges from a literal reading of §4.2-style
  fatality, and the divergence is documented here. Telemetry is
  preserved through the overflow counter.
- **`RootMissing`** → `NotFound` at `watch()` entry, before any OS
  call. Trigger: the supplied `CanonicalPath` resolves but does not
  exist (or has been deleted between canonicalisation and the
  `stat` check). The arm is the existing `platform::Error::NotFound`;
  no prefix is stamped because the caller branches on the typed arm
  alone.

### 10.3 Translation into `core::Error`

The hot-reload barrier (`core::HotReloadBarrier`) is the dominant
consumer of `FileEvent`. When a `FileWatcher::watch` / `take_events`
failure surfaces into the barrier's phase-8 logic, the barrier maps
the platform arm into its own surface per
`error-model.md` Composition Rule 2 ("Cross-context translation
happens at the call site that crosses the boundary"):

- `IoFailure { OsCode { 0 } }` with prefix `"watcher-unavailable"` →
  the barrier logs at `warn`, suppresses the reload (it has no signal
  to reload from), and continues with the previous-good plugins.
  This is *not* a `core::Error::HotReloadRefused` — there is no
  reload to refuse; there's just no signal.
- `NotFound` on `take_events` → consumer bug (token not reseated
  through the middleman); barrier logs at `error` and returns
  `core::Error::HotReloadRefused` for the affected plugin.
- `PermissionDenied` on `watch` → operator-actionable; barrier logs
  at `error`, surfaces through the editor UX, leaves previous-good
  plugins live.

The watcher itself never invents a `core::Error` arm; the translation
is the consumer's responsibility.

### 10.4 Unrecoverable shapes

- **I/O thread panic / abort.** `noexcept` discipline plus
  `-fno-exceptions` means an I/O-thread panic is `std::terminate`,
  which is the project-wide unrecoverable shape (`error-model.md`).
  Pre-`terminate` handlers may dump crash data via the future `obs`
  context.
- **Runaway BLAKE3 work.** Bounded by the I/O thread's CPU budget;
  no main-thread effect. If this becomes a problem in practice, the
  knob in §9.4 (rate-limit hash work) is the answer; not in MVP.
- **Sub-arena exhaustion.** Surfaces as `IoFailure { OsCode { ENOBUFS } }`
  with `"out-of-budget"` prefix per `perf-budget.md` Allocator Rule
  #2. Caller backs off (cancel an unused subscription) or the
  operator increases the platform budget (out of MVP scope).

## 11. Test plan

### 11.1 Unit tests (Catch2; under `tests/platform/watcher/`)

Each test name follows `glibre/platform/watcher: <invariant>` so it
maps directly to SPEC §11 acceptance criteria #356 / #357.

1. **`debounce_collapses_rapid_writes`** — write the same content to
   one file 100 times within a debounce window; assert exactly one
   `FileEvent::Modified` is delivered. Discharges SPEC §4.3 inv #2.
2. **`debounce_preserves_distinct_content_changes`** — write
   different content five times spaced one debounce window apart;
   assert five `Modified` events with the right timing. Discharges
   the inverse of inv #2.
3. **`content_hash_filters_metadata_only_events`** — `touch` a file
   without changing its content; assert no event is delivered.
   Discharges SPEC §4.3 inv #2 + harmonius R-14.6.6.
4. **`canonicalisation_collapses_aliased_paths`** — subscribe a
   symlink-pointed root; trigger an event under the target; assert
   the delivered `CanonicalPath` is the canonical (target) form.
   Discharges SPEC §4.3 inv #1.
5. **`rename_reassembled_within_window`** — `mv` a file across a
   subdirectory boundary inside the same root; assert exactly one
   `Renamed { from, to }` is delivered (no `Deleted` + `Created`
   pair). Discharges SPEC §4.3 inv #3.
6. **`rename_emits_pair_when_reassembly_window_lapses`** — `mv` a
   file but inject a delay larger than the debounce window between
   the OS halves (test fixture forces this); assert separate
   `Deleted` and `Created` events. Tests the conservative branch of
   inv #3.
7. **`token_invalidated_after_unwatch`** — `unwatch(token)`,
   then call `take_events(token, …)`; assert `NotFound`.
8. **`stale_token_after_recreate`** — token zero is invalid;
   assert `take_events(WatchToken{0}, …)` returns `NotFound`.
9. **`subscription_capacity_overflow_reports_out_of_budget`** —
   subscribe more roots than the sub-arena admits (test-only
   shrink); assert `IoFailure` with `"out-of-budget"` prefix.
10. **`spsc_ring_overflow_drops_oldest_and_logs_warn`** — exhaust
    the per-token ring without draining; assert subsequent events
    push out the oldest and the next `take_events` reports the
    overflow counter.
11. **`take_events_bounded_by_output_span`** — push 100 events,
    drain into a `span` of size 10; assert exactly 10 events
    written; remaining 90 still drainable on the next call.
12. **`watch_rejects_non_canonical_path`** — construct a forged
    non-canonical `CanonicalPath` (test-only ctor); assert
    `Unsupported` from `watch`. Discharges SPEC §4.3 inv #1's
    refusal arm.
13. **`watch_returns_not_found_on_missing_root`** — supply a path
    that resolved canonically but does not exist on disk; assert
    `NotFound`.
14. **`watch_returns_unsupported_on_file_root`** — supply a regular
    file; assert `Unsupported`.
15. **`watch_returns_permission_denied_on_eperm_from_stat`** —
    Test 15 uses a test-local `fileio_ops_t` interface — defined in
    `tests/platform/file-watcher/test_helpers.hpp` — that wraps the
    underlying `stat()` / `open()` syscalls. The production
    `FileWatcher` does not export this interface; it is a test-only DI
    seam injected via a test-only constructor
    `FileWatcher::create_for_test(Clock&, fileio_ops_t&)`. The
    `_for_test` factory is gated by `#ifdef GLIBRE_TESTING_HOOKS` and
    is never present in shipping builds. `fileio_ops_t` is owned by
    the test helpers, not by `FileIo` or by `FileWatcher`: `FileIo`
    owns production filesystem operations; tests own their own mock
    interface — defining it test-local resolves the SRP concern
    cleanly. The test configures the mock to return `EPERM` for the
    target path and asserts the design routes this through the error
    translator → `platform::Error::PermissionDenied` (per
    `platform-error-design.md` §3.3 admission-gate mapping). No
    sandbox profile required; runs in standard unsigned Catch2 CI
    without entitlement-based code signing.
16. **`destructor_releases_all_streams_synchronously`** — subscribe
    14 roots (the MVP capacity, per §9.2), drop the `FileWatcher`;
    assert no FSEvents stream leak via the `lsof`-equivalent test
    fixture probe. Discharges SPEC §4.3 inv #4.
17. **`io_thread_does_not_appear_on_main_thread_stack`** — exercise
    a long-running BLAKE3 hash on a large file; assert main-thread
    blocking time stays at 0 ms (within precision). Discharges SPEC
    §4.3 inv #5.
18. **`take_events_off_main_thread_returns_unsupported`** — call
    from a worker thread; assert `Unsupported`.
19. **`subscription_capture_hook_returns_live_roots`** — friend-test
    of the §3.10 capture hook; assert the snapshot matches the
    set of `watch` calls.
20. **`stop_request_terminates_io_thread_within_bound`** — set
    `stop_requested_`, signal the runloop; assert thread joins
    inside 100 ms. Discharges §3.9 step 4.

### 11.2 Integration tests

Under `tests/platform/watcher/integration/`. These exercise the
full create / modify / delete / rename lifecycle against the real
FSEvents service on macOS:

1. **`full_lifecycle_create_modify_delete`** — subscribe a temp
   directory; create a file, write to it twice (separated by a
   debounce window), delete it; assert the delivered event sequence
   is `Created`, `Modified`, `Deleted`. Discharges SPEC §4.3 inv #1
   + #2 + harmonius R-14.6.5.
2. **`full_lifecycle_rename_within_root`** — create then rename a
   file inside the watched root; assert one `Renamed`.
3. **`full_lifecycle_rename_across_roots`** — subscribe two roots,
   rename a file from one to the other; assert one `Deleted`
   under the source-root token and one `Created` under the
   destination-root token (renames *only* reassemble inside one
   root; cross-root moves surface as a delete/create pair). Locks
   the design semantic.
4. **`churn_storm_300_events_per_second_for_5_seconds`** — fire
   1500 file modifications uniformly; assert no event-loss errors,
   the SPSC overflow counter stays bounded, and the dedup LRU
   collapses redundant content correctly. Mirrors harmonius
   R-14.6.5 verification ("Perform 100 rapid writes; verify
   debounce coalesces events") but at 15× the rate.
5. **`recursive_subscription_includes_nested_subdirectory_creates`** —
   subscribe `/tmp/glibre-watch-X`, create `/tmp/glibre-watch-X/a/b/c.txt`;
   assert the resulting `Created` is delivered with the nested
   canonical path.
6. **`watcher_unavailable_on_unmount_engages_recovery_path`** —
   subscribe a mounted disk image, eject the volume; assert next
   `take_events` reports the surface error path (`watcher-unavailable`
   diagnostic prefix observed via the test-only TLS hook); call
   `unwatch` and verify it returns `NotFound` (already torn down).
   Discharges SPEC §10.5 steps 1-2.
7. **`platform_self_reload_replays_created_for_existing_files`** —
   subscribe a directory containing 10 files, trigger a synthetic
   platform self-reload via the §8.7 test hook
   (`enqueue_platform_reload`), re-subscribe in the new instance,
   assert 10 fresh `Created` events arrive; assert dedup against an
   immediate second re-subscribe collapses correctly. Discharges
   SPEC §8.3 step 4.3.

### 11.3 E2E coverage

The user-stories #356 and #357 (SPEC §11) drive the closure gates.
Each story carries:

- Manual test script (per AGENTS.md) — operator runs the script
  against a real macOS dev machine.
- E2E `.glibre-trace` — replay-driven test under `tests/e2e/platform/
  watcher/`. The trace fixture creates a temp directory, performs a
  scripted sequence of file operations, and asserts the resulting
  `FileEvent` stream byte-equals the golden trace.

E2E green in CI is the prerequisite for manual testing per the
project workflow; manual PASS is the prerequisite for closing the
story. Neither this design nor the spike issue closes those stories.

## 12. Open questions

- `[BLOCKING IMPLEMENTATION]` **`FileWatcher::create()` Clock injection.**
  `create()` must take `Clock& clock` as its sole parameter (SPEC §4.4
  inv #5). The §5.8 stub in `specs/platform/SPEC.md` must be amended to
  reflect this signature before the first plan PR that implements this
  aggregate. The amendment is a one-line change to the stub (`create()`
  → `create(Clock&)`). Two concrete callers exist in MVP: the editor
  hot-reload coordinator and the shipping runtime asset reload path;
  both already hold a `Clock` reference from their own injection chain.
  The amendment spike must land before any plan PR.

- `[NON-BLOCKING]` **Ring count set to 14 (reduced from 16).**
  The §9.2 table was updated to 14 tokens × 256 KiB = 3584 KiB,
  leaving ~256 KiB headroom within the 4 MiB cell. The headroom
  protects against future overhead growth (e.g. dedup LRU widening,
  additional per-root metadata). If a future consumer requires more
  than 14 simultaneous roots, the §9.2 arithmetic must be re-evaluated
  and the arena budget amended.

- `[OPEN]` **Surface a runtime debounce-window knob.** MVP fixes
  the FSEvents `latency` argument at 100 ms (§3.6). A post-MVP
  editor-side workflow (rapid asset iteration) may want to lower
  this to ≈ 30 ms; a long-running automated build pipeline may
  want to raise it to ≈ 500 ms. Owner: editor / hot-reload
  coordinator spike (the same gate that re-opens `WatcherUnavailable`
  arm promotion per SPEC §10.8).

- `[OPEN]` **Promote `WatcherUnavailable` from diagnostic prefix to
  first-class `platform::Error` arm.** SPEC §10.8 already records
  the trigger ("a second consumer beyond the editor / hot-reload
  coordinator that wants to branch on watcher loss without parsing
  the prefix string"). Re-evaluated when the editor's content-tree
  hot-reload coordinator lands.

- `[OPEN]` **Sub-arena sizing audit at 4 MiB.** SPEC §9.6 records
  the provisional 4 MiB number. This design partitions it (§9.2);
  a measurement under realistic editor + content-tree workloads
  should validate or amend. Owner: post-MVP perf-budget review.

- `[OPEN]` **Configurable per-token SPSC ring depth.** Current
  default 4096 slots is uniform across all tokens. A consumer that
  monitors a small, low-traffic engine-asset tree may want to drop
  the depth (saving ~250 KiB) and re-allocate the slack to a
  high-traffic editor content tree. No consumer requests this in
  MVP; deferred.

- `[OPEN]` **kqueue / inotify / `ReadDirectoryChangesW` ports.** SPEC
  §6.4 already names the file-organisation pattern
  (`watcher/backends/<os>.cpp`, compile-time selection). Bodies are
  out of scope for MVP (macOS-only). Re-opened when the engine
  expands to a non-macOS host.

- `[OPEN]` **BLAKE3 rate-limit on the I/O thread.** §9.4 calls out
  the path; deferred until a measured workload justifies the knob.
