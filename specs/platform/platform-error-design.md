# platform — Detailed Design: platform-error aggregate

> Detailed design for the closed sum `platform::Error` plus the
> POSIX `errno` / SDL3 `SDL_GetError()` / mach `kern_return_t` /
> AppKit `NSException` translation seam declared in
> `specs/platform/SPEC.md` §4.7, §5.1, §6.10, §10.1, §10.2, §10.6.
> Refines those sections in place; cites
> `reviews/decisions/error-model.md`,
> `reviews/decisions/perf-budget.md`,
> `reviews/decisions/frame-phases.md`,
> `reviews/decisions/plugin-abi.md`,
> `reviews/decisions/hot-reload-protocol.md`, and
> `reviews/decisions/fory-codegen.md`. Introduces no new public
> surface beyond the §5.1 stub already locked into
> `specs/platform/SPEC.md`; deviations from the cited records would
> require an amendment spike, not an in-place edit.
>
> Refs: spike #727 — `[SPIKE] design-platform-platform-error-detailed`.
> Parent #714. Sibling task-breakdown spike is blocked by this
> deliverable. All conclusions re-derived; harmonius prior art
> (`harmonius/docs/requirements/platform/os-integration.md` R-14.2.8,
> `harmonius/docs/requirements/platform/filesystem.md`,
> `harmonius/docs/requirements/platform/threading-async.md`) is
> research input only.

## 1. Purpose

The platform-error aggregate is the **single translation seam** between
the four OS error sources the engine is allowed to call (POSIX
`errno`, SDL3 `SDL_GetError()`, mach `kern_return_t`, AppKit
`NSException`) and the closed sum `platform::Error` returned across
every platform-aggregate boundary via `std::expected<T, glibre::Error>`
(per `reviews/decisions/error-model.md` and SPEC §4.7). Its one
responsibility — the only reason it changes — is **"the OS reports
failures in a different shape than glibre's closed sum models"**: a
new SDL3 release renames a prose prefix, an Apple SDK revision adds
an `NSException` class, the POSIX errno table grows a code with a
new family. Concretely the aggregate owns:

1. The closed `platform::Error` variant (§5.1 of SPEC; restated in §3
   below) and the rule that adding / removing / renaming a variant is
   a deliberate central edit gated by the second-consumer trigger
   from SPEC §10.8.
2. The four translator free functions in `engine/platform/src/detail/error/`:
   `errno_to_error(int)`, `sdl_to_error(const char*)`,
   `mach_to_error(kern_return_t)`, and `ns_to_error(NSException*)`
   (the last one is the only function in the engine permitted to
   *touch* an `NSException*`, callable only from `bridge.mm` per
   SPEC §6.2).
3. The thread-local diagnostic prefix buffer (`error::tls::detail`)
   that callers use to discriminate `IoFailure { OsCode }` payloads
   without parsing prose at the public boundary (per SPEC §6.10,
   §10.4, §10.5).
4. The translation-table source-of-truth: each translator's row set
   is the test-fixture goldens lives at `tests/platform/error/` and
   is the single audit point a reviewer reads to verify "no integer
   leaks across the §5 surface".
5. The Fory schema for the *log/replay carrier* — a small,
   stable-on-the-wire `(tag, os_code, prefix)` triple that
   `glibre::log_error` and the e2e replay harness use to round-trip
   a `platform::Error` through `spdlog`'s structured field set
   without losing variant identity.

What this aggregate explicitly **refuses to own**:

