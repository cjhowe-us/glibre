# platform — Detailed Design: process aggregate

> Detailed design for the `Process` / `Argv` / `Env` / `ExitCode` /
> `SignalHandler` aggregate declared in `specs/platform/SPEC.md` §4.5.
> Refines §4.5, §5.10, §6.7 (POSIX argv / env / signals seam), §6.8
> (threading topology — the lone signal-handler-table mutex), §6.9
> (allocation discipline — boot-only), §8.3 (platform self-reload
> signal capture / re-install clauses), §9.1 (process per-frame budget
> = 0), §9.2 (process heap sub-arena = 256 KiB), §10.3.5 (process
> failure arms).
>
> Cites `reviews/decisions/error-model.md`,
> `reviews/decisions/frame-phases.md`,
> `reviews/decisions/perf-budget.md`,
> `reviews/decisions/plugin-abi.md`,
> `reviews/decisions/hot-reload-protocol.md`,
> `reviews/decisions/fory-codegen.md`. Does not introduce any new
> public surface beyond the §5.10 stubs in `specs/platform/SPEC.md`;
> deviations from those records require an amendment spike, not an
> in-place edit.
>
> Harmonius prior art (`harmonius/docs/requirements/platform/
> crash-reporting.md` — the in-process signal-handler stub of
> R-14.4.1; the out-of-process monitor R-14.4.7 / R-14.4.2 /
> R-14.4.3 are explicitly **refused for MVP** per SPEC §1, §2 row
> "in-process signal-safe stub is the only platform surface") cited
> as research input only — every conclusion below was independently
> re-derived per `PHILOSOPHY.md`.

Refs: spike #725 — `[SPIKE] design-platform-process-detailed`.
Parent: #714. Sibling task-breakdown spike blocked-by this
deliverable.

## 1. Purpose

The `process` aggregate is the single component in the platform
context permitted to call POSIX process-identity / process-control
syscalls — `_NSGetArgv` / `_NSGetEnviron` / `getcwd` /
`_NSGetExecutablePath` / `getpid` / `sigaction` — and the sole owner
of the engine's exit-code latch and signal-handler installation
table. Its one responsibility is **freezing the running engine
process's identity at boot, exposing it as read-only snapshots, and
mediating signal-handler installation against the OS** (SPEC §4.5).
Argv / env / cwd / executable_path / pid / set_exit_code /
install_signal / uninstall_signal share an aggregate because they
share a lifetime (the OS process) and because changing one
(installing SIGSEGV for a crash dump) interacts with another (the
exit-code path the engine returns through `main`); splitting them
would scatter the OS-process-control contract across modules that
must stay in sync.

What this aggregate explicitly refuses to own:

- **Subprocess spawning.** No `posix_spawn` / `fork+exec` / pipe-stdio
  / wait-for-exit surface lives in this aggregate. Tools that need
  to invoke external binaries (`tools/glibre-cook`,
  `glibre-codegen`, `glibre-foryc`) are themselves separate
  binaries launched directly by the editor through the OS shell,
  not via an in-engine spawn API. If a future spike argues for an
  engine-side spawn surface, it lands as a *new* aggregate in the
  `tools` context (or its own `subprocess` context), not as growth
  of `Process`. SRP rationale: spawn / wait / pipe / kill is "OS
  process *creation* contract", a different reason to change from
  "OS process *identity / signals* contract".