- **`core::Error`.** `PluginAbiHashMismatch`, `PluginInitFailed`,
  `SchemaMigrationFailed`, `HotReloadRefused`, `FramePhaseMisordered`,
  `OutOfBudget` are owned by `core` per
  `reviews/decisions/error-model.md` Type Sketch and
  `specs/core/SPEC.md` §10. A platform-side translator that fabricated
  a `core::Error` would violate composition rule 1 ("per-context
  enums are leaves; nothing nests another context's enum inside its
  own").
- **`render::Error` / `physics::Error` / `data::Error`.** Same rule.
  Render maps `platform::Error::IoFailure` with prefix
  `"surface-lost"` to `render::Error::DeviceLost` at the call site
  that crosses the boundary (per SPEC §10.4 and error-model.md
  composition rule 2). The platform translator never reaches into
  render's enum; render's mapping function is render's source code.
- **The choice of `magic_enum` vs hand-written `to_string`.** Owned
  by `core/error.hpp` per `error-model.md` Open Q #1; this design
  consumes whatever core decides without forking. The only platform-
  side requirement is that the chosen approach round-trips every
  variant name (§7).
- **Per-aggregate failure semantics.** Which arms `Window::open` may
  return, the recovery contract for `IoFailure { OsCode }` with
  prefix `"surface-lost"`, and the polling fallback for
  `"watcher-unavailable"` are owned by the sibling designs
  (`window-surface-design.md`, `file-watcher-design.md`) and SPEC
  §10.3 / §10.4 / §10.5. This aggregate produces the typed arm; the
  sibling consumes it.
- **The `glibre::log_error` formatter.** Owned by `core`. Platform
  contributes the structured payload (§7) and the variant name
  table; core's helper does the spdlog dispatch.
- **Cross-aggregate retry / chain.** Each platform aggregate
  constructs its own errors at the site they happen (SPEC §4.7
  inv #3); this aggregate does not auto-translate one aggregate's
  arm into another's. A `FileWatcher` call that internally invokes
  `FileIo::stat_path` for canonical-path normalisation and gets
  back `NotFound` returns `NotFound` to *its* caller, but the
  translator was not the chain — the call site was.

The aggregate's SRP boundary is the sharpest in the platform context:
if SDL3 stops returning prose and starts returning a stable integer
code, if Apple deprecates `NSException` in favour of an opaque
`NSError*` pointer, if mach grows a new `kern_return_t` family the
file-watcher trips, or if the `IoFailure` payload widens beyond
`OsCode`, this design changes. Anything else is out of scope.

## 2. Requirements coverage

Mapping of harmonius requirements consulted as research input
(`PHILOSOPHY.md` §"How harmonius is used" — re-derived, not ported)
onto the MVP coverage in this aggregate. Every entry is independently
re-derived; coverage does not imply harmonius's design was correct,
only that the underlying *requirement* survives the re-derivation.

| Harmonius source clause                                                                       | Re-derived MVP requirement                                                                                                            | Coverage in this aggregate                                                                                                                                                                                  |
|-----------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `os-integration.md` R-14.2.8 (structured errors with platform-specific codes mapped to engine error types) | Every public platform call returns a typed-arm sum, never a raw OS integer; the OS code survives only inside `IoFailure { OsCode }`. | §3 closed variant + §4.1 translator surface; §5 hot/cold split (raw codes never appear on the success path).                                                                                                |
| `os-integration.md` R-14.2.8 (rationale: distinguish transient vs permanent failures)         | The translation table classifies `EAGAIN` / `EWOULDBLOCK` (transient I/O) under `IoFailure` with prefix `"again"` and `EROFS` / `ENOSPC` (permanent) under separate prefixes. | §3 prefix table + §4.1 errno rows; §10 test fixtures verify both classifications.                                                                                                                          |
| `os-integration.md` R-14.2.8 (verification: simulate clipboard lock + assert typed error)     | Translator tests are golden mappings, one row per OS code, runnable without an OS. Glibre refuses clipboard from MVP (§3 SPEC refusals). | §11 unit tests: every translator row drives a fixture call; failure case routes to the `OsCode` raw-fallback arm.                                                                                          |
| `filesystem.md` (structured failure for `read` / `write` / `stat` / `list`)                   | POSIX `errno` is the dominant source for FileIo and FileWatcher backends; one translator covers both.                                | §3 errno → arm table (the cell already in SPEC §10.2.1, restated for the design audit); §11 cross-aggregate uniformity test.                                                                                |
| `threading-async.md` `GraphError::Cycle` / `AccessConflict` / `MissingDependency` (compile-time graph errors) | Refused from platform: the task-graph context owns those; platform does not host scheduling. Recorded as a refusal so a future re-read does not relitigate. | §1 Refusals (`core::Error` + sibling-context errors); no platform translator row.                                                                                                                          |
| `crash-reporting.md` (signal capture path, structured logs)                                   | Crash-dump capture is a platform `Process::install_signal` concern (SPEC §4.5); the dump *format* is an `obs` context concern. The error aggregate's only contribution to crash paths is the signal-safe `OsCode` carrier. | §6 (signal-safe construction): `Error` is POD by construction; constructing it in a SIGSEGV handler is allowed because no allocation, no virtual dispatch, no TLS write occurs on the construction path. |
| `windowing.md` (surface-lost recovery)                                                        | `IoFailure { OsCode }` with prefix `"surface-lost"` per SPEC §10.4. Render branches on the prefix; the typed arm stays narrow.        | §3 prefix registry; §11 prefix round-trip fixture.                                                                                                                                                          |

Refusals routed elsewhere (research input from harmonius that
**does not** belong in this aggregate, recorded so the boundary is
re-traceable):

- **Per-domain enum families** (`ClipboardError`, `DialogError`,
  `NotificationError`). Refused — clipboard / dialog / notification
  are post-MVP and live in a future `os-integration` plugin that
  will, if it lands, add its own arm to `glibre::Error` per
  `error-model.md` composition rule 1.
- **Auto-retry policies** (harmonius implied transient errors should
  retry inside the engine). Refused — caller-domain decision per SPEC
  §10.1; the only retry the platform performs is the bounded `EINTR`
  retry inside the I/O thread (SPEC §10.2.1 last paragraph).
- **Localised error messages.** Refused — the prose lives in a TLS
  diagnostic buffer for telemetry only; the engine never branches on
  the message and never localises it. UI-side localisation is a
  `tools` / future-`l10n` concern.

## 3. Detailed model

### 3.1 The closed `platform::Error` variant

Restated from SPEC §4.7 / §5.1 for the design audit; this section
adds the *rationale* per arm and the *invariants* that govern
addition / removal / renaming.

```cpp
// engine/platform/include/glibre/platform/error.hpp — exposed via
// the §5 single-header projection in specs/platform/SPEC.md.
namespace glibre::platform {

struct OsCode {
    std::int32_t value{0};   // platform-native errno / kern_return_t /
                             // NSError code / SDL3 numeric tag.
    constexpr bool operator==(const OsCode&) const noexcept = default;
};

// Zero-sized tag structs. Each constexpr-default-constructible, all
// trivially copyable. Adding a payload to one is a SPEC §4.7 amendment
// spike, not an in-place edit.
struct NotFound          { constexpr bool operator==(const NotFound&)         const noexcept = default; };
struct PermissionDenied  { constexpr bool operator==(const PermissionDenied&) const noexcept = default; };
struct AlreadyExists     { constexpr bool operator==(const AlreadyExists&)    const noexcept = default; };
struct Interrupted       { constexpr bool operator==(const Interrupted&)      const noexcept = default; };
struct Unsupported       { constexpr bool operator==(const Unsupported&)      const noexcept = default; };

// Single-payload arm. The OsCode value is opaque to callers; only
// the diagnostic prefix (see §3.3) is load-bearing for branching.
struct IoFailure {
    OsCode code{};
    constexpr bool operator==(const IoFailure&) const noexcept = default;
};

// Raw fallback. Constructed only by the four translators (§4.1) when
// no row matched. Engine code never constructs OsCode directly as a
// public arm; it surfaces via translator output only.

using Error = eastl::variant<
    NotFound,
    PermissionDenied,
    AlreadyExists,
    Interrupted,
    Unsupported,
    IoFailure,
    OsCode>;

}  // namespace glibre::platform
```

Per-arm rationale (consumed-once table; SPEC §10.1 has the
trigger / recovery / severity per arm — this table justifies
*existence*, not behaviour):

| Arm                  | Reason for existence                                                                                                                   | Promotion / demotion gate                                                                                                                                                                                                                        |
|----------------------|----------------------------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `NotFound`           | Most common branch in callers (asset lookup, stale handle dereference); promoting it from the prefix-discriminated `IoFailure` saves the prose-parse. | Frozen.                                                                                                                                                                                                                                        |
| `PermissionDenied`   | Operator-actionable; the only entitlement-class arm worth branching on.                                                                | Frozen.                                                                                                                                                                                                                                        |
| `AlreadyExists`      | Programming-error class; CI promotes to build failure (SPEC §10.1). Distinct severity from `IoFailure` justifies its own arm.         | Frozen.                                                                                                                                                                                                                                        |
| `Interrupted`        | EINTR-class; routine on processes that trap SIGINT. Distinct from `IoFailure` because severity is `debug`, not `error`.               | Frozen.                                                                                                                                                                                                                                        |
| `Unsupported`        | Capability-gap signal (release-build assert path, hardware feature absence). Distinct from `IoFailure` because callers route around the gap rather than retry. | Frozen.                                                                                                                                                                                                                                        |
| `IoFailure { OsCode }` | Carrier for the long tail of OS errors that classify as I/O-class but do not justify a separate arm; carries the diagnostic prefix discriminator (§3.3). | Promotion path: when a *second* consumer needs to discriminate on a prefix without parsing TLS prose, the prefix becomes a first-class arm. SPEC §10.8 lists `SurfaceLost` / `WatcherUnavailable` as the two pending candidates. |
| `OsCode`             | Catch-all when no translator row matched; preserves the raw value for telemetry without losing the typed-sum shape.                   | Recurring `OsCode` appearances in telemetry are the trigger for adding a new translator row (§10) — not for adding a new top-level arm.                                                                                                       |

The seven arms are the closed sum. Adding an eighth is a deliberate
central edit (SPEC §4.7 inv #1) requiring: (a) a SPEC §4.7 amendment
spike, (b) an ABI hash bump (§7), (c) a hot-reload barrier-time
re-validation (§8), and (d) every existing handler's `eastl::visit`
audited for missing-arm coverage. The `[[nodiscard]]` discipline on
every public-surface return (SPEC §5) and the closed-sum rule
together make missing-arm handling a compile error.

### 3.2 Translator surface (free functions, not methods)

Four free functions in `engine/platform/src/detail/error/`, one per
OS source. They are free functions — not member functions of a
"translator class" — to make the SRP boundary mechanical: each
function depends on exactly one OS header, has zero state, and is
trivially callable from the signal-safe path (§6.2).

```cpp
// engine/platform/src/detail/error/translators.hpp
//
// Internal — platform-context-only. Engine code outside platform
// never includes this header; the public §5 surface is the only
// way platform errors cross a boundary.
namespace glibre::platform::detail::error {

// 3.2.1 POSIX errno → platform::Error
//   Source: <errno.h>. Pure function. Used by FileIo, FileWatcher
//   (kqueue / inotify path), Process (signal install / argv parse).
[[nodiscard]] auto errno_to_error(int posix_errno) noexcept
    -> Error;

// 3.2.2 SDL3 SDL_GetError() → platform::Error
//   Source: <SDL3/SDL_error.h>. Reads SDL's TLS prose, parses for
//   known prefixes (table in §4.1 below), stamps the diagnostic
//   prefix into platform's TLS prefix buffer (§3.3) on its way to
//   the translated arm. The msg pointer is borrowed (SDL owns it);
//   the function copies any prose it stashes.
[[nodiscard]] auto sdl_to_error(const char* sdl_msg) noexcept
    -> Error;

// 3.2.3 mach kern_return_t → platform::Error
//   Source: <mach/kern_return.h>. Used by the bridge.mm AppKit
//   layer (CALayer / CAMetalLayer kernel-side errors that surface
//   as kern_return_t before NSException wraps them) and by
//   FSEvents-stream-level failures the kernel reports directly.
//   Rare on the success path; documented for completeness.
[[nodiscard]] auto mach_to_error(int kern_return) noexcept
    -> Error;

// 3.2.4 AppKit NSException → platform::Error
//   Source: <Foundation/NSException.h>. Only callable from
//   bridge.mm (the lone Objective-C++ TU per SPEC §6.2); the
//   header signature uses void* to keep this header pure C++23
//   and so a cross-TU include never drags Foundation into engine
//   code. The void* is reinterpret_cast to NSException* inside
//   the bridge implementation.
[[nodiscard]] auto ns_to_error(void* ns_exception) noexcept
    -> Error;

}  // namespace glibre::platform::detail::error
```

The four functions are the **only** places in the entire engine where
a raw OS error integer / pointer exists at the call boundary; no
caller sees the integer, no `if (errno == ...)` ladder appears
outside these files, no `try { ... } @catch (NSException*)` block
appears outside `bridge.mm`. The SPEC §6.10 "errors are constructed
at the site they happen" rule is made physical by this signature
discipline.

### 3.3 Diagnostic prefix registry (TLS, not part of the variant)

Several arms (`IoFailure { OsCode }`, the catch-all `OsCode`) are
intentionally coarse so the closed sum stays small (§3.1 promotion
gate). When a caller needs to discriminate — e.g. render branching
on `"surface-lost"` (SPEC §10.4), the hot-reload coordinator
branching on `"watcher-unavailable"` (SPEC §10.5), or telemetry
classifying I/O failures — it reads a *thread-local diagnostic
prefix* the translator stamps just before returning. The prefix is
**not** part of `platform::Error`'s value identity; two `IoFailure`
values with `OsCode { 0 }` compare equal regardless of prefix.

```cpp
// engine/platform/src/detail/error/diagnostic.hpp
namespace glibre::platform::detail::error {

// Fixed-capacity stable-string registry. Every prefix below is a
// compile-time literal; the TLS slot stores a `const char*` to one
// of these literals, never an owned string.
namespace prefix {
inline constexpr const char* none                = "";
inline constexpr const char* again               = "again";
inline constexpr const char* io                  = "io";
inline constexpr const char* resource            = "resource";
inline constexpr const char* surface_lost        = "surface-lost";
inline constexpr const char* watcher_unavailable = "watcher-unavailable";
inline constexpr const char* queue_full          = "queue-full";
inline constexpr const char* out_of_budget       = "out-of-budget";
inline constexpr const char* sdl_empty           = "sdl-empty";
inline constexpr const char* mach_unmapped       = "mach-unmapped";
inline constexpr const char* ns_unmapped         = "ns-unmapped";
// file-io translator prefixes (see file-io-design.md §3.9)
inline constexpr const char* path_outside_root   = "path-outside-root";  // Consumer: FileIo translator (path-validation gate)
inline constexpr const char* project_readonly    = "project-readonly";   // Consumer: FileIo translator (sandbox writable-scope gate)
inline constexpr const char* read_too_large      = "read-too-large";     // Consumer: FileIo read_all (size cap on read_arena)
}  // namespace prefix

// Every translator stamps the slot before returning; callers that
// need to discriminate read it immediately after they observe the
// arm. The slot is reset to prefix::none at the start of every
// frame's phase 1 input drain (idempotent; cheap).
auto current_prefix() noexcept -> const char*;
auto set_prefix(const char* literal) noexcept -> void;
auto reset_prefix() noexcept -> void;

}  // namespace glibre::platform::detail::error
```

Why a TLS pointer rather than a payload on the variant: payload
widening would force every call site that visits the variant to
handle the new field, and 99% of callers never branch on the
prefix. TLS keeps the hot-path arm narrow (fits in two registers —
tag byte + 4-byte `OsCode`) and pays the discrimination cost only at
the call sites that need it. The trade-off is documented as a
deliberate one in §5: prefix is **cold-path only**, never read on
the success path of an `Error`-free call.

The prefix literal-set is **closed** at the same gate as the
variant: adding a new prefix is a deliberate edit reviewed against
SPEC §10.6 (logging severity table needs a row for it) and the
relevant aggregate's §10.3 sub-section. New prefixes do not bump the
ABI hash (they live in the TU-local `.rodata`, not in
`glibre-types.dylib`); they do bump the *log schema* version (§7).

### 3.4 Closed-sum invariants enforced by the design

Three invariants survive from SPEC §4.7 and are made mechanical here:

1. **Closed sum.** The variant is sealed at design time. `eastl::variant`
   plus `[[nodiscard]]` plus `eastl::visit`-style exhaustive handling
   means a new arm is a compile-time fan-out: every existing handler
   either explicitly handles the new arm or fails to compile. There
   is no "default" branch in handler code (audited by clang-tidy
   `bugprone-switch-missing-default-case` flipped to *required* on
   `eastl::visit` lambdas — owned by core's lint config, but platform's
   tests assert the discipline).
2. **No exceptions cross the boundary.** Every public function in §5
   is `noexcept`. The single `bridge.mm` file is the only place an
   `NSException` may exist; `ns_to_error` converts before returning,
   never re-throws. The build enforces `-fno-exceptions` engine-wide
   per `error-model.md`; `bridge.mm` is built with `-fexceptions`
   for the `@try / @catch` only and never re-exports a
   throw-allowing symbol.
3. **Errors are constructed at the site they happen.** No translator
   calls another translator. `errno_to_error` does not delegate to
   `sdl_to_error`; `ns_to_error` does not delegate to
   `errno_to_error`. If the bridge needs to surface a POSIX-class
   error from inside an `@catch` (rare; happens when AppKit forwards
   a Cocoa file error that wraps an `errno`), the bridge code
   extracts the integer from `[exception userInfo]` and calls
   `errno_to_error(int)` directly — but that is the *bridge's*
   composition decision, not the translator's.

## 4. Public surface

This aggregate adds no symbols to the public §5 surface beyond what
SPEC §5.1 already declares (`OsCode`, the seven tag structs, the
`Error` variant alias, and the `Result<T>` alias). The translator
functions (§3.2) are *internal* to the platform context — they live
under `engine/platform/src/detail/error/` and are not in any public
include path.

### 4.1 Translation tables (test-fixture goldens)

The four translator implementations are table-driven; the tables
*are* the contract. Every row is a unit-test golden (§11). New rows
require: a one-line entry, a unit test, and zero changes to consumer
code (the typed arm is unchanged; only the prefix may be new, and
new prefixes go through the §3.3 registry gate).

#### 4.1.1 `errno_to_error` — POSIX errno

Restated from SPEC §10.2.1 with prefix-stamping made explicit:

| `errno`                                   | Returned arm                          | Prefix stamp                          |
|-------------------------------------------|---------------------------------------|---------------------------------------|
| `ENOENT`, `ENOTDIR`, `ESRCH`              | `NotFound`                             | `none`                                 |
| `EACCES`, `EPERM`, `EROFS`                | `PermissionDenied`                     | `none`                                 |
| `EEXIST`, `ENOTEMPTY` (on create-only path) | `AlreadyExists`                      | `none`                                 |
| `EINTR` (after one bounded retry)         | `Interrupted`                          | `none`                                 |
| `ENOSYS`, `ENOTSUP`, `EOPNOTSUPP`, `EINVAL` | `Unsupported`                        | `none`                                 |
| `EAGAIN`, `EWOULDBLOCK`                   | `IoFailure { OsCode { errno } }`       | `again`                                |
| `EIO`, `ENXIO`, `EBADF`, `ENOSPC`, `EFBIG` | `IoFailure { OsCode { errno } }`      | `io`                                   |
| `EDQUOT`, `EUSERS`, `ELOOP`, `ENAMETOOLONG` | `IoFailure { OsCode { errno } }`     | `resource`                             |
| anything else                             | `OsCode { errno }`                    | `none`                                 |

EINTR retry rule: the *caller* (the syscall wrapper) retries exactly
once before invoking `errno_to_error`. The translator is pure; it
does not retry, does not call `errno`, does not maintain state. This
keeps the translator signal-safe (§6.2).

#### 4.1.2 `sdl_to_error` — SDL3 prose

Restated from SPEC §10.2.2 with the prefix stamping made explicit
and the `surface-lost` row added (currently a SPEC §10.4 footnote;
this design lifts it into the table for audit visibility):

| `SDL_GetError()` substring                                                | Arm                                | Prefix stamp        |
|---------------------------------------------------------------------------|------------------------------------|---------------------|
| `"Permission denied"`                                                     | `PermissionDenied`                 | `none`              |
| `"No such file"` / `"not found"`                                          | `NotFound`                         | `none`              |
| `"already"` / `"exists"`                                                  | `AlreadyExists`                    | `none`              |
| `"not supported"` / `"unsupported"`                                       | `Unsupported`                      | `none`              |
| `"interrupted"`                                                           | `Interrupted`                      | `none`              |
| `"Surface lost"` / `"Could not create CAMetalLayer"` / `"Drawable is nil"` | `IoFailure { OsCode { 0 } }`       | `surface_lost`      |
| any other non-empty msg                                                   | `IoFailure { OsCode { 0 } }`       | `io`                |
| msg null / empty                                                          | `OsCode { 0 }`                     | `sdl_empty`         |

The substring matcher is order-sensitive and stops at the first
match (longest-prefix wins by table ordering). This is documented in
the test fixture so a future row addition cannot accidentally shadow
an earlier match.

#### 4.1.3 `mach_to_error` — kern_return_t

This row set is small at MVP; mach-level errors mostly transit through
SDL3 or NSException before reaching us. Listed for completeness:

| `kern_return_t`                                       | Arm                                                  | Prefix stamp           |
|-------------------------------------------------------|------------------------------------------------------|------------------------|
| `KERN_SUCCESS` (translator never called on success)   | (undefined; assert in debug builds)                  | (n/a)                  |
| `KERN_INVALID_ARGUMENT`, `KERN_INVALID_VALUE`          | `Unsupported`                                         | `none`                 |
| `KERN_NO_ACCESS`, `KERN_PROTECTION_FAILURE`            | `PermissionDenied`                                    | `none`                 |
| `KERN_RESOURCE_SHORTAGE`, `KERN_NO_SPACE`              | `IoFailure { OsCode { kern_return } }`                | `resource`             |
| `KERN_ABORTED`, `KERN_OPERATION_TIMED_OUT`             | `Interrupted`                                         | `none`                 |
| `KERN_FAILURE` and unmapped families                   | `OsCode { kern_return }`                              | `mach_unmapped`        |

#### 4.1.4 `ns_to_error` — NSException

Restated from SPEC §10.2.3 with prefix stamping made explicit:

| `[exception name]` / class                              | Arm                                                    | Prefix stamp           |
|---------------------------------------------------------|--------------------------------------------------------|------------------------|
| `NSFileNoSuchFileException`                             | `NotFound`                                              | `none`                 |
| `NSFileLockingException`                                | `PermissionDenied`                                      | `none`                 |
| `NSInvalidArgumentException`                            | `Unsupported`                                           | `none`                 |
| `NSRangeException`                                      | `Unsupported`                                           | `none`                 |
| any FSEvents-stream-failed code                         | `IoFailure { OsCode { code } }`                         | `watcher_unavailable`  |
| any CALayer / Metal acquire-drawable failure            | `IoFailure { OsCode { code } }`                         | `surface_lost`         |
| anything else                                           | `OsCode { code }`                                        | `ns_unmapped`          |

`code` is read from `[exception code]` (or zero if the exception
class does not carry one); the bridge's `@catch (NSException* e)`
block also stamps `[e name]`'s UTF-8 view into the prose TLS slot
(separate from the prefix slot — §3.3) so `glibre::log_error` can
emit it as `error.detail`.

### 4.2 Composition with `glibre::Error`

Per `error-model.md` Type Sketch, `glibre::Error` is the engine-wide
`eastl::variant` rolling up every per-context error enum.
`platform::Error` is one arm of that variant. The composition is
purely additive: a function returning
`std::expected<T, glibre::Error>` may receive a
`platform::Error` from a platform call and propagate it
unchanged, or it may extract the `platform::Error` and map it to
its own context's enum at the call site (composition rule 2).

```cpp
// example: render maps surface-lost to DeviceLost at its boundary
glibre::Result<Frame> begin_frame(Window& w) noexcept {
    return w.surface()
        .transform([](Surface s) noexcept { return Frame{std::move(s)}; })
        .or_else([](glibre::Error e) noexcept -> glibre::Result<Frame> {
            // Composition rule 2: map at the boundary that crosses
            // contexts. Render branches on platform's TLS prefix to
            // discriminate IoFailure variants.
            if (auto* p = std::get_if<platform::Error>(&e.code());
                p && std::get_if<platform::IoFailure>(p) &&
                detail::error::current_prefix() ==
                    detail::error::prefix::surface_lost) {
                return std::unexpected(glibre::Error{render::Error::DeviceLost});
            }
            return std::unexpected(std::move(e));   // pass-through
        });
}
```

The example is illustrative; render owns its own boundary code. The
load-bearing point is that *no part of platform* names `render::Error`
— the mapping is render's, made at render's call site, per the
composition rule.

## 5. Hot/cold path split

Glibre's `>= 1.5 ms headroom` requirement (`perf-budget.md`) and
platform's 0.20 ms sim + 0.05 ms submit cell (SPEC §9) collapse to
one rule for this aggregate: **error construction is off the success
path**. Concretely:

### 5.1 Hot path (success path)

The success path of every public §5 call returns
`std::expected<T, glibre::Error>` carrying a `T`. The
`platform::Error` value is *not constructed* on this path — `expected`
holds only `T`. The cold-path branch is a single tag check
(`expected::has_value()`); on success, the variant slot is never
visited, the TLS prefix is never read, and no translator runs.
Steady-state branch-prediction makes the success path a free
fall-through.

### 5.2 Cold path (failure construction)

When a backend call fails, the syscall wrapper invokes the
appropriate translator (§4.1). All translator costs are absorbed in
the cold path:

- **Table lookup.** A `switch` on the OS code (errno / kern_return)
  or a small `eastl::array<{const char*, Arm, const char*}, N>`
  scan (SDL prose / NSException name). Both are O(1) for fixed N
  ≤ 16 with hot-cache code.
- **Prefix stamp.** A single `__thread const char*` write. No
  allocation, no string copy.
- **Arm construction.** The variant arms are zero-sized (or
  4-byte `OsCode`); construction is trivial copy.
- **`std::unexpected` wrap.** A move into the `expected` failure
  slot; `glibre::Error` itself is a small variant, and its
  `ErrorContext` is `__FILE__` / `__LINE__` literal pointers
  (per `error-model.md`).

Aggregate cold-path cost is bounded by **<1 µs per failure** on M1
firestorm; the platform's 0.20 ms cell can absorb hundreds of
failures per frame without breaching budget. The CI gate (§9) does
not assert this directly because the cell already covers it as part
of the `Pump` and `FileIo` aggregate budgets that *call* the
translators.

### 5.3 What is forbidden on the hot path

- **Never**: an `eastl::visit` over `platform::Error` on the success
  path of a public call.
- **Never**: a TLS prefix read in steady-state code that does not
  hold an `Error` value.
- **Never**: dynamic allocation inside a translator. Translators are
  pure; allocation would interact with the per-context
  `PerContextAllocator` and could deadlock the signal-safe path
  (§6.2).
- **Never**: a `dlsym` lookup, virtual dispatch, or vtable call on
  the translator path. The translators are inline-call free
  functions; the compiler may inline them at the call site.

### 5.4 Small-string source-tag option (cold path only)

When `glibre::log_error` formats a `platform::Error` for spdlog, it
*may* read a thread-local `source_tag` slot that the translator can
optionally populate with a static string identifying the OS source
("posix", "sdl", "mach", "appkit"). The slot is purely informational
— no engine code branches on it — and lives next to the prefix slot
(§3.3) in the same TLS struct so a single `__thread` access reaches
both. The `source_tag` is **opt-in**: translators that benefit from
the disambiguation set it; `errno_to_error` always sets `"posix"`,
`sdl_to_error` always sets `"sdl"`, etc. The slot is reset alongside
the prefix at frame phase 1.

## 6. Concurrency

### 6.1 Thread-safety by construction

`platform::Error` values are POD: zero-sized tag structs, a
4-byte-int `OsCode` payload, an `eastl::variant` tag byte. They are
trivially copyable, trivially move-constructible, and have no
non-trivial destructor. Two threads constructing two different
`Error` values share no state; passing an `Error` from thread A to
thread B is a value copy and races on nothing.

This is the load-bearing property that makes the cross-aggregate
error surface work without a lock or atomic anywhere on the value
itself: every `std::expected<T, glibre::Error>` returned across the
public §5 surface is by-value, and every consumer either propagates
the value (no shared state) or maps it to its own enum (also
by-value).

### 6.2 Signal-safety of the construction path

Crash-dump capture (SPEC §4.5, harmonius `crash-reporting.md` R-14.4)
runs in a SIGSEGV / SIGBUS handler. The handler must not
allocate, must not lock, must not call any non-async-signal-safe
function. Construction of `platform::Error` is therefore allowed in
a signal handler:

- Tag-struct construction is constexpr-default + trivial copy. Safe.
- `OsCode` construction is a 4-byte integer init. Safe.
- `IoFailure { OsCode { errno } }` construction reads `errno`
  (async-signal-safe per POSIX) and writes a stack-local. Safe.
- TLS prefix write: `__thread` is implemented by the dynamic linker
  via the TLS descriptor table; on macOS this *is* lazy at first
  access per dylib, which means the *first* TLS access in a signal
  handler can trip a lazy resolver. To stay signal-safe, the
  translator on the signal path takes a different shape: it returns
  the typed arm without stamping the prefix. The prefix slot is
  pre-touched (read once at process init) to force resolver
  materialisation; subsequent accesses are purely a load/store
  against an already-resolved slot, which *is* safe.
- `std::unexpected` wrap: constexpr; safe.

The signal-safe construction discipline is a single test: the e2e
crash-capture fixture installs `SIGSEGV`, dereferences a null
pointer, and asserts that the captured `platform::Error` round-trips
through the structured log format (§7) without aborting the
captured-frame snapshot.

### 6.3 Translators are reentrant

The four translators read no module-private state (errno is per-thread
by POSIX rule, SDL_GetError() reads SDL's TLS, NSException is
caller-stack-bound, kern_return_t is a plain integer). They may be
called concurrently from any number of threads. The TLS prefix slot
is per-thread; two threads stamping different prefixes do not race.
Single-threaded reasoning suffices for every consumer.

### 6.4 Cross-thread error propagation

When the `FileIo` worker thread (SPEC §6.5) or the `FileWatcher`
FSEvents callback thread (SPEC §6.4) constructs an `Error` and
hands it to the main thread via the SPSC ring, the value travels
by-copy. The TLS prefix does **not** travel — it is a per-thread
slot, and the consumer thread reads its own slot, which reflects
its own most recent translator call (or `prefix::none`). For
cross-thread prefix-discrimination, the producer copies the prefix
literal pointer into the SPSC slot alongside the `Error`; consumer
reads it from the slot, not from TLS. This is a **per-aggregate**
decision (FileIo and FileWatcher implement it; not part of the §5
public surface), and the SPSC slot layout is in the respective
sibling design (`file-io-design.md`, `file-watcher-design.md`).

## 7. Persistence + ABI

### 7.1 Live state — none

`platform::Error` values are by-value error returns. They are not
stored as components, not stored as singletons, not stored in any
plugin-private heap. SPEC §7.1 confirms: zero `.fory` schemas,
zero persistent state.

### 7.2 Log / replay carrier — `PlatformErrorRecord`

For `glibre::log_error` to emit a structured spdlog record, and for
the e2e replay harness (`specs/e2e/SPEC.md`) to capture a
`platform::Error` into the trace stream, a small Fory schema
serialises the *log carrier* — not the error value itself. The
carrier's purpose is single-shot logging / replay, never
reconstruction of a live `Error` (the variant is rebuilt only for
replay assertions, never returned to engine code).

```fory
// data/schemas/platform/PlatformErrorRecord.fory
//
// Wire shape used by glibre::log_error and by the e2e replay
// trace stream. Stable on the wire; field tags immutable per
// fory-codegen.md migration rules.
type PlatformErrorRecord {
    1: required uint8       arm_tag;        // 0..6 matching variant index
                                            // (NotFound, PermissionDenied,
                                            //  AlreadyExists, Interrupted,
                                            //  Unsupported, IoFailure, OsCode)
    2: optional int32       os_code;        // present when arm_tag == 5 or 6
    3: optional string      prefix;         // diagnostic prefix literal (§3.3)
    4: optional string      source_tag;     // "posix" / "sdl" / "mach" / "appkit"
    5: optional string      detail;         // SDL prose / NSException name (cold-path only)
    6: required uint64      monotonic_ns;   // Clock::now() at construction
    7: required uint32      thread_id;      // POSIX TID for cross-thread audit
}
```

Schema rules (per `fory-codegen.md`):

- `SchemaVersion` starts at 1; field tag numbers immutable.
- Removing a field moves the tag to the reserved set.
- Adding a field bumps the schema version and adds a one-step
  migrate function (pure, allocator-arena-only).
- The schema lives at `data/schemas/platform/PlatformErrorRecord.fory`
  and contributes to `glibre_types_abi_hash()` via `glibre-types.dylib`.

### 7.3 ABI hash contribution

The `PlatformErrorRecord` schema is the **only** platform-context
contribution to `glibre_types_abi_hash()` from the error aggregate
(SPEC §7's "platform contributes nothing to AbiHash" line is an
MVP-scope statement; once log replay lands the ledger gains this
one entry). Per `plugin-abi.md`, any change to the record's field
tags / types triggers an ABI hash bump and forces every plugin
(including the platform plugin itself) to be rebuilt against the
new `glibre-types.dylib`.

The closed `platform::Error` variant *itself* — its arm count, its
arm names, its tag-discriminant indices — does **not** live in
`glibre-types.dylib` (the variant is part of the platform plugin's
private surface, not a middleman type). However, the
`PlatformErrorRecord::arm_tag` field encodes the variant's arm
ordering, so adding a new arm to `platform::Error` requires:

1. Bumping the schema version (`PlatformErrorRecord` v1 → v2).
2. Adding a migrate function that maps every old-schema record's
   `arm_tag` to the new index (almost always identity if the new
   arm is appended).
3. The ABI hash bump that the schema-version edit triggers.
4. Barrier-time validation at hot-reload (§8): the platform plugin
   refuses to swap if the live engine's `arm_tag` enumeration
   disagrees with the loaded plugin's compiled-in mapping.

This is the single load-bearing reason `platform::Error`'s arm
ordering is documented in §3.1 alongside the variant declaration —
the order is the wire-format order, not just an implementation
detail.

## 8. Hot-reload

### 8.1 Per-aggregate disposition

| Concern                                     | Disposition                                                                                                                                                                       |
|---------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `platform::Error` value crossing the swap   | Pass-through (SPEC §8.1). The value is by-value; the swap is invisible to it.                                                                                                      |
| Translator function pointers                | Re-resolved by Q's `register` (the new platform plugin's text segment owns the translator code). Engine code never holds a translator function pointer; it calls them by name only. |
| TLS prefix slot                             | Reset to `prefix::none` at the start of the next phase 1 input drain. Pre-swap pointer values point to literals in P's `.rodata` and become invalid after `dlclose(P)`; reset is the load-bearing rule. |
| Diagnostic prose TLS slot                   | Reset alongside the prefix slot. Any unread prose is lost across the swap; this is acceptable because the engine never branches on prose, only logs it.                            |
| `PlatformErrorRecord` schema                | Survives via `glibre-types.dylib` middleman. Migrate function runs at phase 8 step 3 if the schema version bumped (§7.2).                                                            |
| Closed-sum invariant validation             | At swap step 2 (ABI hash check): the loaded plugin's compiled-in arm count and ordering must match `glibre-types.dylib`'s `PlatformErrorRecord::arm_tag` enumeration. Mismatch = `core::Error::PluginAbiHashMismatch`. |

### 8.2 Barrier-time validation

When the platform plugin reloads (SPEC §8.3), the loader runs the
following error-aggregate-specific checks at swap step 2:

1. **ABI hash check** (already required by `plugin-abi.md`). Covers
   the `PlatformErrorRecord` schema; covers `glibre-types.dylib`
   identity.
2. **Arm-count compile-time check.** The new platform plugin
   compiles in a `static_assert` that
   `eastl::variant_size_v<platform::Error> == ARM_COUNT_LITERAL`
   matching the wire-format arm count from `glibre-types.dylib`.
   A mismatch is a compile error in the plugin's build, not a
   runtime check; the runtime check is the ABI hash.
3. **Prefix-literal residence check.** Any TLS slot still holding a
   prefix literal pointer from the outgoing plugin P must be reset
   *before* `dlclose(P)`, or the next read returns a dangling
   pointer to unmapped memory. The `drain` step (SPEC §8.3 step 1)
   adds `detail::error::reset_prefix()` as the first action; the
   `register` step does the same on every thread the new plugin
   accesses.

### 8.3 What survives, what is discarded

- **Survives:** the `platform::Error` variant *type* (it's a
  middleman-relevant shape via `PlatformErrorRecord::arm_tag`); the
  schema version and migrate chain; the engine-wide
  `glibre::Error`'s platform arm slot.
- **Discarded:** in-flight prose / prefix TLS slot contents
  (acceptable; reset on next access); cached translator function
  pointers (engine never caches them); any plugin-private translator
  state (translators are stateless by design).

### 8.4 Refusal cases inherited

Platform-error inherits SPEC §8.4 universal refusals (ABI hash
mismatch, plugin init failure) and adds none of its own. The
mid-frame refusal (P1 in SPEC §8.4) is the window-surface aggregate's
concern, not platform-error's.

## 9. Performance

The platform-error aggregate sits inside platform's 0.20 ms sim +
0.05 ms submit cell (SPEC §9.1) and contributes **0 ms / frame** in
steady state because error construction is off the success path
(§5). Failure-path costs are absorbed by the consuming aggregate's
budget (`Pump`, `FileIo`, `FileWatcher`, `Window`), not by an
error-aggregate-specific budget.

### 9.1 Steady-state cost

| Path                                | Cost                                                                                  |
|-------------------------------------|---------------------------------------------------------------------------------------|
| Success path (no `Error` constructed) | **0** — `expected::has_value()` is a single tag check; success short-circuits.       |
| TLS prefix slot access (success)     | **0** — never read on the success path.                                               |
| Translator invocation (failure)      | **<1 µs** — table lookup + tag-struct construct + TLS write + `unexpected` wrap.    |
| `eastl::visit` on `Error` (handler) | **<200 ns** — visitor over 7 arms is a jump-table dispatch.                           |

### 9.2 Heap impact

Zero per-frame allocations from this aggregate. The TLS slots
(prefix pointer, prose buffer, source_tag pointer) are allocated
once at thread creation by the platform sub-arena (§9.2 of SPEC,
absorbed under `Window/Surface` 1 MiB or the FSEvents callback
ring); per-call cost is a single TLS write.

The diagnostic prose buffer is a fixed 256-byte stack-local scratch
inside the sibling translator, never owned by the `Error` value
itself. The `Error` value's heap footprint is its variant size:
**8 bytes** worst case (4-byte `OsCode` payload + tag + padding).

### 9.3 CI gate fixture

A Catch2 `BENCHMARK` block under `tests/platform/error/perf/`
asserts:

1. **Translator throughput.** `errno_to_error` invoked 1 000 000
   times over the full row set must complete in **≤ 5 ms** wall
   clock on M1 firestorm (sub-5 ns per call). Verifies the
   table-lookup is O(1) and inlines as expected.
2. **Success-path zero-cost.** A success-path benchmark constructs
   `expected<int, glibre::Error>` 1 000 000 times with `T = 42` and
   verifies the `expected` slot's `Error` is never constructed
   (counted via a sentinel `ErrorContext` initial value); time is
   bounded by 1 ms.
3. **Cold-path budget cap.** A failure-path benchmark constructs
   `unexpected(Error{IoFailure{OsCode{ENOENT}}})` 100 000 times and
   asserts wall-clock ≤ 1 ms (≤ 10 ns per construction).

These benchmarks live alongside the sibling-aggregate perf gates
(SPEC §9.4) but do not assert against the 0.20 ms cell directly —
the cell is a per-aggregate budget, and platform-error rolls into
the consuming aggregate's row.

## 10. Failure modes

### 10.1 Translator-internal failure modes

The translators themselves can fail to recognise an OS code (the
"meta" failure mode). The contract is:

1. **Unrecognised errno.** Falls through to the `OsCode { errno }`
   raw-fallback arm (§4.1.1 last row). Prefix stamp: `none`.
   Severity: `error` (per SPEC §10.1 `OsCode` row — "we refuse to
   silence what we have not classified"). Telemetry signal: a
   recurring unmapped errno is the trigger to add a new translator
   row in the next §4.7 edit.
2. **Unrecognised SDL prose.** Falls through to
   `IoFailure { OsCode { 0 } }` with prefix `io` (non-empty msg) or
   to `OsCode { 0 }` with prefix `sdl_empty` (null/empty msg).
   Severity: `error` for `OsCode` raw-fallback, `error` for
   unclassified `IoFailure` (per SPEC §10.6).
3. **Unrecognised kern_return_t.** Falls through to
   `OsCode { kern_return }` with prefix `mach_unmapped`.
   Severity: `error`.
4. **Unrecognised NSException class.** Falls through to
   `OsCode { code }` with prefix `ns_unmapped`. Severity: `error`.

**Rationale (one-paragraph audit answer per the spike's
"meta-failure-mode" goal):** the catch-all `OsCode` arm exists
specifically so the translator never returns a "successfully
translated" arm when the OS code is in fact unmapped. The temptation
to default to `IoFailure { OsCode }` for the catch-all is rejected
because `IoFailure`'s severity is `error` *only when the prefix is
unclassified* — the SPEC §10.6 table grants `warn` to known
recoverable prefixes. Using the catch-all `OsCode` arm instead makes
the meta-failure loud (`error` always) and visible in telemetry as a
distinct top-level arm count (`error.tag = "OsCode"` is countable in
the structured log stream and appears as a perf-budget tripwire when
its frequency rises). The cost of wrongly classifying an unmapped
code as `IoFailure` is silent operational degradation; the cost of
correctly surfacing it as `OsCode` is one extra row in the next §4.7
edit. The asymmetry favours `OsCode`. Promotion to a typed arm
follows the SPEC §10.8 second-consumer rule, never the
"saw it once in telemetry" reflex.

### 10.2 Translator caller misuse

| Misuse                                                          | Disposition                                                                                                                                                                                            |
|-----------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Caller passes `errno = 0` to `errno_to_error`                  | Returns `OsCode { 0 }` with prefix `none`. Debug builds assert; the assert message names the call site. POSIX guarantees `errno` is unset only after `errno = 0` is explicitly performed; getting 0 here means the wrapper failed to read `errno` before recovery work clobbered it. |
| Caller calls `sdl_to_error` with `nullptr` msg                  | Returns `OsCode { 0 }` with prefix `sdl_empty`. No crash; the null pointer is checked before `strstr`-style scan.                                                                                       |
| Caller calls `mach_to_error` with `KERN_SUCCESS`                | Returns `OsCode { 0 }` (success-misuse path); debug assert fires.                                                                                                                                       |
| Caller calls `ns_to_error(nullptr)` from outside `bridge.mm`    | Build error: the symbol is link-private to `bridge.mm.o` via internal-linkage `inline` definition. A call from another TU does not link.                                                                |
| Caller adds a new public arm without bumping `PlatformErrorRecord` | Compile error: the wire-format `arm_tag` enumeration is generated from `platform::Error`'s declaration order via codegen; a new arm without a schema bump fails the codegen consistency check. |

### 10.3 Concurrency-mode failure

| Mode                                                                         | Outcome                                                                                                                  |
|------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------|
| Two threads stamping different prefixes                                      | Each writes its own TLS slot; no race. Deterministic per-thread.                                                          |
| Translator called from a signal handler before TLS resolver materialised     | Signal-safe path skips the prefix stamp (§6.2); arm value still correct.                                                  |
| Plugin reload mid-stamp on another thread                                    | Forbidden by SPEC §8 phase 8 barrier — reload runs only on the game-loop driver thread when no other thread is in plugin code. The TLS prefix reset (§8.2) runs after every thread's plugin-code re-entry has been suspended by the loader. |

## 11. Test plan

### 11.1 Unit tests (Catch2)

Live under `tests/platform/error/`. The tables in §4.1 *are* the
test goldens; every row has a matching test. Test files map 1:1 to
translators:

- `tests/platform/error/errno_to_error_test.cpp` — one `SECTION`
  per row in §4.1.1. Includes the `EINTR-after-retry` semantics
  (translator does not retry; it produces `Interrupted` only when
  invoked, and the syscall wrapper's retry behaviour is exercised
  in the FileIo-aggregate test, not here).
- `tests/platform/error/sdl_to_error_test.cpp` — one `SECTION`
  per prose substring; longest-prefix-wins ordering is verified by
  a row that crafts a message matching two substrings and asserts
  the first-table-row wins.
- `tests/platform/error/mach_to_error_test.cpp` — one `SECTION`
  per kern_return_t family.
- `tests/platform/error/ns_to_error_test.cpp` — bridge-only test;
  compiled into the `bridge.mm` test fixture (the only `.mm` file
  in `tests/`) using a stub `NSException`-shaped struct to keep
  the test TU pure C++23 except for that one fixture file.
- `tests/platform/error/diagnostic_prefix_test.cpp` — verifies
  TLS prefix stamp / read / reset, including signal-safe access
  after a pre-touch.

Every translator test asserts:

1. The returned arm matches the row's expected variant.
2. The TLS prefix matches the row's expected prefix literal
   (pointer-equality against `prefix::xxx`, not string equality).
3. The arm round-trips through `PlatformErrorRecord` Fory
   serialisation and deserialisation byte-equal.
4. `eastl::variant_size_v<platform::Error>` matches the
   `PlatformErrorRecord::arm_tag` enum size at compile time.

### 11.2 Cross-aggregate uniformity tests

Live under `tests/platform/error_uniformity_test.cpp`. Asserts that
every public `Result<T>` returning function in §5 surfaces only
arms drawn from `platform::Error`'s closed sum. The test
enumerates every public function, calls it under fixture-induced
failure (a `FileIo::read_all` on `/proc/self/fd/9999`, a
`Window::open` with a zero-dimension `WindowDesc`, etc.), unwraps
the returned `glibre::Error`, and asserts the platform arm is one
of the seven variants. No fixture may surface a `core::Error` /
`render::Error` / etc. through a platform call.

### 11.3 Integration tests (against e2e replay)

Live under `tests/e2e/platform/error_round_trip/`. Constructs a
known sequence of `platform::Error` values (one per arm, one per
prefix), serialises each through `PlatformErrorRecord`, captures
into a `.glibre-trace`, and asserts the trace deserialises into
the original arm + prefix. The test exercises the §7 wire-format
contract end-to-end and is the single fixture that breaks on a
schema-version regression.

### 11.4 Performance tests

`tests/platform/error/perf/` — three Catch2 `BENCHMARK` blocks
matching §9.3.

### 11.5 Compile-fail tests

Under `tests/platform/error/compile_fail/`, using
`add_test(... CONFIGURATIONS CompileFail)` in CMake:

- A test that adds a hypothetical 8th arm to `platform::Error`
  without bumping the schema must fail the codegen consistency
  check.
- A test that calls `ns_to_error` from a non-`bridge.mm` TU must
  fail at link.
- A test that uses `try { } catch (...)` inside engine code must
  fail under `-fno-exceptions`.

### 11.6 Fuzzing (deferred, post-MVP)

A future fuzz harness (`tests/platform/error/fuzz/`) feeds random
errno / kern_return_t / NSException name byte streams into the
translators and asserts: (a) no crash, (b) the returned arm is one
of the seven, (c) the TLS prefix is one of the registered literals
or `prefix::none`. Recorded here as a forward declaration; not
required for MVP closure (SPEC §11 acceptance criteria do not
include a fuzz story).

## 12. Open questions

- [OPEN] Should the diagnostic prefix slot widen from `const char*`
  (literal pointer) to `eastl::string_view` (literal pointer + length)
  to make spdlog formatting allocation-free in all cases? Current
  literals are NUL-terminated; spdlog can format them via `%s`. The
  trade-off is a doubled TLS slot footprint (16 B vs 8 B) for a
  marginal format-time speedup. Re-opened by the
  `task-breakdown-error-perf` plan when CI gate timings land.
  No platform-aggregate-side residue if deferred.

- [OPEN] Should `PlatformErrorRecord::arm_tag` be a generated
  enumeration from `platform::Error`'s declaration (codegen-driven)
  or a hand-maintained `uint8_t` index list? Codegen is tighter
  (drift-impossible) but introduces a build-time dependency between
  `glibre-types-codegen` and the platform plugin's translation unit.
  Hand-maintained is simpler at MVP but invites drift. Resolution
  gate: the fory-codegen plan that introduces
  `data/schemas/platform/PlatformErrorRecord.fory` decides at
  authoring time. Until then the schema's `arm_tag` is hand-maintained
  with a unit test asserting parity (§11.1 row #4).

- [OPEN] Promotion of `IoFailure` prefix `surface_lost` to a
  first-class `SurfaceLost` arm. Same shape as SPEC §10.8 entry —
  trigger is a second consumer beyond render needing typed dispatch.
  Mirrored here so the spike audit captures the gate. No platform-
  aggregate-side residue if deferred.

- [OPEN] Promotion of `IoFailure` prefix `watcher_unavailable` to a
  first-class `WatcherUnavailable` arm. Same shape as SPEC §10.8;
  trigger is the editor / hot-reload coordinator landing. Mirrored
  here. No residue.

- [OPEN] Cross-thread prefix propagation for FileIo / FileWatcher
  (§6.4). Each sibling aggregate decides its own SPSC-slot layout;
  the design here only requires that they not rely on the consumer
  thread's TLS for prefix discrimination. Resolution gate: the
  respective sibling's task-breakdown spike (file-io, file-watcher)
  decides whether to copy the prefix into the slot or to add a
  per-row prefix column. No platform-error-aggregate-side residue.

- [OPEN] Should the signal-safe path also stamp `source_tag`
  (§5.4)? Currently both prefix and source_tag stamps are skipped
  in signal context to avoid the lazy-resolver hazard (§6.2). If
  pre-touching one slot makes the other equally cheap, the
  decision is "stamp both". Resolution gate: the crash-capture
  end-to-end test in `tests/e2e/platform/crash/` measures the
  cost. No platform-error-aggregate-side residue at MVP.