- **Out-of-process crash-reporter monitor.** R-14.4.7 (separate
  monitor binary capturing dumps over a pipe) is **refused for MVP**
  per SPEC §1, §2 row, §3 refusal list ("out-of-process monitor
  binary lifecycle (R-14.4.2, R-14.4.3, R-14.4.7) → defer.
  In-process signal-safe stub is the only platform surface").
  The aggregate's signal-handler table is the entire crash-reporting
  surface platform contributes; a future monitor binary would be a
  separate plugin, not an extension of `Process`.
- **Crash-dump writing / minidump format / symbol upload /
  clustering.** R-14.4.1 / R-14.4.2 / R-14.4.3 / R-14.4.4 /
  R-14.4.8. Out of MVP scope. The signal-handler trampoline calls
  the user-supplied `SignalHandlerFn`; it does not embed dump-
  writing logic. R-14.4.4 structured logging is owned by `core/log`,
  not platform.
- **Window / surface / DPI** (SPEC §4.1, sibling aggregate #715).
- **Event pumping** (SPEC §4.2, sibling aggregate #717).
- **File watching** (SPEC §4.3, sibling aggregate #719).
- **Clock** (SPEC §4.4, sibling aggregate #723).
- **File I/O** (SPEC §4.6, sibling aggregate #721).
- **Platform error policy** (SPEC §4.7, §10, sibling aggregate
  #727). This design surfaces failures into pre-existing
  `platform::Error` arms (`AlreadyExists`, `NotFound`, `Unsupported`)
  via the §6.10 `errno_to_error` translator; it does not invent or
  rename arms.
- **Threading / job graph / async runtime.** R-14.3.* threading-
  async requirements are owned by `core` scheduling decisions; the
  process aggregate is single-threaded for installation
  (main-thread-only) and async-signal-context for delivery, with no
  thread pool of its own. The lone mutex (§6.8) guards the handler
  table against the contract-violating off-main-thread install
  case.

The aggregate's SRP boundary is sharp: if the macOS POSIX
process-identity contract shifts (a new `_NSGet*` deprecation,
notarization rule that forbids `sigaction` on certain signals,
sandbox edit that changes `getcwd` semantics, app-extension
restriction on exit-code reporting), this design changes. Anything
else — argv parsing into typed flags, env-var schema validation,
shell-interpolation of cwd, crash-dump format, subprocess control
— is out of scope and routed elsewhere.

## 2. Requirements coverage

Mapping of harmonius requirements that touch process identity /
signals / exit / argv / env onto MVP coverage in this aggregate.
The harmonius coverage source for this aggregate is
`harmonius/docs/requirements/platform/crash-reporting.md` (R-14.4.*
in-process signal handler is the only direct overlap; everything
else in that file is refused or routed to other contexts) plus
incidental references in `os-integration.md` /
`platform-services.md` / `threading-async.md` /
`sdk-integration.md` (none of which add process-identity
requirements). Every entry is independently re-derived; coverage
sites refer to sections of `specs/platform/SPEC.md` and to the
design sections below.

| Harmonius clause                                                              | Glibre disposition (MVP)                                                                                                                                                                                                                                                                                                                                                                  |
|-------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **R-14.4.1** process-wide crash handler intercepts unhandled exceptions, segfaults, aborts; writes platform-native dump | **Partial — signal-handler installation seam only.** The aggregate exposes `Process::install_signal(Signal::{SegFault, BusError, IllegalInst, FpError, ...}, fn)` (SPEC §5.10). The dump-writing body is **the caller's responsibility**, supplied as the async-signal-safe `SignalHandlerFn` (§4.5 inv #4). MVP ships an empty caller (the eventual `core/crash` plugin); the platform's contribution stops at "give me a slot to install into and a trampoline that calls back". §3.4. |
| **R-14.4.2** debug symbol upload during build                                 | **Refused (out of MVP, out of platform).** Build-pipeline tooling, not a runtime aggregate. SPEC §1, §2 row.                                                                                                                                                                                                                                                                              |
| **R-14.4.3** crash-report clustering / alerting service                       | **Refused (out of MVP, out of platform).** Server-side aggregation; not a runtime aggregate.                                                                                                                                                                                                                                                                                              |
| **R-14.4.7** out-of-process monitor binary captures dumps via pipe / socket   | **Refused for MVP.** SPEC §1 explicitly refuses "out-of-process monitor binary lifecycle … In-process signal-safe stub is the only platform surface". A future monitor binary lives outside `process/` (separate context or `tools` plugin); the aggregate would gain no new surface. §3.4 refusal note.                                                                                  |
| **R-14.4.8** retain N most recent crash dumps + metadata attachment           | **Refused (out of platform).** Dump rotation / metadata is a `core/crash` policy concern, not process identity.                                                                                                                                                                                                                                                                          |
| **R-14.4.4** structured-log async ring buffer                                 | **Refused (out of platform).** Owned by `core/log`. The signal-handler trampoline (§3.5) is logging-free by construction (logging is not async-signal-safe).                                                                                                                                                                                                                              |
| **R-14.4.5 / .6 / .9 / .10 / .11** counters / GPU breadcrumbs / log filters / OS-native log + profiler sinks | **Refused (out of platform).** Not process-identity. Routed to `core/log`, `render`, `tools/profiler`.                                                                                                                                                                                                                                                                                    |
| **R-14.3.6** Tokio `current_thread` runtime                                   | **Refused (collapsed).** `Process` is single-threaded for installation; no async runtime. The engine's whole-engine driver model is single-threaded for MVP per SPEC §6.8.                                                                                                                                                                                                                |

Glibre-native requirements added beyond harmonius (re-derived from
SPEC §4.5 / §5.10 / §6.7):

- **Argv / env / cwd / executable_path are read-only snapshots
  captured once at `main()`-shim entry.** SPEC §4.5 inv #1. No
  caller may mutate. Out-of-band `setenv` / `chdir` are
  unsupported (calling them from engine code is undefined
  behaviour the aggregate refuses to detect; calling them from
  third-party libraries — SDL3, FBX SDK — is a known risk
  re-mitigated only by the snapshot-once rule). §3.2.
- **Exit code is set, never read back.** SPEC §4.5 inv #2.
  `set_exit_code(int)` writes to a `std::atomic<int>` consumed by
  the `main()` shim on return. There is no getter. §3.6.
- **At most one engine-side handler per signal.** SPEC §4.5 inv
  #3. Duplicate install is `AlreadyExists`. The aggregate does not
  chain handlers (forwarding to a prior `sa_handler` is a feature
  that would let one consumer mask another's crash dump silently;
  the design refuses). §3.5.
- **Signal handlers are async-signal-safe.** SPEC §4.5 inv #4.
  The aggregate documents the contract; it does not enforce it
  beyond compile-time `noexcept` on the `SignalHandlerFn` typedef
  and a runtime-debug aid (§3.5). Calling unsafe functions from
  the handler is the caller's bug.
- **Single-instance.** SPEC §4.5 inv #5. `Process::get()` returns
  the singleton; no public constructor. Construction (the static
  initializer that fills the singleton) refuses if invoked twice
  in one process — a programming error, asserted in debug,
  defensively-aborted in release.
- **No Obj-C++ in `process/`.** CLAUDE.md "No Obj-C++ in engine
  code". The bridge file rule (`surface/bridge.mm` is the lone
  `.mm` in the engine) means `process/process_macos.cpp` calls
  `_NSGetArgv` / `_NSGetEnviron` / `_NSGetExecutablePath` through
  their plain C function declarations from `<crt_externs.h>` —
  these are C ABIs even though they sit under "NS" naming. Verified
  in §3.10.
- **Boot-only allocation.** SPEC §6.9, §9.2, §9.3. Argv / env
  copies are made into the 256 KiB sub-arena during
  `Process::init()`; after init returns, no aggregate function
  allocates. `argv()` / `env()` / `cwd()` / `executable_path()` /
  `pid()` are pure read views. `set_exit_code` is a relaxed
  atomic store. `install_signal` / `uninstall_signal` mutate a
  fixed-size handler table (no allocation).

## 3. Detailed model

### 3.1 Module layout

The `process/` module is one of the seven sibling modules under
`engine/platform/src/` (SPEC §6.1). Files:

```
engine/platform/src/process/
  process.cpp                  // host-agnostic glue (singleton, table, init/shutdown)
  process_macos.cpp            // POSIX + _NSGet* — the lone host-specific TU
  signal_trampoline.cpp        // async-signal-safe trampoline; built with no_sanitize_address
  detail/handler_table.hpp     // fixed-size [Signal × SignalHandlerFn] table
  detail/snapshot_arena.hpp    // 256 KiB boot-only arena for argv/env/cwd/exepath copies

engine/platform/include/glibre/platform/process/
  process.hpp                  // §5.10 surface — class Process, enum Signal, SignalHandlerFn
```

Public header projection: `engine/platform/include/glibre/platform/
platform.hpp` (SPEC §5) re-exports `process.hpp`. No new public
type beyond the §5.10 stubs.

`signal_trampoline.cpp` is a separate translation unit because
`__attribute__((no_sanitize_address))` (per SPEC §6.7) needs to
attach to the *function body*, and isolating it in its own TU also
isolates it from any future TU-level sanitizer pragmas. The body is
small (one indirect call into the table, one `errno`-preservation
prologue/epilogue) and lives in its own `.text` segment so Address
Sanitizer / Undefined Behaviour Sanitizer instrumentation cannot
corrupt the signal-safety guarantee.

### 3.2 Boot snapshot (argv / env / cwd / executable_path / pid)

Captured exactly once during `Process::init()`, called from the
engine's `main()` shim before any other engine subsystem
constructs. The shim is the single `main` in the engine binary
(`runtime/src/main.cpp` for the shipping runtime;
`tools/glibre-editor/main.cpp` for the editor). Both shims forward
to `glibre::platform::Process::init(argc, argv)` as their first
non-trivial statement.

Snapshot policy:

1. **Argv.** `_NSGetArgv()` returns `char***`; `_NSGetArgc()`
   returns `int*`. The aggregate copies each `argv[i]` (UTF-8,
   NUL-terminated) into the 256 KiB sub-arena and stores
   `eastl::string_view`s pointing into the arena copies into a
   contiguous `eastl::span` member. `init` parameters
   `(argc, argv)` are accepted as a fallback for platforms where
   `_NSGet*` does not exist (future Linux port reads `__libc_argv`
   / `environ`; Windows reads `__argc` / `__wargv` then decodes
   UTF-16→UTF-8). The MVP implementation prefers `_NSGet*` even
   when the parameters are passed, because the shim's `argv`
   pointer is already a copy on Apple platforms and `_NSGetArgv()`
   reaches the canonical pre-shim values.
2. **Env.** `_NSGetEnviron()` returns `char***`. Iterate
   `KEY=VALUE` strings until the terminating `nullptr`; for each
   entry, copy the full `KEY=VALUE` byte sequence into the
   sub-arena and split on the *first* `=` to populate a
   `(view-key, view-value)` pair into a small flat sorted vector
   for `O(log N)` `env(name)` lookups. Duplicate keys (legal in
   POSIX) are resolved by **first-wins**. The aggregate matches
   POSIX `getenv` semantics on macOS / glibc / musl, all of which
   scan `environ` top-to-bottom and return the first matching
   entry. (Shells such as bash/zsh typically apply last-wins when
   assigning variables, but `getenv()` reads first-wins — the
   design aligns with C library behavior, not shell assignment
   behavior.) Engine code that previously called `getenv` directly
   can be migrated mechanically.
3. **CWD.** `getcwd(buf, PATH_MAX)` once; copy the byte sequence
   into the sub-arena; canonicalize via
   `CanonicalPath::from_absolute(view)` (SPEC §5.2). Store the
   resulting `CanonicalPath`. If `getcwd` returns
   `nullptr`/`ENOENT` (parent was rmdir'd before `init`), the
   aggregate stores a sentinel `CanonicalPath{}` and logs at
   `warn`. The engine continues; later code that requires a real
   cwd (e.g. `FileIo::read_all` with a relative-prefixed path)
   will surface the failure at its own boundary.
4. **Executable path.** `_NSGetExecutablePath(buf, &len)` once;
   `realpath(buf, resolved)` to dereference any
   `dyld`-relative path; copy `resolved` into the sub-arena;
   canonicalize. This is the same primitive used by SDL3's
   `SDL_GetBasePath`; we *do not* call SDL3 because that would
   create a init-order coupling between `Process` and the SDL3
   subsystem (`Window`'s sibling), and `Process::init()` runs
   first.
5. **PID.** `getpid()` once. `std::uint32_t` fits 32-bit POSIX
   pids (macOS caps at `PID_MAX = 99999`); the SPEC's
   `std::uint32_t` return type (§5.10) is platform-agnostic and
   matches Windows' `GetCurrentProcessId` width too.

After `init` returns, the snapshot is **immutable**. Concurrent
reads from any thread are safe (the only writer was the boot-time
shim; subsequent threads see the post-init state through the
acquire-semantics of normal C++ static-initialization rules
combined with the Meyer's-singleton accessor).

The 256 KiB sub-arena (SPEC §9.2) is sized for: argv ≤ 56 KiB
(typical engine invocations are dozens of args, but the editor may
pass long content paths; ARG_MAX is 256 KiB combined argv+env, and
56+(128+8+32) = 224 KiB env+argv envelope leaves 32 KiB headroom
under ARG_MAX), env ≤ 128 KiB (macOS caps `getconf ARG_MAX = 256
KiB` for argv+env *combined*; we reserve roughly half for each),
cwd + executable_path ≤ 8 KiB (PATH_MAX ≈ 1024 each, with slack
for canonicalization), key→value flat-vector index ≤ 32 KiB
(~1024 env entries × 2 spans × 16 B; sized against measured macOS
environment populations). If any individual component overflows its
slice the aggregate aborts at boot — argv / env that exceeds 256
KiB is a sandbox configuration the engine cannot represent without
reshaping its discipline; failing fast is correct.

### 3.3 Singleton accessor + init / shutdown lifecycle

```cpp
namespace glibre::platform {
class Process {
public:
    [[nodiscard]] static Process& get() noexcept;
    // ... SPEC §5.10 surface ...
};
}
```

`Process::get()` is a Meyer's-style singleton: the static local
inside `get()` is constructed on the first call and destroyed at
program exit. **However**, the construction body is *empty*; the
singleton's interior state is filled by `Process::init(argc,
argv)`, called explicitly from the `main()` shim (§3.2). The
two-phase pattern is necessary because:

- Meyer's-singleton construction is invisible to the call site;
  letting it walk `_NSGet*` / `getcwd` / `getpid` from a static-
  init context would violate the "snapshot at known program
  point" invariant (SPEC §4.5 inv #1) and would race with any
  other static initializer that happened to call `Process::get()`
  before `main`.
- The shim controls the program point. `init` is idempotent across
  *thread* calls but **refuses double-init across program
  invocations**: the second `init` call returns `Result<void>`
  with `Error::AlreadyExists` (SPEC §4.5 inv #5 — second-instance
  is a programming error). Debug builds `std::abort()`; release
  builds log `error` and return the typed failure.

Shutdown: program exit. The singleton's destructor releases the
sub-arena (256 KiB returns to the platform `ContextTag` allocator)
and uninstalls every signal handler still installed (calling
`sigaction` with `SIG_DFL` for each occupied table slot). The
ordering — uninstall *then* free arena — matters: a signal
delivered after the table is gone but before the trampoline is
detached would dereference torn memory. We tear down the OS
subscription first (post-condition: no further signal can reach
our trampoline), then free the arena.

### 3.4 Signal-handler installation seam

```cpp
// SPEC §5.10 — extracted for reference.
using SignalHandlerFn = void (*)(int signal) noexcept;

enum class Signal : std::uint8_t {
    Interrupt,    // SIGINT
    Terminate,    // SIGTERM
    SegFault,     // SIGSEGV — install only for crash dumps.
    BusError,     // SIGBUS
    IllegalInst,  // SIGILL
    FpError,      // SIGFPE
};
```

Six signals are exposed. The closed enum is deliberate: SIGKILL /
SIGSTOP / SIGCONT / SIGCHLD / SIGUSR1 / SIGUSR2 / SIGPIPE /
SIGALRM / SIGHUP are *not* installable through this aggregate.
Rationale per signal:

- **SIGKILL / SIGSTOP** are not catchable by the OS; exposing them
  would mislead callers.
- **SIGCHLD** is for processes that fork children; the engine
  refuses subprocess spawn (§1) so SIGCHLD has no engine consumer.
- **SIGUSR1 / SIGUSR2** are sometimes used as "graceful reload"
  triggers; the engine's hot-reload (`reviews/decisions/
  hot-reload-protocol.md`) is in-process and does not need a
  signal trigger. Adding them later is a strict expansion of the
  enum and a §4.7 / §10 audit, not silently routable through a
  catch-all install.
- **SIGPIPE** would surface from network or stdio writes the
  engine does not do in MVP. Routed to OS default (write returns
  `EPIPE`, which `FileIo` translates to `IoFailure`) rather than
  installed.
- **SIGALRM / SIGHUP** are timer / terminal-disconnect events the
  engine does not subscribe to (the engine has no controlling
  terminal in shipping; the editor's terminal-attach case can
  lift this restriction later).

The closed enum is **the** SRP boundary. Adding a new entry is a
deliberate spec edit (SPEC §5.10), not an API growth.

### 3.5 Signal trampoline + handler table

The handler table is a fixed-size array indexed by `Signal`:

```cpp
struct HandlerSlot {
    std::atomic<SignalHandlerFn> fn{nullptr};   // nullptr = unused.
    int                          posix_signum{0}; // SIGINT, SIGSEGV, ...
};
constexpr std::size_t kSignalCount = 6;
HandlerSlot g_handler_table[kSignalCount];
std::mutex  g_install_mutex;   // guards install/uninstall, not delivery.
```

The trampoline (in `signal_trampoline.cpp`):

```cpp
extern "C" [[gnu::no_sanitize_address]]
void glibre_signal_trampoline(int signum, siginfo_t* /*info*/, void* /*ucxt*/) noexcept {
    // Save errno across the handler call (signal-safety: handlers must not
    // corrupt errno from the interrupted syscall path).
    int saved = errno;

    for (std::size_t i = 0; i < kSignalCount; ++i) {
        auto& slot = g_handler_table[i];
        if (slot.posix_signum == signum) {
            // Acquire-load: we observe the most-recent install-side store.
            auto fn = slot.fn.load(std::memory_order_acquire);
            if (fn != nullptr) {
                fn(signum);   // user-supplied async-signal-safe body
            }
            break;
        }
    }

    errno = saved;
}
```

Properties:

- **No allocation, no locking.** The mutex is install/uninstall-side
  only; the trampoline reads `posix_signum` (which is never
  rewritten after the slot is bound to its signum at first install)
  and `fn` (atomic acquire load). No path through the trampoline
  can deadlock against the install mutex, because the trampoline
  never tries to acquire it.
- **`errno` preservation.** The trampoline saves and restores
  `errno` so a SIGINT delivered mid-`read(2)` does not corrupt the
  interrupted syscall's error code from the user's body's POV.
  (User bodies are still expected to be async-signal-safe; this
  defends against the most common accidental violation.)
- **Iteration cost.** Six slots, branch-predictable. The runtime
  cost is a few cycles even on a hostile signal storm — well within
  the "delivery is async, no per-frame budget" rule (SPEC §9.1
  row).
- **No chaining.** The for-loop iterates table slots looking for
  this signum, but only ever calls *one* `fn`. There is no
  fall-through to `SIG_DFL` or to any prior handler. Chaining
  would silently mask installation conflicts; SPEC §4.5 inv #3
  forbids it explicitly.

`install_signal(Signal s, SignalHandlerFn fn)`:

**Pre-condition (caller obligation — applies to `glibre_plugin_register`):**
Plugin's `glibre_plugin_register` MUST call
`glibre::platform::detail::error::pre_touch_all()` BEFORE invoking
`Process::install_signal`. `pre_touch_all()` pre-touches the three
TLS slots (prefix pointer, prose buffer, source_tag pointer — per
`platform-error-design.md` §9.2) so that the lazy linker stub
resolver for those `__thread` variables is resolved on the calling
thread before any signal can fire. If the resolver has not run and
a signal fires first, the lazy stub itself is not async-signal-safe —
UB on macOS. See `platform-error-design.md` §6.2 for the mandatory
ordering contract.

1. Lock `g_install_mutex`.
2. Find the slot for `s`; if its `fn` is non-null,
   `unlock + return Error::AlreadyExists`.
3. Otherwise, store `posix_signum = posix_for(s)` if zero (first
   install for this signum binds the slot), then atomic-release
   store `fn` into the slot.
4. Build a `struct sigaction` with:
   - `sa_sigaction = &glibre_signal_trampoline`
   - `sa_flags     = SA_SIGINFO | SA_RESTART | SA_ONSTACK`
   - `sigemptyset(&sa_mask); sigaddset(sa_mask, posix_for(s))`
     (mask the same signal during handler execution to avoid
     re-entrancy).
5. Call `sigaction(posix_for(s), &sa, prev /* discarded */)`. If
   the syscall returns `-1`, errno-translate via §6.10
   (`errno_to_error`) — likely `Unsupported` for `EINVAL` (signal
   not installable on this OS, e.g. SIGKILL), `IoFailure` for the
   rare unrecognized errno. Roll back the slot's `fn` to `nullptr`
   on failure, before unlocking.
6. Unlock; return `Result<void>{}`.

`uninstall_signal(Signal s)`:

1. Lock `g_install_mutex`.
2. Find the slot for `s`; if its `fn` is null, unlock and return
   `Error::NotFound` (SPEC §10.3.5).
3. Atomic-release store `fn = nullptr`.
4. Build a `struct sigaction` with `sa_handler = SIG_DFL`,
   `sa_flags = 0`, empty mask. Call `sigaction(posix_for(s), &sa,
   prev)`. Failure here logs `error` but still returns
   `Result<void>{}` — the engine-side slot is cleared, and a
   `SIG_DFL` reinstall failure is rare (only `EINVAL` on
   un-resettable signals, which the closed enum already excludes).
5. Unlock.

The mutex is the lone synchronization primitive in the entire
`process/` module (SPEC §6.8). It exists so that two engine plugins
that race on installing the same signal (a contract violation —
install is main-thread-only by convention) produce a deterministic
`AlreadyExists` for the loser rather than a torn write that
orphans one handler.

`SA_ONSTACK` requires an alternate signal stack; the aggregate
allocates a **32 KiB** `sigaltstack` once during `init` (in the same
sub-arena, so it counts against the 256 KiB ceiling — fits exactly
within 256 KiB sub-arena per §9) and calls `sigaltstack(2)`.
32 KiB equals MINSIGSTKSZ on macOS 26 Apple Silicon (defined in
`<sys/signal.h>`); using a smaller value causes `sigaltstack(2)` to
return `EINVAL`, leaving `SA_ONSTACK` unconfigured — exactly the
re-fault failure §3.5 warns about. The sigaltstack budget is pinned
at MINSIGSTKSZ; argv was trimmed from 64 KiB to 56 KiB to keep the
§9 sub-arena at ≤ 256 KiB. The alternate stack is critical for
SIGSEGV on stack overflow: the default-stack handler would re-fault.
The alt-stack lives for the program's lifetime and is freed by the
singleton's destructor.

### 3.6 Exit-code latch

```cpp
inline std::atomic<int> g_exit_code{0};

void Process::set_exit_code(int code) noexcept {
    g_exit_code.store(code, std::memory_order_relaxed);
}
```

Read once, by the `main()` shim, just before `return`:

```cpp
// runtime/src/main.cpp (shim, illustrative)
int main(int argc, char** argv) {
    glibre::platform::Process::init(argc, argv).value();
    int rc = glibre::run_engine();   // engine body; may call set_exit_code along the way
    int latched = glibre::platform::detail::take_exit_code();
    return latched != 0 ? latched : rc;
}
```

Semantics:

- **Set-only.** No public getter (SPEC §4.5 inv #2). Debug code
  reads it from logs (the shim logs the latched value before
  returning).
- **Last-write-wins.** Multiple `set_exit_code(N)` calls before
  return all win in store-order; the final return reads the
  latest. Relaxed memory order is correct because the shim's
  `take_exit_code()` runs *after* every engine thread is joined
  (the engine is single-threaded for MVP; future-proofed by
  joining at shutdown anyway). No happens-before edge across
  threads is needed beyond the join.
- **Initial value 0.** A clean exit is implicit: callers that do
  not call `set_exit_code` leave 0 in the latch and the shim
  returns 0.
- **Signal handlers may call `set_exit_code`.** Atomic-store with
  relaxed order is async-signal-safe (it compiles to a single
  word write on Apple Silicon, naturally aligned). A SIGINT
  handler that wants to terminate with code 130 can write 130
  before returning; if the handler then calls `_exit(130)`
  (async-signal-safe, unlike `exit`), the latch is moot but
  consistent. If the handler instead returns and the engine
  shuts down cleanly, the latched value flows through the shim.

### 3.7 Hot/cold path split

| Surface                    | Path | Frequency                                            |
|----------------------------|------|------------------------------------------------------|
| `Process::init`            | cold | once per program invocation (boot)                   |
| `Process::get`             | hot  | many times per frame (anyone may read snapshots)     |
| `argv()`, `env(name)`, `cwd()`, `executable_path()`, `pid()` | hot | called freely; pure read views over snapshot |
| `set_exit_code`            | warm | rare — typically only on error / shutdown / signal   |
| `install_signal`           | cold | once per consumer (boot or plugin-load)              |
| `uninstall_signal`         | cold | once per consumer (shutdown or plugin-unload)        |
| Trampoline body            | async | only on signal delivery — not on the hot path       |
| Shutdown destructor        | cold | once per program invocation                          |

The aggregate has **zero** per-frame work in steady state (SPEC
§9.1 row). The hot reads are O(1) span / map lookups against
read-only memory, do not allocate, and do not touch any
synchronization primitive. The trampoline runs only on signal
delivery, which is unbudgeted (signal delivery is async; if it
happens during a frame, the frame eats the latency, but signal
storms are not a steady-state concern — they accompany shutdown or
fatal-error scenarios).

### 3.8 Public surface (no growth beyond §5.10)

Reproducing SPEC §5.10 verbatim for the reader's convenience; this
design adds **no** type, function, or invariant beyond it:

```cpp
namespace glibre::platform {

using SignalHandlerFn = void (*)(int signal) noexcept;

enum class Signal : std::uint8_t {
    Interrupt,    // SIGINT
    Terminate,    // SIGTERM
    SegFault,     // SIGSEGV — install only for crash dumps.
    BusError,     // SIGBUS
    IllegalInst,  // SIGILL
    FpError,      // SIGFPE
};

class Process {
public:
    [[nodiscard]] static auto get() noexcept -> Process&;

    [[nodiscard]] auto argv()             const noexcept -> eastl::span<const eastl::string_view>;
    [[nodiscard]] auto env(eastl::string_view name) const noexcept -> eastl::optional<eastl::string_view>;
    [[nodiscard]] auto cwd()              const noexcept -> CanonicalPath;
    [[nodiscard]] auto executable_path()  const noexcept -> CanonicalPath;
    [[nodiscard]] auto pid()              const noexcept -> std::uint32_t;

    auto set_exit_code(int code) noexcept -> void;

    [[nodiscard]] auto install_signal(Signal, SignalHandlerFn) noexcept -> Result<void>;
    [[nodiscard]] auto uninstall_signal(Signal)                 noexcept -> Result<void>;

    Process(const Process&)            = delete;
    Process& operator=(const Process&) = delete;

private:
    Process() noexcept = default;
};

}  // namespace glibre::platform
```

A *non-public* `Process::init(int argc, char** argv) -> Result<void>`
exists in `process/process.cpp` and is called from the `main()`
shim. It is not in `glibre/platform/process/process.hpp` — only the
shim translation units include `glibre/platform/detail/init.hpp`.
This keeps the public surface honest (engine code cannot
accidentally re-init).

### 3.9 Concurrency

Single-threaded for the writer side (boot snapshot, install /
uninstall). The mutex (§3.5) defends against off-main-thread
install attempts that violate convention. Reader side is lock-free
on every member function:

| Surface                | Thread-safety                                                                                                                                 |
|------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------|
| `argv` / `env` / `cwd` / `executable_path` / `pid` | Safe from any thread after `init` returns. Pure reads of immutable post-init memory. No data race possible.                              |
| `set_exit_code`        | Safe from any thread, including signal context. Single relaxed-atomic store.                                                                  |
| `install_signal`       | Main-thread-only **by convention**; mutex enforces deterministic outcome on convention violation. Other threads succeed but the trampoline-side acquire-load on the next signal sees the install. |
| `uninstall_signal`     | Same as `install_signal`.                                                                                                                     |
| Trampoline             | Async-signal-safe. Reads `slot.fn` with acquire order; the install-side release-store happens-before the next trampoline invocation through OS-level publish (the `sigaction` call's internal barrier is ordered-after the slot store in install code, before the OS may deliver a fresh signal that runs the trampoline). |

No condvar, no thread pool, no fiber. SPEC §6.8 names exactly one
mutex inside `process/` — this one — and this design preserves
that.

### 3.10 macOS-specific seams

`process_macos.cpp` is the lone host-specific TU:

- **`<crt_externs.h>`** declares `_NSGetArgv` / `_NSGetArgc` /
  `_NSGetEnviron` as plain C functions (despite the "NS" naming).
  No Obj-C runtime touched. Header is part of macOS SDK base; no
  framework link.
- **`<mach-o/dyld.h>`** declares `_NSGetExecutablePath`. Same C-ABI
  treatment.
- **`<unistd.h>`** for `getcwd` / `getpid` / `_exit`.
- **`<signal.h>`** for `sigaction` / `siginfo_t` / `sigaltstack`.
- **`<errno.h>`** for `errno`.
- **`<stdlib.h>`** for `realpath`.

No `objc_*`, `NS*` Obj-C class, AppKit symbol, or Foundation type
is referenced from `process/`. CLAUDE.md's "No Obj-C++ in engine
code" rule is preserved by the absence of any `.mm` file in this
module — the bridge file (`surface/bridge.mm`) is the engine's
*one* exception, and it is not in `process/`.

Future Linux port: replace `_NSGet*` with `__libc_argv` / `environ`
(via `<unistd.h>` extern declarations) and `_NSGetExecutablePath`
with `readlink("/proc/self/exe")`. `sigaction` / `getcwd` /
`getpid` are POSIX-portable. The host-agnostic glue
(`process.cpp`) is unchanged; only `process_macos.cpp` is replaced
by `process_linux.cpp`. Same SPEC §5.10 surface; no public
recompilation churn.

Future Windows port: replace POSIX with `__argc` / `__wargv` /
`GetEnvironmentStringsW` (UTF-16 → UTF-8 conversion at copy time)
/ `GetCurrentDirectoryW` / `GetModuleFileNameW` /
`GetCurrentProcessId`. Signals become `SetConsoleCtrlHandler`
(SIGINT / SIGTERM analogues) + `AddVectoredExceptionHandler`
(SIGSEGV / SIGFPE analogues). The aggregate's surface (§5.10) is
host-agnostic; only the implementation file changes.

## 4. Public surface

This design introduces **no new public surface** beyond
`specs/platform/SPEC.md` §5.10 (reproduced in §3.8). The aggregate
is closed:

- One enum: `Signal`.
- One typedef: `SignalHandlerFn`.
- One class: `Process` (singleton accessor + 8 member functions).
- All fallible functions return `Result<T> = std::expected<T,
  glibre::Error>` per `reviews/decisions/error-model.md`. Failures
  surface as `platform::Error` arms (§4.7) and join the engine-
  wide variant.
- Total functions on the surface: 9 (1 static accessor + 8
  members). One is set-only (`set_exit_code` returns `void`); five
  are pure reads (`argv` / `env` / `cwd` / `executable_path` /
  `pid`); two are fallible mutators (`install_signal` /
  `uninstall_signal`).
- No iterators, no callbacks crossing the boundary outbound (SPEC
  §4.6 inv #7 spirit applied: `SignalHandlerFn` is a *registered*
  callable, not a visitor). The trampoline calling the registered
  function is the only callback — and it is owned by the
  aggregate, not crossing back inbound.

Exceptions: every public function is `noexcept`. The two `Result<>`-
returning functions handle all failure paths typed.

ABI surface for plugin consumers: the SPEC §5.10 stub is included
verbatim into `glibre::types::platform` middleman headers
(`reviews/decisions/plugin-abi.md`). The `Process&` reference is
not crossed across `.dylib` boundaries (the singleton lives in the
core `libglibre_platform.a`, statically linked once into the host
binary; plugins call into it through `Process::get()` which
resolves via the host ABI hash table). Standard plugin-loader ABI
rules apply — no special-casing.

## 5. Hot/cold path split

(Restated from §3.7 with the two budget-relevant rows pulled out.)

- **Hot reads (per-frame, possibly many times):** `argv()`,
  `env(name)`, `cwd()`, `executable_path()`, `pid()`. All are
  O(1) (`env` is O(log N) over ≤ ~1024 entries — effectively
  constant given the small-N), allocate nothing, and run on any
  thread. Per-frame budget: 0.000 ms (SPEC §9.1).
- **Cold writes (boot / shutdown / rare events):** `init`,
  `install_signal`, `uninstall_signal`, `set_exit_code`. Acceptable
  to allocate (only `init` does, into the boot arena) and to take
  the install mutex. Budget: not counted against the per-frame
  cell.
- **Trampoline:** unbudgeted. Async signal delivery is rare and
  out-of-band; the trampoline body is ~10 instructions and never
  allocates / never locks.

The aggregate's contribution to the steady-state per-frame
allocation rate is **zero bytes** (SPEC §9.3, §6.9 row "`Process::
env()` / `argv()` does not allocate"). The 256 KiB sub-arena is
filled at boot and never grown.

## 6. Concurrency

Threading topology constraints satisfied (SPEC §6.8):

- `Process` creates and owns **zero** threads. It does not appear
  in the SPEC §6.8 thread table.
- `Process` introduces **one** mutex (`g_install_mutex`,
  install/uninstall-side only). SPEC §6.8 names exactly one mutex
  inside `process/` and this is it.
- Reader side is lock-free, atomic-free for the snapshot reads
  (immutable memory) and single-atomic for the exit-code latch.
- Trampoline is async-signal-safe: no malloc, no locks, no
  non-reentrant libc, errno preserved.

Cross-thread documentation:

- Engine main thread: the conventional install / uninstall caller.
- Off-main-thread install: not contract-conforming, but the mutex
  ensures deterministic `AlreadyExists` on race rather than
  silent slot tear.
- Signal-delivery thread: the kernel chooses which thread runs the
  handler (typically the thread that triggered the synchronous
  signal — SIGSEGV / SIGFPE / SIGBUS / SIGILL — or an arbitrary
  thread for asynchronous signals — SIGINT / SIGTERM). The
  trampoline's table read is correct on every thread because the
  table memory is publish-after-install.
- Shutdown thread: the `main()` shim (= main thread) reads the
  exit-code latch after engine join. No race.

No work is enqueued onto another context's thread. The aggregate
is a leaf in the threading topology.

## 7. Persistence + ABI

**No `.fory` schema.** SPEC §7.1, §7.3 list `Process` as carrying no
serializable state (SPEC table row "`Process` | No | argv / env /
cwd / exit code / signal table are OS-owned process state, captured
fresh at startup (§4.5 inv #1). The aggregate has no value the
engine writes back."). This design preserves that. Justification:

- Argv / env / cwd / exepath / pid are *OS-owned* — re-reading
  them is the only way to get a current value. Serializing the
  startup snapshot to be replayed on another run would lie about
  the new run's process identity.
- Exit-code latch is a transient single-`int` flag; persisting
  zero or stale values would mis-direct the next run.
- Signal-handler table maps `Signal → SignalHandlerFn`; function
  pointers are process-local and not portable across runs (or even
  across `dlopen` cycles). Survival across hot-reload is via
  *re-installation by the new plugin*, not by serializing function
  addresses (§8 below).

**ABI implications.** The §5.10 surface is plugin-loadable per
`reviews/decisions/plugin-abi.md`:

- `enum class Signal : std::uint8_t` is layout-stable; adding a
  variant is an ABI break (caught by the loader's
  `host_glibre_types_abi_hash`).
- `SignalHandlerFn = void (*)(int signal) noexcept` is a stable
  C calling convention; `noexcept` on a function-pointer type
  is encoded in the signature in C++17+ ABIs and the Clang ABI
  hash includes it.
- `class Process` is non-copyable, non-movable, has no `virtual`,
  has no data members in the public header (PIMPL is an implementation
  detail of `process.cpp`'s singleton storage; the header carries
  zero-byte type). The class's *interface* is what crosses the ABI;
  the singleton's *storage* lives inside the platform static
  library, exposed only through `get()`.

The aggregate adds **zero** entries to the engine-wide ABI hash
manifest (`reviews/decisions/plugin-abi.md` §"ABI hash inputs")
beyond what is implied by §5.10 inclusion. No new middleman type
in `glibre::types::platform::*` is introduced.

## 8. Hot-reload

`Process` is platform-internal. Other plugins do **not** hold
`Process` handles across reload (the singleton accessor returns a
reference into the platform static library, which lives in the host
binary; the host binary does not reload). The hot-reload contract
(`reviews/decisions/hot-reload-protocol.md`) only kicks in for the
**platform-plugin self-reload** case (SPEC §8.3, §8.5).

### 8.1 Survives peer-plugin reload

Trivially. When any non-platform plugin reloads (e.g. `render`,
`content`, `physics`):

- The platform binary is unchanged; `Process::get()` returns the
  same reference.
- The handler table is unchanged; previously-installed signal
  handlers still fire. **However**, if the reloading peer plugin
  installed a handler from its now-being-unloaded text segment,
  that handler's function pointer becomes dangling on `dlclose`.
  The loader's contract (`hot-reload-protocol.md` Drain step)
  requires every plugin to drain its OS subscriptions; this
  aggregate documents the rule for signals: **a peer plugin that
  installed a `SignalHandlerFn` MUST `uninstall_signal` it in
  its drain step**, and re-`install_signal` after its register
  step (with the new copy of the function in its newly-loaded
  text segment).
- Forgetting to drain a signal handler is a use-after-free hazard.
  The platform aggregate cannot detect this (it does not know
  which plugin owns each `fn`); the loader's `host_glibre_types_
  abi_hash` does not cover it (function pointers are not in the
  hash). Detection is **caller-side**: each plugin's drain
  function lists the signals it installed, and the loader audits
  the list against the platform's handler table at the *swap*
  step. A non-empty intersection at swap time is a refusal cause
  P-equivalent to SPEC §8.4 P1, surfacing
  `Error::Unsupported{ detail: "signal-handler-orphan" }`. (This
  is auditing the *contract*, not the function pointer values.)

### 8.2 Platform-plugin self-reload

SPEC §8.3 step 1 already specifies the drain clauses for `Process`
(quoted, slightly trimmed for context):

> 5. Uninstall every signal handler installed via
>    `Process::install_signal`; capture the `(Signal, fn)` set
>    into a middleman singleton for re-installation by Q.

And SPEC §8.3 step 4 (Resume) clause 4:

> 4. Re-install every captured `(Signal, SignalHandlerFn)` pair
>    against the *same function pointers in Q's text segment*.
>    The middleman captures `Signal` enum values, not raw function
>    pointers, and Q is responsible for re-resolving the symbol;
>    [...]

This design refines the protocol mechanics:

1. **Drain step.** `glibre::platform::glibre_plugin_drain` walks
   `g_handler_table`; for each occupied slot, it calls
   `uninstall_signal(slot_to_signal_enum(i))` (which both clears
   the slot and reinstalls `SIG_DFL` via `sigaction`), and
   appends the `Signal` enum value to a middleman vector
   (`eastl::vector<Signal>` allocated in middleman memory).
   The function pointers themselves are *not* captured — they
   point into outgoing-plugin text and would dangle. Instead, the
   middleman captures only the enum values; Q's register code
   re-resolves the same symbol by name (e.g.
   `glibre::core::crash_handler_segfault`) in Q's freshly-loaded
   text and re-installs.
2. **Swap step.** Standard. ABI hash check binding. The middleman
   `eastl::vector<Signal>` is included in
   `host_glibre_types_abi_hash`.
3. **Migrate step.** Empty. No `.fory` schema (§7).
4. **Resume step.** Before iterating captured `Signal` values to
   call `install_signal()`, the resumed plugin's
   `glibre_plugin_register` MUST have already invoked
   `glibre::platform::detail::error::pre_touch_all()`. The same
   ordering contract from §3.5 applies on Resume because the lazy
   linker resolver is bound to the freshly-`dlopen`'d plugin's TLS,
   which was reset by Pause. Resume == fresh `dlopen` for ordering
   purposes per `platform-error-design.md` §6.2 rebind clause. The
   Resume orchestrator therefore calls `glibre_plugin_register`
   first (which itself must call `pre_touch_all()` then
   `install_signal()`). See §3.5 install_signal pre-condition; the
   same ordering applies on Resume.

   `Q::glibre_plugin_register` then re-creates the `Process`
   singleton's bookkeeping (the singleton's storage lives in the
   static library, which is *not* reloaded — the reload is for the
   platform plugin's `.dylib` if and only if `platform/` is built
   as a separate plugin; under the current architecture `platform/`
   is a static library inside the host binary, so platform
   self-reload is moot. The clauses above apply if a future spike
   promotes `platform/` to its own reloadable `.dylib`. Until then,
   this is dead-code documented for future-proofing.) Q then
   iterates the middleman `Signal` vector and calls
   `install_signal(s, resolve_symbol_for(s))` for each.
5. **Refusal.** If `install_signal` fails on resume (e.g. a peer
   plugin re-installed the same signal between drain and resume),
   the loader marks the swap as `HotReloadRefused` and rolls
   back to the prior plugin. The middleman's drained `Signal` set
   is fed back to the prior plugin's resume to re-install the
   pre-drain handlers. SPEC §8.4 refusal-case ergonomics apply.

In-flight signal during swap: the OS may deliver a signal between
"`uninstall_signal` returns" and "`sigaction` to `SIG_DFL`
completes" — but those happen inside one critical section, and
`sigaction` is itself atomic at the OS level. So the only window
is "between drain step 1's `uninstall_signal` calls". A signal
delivered there hits `SIG_DFL` (the just-reinstalled default,
typically termination for SIGSEGV / SIGTERM / etc). This is a
reload-scenario degradation: a SIGSEGV during platform self-reload
terminates the process rather than running the user crash handler.
Documented; acceptable trade-off (the alternative — keeping the
old handler live across the swap — would dangling-pointer on
`dlclose`).

### 8.3 Peer plugins that install signals (e.g. `core/crash`)

Per §8.1, peer plugins that call `Process::install_signal` must
include a corresponding `uninstall_signal` in their
`glibre_plugin_drain` and re-install in `glibre_plugin_register`.
The `process/` aggregate provides the install / uninstall surface;
it does not enforce the per-plugin discipline (that is the
loader's audit). This split keeps the SRP boundary at "OS process-
control contract" rather than spilling into "plugin-lifecycle
audit logic".

## 9. Performance

Per SPEC §9.1 row "`Process` (§4.5)": **0.000 ms CPU per frame**
in both sim and submit phases. This design preserves that:

- Hot reads (`argv` / `env` / `cwd` / `executable_path` / `pid`)
  are unbudgeted because they are *not* on the per-frame critical
  path of any aggregate. They may be called freely by editor /
  tools / debug code; their O(1) cost is amortized into the
  reserved-tail (0.099 ms sim, 0.049 ms submit) of the platform
  cell if any caller does invoke them per-frame.
- `set_exit_code` is rare (typically zero or one call per program
  invocation); its cost is a single relaxed atomic store.
- Signal delivery is unbudgeted — async, out-of-band.

Per SPEC §9.2 row "`Process` (§4.5)": **256 KiB heap sub-arena**.
This design partitions it as (§3.2):

| Slice                         | Size       | Filled at | Frees at      |
|-------------------------------|------------|-----------|---------------|
| Argv copy                     | ≤ 56 KiB   | `init`    | shutdown      |
| Env copy                      | ≤ 128 KiB  | `init`    | shutdown      |
| Cwd + executable_path copies  | ≤ 8 KiB    | `init`    | shutdown      |
| Env (key, value) flat-vec idx | ≤ 32 KiB   | `init`    | shutdown      |
| Sigaltstack                   | 32 KiB     | `init`    | shutdown      |
| Handler table                 | trivial    | static    | static        |
| **Total**                     | **≤ 256 KiB** | —      | —             |

Slice arithmetic: 56 + 128 + 8 + 32 + 32 = 256 KiB exactly,
meeting the SPEC §9.2 ceiling. Sigaltstack is pinned at
MINSIGSTKSZ (macOS 26 = 32 KiB); argv was trimmed from 64 KiB to
56 KiB to keep the sub-arena ≤ 256 KiB (see §3.5 and §12
[NON-BLOCKING] open item). If the total snapshot exceeds 256 KiB
at boot, `init` aborts with a clear diagnostic — strictly
preferable to silent truncation that would orphan environment
variables a tool relies on.

Steady-state allocation rate (post-`init`): **0 bytes per frame**.
Every aggregate function on the public surface is annotated with
the SPEC §6.9 promise "does not allocate".

CI gate fixture (per SPEC §9.4): a Catch2 `BENCHMARK` block under
`platform/test/perf/process_bench.cpp` asserts:

- `bench.process_argv_read` — `< 50 ns` for `argv()` + iteration
  over a single `string_view`. Covers the read-view path.
- `bench.process_env_lookup` — `< 200 ns` for `env("PATH")` over
  a 200-entry env. Covers the flat-vector lookup.
- `bench.process_install_uninstall_pair` — `< 50 µs` for one
  `install_signal` + `uninstall_signal` round-trip (excluding
  OS `sigaction` jitter). Covers the cold path.
- `bench.process_set_exit_code` — `< 20 ns`. Single atomic store.

Process-aggregate sub-arena resident-bytes assertion:
`core::PerContextAllocator::resident_bytes(ContextTag::Platform,
SubArenaTag::Process) <= 256 KiB` after `init` and never after
that — checked by the platform cell test fixture (§11) and by the
strict-mode allocator tag enforcement (`perf-budget.md` Allocator
Rule #2).

## 10. Failure modes

Per SPEC §10.3.5, the `Process` aggregate surfaces only two
runtime fail-able entry points:

| Entry point                       | Returnable arms                              | Trigger                                                                                                  |
|-----------------------------------|----------------------------------------------|----------------------------------------------------------------------------------------------------------|
| `Process::install_signal`         | `AlreadyExists`, `Unsupported`, `IoFailure` | duplicate handler (§4.5 inv #3); signal not installable on this OS (`EINVAL` from `sigaction`); rare unrecognized errno from `sigaction` |
| `Process::uninstall_signal`       | `NotFound`                                   | no handler currently installed for that signal                                                            |

Detail per arm:

- **`AlreadyExists`** (install). Trigger: §3.5 step 2 detects a
  non-null slot. Recovery: caller is buggy (two consumers
  competing for the same signal); at least one must defer to the
  other. Severity: `warn` at the install site (the previous
  handler keeps working); CI promotes to a build failure in test
  runs. SPEC §10 §"`AlreadyExists`" section already documents
  this exact case.
- **`Unsupported`** (install). Trigger: `sigaction` returned
  `-1` with `errno == EINVAL` — typically a signal the OS has
  classified non-installable (the closed enum already excludes
  the canonical cases SIGKILL / SIGSTOP, so this arm fires only
  for hardening-rule edge cases like a sandboxed binary whose
  entitlements forbid SIGSEGV trapping). Recovery: caller routes
  around (skips dump-on-segfault, uses an alternate diagnostic).
  Severity: `warn`. SPEC §10 §"`Unsupported`" section documents
  the framing.
- **`IoFailure { OsCode }`** (install). Trigger: `sigaction`
  returned `-1` with an unrecognized `errno`. Carries the raw
  errno value as `OsCode` for telemetry. Recovery: terminal —
  caller logs `error` and continues without the handler.
  Severity: `error`. SPEC §10 §"`IoFailure`" matches.
- **`NotFound`** (uninstall). Trigger: §3.5 uninstall-step 2
  detects null slot. Recovery: caller is buggy or already
  uninstalled (idempotent shutdown). Severity: `debug` —
  uninstalling a never-installed signal is a no-op contract
  violation, not an operational failure. SPEC §10 §"`NotFound`"
  matches.

The other surface entries are **total** (cannot fail), per SPEC
§10.3.5:

- `argv` / `env` / `cwd` / `executable_path` / `pid` —
  read-views over post-`init` immutable memory. Cannot fail.
- `set_exit_code` — single atomic store. Cannot fail.
- `Process::get()` — returns a reference; cannot fail (the
  singleton is constructed at first call; storage allocation is
  static).

`Process::init` itself is fallible internally (`AlreadyExists`
on double-init; `Unsupported` on snapshot overflow per §3.2;
`IoFailure` on `sigaltstack` failure) but is not part of the
public §5.10 surface (§3.8). The shim handles its `Result<void>`
directly with `.value()` — boot failure is fatal and the engine
must not start.

`Process` does not contribute new arms to the §4.7 closed sum.
All failure paths surface into existing arms (`AlreadyExists`,
`Unsupported`, `NotFound`, `IoFailure`).

## 11. Test plan

Tests live under `tests/platform/process/` (Catch2). Tags:
`[platform]`, `[process]`, plus `[signal]` for tests that install
real signal handlers (these may be flaky on CI runners that
disallow `sigaction`; gated below).

### 11.1 Unit tests (no signal install)

Bound to SPEC §4.5 invariants and §10.3.5 failure rows.

- **`process.argv_returns_init_args`** — `Process::init` with a
  known argv; `Process::get().argv()` returns a span of equal
  length and byte-for-byte equal contents.
- **`process.argv_does_not_allocate`** — wrap `argv()` in an
  allocator-fence (`PerContextAllocator::resident_bytes` before /
  after); assert no growth. Covers SPEC §6.9.
- **`process.env_lookup_hit`** — preload a fake env; assert
  `env("FOO")` returns `Some("bar")`.
- **`process.env_lookup_miss`** — assert `env("DOES_NOT_EXIST")`
  returns `None` (`eastl::optional<...>{}`).
- **`process.env_lookup_first_wins_on_duplicate_key`** —
  preload `["X=1", "X=2"]`; assert `env("X") == "1"`. Matches
  POSIX `getenv` first-wins semantics on macOS / glibc / musl per
  §3.2.
- **`process.cwd_canonicalizes`** — set cwd to a path with a
  trailing slash / `.` segment; assert `cwd()` returns the
  canonicalized form.
- **`process.executable_path_canonicalizes`** — same.
- **`process.pid_is_getpid`** — assert `pid() == getpid()`.
- **`process.set_exit_code_latches`** — call `set_exit_code(7)`;
  read latch via `detail::take_exit_code()`; assert `7`.
- **`process.set_exit_code_last_wins`** — call
  `set_exit_code(3); set_exit_code(5)`; assert latch is `5`.
- **`process.double_init_returns_already_exists`** — call
  `Process::init` twice; second call returns
  `Result<void>{ Error::AlreadyExists }`. Covers SPEC §4.5 inv
  #5.
- **`process.snapshot_overflow_aborts`** — preload a synthetic
  argv whose total exceeds 256 KiB; assert `Process::init`
  returns `Result<void>{ Error::Unsupported }` with detail prefix
  `"snapshot-overflow"`. (Death-test variant for release
  builds where the abort is a `std::abort()`.)

### 11.2 Unit tests (signal-handler installation, no real
delivery)

Use a synthetic-signum test seam: instead of calling `sigaction`,
the test stubs the syscall via a function-pointer hook in
`process_macos.cpp` (compiled in test builds only). The
trampoline body is never invoked; only the table-management code.

- **`process.install_signal_succeeds_first_time`** — `install_
  signal(Signal::Interrupt, &noop)` returns `Ok`. Slot reads
  populated.
- **`process.install_signal_returns_already_exists_on_duplicate`**
  — install twice; second returns `AlreadyExists`. Covers SPEC
  §4.5 inv #3.
- **`process.uninstall_signal_returns_not_found_when_absent`** —
  uninstall without prior install; returns `NotFound`. Covers
  SPEC §10.3.5.
- **`process.install_then_uninstall_round_trip`** — install,
  uninstall, install again — second install returns `Ok`
  (idempotent slot reuse).
- **`process.install_signal_translates_einval_to_unsupported`** —
  stub `sigaction` to return `-1 / EINVAL`; assert
  `Unsupported`.
- **`process.install_signal_translates_unknown_errno_to_io_
  failure`** — stub `sigaction` to return `-1` with
  errno = `0xDEADBEEF`; assert `IoFailure { OsCode { 0xDEADBEEF
  } }`.

### 11.3 Integration tests (real signal delivery, fixture-gated)

Tagged `[signal][platform-fixture]`; skipped on runners that
forbid `sigaction` or that run the test binary under
sanitizers that conflict with `SA_ONSTACK` (`tsan` historically
has trouble; `asan` is fine because the trampoline is `[[gnu::
no_sanitize_address]]`).

- **`integration.install_sigint_handler_is_invoked`** — install a
  handler that sets a global atomic flag; raise SIGINT via
  `kill(getpid(), SIGINT)`; sleep up to 100 ms polling the
  flag; assert the flag is set within the timeout.
- **`integration.errno_preserved_across_handler`** — call a
  failing syscall to set `errno`, deliver SIGINT, assert `errno`
  unchanged after the handler returns. Covers §3.5 errno-save.
- **`integration.handler_runs_on_alt_stack`** — install a
  handler that captures the current stack pointer via
  `__builtin_frame_address(0)`; deliver SIGSEGV via deliberate
  null-deref in a sandboxed sub-routine (gated by
  `#if !defined(__has_feature) || !__has_feature(address_
  sanitizer)`); assert captured SP is inside the alt-stack
  range. Covers §3.5 `SA_ONSTACK`.
- **`integration.uninstall_restores_default`** — install,
  uninstall, then send SIGTERM. Assert the process terminates
  with the default disposition (the test binary forks a child
  for the actual termination path).
- **`integration.exit_code_set_from_handler`** — install a
  SIGINT handler that calls `set_exit_code(130)`; deliver SIGINT;
  the handler returns; the test main reads
  `detail::take_exit_code()` and asserts `130`. Verifies §3.6
  signal-context safety.

### 11.4 Hot-reload audit tests

- **`integration.peer_drain_audit_orphan_signal_refused`** — a
  test-only "stand-in" plugin installs a signal handler in its
  `register` and *does not* uninstall in its `drain`; the loader's
  swap step detects the orphan and returns `HotReloadRefused`
  with `Unsupported { detail: "signal-handler-orphan" }`.
  Covers §8.1 audit rule.
- **`integration.peer_drain_clean_install_uninstall`** —
  symmetric stand-in plugin that *does* uninstall in drain;
  the swap succeeds and re-install in resume succeeds. Covers
  §8.1 / §8.3.

### 11.5 Performance microbenchmarks

`tests/platform/perf/process_bench.cpp` (Catch2 `BENCHMARK`):

- `bench.process_argv_read` — < 50 ns.
- `bench.process_env_lookup` — < 200 ns over 200-entry env.
- `bench.process_install_uninstall_pair` — < 50 µs (excludes
  OS jitter; mocked `sigaction` for stability).
- `bench.process_set_exit_code` — < 20 ns.

CI gates per `perf-budget.md` §"CI Gate Spec" #1: any benchmark
exceeding its budget fails the PR.

### 11.6 E2E coverage

E2E traces under `tests/e2e/platform/process/`:

- **`process-boot-snapshot`** — full `init` → `argv` /
  `env("PATH")` / `cwd` / `executable_path` / `pid` reads,
  recorded as a `.glibre-trace` golden. CI replays asserts
  byte-equal trace output. Covers SPEC story #359 acceptance.
- **`process-signal-roundtrip`** — install → raise → handler
  observed → uninstall, recorded as a `.glibre-trace` golden.
  Covers SPEC story #359 second half.

Both run on `macos-26-m1` CI only.

### 11.7 Fuzz / property tests

- **`fuzz.env_parser_roundtrip`** — random `KEY=VALUE\0KEY2=
  VALUE2\0...\0` byte sequences (including pathological cases:
  empty value, empty key, multiple `=` in value, very long key,
  embedded `=` in key — illegal but parseable, treated as
  last-`=`-wins for key/value split). Assert `env(key)` returns
  the same value the parser stored, never crashes.
- **`fuzz.argv_quoting_doesnt_matter`** — random argv vectors
  including empty strings, NULs in shell-quoted form (the engine
  receives already-NUL-terminated strings; embedded NULs inside
  one argv slot are not representable in POSIX, so the test
  validates rejection rather than crash).

## 12. Open questions

- **[OPEN] Promotion of `process/` from static library to its own
  reloadable `.dylib`.** The current architecture compiles
  `engine/platform/` as a single static library
  `libglibre_platform.a` linked once into the host binary
  (SPEC §6.1). SPEC §8.3 documents platform self-reload mechanics
  for the case where `platform/` is its own plugin; until that
  promotion happens, the §8.2 clauses are contingency
  documentation. Resolve when (if) a future spike argues for
  hot-swapping platform itself — currently no plan calls for it.
- **[OPEN] Out-of-process crash-monitor binary (R-14.4.7).** SPEC
  §3 refusal list defers it; this design holds the line. Promotion
  trigger: a second consumer of crash-dump capture beyond the
  in-process signal-handler stub (e.g. the editor's
  always-running diagnostic process), at which point the
  engine adds a `tools/glibre-crash-monitor` binary and the
  signal-handler trampoline grows a "forward to monitor over
  pipe" code path. Re-derive ownership at that spike; provisional
  expectation is that the *spawning* of the monitor is editor-
  side (out of `process/` aggregate scope, see §1 refusal of
  subprocess spawn) and the *forwarding* is a new `process/`
  surface gated by a feature flag.
- **[OPEN] Subprocess-spawn surface for tools.** Tools
  (`glibre-cook`, `glibre-codegen`, `glibre-foryc`) are launched
  by the editor through the OS shell today. If the editor's
  tool-orchestration spike argues for an in-engine spawn API
  (capturing stdout / stderr into a typed buffer; awaiting exit
  codes typed; killing on timeout), it lands as a *new*
  aggregate (`platform/Subprocess` or its own context), not as
  growth of `Process`. SRP rationale: spawn / wait / pipe /
  kill is "OS process *creation* contract" — a different reason
  to change from "OS process *identity / signals* contract".
  Open until the editor spike makes the call. Expected gating
  consumer: `tools/glibre-editor`'s "build" / "cook" / "codegen"
  panel; until that work lands, shell invocation remains
  sufficient.
- **[OPEN] [BLOCKING IMPLEMENTATION] Signal handlers that need
  access to per-thread state.** The current
  `SignalHandlerFn = void (*)(int signal) noexcept` takes only
  the signum. R-14.4.6 GPU breadcrumbs + R-14.4.1 stack-trace
  capture want access to the faulting thread's `siginfo_t` and
  `ucontext_t`. The trampoline already receives them (it uses
  `SA_SIGINFO`); exposing them would widen `SignalHandlerFn` to
  e.g. `void (*)(int, const glibre::types::platform::SignalCtx&)
  noexcept`, where `SignalCtx` is a host-agnostic middleman
  wrapping the platform-specific bits the dump-writer needs.
  **BLOCKING IMPLEMENTATION**: per the design preamble (line 19),
  widening the `SignalHandlerFn` signature changes the §5.10
  public surface — which requires an amendment spike before any
  plan PR can adopt the wider signature. Resolution requires:
  (1) a SPEC §5.10 amendment spike that adds `siginfo_t*` /
  `ucontext_t*` (or a `SignalCtx` wrapper) to `SignalHandlerFn`,
  (2) a SPEC §12 tracking entry in the platform SPEC, (3) at
  least two concrete callers demonstrating need for the widened
  info before approval. Defer until the in-process crash-handler
  plugin (#TBD) is drafted; the trade-off is ABI surface growth
  vs giving the crash-handler enough info to record register
  state.
- **[OPEN] Env-var change at runtime.** §3.2 rule #2 documents
  "out-of-band `setenv` is unsupported and undefined". A future
  consumer (e.g. an editor "edit env var, restart engine"
  workflow) might want a sanctioned mutation API. Routed to a
  separate spike; expected resolution is "we don't mutate; we
  restart the engine binary with new argv/env" — preserving
  the snapshot-once invariant. Open until a consumer exists.
- **[OPEN] [NON-BLOCKING] Sigaltstack growth.** The current 32 KiB
  allocation (§3.5, §9 table) equals MINSIGSTKSZ (macOS 26 = 32
  KiB) and fits the 256 KiB sub-arena ceiling exactly (see §9).
  If runtime stack-overflow signals exhaust the 32 KiB sigaltstack
  (e.g. a crash-handler that walks the stack into a large scratch
  buffer), file a SPEC §9.2 amendment to grow the sub-arena
  ceiling. Defer until the in-process crash handler is drafted and
  benchmarked.
- **[OPEN] Closed signal-enum growth.** §3.4 documents which
  signals are exposed today (6) and why others are not. A
  future consumer (e.g. SIGUSR1 used as a reload trigger from a
  CI script) requires a strict expansion. Adding to the enum
  is an ABI break; the loader's hash will catch it. Open until
  a concrete consumer requests the new entry.
