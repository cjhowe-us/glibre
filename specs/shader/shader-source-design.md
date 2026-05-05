# Shader-Source Detailed Design

> Detailed design for the `shader` context's `ShaderSource` aggregate
> (SPEC §4.1, §5 `ShaderSource` / `EntryPoint` / `PreprocessedSource`).
> Refines `specs/shader/SPEC.md` §4.1 and the harmonius-mined
> ingredients of §3 collapse 1 (Slang-only source language) and §3
> collapse 4 (no bytecode-container reflection).
> All conclusions re-derived; harmonius prior art
> (`/Users/cjhowe/Code/harmonius/docs/requirements/content-pipeline/hot-reload.md`
> R-12.4.3, `docs/requirements/tools/shared-cache.md` R-15.11.2,
> `docs/requirements/rendering/gpu-abstraction-layer.md` R-2.1.16,
> `docs/design/rendering/shader-variants.md`,
> `docs/design/rendering/render-pipeline.md`) cited as research input
> only.

Refs: spike #745 — `[SPIKE] design-shader-shader-source-detailed`.
Parent sub-epic #744 (`[SUB-EPIC] Detailed Designs — shader`). Sibling
task-breakdown spike blocked-by this deliverable.

## 1. Purpose

`ShaderSource` is the single aggregate in the `shader` context permitted
to read `.slang` files from disk, expand their `#include` closure, and
produce the canonical translation-unit value (`PreprocessedSource`)
that every downstream aggregate keys against. Its one responsibility is
**ingesting authored Slang text and presenting it as a sealed,
content-addressed translation unit**: resolve a project-relative path
against the project source root, parse `#include` directives, walk the
directed include graph with cycle detection and escape rejection,
normalize the post-include byte stream, scan stage-tagged entry points,
and finalise a BLAKE3 content hash over the normalized bytes plus the
ordered include closure. On any refusal it leaves the disk untouched
and the caller's prior `ShaderSource` (if any) live and unchanged.

What `ShaderSource` explicitly refuses to own:

- **The permutation key.** `PermutationKey` (SPEC §4.2) is a separate
  aggregate; ingestion never reads or mutates a key. The detailed
  design lives in spike #747.
- **Compilation.** Driving slangc, shaping argv, sandboxing the
  subprocess, capturing stderr — `CompilationPipeline` (SPEC §4.3,
  spike #749). `ShaderSource` produces inputs; the pipeline consumes
  them.
- **Reflection.** Ingesting slangc's native reflection JSON and tagging
  bindings with descriptor frequency — `ReflectionBlob` (SPEC §4.4,
  spike #751).
- **Descriptor layout.** Projecting reflection onto the four-frequency
  schema — `DescriptorLayout` (SPEC §4.5, spike #753).
- **Cache identity arithmetic.** The shader cache key composes the
  source hash with the resolved key, canonical compile flags, and
  target (SPEC §2 `Shader Hash`); `ShaderSource` produces only the
  source-hash component. The composing aggregate is the
  `ShaderCache` (SPEC §4.6, spike #755).
- **File-system watching.** The watcher loop, debounce policy, and
  event coalescing live in the platform `FileWatcher` (spike #719);
  `ShaderSource` exposes a re-ingestion entry point the watcher calls,
  but does not poll, schedule, or own the watch.
- **Slang authoring conventions.** Material-graph codegen into Slang
  belongs to the `material` context (SPEC §3 refusal 2). `ShaderSource`
  treats every input as authored bytes, regardless of provenance.
- **GPU memory, PSO objects, runtime binding.** SPEC §3 refusals 1,
  4, 5.

If the resolution algorithm, the include syntax, the cycle-detection
rule, the escape-rejection rule, the entry-point attribute scanner, the
normalization rule, or the BLAKE3 composition over the closure
changes, this design changes. Anything else is out of scope.

## 2. Requirements Coverage

Mapping of harmonius requirements to MVP refusal-or-coverage. Every
entry is independently re-derived; harmonius is research input only
(PHILOSOPHY §"How harmonius is used").

| Harmonius source                                                                              | Glibre disposition (MVP)              | Coverage site                                                                                                                          |
|-----------------------------------------------------------------------------------------------|---------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------|
| `requirements/content-pipeline/hot-reload.md` R-12.4.3 — detect shader source change, recompile affected permutations, swap at frame boundary, viewport-overlay errors, prior shader remains active | **Covered (ingest half)**             | §8 below: re-ingestion on watcher tick; trigger condition is `total_hash` change; refusal leaves prior `ShaderSource` live (SPEC §8.4). |
| `requirements/tools/shared-cache.md` R-15.11.2 — cache by **source hash**, platform, permutation flags                                  | **Covered (source-hash half)**        | §7 below pins BLAKE3 composition; the platform-/flag-/key-side composition is `ShaderCache`'s (#755).                                  |
| `requirements/rendering/gpu-abstraction-layer.md` R-2.1.16 — Slang front-end, no runtime compile in shipping                            | **Covered**                           | §5 surface ships only; `ShaderSource::open` is excluded from shipping link target (SPEC §6.1, §6.5); the runtime path uses cached blobs keyed by hash. |
| `design/rendering/shader-variants.md` — preprocessor closure feeds permutation enumeration                                              | **Covered (closure half)**            | §3.4 constructs the closure; permutation enumeration consumes it without mutating it.                                                  |
| `design/rendering/render-pipeline.md` § "Shader Compilation Pipeline" — single Slang front-end, no transpile chain                       | **Covered**                           | §3 below treats Slang as the only input language; no fallback parser is admitted.                                                      |
| `design/rendering/shader-variants-test-cases.md` — deterministic content hash, bit-stable across hosts                                  | **Covered**                           | §7 + §11: BLAKE3 over normalized bytes + ordered closure; goldens enforce bit-equality across runs.                                    |
| `design/rendering/render-pipeline.md` § RF-9 — re-run reflection on new bytecode so descriptor layout stays in sync                       | **Refused (out of scope here)**       | RF-9's reflection-side obligation belongs to `ReflectionBlob` (#751); `ShaderSource` only re-publishes a new `total_hash`.              |

Glibre-native requirements added beyond harmonius:

- **Project-rooted include resolution; no absolute paths, no
  upward-escapes.** SPEC §4.1 invariant 3. Defense against
  cross-project leakage and against non-deterministic includes that
  depend on host-side filesystem layout.
- **Acyclic include graph with explicit cycle reporting.** SPEC §4.1
  invariant 3. Cycles are a first-class refusal arm (`IncludeCycle`)
  not an infinite-recursion crash. The cycle path is attached to the
  diagnostic envelope so the editor can render it.
- **Total content hash composes normalized bytes + ordered closure.**
  Two byte-equal authored sources whose include-graph closures differ
  must produce different `total_hash`. Re-derived: a cache key that
  ignores the closure aliases two authoring states and breaks
  R-15.11.2.
- **Encoding is fixed (UTF-8, LF-normalized, BOM-stripped).** Required
  for cross-host bit-stability of `total_hash` (PHILOSOPHY §7
  determinism). Authoring tools that emit CRLF or UTF-8-BOM are
  normalized at ingestion; the on-disk file is never rewritten.
- **One translation unit per ingest.** `ShaderSource::open` admits one
  `.slang` file at a time. Batched ingestion is the cooker's
  responsibility (parallelism per §6 below); no fan-out lives inside
  the aggregate.

## 3. Detailed Model

### 3.1 Aggregate composition

```text
ShaderSource (root, value-shaped)
├── SourceId                  id_                    // §5
├── PreprocessedSource        preprocessed_          // §5
│     ├── eastl::vector<std::byte>     bytes_         // normalized, post-include
│     ├── eastl::vector<IncludeNode>   include_closure_
│     └── ShaderHash                 total_hash_
└── eastl::vector<EntryPoint> entry_points_         // §5
```

`ShaderSource` is constructed exclusively through the static factory
`open(project_root, project_relative)`. There is no public
constructor; copy / move are defaulted (the value is large but cheap
to move). The aggregate holds no file handles after `open` returns —
it is a pure in-memory snapshot of one ingestion.

The implementation pipeline is internal and split into four pure
subroutines, each called exactly once per `open`:

```text
open(project_root, project_relative)
  1. abs          := resolve(project_root, project_relative)    → AbsolutePath
  2. (bytes, inc) := ingest_closure(abs)                       → (normalized_bytes, ordered_includes)
  3. eps          := scan_entry_points(bytes)                  → entry_points
  4. hash         := hash_total(bytes, inc)                    → ShaderHash
```

`open` is the sole caller of all four subroutines. They are not
chained — each subroutine receives its inputs from `open` directly,
not from the return value of its predecessor.

Each subroutine has no side effects beyond reading from the project
source root and allocating against the `ContextTag::shader` arena
(SPEC §9.4). They are tested in isolation in §11.

### 3.2 Resolver (`resolve`)

Project-rooted resolution against a single canonical project source
root. The resolver is the sole owner of the path-validation contract.

```text
resolve(project_root, project_relative):
    1. Reject if project_relative is absolute             → IncludeEscape
    2. Reject if project_relative contains "." segments
       that escape the root (`..`-walk normalisation)     → IncludeEscape
    3. Compose abs := canonical(project_root / project_relative)
    4. Reject if commonpath(abs, canonical(project_root))
       != canonical(project_root)                          → IncludeEscape
    5. Reject if !exists(abs) or !is_regular_file(abs)    → SourceNotFound
    6. Return abs
```

Notes:

- Canonicalisation uses `std::filesystem::weakly_canonical` for
  include-target paths (the file may not yet exist during a partial
  edit; the canonicalised form is still well-defined) and
  `std::filesystem::canonical` for the project root (must exist at
  ingestion time).
- Symlinks are followed for resolution but the *canonical* target must
  still lie inside the project root. A symlink whose target leaves the
  root is `IncludeEscape`.
- The project root is captured at `open()` time and threaded through
  every recursive include resolution; the resolver has no global
  state.
- The resolver is identical for the entry-point file and for every
  `#include` directive encountered during ingestion. There is one
  algorithm; the SPEC §4.1 invariant "include-graph closure resolves
  entirely under the project source root" holds for the root file as
  well.

### 3.3 Include directive parser

The parser recognises exactly two `#include` syntactic forms; anything
else is forwarded to slangc as Slang source and is not interpreted by
`ShaderSource`:

```slang
#include "path/relative/to/the/including/file.slang"   // form A
#include <project-relative/path.slang>                  // form B
```

Re-derivation of the syntax: harmonius's render docs use both forms
informally. Form A (quotes) follows the C/C++ convention of "search
relative to the including file first". Form B (angle-brackets) follows
the C/C++ convention of "search a system path"; in Slang the
project-source-root is the only system path, by collapse. There is no
support for include-search-path lists, no `<system>` vs
`<project>` distinction beyond this one, and no compiler-defined
search path.

| Form | Resolution rule                                                              |
|------|------------------------------------------------------------------------------|
| A    | `resolve(project_root, dirname(including_file) / quoted_path)`               |
| B    | `resolve(project_root, bracketed_path)`                                      |

Both resolutions go through §3.2's `resolve`, so the escape /
not-found / canonicalisation contract is identical for the two forms.

Lexical rules (kept minimal; everything not in this list is delegated
to slangc):

1. The directive is recognised only at the start of a logical line,
   after optional ASCII whitespace, and before any non-whitespace
   character that is not `#`.
2. `// line comment` and `/* block comment */` constructs are
   recognised so a directive embedded in commented-out code is **not**
   processed. The recogniser is intentionally line-oriented and
   small; it does not parse Slang expressions.
3. No macro expansion occurs in the path. The path is the literal
   text between the quotes / angle-brackets, byte-for-byte.
4. `\` line continuations inside the directive line are not
   supported; the directive must fit on one logical line.
5. UTF-8 byte sequences are passed through unchanged in the path
   bytes. Path canonicalisation operates on the byte sequence
   delivered to `std::filesystem::path`.

The parser does not handle Slang preprocessor macros (`#define`,
`#ifdef`, `#if`, `#else`, `#endif`). slangc owns those; they appear
unchanged in the normalized byte stream (§3.5) and contribute to the
`source_hash` exactly as written. Conditional includes (`#if … #include
… #endif`) are therefore ingested **unconditionally** — the include
target is admitted regardless of whether slangc's preprocessor would
later select that branch. Re-derivation: ingestion must be
deterministic and independent of permutation; conditional resolution
would couple ingestion to the key (SPEC §3 refusal: keys are
`PermutationKey`'s, #747's, not `ShaderSource`'s).

### 3.4 Closure walker (`ingest_closure`)

Iterative DFS over the directed include graph rooted at the entry
file. The walker maintains:

- `visited` — `eastl::hash_set<eastl::string>` of canonicalised
  project-relative paths already fully ingested, keyed by the byte
  string returned by `resolve`. Insertion order is irrelevant (set
  semantics); the closure's *output* order is recorded separately.
- `on_stack` — `eastl::vector<eastl::string>` recording the current
  recursion path for cycle detection. Push on entry, pop on exit.
- `closure_order` — `eastl::vector<IncludeNode>` capturing the
  ingestion order: a node is appended **after** its includes have
  been fully ingested (post-order). This guarantees a topological
  ordering: every include appears before every file that includes it.
- `out_bytes` — `eastl::vector<std::byte>` accumulating the
  normalized post-include byte stream (§3.5 explains the splice).

Algorithm:

```text
ingest_closure(root_abs):
    visited        := ∅
    on_stack       := []
    closure_order  := []
    out_bytes      := []
    walk(root_abs)
    return (out_bytes, closure_order)

walk(abs):
    if abs ∈ on_stack:
        raise IncludeCycle (detail = on_stack ++ [abs])
    if abs ∈ visited:
        # Already in the closure; splice nothing — the prior splice
        # contributed the bytes once, and the SPEC §4.1 invariant 2
        # (byte-equal preprocessed content for byte-equal source) is
        # preserved by visiting each file at most once per closure.
        return
    on_stack.push(abs)

    raw    := read_file(abs)            # may raise SourceNotFound
    bytes  := normalize(raw)            # §3.5 (UTF-8, LF, BOM strip,
                                        #       trailing newline)
    parts  := split_by_include_directives(bytes)
                                        # §3.3 lex; yields a sequence
                                        # of (verbatim_chunk |
                                        # IncludeDirective) tokens.

    for each token in parts:
        if token is verbatim:
            out_bytes.append(token.bytes)
        else:  # IncludeDirective
            target_rel := resolve_include_target(
                              project_root, abs, token.path, token.form)
            walk(target_rel)            # recurse first
            # The directive line itself is dropped from out_bytes;
            # the includee's bytes already entered out_bytes during
            # the recursive walk. This is the splice — no #line
            # markers are emitted (§3.5 explains why).

    on_stack.pop()
    visited.insert(abs)
    closure_order.append(IncludeNode{
        project_relative_path = relative_to_root(abs),
        content_hash          = blake3(raw)              # per-file hash; raw = pre-normalization bytes
    })
```

Notes:

- Recursion is iterative in the implementation (a manual stack of
  frames) to keep stack depth bounded for deeply-nested includes;
  the algorithm's semantics are the same as the recursive form
  written here.
- The same file included transitively from two different ancestors
  contributes its bytes **once**, at the first walk. This matches
  C/C++ `#pragma once` semantics by default. Include-guards in Slang
  source remain effective; they appear in the byte stream once and
  are inert on the (skipped) subsequent visit.
- `SourceNotFound` raised inside `walk` aborts the entire closure;
  partially-built `out_bytes` and `closure_order` are dropped.
- `IncludeCycle` carries the `on_stack` snapshot as its detail string
  (e.g. `a.slang -> b.slang -> c.slang -> a.slang`); the editor uses
  this to highlight the cycle.

### 3.5 Normalizer

Five rules pinned by determinism (PHILOSOPHY §7) and by the SPEC §4.1
invariant 2 (byte-equal preprocessed content → equal
`PreprocessedSource`):

1. **UTF-8 only.** Any byte sequence that is not valid UTF-8 is
   refused with `EncodingInvalid`. Re-derivation: hashing
   undefined-encoding bytes makes the hash a property of the editor
   that wrote the file, not of the content. UTF-8 is the only
   encoding admitted; UTF-16 / latin-1 / shift-jis files are refused
   at ingestion.
2. **BOM strip.** A leading UTF-8 BOM (`EF BB BF`) is stripped before
   the hash is computed. The on-disk file is never rewritten; the
   strip is a normalization at ingestion time.
3. **LF line-endings.** CRLF (`0D 0A`) sequences are normalized to LF
   (`0A`); bare CR (`0D`) is also normalized to LF. The intent is
   that a Slang file authored on Windows hashes byte-equal to the
   same file authored on macOS / Linux. The on-disk file is never
   rewritten.
4. **Trailing newline.** A file without a terminating LF gets one
   appended in the normalized byte stream. The on-disk file is never
   rewritten. Note: `IncludeNode.content_hash` is the BLAKE3 of
   **pre-normalization bytes** (`raw` from the §3.4 walker, before
   LF-normalization and trailing-newline append); it is deliberately
   distinct from the post-normalization bytes that enter `out_bytes`.
   See §7.3 for the rationale.
5. **No `#line` markers.** The normalizer does **not** emit `#line`
   pragmas at include splice points. Re-derivation: slangc receives
   the spliced byte stream but also receives the include closure
   metadata (via the §6.2 driver argv); if slangc needs source-line
   provenance for diagnostics, it gets it from the closure metadata,
   not from in-band markers. Embedding `#line` would couple the
   normalized byte stream's hash to the project root layout, defeating
   bit-stability across repository checkouts.

The normalizer is a pure function. Two ingestions of the same on-disk
project tree produce byte-equal `PreprocessedSource.bytes` (SPEC §4.1
invariant 2).

### 3.6 Entry-point scanner

A line-oriented scanner that recognises the Slang `[shader("...")]`
attribute on function declarations:

```slang
[shader("vertex")]
float4 main_vs(...) : SV_POSITION { ... }
```

Algorithm sketch (small; not a Slang parser):

```text
scan_entry_points(normalized_bytes):
    out := []
    for each `[shader("<stage>")]` annotation in normalized_bytes:
        # Allow ASCII whitespace and line breaks between the
        # attribute and the next identifier; reject if a second
        # `[shader(...)]` annotation precedes the function name.
        stage_str := <stage>
        stage     := parse_stage(stage_str)        # one of §5 Stage
                                                   # else EntryPointStageAmbiguous
        name      := next_identifier_after_attribute()
                                                   # else SourceParseFailed
        if (any preceding [shader(...)] for the same function name):
            raise EntryPointStageAmbiguous (detail = name)
        out.append(EntryPoint{ name, stage })
    return out
```

Behaviour pinned by SPEC §4.1 invariant 1 ("every emitted EntryPoint
has exactly one stage attribute"):

- Zero stage attributes on a function → not enumerated; not an error
  (the function may be a helper).
- Two or more stage attributes on the same function →
  `EntryPointStageAmbiguous`.
- A stage string that does not parse to a §5 `Stage` →
  `EntryPointStageAmbiguous` (detail attaches the offending string).
- An attribute with no following function declaration →
  `SourceParseFailed` (detail = "orphaned [shader(...)] attribute;
  no function declaration follows"). Re-derivation: an orphaned
  attribute is a structural parse error in the Slang source that
  makes the author's intent unknowable. SPEC §10.2 scopes
  `SourceParseFailed` to "malformed `[shader(...)]` annotation"
  open-time errors — this is the canonical arm for ingestion-time
  scan failures, distinct from the compile-time `EntryPointMissing`
  arm which fires when the caller asks the `CompilationPipeline` to
  compile a named entry point the `ShaderSource` manifest does not
  expose.

The scanner runs against the post-include normalized bytes, so an
entry point declared in an included file is enumerated from the
ingesting `ShaderSource`. This matches harmonius's render-pipeline
expectation that material-graph-codegen Slang (which `#include`s
shared library code) exposes its own entry points without
re-declaration. (Harmonius `advanced-materials.md` R-2.12.9; SPEC §3
collapse 1.)

### 3.7 Hash composition (`hash_total`)

The total content hash is BLAKE3 over a canonical byte sequence that
binds **the normalized bytes** and **the ordered include closure**:

```text
hash_total(normalized_bytes, closure_order):
    state := blake3_init()
    blake3_update(state, MAGIC := b"glibre.shader.source.v1")
    blake3_update(state, u32_le(len(normalized_bytes)))
    blake3_update(state, normalized_bytes)
    blake3_update(state, u32_le(len(closure_order)))
    for node in closure_order:                    # §3.4 fixed order
        path_bytes := utf8(node.project_relative_path)
        blake3_update(state, u32_le(len(path_bytes)))
        blake3_update(state, path_bytes)
        blake3_update(state, node.content_hash.bytes)   # 32 B BLAKE3
    return ShaderHash{ blake3_finalize(state) }
```

Rationale:

- **Magic prefix.** Domain-separates `ShaderSource` hashes from any
  other BLAKE3 use in the engine (cache-keys, schema-ABI hash). A
  byte stream that happens to look like a normalized Slang file
  cannot collide with a non-`ShaderSource` BLAKE3 even if both are
  composed into the cooker.
- **Length-prefixing every variable-length input.** Prevents
  collision attacks via boundary ambiguity (`{ "a", "b" }` vs `{ "ab",
  "" }`).
- **Closure inclusion.** A pure-bytes hash would alias two ingestion
  states whose normalized byte stream is identical but whose closure
  paths differ (e.g. moving a shared header from one project-relative
  location to another while keeping its content). The closure binding
  defeats this aliasing; SPEC §4.1 invariant 2 reads "byte-equal
  preprocessed content **and** byte-equal include-graph closure".
- **`u32_le` lengths.** Endianness is fixed; PHILOSOPHY §7
  determinism requires no platform-intrinsic encoding leaks into a
  hashed value.
- **Per-file `content_hash`.** Already a 32-byte BLAKE3 of each
  file's pre-normalization bytes (§3.4 walker). Including these
  binds the closure's *content*, not just its *paths* — two files at
  the same project-relative path but with different bytes produce
  different total hashes even when the splice produces identical
  `normalized_bytes` (an unrealistic case but cheap to defend).

The 32-byte digest is stored in `PreprocessedSource.total_hash` as the
SPEC §5 `ShaderHash`. The `ShaderCache` (#755) composes this with the
permutation key, canonical compile flags, and target ordinal to form
the final `ShaderHash` cache key (SPEC §2). The `ShaderSource`
aggregate is **not** the cache key; it is one of four inputs to the
cache key.

## 4. Public Surface

The §5 surface in `specs/shader/SPEC.md` is authoritative. This
section restates the subset owned by `ShaderSource` with per-method
behaviour annotations and pre-/post-conditions.

### 4.1 Types (locked from SPEC §5)

```cpp
namespace glibre::shader {

struct SourceId {
    eastl::string project_relative_path;
    friend bool operator==(const SourceId&, const SourceId&) noexcept = default;
};

struct EntryPoint {
    eastl::string name;
    Stage         stage{Stage::Vertex};
    friend bool operator==(const EntryPoint&, const EntryPoint&) noexcept = default;
};

struct IncludeNode {
    eastl::string project_relative_path;
    ShaderHash    content_hash{};            // 32-byte BLAKE3 per §3.4
};

struct PreprocessedSource {
    eastl::vector<std::byte>   bytes;            // post-include byte stream
    eastl::vector<IncludeNode> include_closure;  // ordered, acyclic, project-rooted
    ShaderHash                 total_hash{};
};

class ShaderSource {
public:
    static std::expected<ShaderSource, Error>
    open(const std::filesystem::path& project_root,
         const std::filesystem::path& project_relative);

    const SourceId&               id()             const noexcept;
    eastl::span<const EntryPoint> entry_points()   const noexcept;
    const PreprocessedSource&     preprocessed()   const noexcept;

private:
    // Private constructor used only by the test factory and by open().
    ShaderSource(SourceId, eastl::vector<EntryPoint>, PreprocessedSource);

#if defined(GLIBRE_E2E)
    // Grants the test-support factory access to the private constructor.
    // This guard is the only conditional in the aggregate header; the rest
    // of the class definition is unconditionally clean. §8.5.
    friend std::expected<ShaderSource, Error>
        glibre::shader::test::make_shader_source_from_bytes(
            eastl::span<const std::byte>, SourceId);
#endif
};

}  // namespace glibre::shader
```

Surface rules:

- **`std::expected<T, shader::Error>` at every fallible boundary.**
  Per `reviews/decisions/error-model.md`. `shader::Error` is a closed
  enum (SPEC §5); it composes into engine-wide `glibre::Error` at the
  cross-context seam, not inside `ShaderSource`.
- **No exceptions cross the public surface.** The aggregate compiles
  with `-fno-exceptions` (per error-model.md). Internal use of
  `std::filesystem` operations that may throw is wrapped at first
  ingress and translated to `Error`.
- **Public ABI uses POD spans / handles only.** SPEC §5 declares
  `eastl::vector` / `eastl::string` inside the *types* but the public
  ABI between plugins crosses through middleman `data` types
  (`reviews/decisions/plugin-abi.md`). `ShaderSource` does not cross
  the plugin ABI directly; consumers (`CompilationPipeline`, watcher
  callbacks) live in the same `shader` plugin and consume
  `eastl::span<const EntryPoint>` / `const PreprocessedSource&`
  in-process.
- **Borrow rules.** `id()`, `entry_points()`, and `preprocessed()`
  return references / spans valid for the lifetime of the
  `ShaderSource`. A consumer that needs stable storage copies the
  contents.

### 4.2 `ShaderSource::open` operations

Per-method contract:

- **`open(project_root, project_relative)`** — runs §3.2 → §3.4 →
  §3.6 → §3.7 in sequence. Pre-conditions: `project_root` is an
  absolute path to a directory that is the project's source root;
  `project_relative` is a project-relative path to a `.slang` file.
  Post-conditions on success: the returned `ShaderSource` satisfies
  every SPEC §4.1 invariant; the on-disk filesystem is unchanged;
  the `ContextTag::shader` arena holds the aggregate's storage.
  Failure modes: §10. Wall-time bound: §9.
- **`id() const`** — returns the `SourceId` derived from
  `project_relative` (after canonicalisation by §3.2). Identity is
  stable: two `open` calls against the same canonical path return
  equal `SourceId`s. Survives moves only via explicit rename (caller
  changes the input path and `open`s again).
- **`entry_points() const`** — span over the entry-point manifest
  produced by §3.6. Order is the order in which annotations were
  encountered in the post-include normalized stream; stable for a
  given input.
- **`preprocessed() const`** — reference to the
  `PreprocessedSource`. Hot consumers (cooker, hot-reload publisher)
  read `total_hash` first to short-circuit; the bytes and closure are
  read only by the `CompilationPipeline` driver argv builder
  (#749).

### 4.3 No public ABI surface

`ShaderSource` is a `shader`-plugin-internal aggregate. There is no
`extern "C"` boundary — the `shader` plugin's `IShaderBackend`
implementation calls `ShaderSource::open` directly (in-process). The
plugin-ABI (`reviews/decisions/plugin-abi.md`) only governs the
trait surface (`IShaderBackend`, SPEC §4.7 / §5), which itself takes
`const ShaderSource&` as a parameter; the type crosses the trait
boundary by reference, not by value.

The shipping cut (SPEC §6.5) excludes `source/` entirely; in
shipping, `ShaderSource::open` is unreachable code by construction
and the symbol is not linked. Any synthesised reference (e.g. test
harness mistakenly enabled in shipping) refuses with
`Error::ShippingCompilationAttempted` via the `IShaderBackend::compile`
guard at the trait boundary (SPEC §10.2.1 cross-reference, §8.4
refusal case 1).

## 5. Hot / Cold Path Split

Source ingestion is a **cold path** by design. The full split:

| Path | Trigger                                            | Frequency                       | Budget                                        |
|------|----------------------------------------------------|---------------------------------|-----------------------------------------------|
| Cold | `ShaderSource::open` from the cooker (offline)     | Once per `.slang` per cook walk | <5 ms per TU at MVP scale (§9)                |
| Cold | `ShaderSource::open` from editor first-load        | Once per `.slang` per session   | Same                                          |
| Cold | `ShaderSource::open` from watcher reaction         | Per save event in editor        | Same; gated by watcher debounce (#719)        |
| Cold | `ShaderSource::open` from tests                    | Per test case                   | Same                                          |
| Hot  | Runtime steady-state                               | n/a                             | **0 ms** — `ShaderSource::open` is link-excluded in shipping (§4.3) |

The cold/hot split is enforced by the build-system gating in SPEC
§6.1: `source/` is shipping-excluded. The shipping `shader.dylib`
contains no path that can call `ShaderSource::open`; runtime shader
resolution goes through `ShaderCache::get(ShaderHash)` (SPEC §4.6
invariant 2). PHILOSOPHY §6 ("zero runtime reflection in shipping")
is therefore extended to "zero runtime ingestion" for this aggregate.

The implication for SPEC §9.1's `shader = 0.00 ms / frame` cell is
that `ShaderSource` contributes **zero** in shipping by absence of
linkage. In editor / dev builds the cost is bounded by §9 below and
charged against the editor / cooker's per-context tag, not against
the runtime frame budget.

## 6. Concurrency

`ShaderSource::open` is a **pure function** of `(project_root,
project_relative, on-disk file system snapshot)`. It holds no shared
mutable state across calls. This admits two concurrency profiles:

### 6.1 Single-translation-unit ingestion is single-threaded

A single `open` call executes on the calling thread. The §3.4
walker is sequential (DFS over the include graph); parallelising the
walker would not reduce wall-time materially for MVP-scale closures
(<32 includes per TU; <8 directory levels deep) and would complicate
cycle detection. Single-threaded keeps the algorithm's correctness
proof small.

### 6.2 Cross-translation-unit ingestion is embarrassingly parallel

The cooker (`cache/cooker.cpp`, SPEC §6.4) walks the resolved
permutation set and for each `(ShaderSource, PermutationKey,
CompileTarget)` triple invokes `ShaderSource::open` followed by
`IShaderBackend::compile`. The `open` calls share no state, mutate
no shared structure, and read disjoint subsets of the project source
tree. The cooker may invoke `open` on N TUs in parallel (one per
worker thread; bounded by the cooker's thread pool, which is itself
bounded by the offline-build configuration).

Concurrency rules:

1. **No global state.** `ShaderSource` carries no static state; the
   resolver, walker, scanner, and hasher are pure functions.
   Per-thread allocator state under `ContextTag::shader` is the only
   per-thread bookkeeping; allocator scopes are LIFO and do not
   escape `open`.
2. **Read-only project tree assumption.** While ingestion is in
   flight against a project root, files in that root are assumed not
   to mutate. The watcher (#719) is single-consumer (the cooker /
   editor) and serialises file events; concurrent `open` calls
   against the same project root see a consistent snapshot. A file
   modified mid-ingestion may produce a non-canonical `total_hash`
   (the bytes used and the bytes on disk diverge); the next watcher
   tick re-runs `open` and the hash converges.
3. **`std::filesystem` thread-safety.** The C++ standard guarantees
   `std::filesystem` operations are thread-safe for distinct paths;
   `ShaderSource` honours this by never sharing path objects across
   threads (each `open` call constructs its own `path` values).
4. **No blocking on locks.** `open` issues file reads through
   `std::ifstream` / `std::filesystem::file_size` synchronously;
   there are no condition variables, no thread pools, no async I/O.
   Cooker-side parallelism is achieved by spawning multiple `open`
   invocations, not by parallelising one.

### 6.3 Re-entrancy

`open` is re-entrant within a single thread. A test harness that
mocks `read_file` (§3.4) and recursively triggers another `open` is
admitted; the only constraint is that each `open` invocation owns its
own `visited` / `on_stack` / `closure_order` / `out_bytes` (no
cross-invocation aliasing).

## 7. Persistence + ABI

### 7.1 `ShaderSource` is not persisted

`ShaderSource` is a pure in-memory ingestion result. It has **no
on-disk form** — neither in the `data/schemas/shader/` directory
(SPEC §7.1) nor in the cooked `ShaderLibrary` (SPEC §7.5). The Slang
authored bytes themselves stay versioned in the project repository
(SPEC §3 collapse 1; §7 "Slang source is never persisted"). What the
cache stores is the *consequences* of ingestion (the source hash,
inside `ShaderArtifactRecord.source_hash`) — never the bytes
themselves.

Re-derivation: persisting `PreprocessedSource` would duplicate the
project repository as a CAS blob and would couple the cache schema
to the include-graph layout. The source hash is sufficient — given
a hash, the cache resolves the artifact; given a project tree at a
specific revision, ingestion reproduces the hash; the project's
version-control system is the source of truth for the bytes.

### 7.2 The hash is the cross-aggregate ABI

The exported value of `ShaderSource` is `PreprocessedSource.total_hash`
(a 32-byte BLAKE3 digest). Two aggregates consume it:

1. **`ShaderCache`** (SPEC §4.6) composes `total_hash` with the
   permutation key bytes, the canonical compile flags hash, and the
   target ordinal to form the on-disk `ShaderHash` (SPEC §2 / §6.4
   step 2d). The composition is owned by `ShaderCache` (#755), not by
   `ShaderSource`; `ShaderSource` never sees the final cache key.
2. **`CompilationPipeline`** (SPEC §4.3) takes the `ShaderSource` as
   input (`compile(ShaderSource&, PermutationKey&, CompileTarget)`),
   reads `preprocessed().bytes` to feed the slangc subprocess, and
   passes `total_hash` through to the cache-insert path. The
   pipeline is also #749's deliverable.

The hash is **stable across hosts** (PHILOSOPHY §7). Bit-stability is
guaranteed by the §3.5 normalizer (UTF-8 + LF + BOM-strip + trailing
newline), the §3.4 fixed closure order, and the §3.7 explicit
length-prefixed concatenation with `u32_le` lengths. There is no
platform-defined value (no `__DATE__`, no compiler ID, no host
endianness, no path-separator) inside the hashed bytes.

### 7.3 Hash composition stability

The §3.7 composition is locked. The fields hashed and their order:

```text
1. MAGIC = b"glibre.shader.source.v1"   # 23 bytes; future v2 bumps
2. u32_le(len(normalized_bytes)) || normalized_bytes
3. u32_le(len(closure_order))
4. For each IncludeNode in closure_order:
       u32_le(len(path_bytes_utf8)) || path_bytes_utf8
       node.content_hash.bytes        # 32 bytes (pre-normalization
                                      #            BLAKE3 per file)
```

Adding a new field to the composition (e.g., a "compiler-pinned-flags"
hash) bumps the MAGIC suffix to `glibre.shader.source.v2`, which
deterministically yields different total hashes for every source —
the cooker must re-walk every TU. This is the **deliberate** evolution
contract: a composition change is a cache-flush event, not a silent
upgrade.

`IncludeNode.content_hash` is **the per-file BLAKE3 of pre-normalization
bytes**, deliberately distinct from the normalized-bytes hash that
contributes to `total_hash`. The pre-normalization per-file hash gives
the editor a stable per-file identity for reporting (e.g., "this
include changed" UI panels); the normalized-bytes hash is what makes
the closure binding determinism-safe (CRLF / LF normalization happens
inside `total_hash`'s splice). Both hashes are BLAKE3-32; the editor /
cooker uses either depending on the question being asked.

### 7.4 Plugin-ABI surface

The `shader` plugin's ABI surface (per `reviews/decisions/plugin-abi.md`)
is the set of trait virtuals on `IShaderBackend` (SPEC §5). `ShaderSource`
is **not** on that surface; it is a value type passed by reference to
`compile()`. The middleman `data` dylib does not need to define a
schema for `ShaderSource` (no `data/schemas/shader/ShaderSource.fory`
file exists or is planned). `ShaderHash` is the only `ShaderSource`-
adjacent type that crosses the plugin ABI, and its layout (32-byte
`eastl::array<std::byte>`) is trivially POD.

## 8. Hot-Reload Integration

`ShaderSource` is the **observation half** of the SPEC §8 hot-reload
contract. It does not own the watcher; it owns the re-ingestion
behaviour the watcher triggers.

### 8.1 Refresh trigger

The `platform` `FileWatcher` (#719) observes a `.slang` file change
under the project source root and emits a `(SourceId, AbsolutePath)`
event.

**Module boundary.** The two objects involved have distinct locations:

- `ShaderSource::open` — the re-ingestion entry point — is
  implemented in `plugins/shader/source/` and is the only object this
  design adds to the `source/` sub-module.
- The watcher-event handler (`on_source_changed`), the inverse index
  (`included_by`), and `fire_recompile` all live in
  `plugins/shader/cache/` — either in `cache/cooker.cpp` directly or
  in a dedicated `cache/source_watcher_adapter.cpp`. The dependency
  direction is **`cache/ → source/`**; `source/` does not import from
  `cache/`.

The handler running inside `cache/`:

```text
on_source_changed(source_id, abs_path):
    new_source := ShaderSource::open(project_root, source_id.project_relative_path)
                  // calls into source/ — §3.2 → §3.4 → §3.6 → §3.7
    if new_source is unexpected:
        log_warn(error)                     # SPEC §8.4 refusal
        return                              # prior ShaderSource stays live

    if new_source.preprocessed().total_hash == prior.preprocessed().total_hash:
        return                              # idempotent: no-op refresh

    # The total_hash changed → the affected permutation set is the
    # subset of the cooker's enumerated keys whose preprocessed
    # closure includes source_id (or any of its include-graph
    # ancestors transitively).
    affected := compute_affected_permutations(source_id)
    fire_recompile(affected, new_source)    # SPEC §8.5
```

`ShaderSource` itself implements only the `open` half. The
`fire_recompile` path lives in `cache/cooker.cpp` (the cooker is the
sole writer; SPEC §6.4) and is governed by SPEC §8.5
(`ShaderArtifactReplaced` event). The watcher integration adapter is
the sole new code added by this design beyond the four §3 subroutines,
and it lives entirely inside `cache/`.

### 8.2 Re-ingestion is the cheapest refresh

An `open` call is bounded by §9 wall-time (<5 ms per TU at MVP
scale). The watcher's debounce / coalescing (#719) ensures that a
burst of save events (editors flush in two or three writes) triggers
one re-ingestion. Idempotence on hash equality (§8.1) ensures that a
"touch" of the file (mtime bumped, content unchanged) is a no-op past
the `open` call itself.

The `total_hash` comparison is the single mechanism that distinguishes
"meaningful change" from "no-op". This is the SPEC §4.1 invariant 2
("byte-equal preprocessed content … produce equal `PreprocessedSource`")
applied at the refresh boundary.

### 8.3 Closure-driven invalidation

A change to an *included* file (say, `lib/common.slang`) invalidates
every `.slang` file whose closure contains it. The mapping is
maintained by an inverse index `included_by: AbsolutePath →
eastl::vector<SourceId>`, populated as the cooker ingests each TU.
On a watcher tick for `lib/common.slang`:

1. Look up `included_by[lib/common.slang]` → set of `SourceId`s.
2. For each `SourceId`, run the §8.1 `on_source_changed` flow.
3. Each re-ingestion re-walks its full closure (re-reads
   `lib/common.slang`'s new bytes through §3.4); the per-file
   `content_hash` changes; the closure-order digest changes; the
   `total_hash` changes.

The inverse index is **not** part of `ShaderSource`'s state; it lives
in `cache/` alongside the cooker / watcher adapter (the same code that
calls `fire_recompile`). The dependency direction is
**`cache/ → source/`** — `cache/` reads `ShaderSource`'s closure and
maintains the inverse index; `source/` is unaware of `cache/`.
`ShaderSource` exposes the closure via `preprocessed().include_closure`
for the adapter to populate the index.

### 8.4 Refusal cases

`ShaderSource::open` may fail during a hot-reload tick. Per SPEC §8.4
(refusal cases) the prior `ShaderSource` remains live and
`render`'s PSO cache continues to bind the prior artifact. The
refusal arms attributable to `ShaderSource` are:

| Arm                          | Trigger (this aggregate)                                                                                    |
|------------------------------|-------------------------------------------------------------------------------------------------------------|
| `SourceNotFound`             | The file moved or was deleted between the watcher event and `open`'s `resolve` step.                        |
| `IncludeEscape`              | An edit introduced an absolute path or `..`-escape in an `#include`.                                        |
| `IncludeCycle`               | An edit introduced a cycle in the include graph.                                                            |
| `EncodingInvalid`            | The file is no longer valid UTF-8 (e.g. a binary blob accidentally saved over the source).                  |
| `SourceParseFailed`          | An edit introduced an orphaned `[shader(...)]` attribute with no following function declaration (§3.6 scanner). Note: a file with zero entry-point annotations is not a refusal — it is a valid library Slang TU. |
| `EntryPointStageAmbiguous`   | An edit introduced two `[shader(...)]` attributes on one function or an unrecognised stage string.          |

In each case the new `ShaderSource` is discarded, no
`ShaderArtifactReplaced` event is published, and the prior cache
state is preserved. The error is logged at `warn` per
`reviews/decisions/error-model.md` and surfaced to the editor's
viewport-overlay diagnostic UI (matching harmonius R-12.4.3's
expectation).

### 8.5 Test hooks

`shader::test::inject_source_diff` (SPEC §8.6, `#if defined(GLIBRE_E2E)`)
admits the same in-process trigger the watcher uses. The production
`ShaderSource` class definition carries **no test-specific members**;
keeping test-framework concerns out of the aggregate header is required
by SRP (two reasons to change: ingestion algorithm changes vs.
test-double scaffolding changes).

Instead, a dedicated test-support header
`tests/shader/source/shader_source_test_support.hpp` — compiled only
when `GLIBRE_E2E` is defined — declares:

```cpp
namespace glibre::shader::test {

/// Constructs a ShaderSource from a synthesised byte stream without
/// reading from disk. Normalization (§3.5), closure walk (§3.4),
/// scan (§3.6), and hash (§3.7) run identically to ShaderSource::open.
/// Available only in E2E builds (GLIBRE_E2E).
std::expected<ShaderSource, shader::Error>
make_shader_source_from_bytes(eastl::span<const std::byte> raw,
                               SourceId id);

} // namespace glibre::shader::test
```

`make_shader_source_from_bytes` requires access to `ShaderSource`'s
private constructor. Because a `friend` declaration must appear inside
the class definition — a C++ language invariant — the aggregate header
`shader_source.hpp` carries **one** conditional guard:

```cpp
#if defined(GLIBRE_E2E)
friend std::expected<ShaderSource, Error>
    glibre::shader::test::make_shader_source_from_bytes(
        eastl::span<const std::byte>, SourceId);
#endif
```

This is the only test-framework concern in the aggregate header. The
rest of the `ShaderSource` class definition is unconditionally clean;
shipping builds see no test-support symbols (the `GLIBRE_E2E` block
compiles to nothing). The full factory declaration lives in
`tests/shader/source/shader_source_test_support.hpp`, which is
included only from E2E test translation units. The §8.6
"inject an Slang diff for `source_id`" bullet calls this factory.

## 9. Performance

The wall-time and memory budget for `ShaderSource::open`. All numbers
are cold-path; shipping is zero by construction (§5).

### 9.1 Per-TU wall-time budget (cold path)

Targeted on the M1 baseline against the MVP-scale project (≤32
includes per TU, ≤8 directory levels deep, total post-include byte
stream ≤256 KiB):

| Step                                      | Budget       | Notes                                                                |
|-------------------------------------------|--------------|----------------------------------------------------------------------|
| §3.2 resolve (root file)                  | <0.05 ms     | One `weakly_canonical` + one `commonpath` check + one `is_regular_file`. |
| §3.4 closure walk (≤32 file reads)        | <2.0 ms      | Sequential reads; OS page cache absorbs repeated includes within a cook. |
| §3.5 normalize (≤256 KiB total)           | <0.5 ms      | Single byte-pass; UTF-8 validate + LF rewrite + trailing-LF append.  |
| §3.6 entry-point scan (≤256 KiB)          | <0.1 ms      | Single byte-pass; small DFA over `[shader("...")]`.                  |
| §3.7 hash composition                     | <0.1 ms      | BLAKE3 throughput on M1 firestorm is ≥3 GiB/s; hashing 256 KiB is ~0.085 ms. |
| **Total**                                 | **<3.0 ms**  | Cold-path TU ingestion; MVP target ≤5 ms per TU including I/O jitter. |

The 5 ms per-TU target derives from the cooker's overall budget: a
project with 1 k TUs cooked single-threaded would take 5 s; the cooker
parallelises across cores (§6.2), reducing wall-clock to ~1.25 s on
M1's 4 firestorm cores at MVP scale. This is **not** in the per-frame
budget (SPEC §9.2 — "cook-time budgets are out of frame-budget
scope"); it is the editor's incremental-cook latency target on save.

### 9.2 Memory budget

Per-`ShaderSource` instance:

| Field                                                  | Bound (MVP)            |
|--------------------------------------------------------|------------------------|
| `id_.project_relative_path`                            | ≤256 B                 |
| `entry_points_`                                        | ≤8 entries × ~64 B = 512 B |
| `preprocessed_.bytes`                                  | ≤256 KiB               |
| `preprocessed_.include_closure`                        | ≤32 nodes × ~96 B = 3 KiB |
| `preprocessed_.total_hash`                             | 32 B                   |
| **Total per TU**                                       | **~260 KiB**           |

Cook-time concurrency: with N parallel `open` invocations, peak heap
is `N × 260 KiB`. For N = 16 (MVP cooker default), peak is ~4 MiB —
well inside the 32 MiB `ContextTag::shader` ceiling
(`reviews/decisions/perf-budget.md`).

The cooker is responsible for releasing each `ShaderSource` after
handing its `total_hash` and `preprocessed().bytes` to the
`CompilationPipeline`; instances are not retained beyond the
per-TU compile step. This keeps the resident set bounded by `N`, not
by the project's TU count.

### 9.3 Allocator integration

`ShaderSource` allocates against `ContextTag::shader`
(`reviews/decisions/perf-budget.md` "Allocator Rules"). The
per-`open` arena is sized once at ingestion start (capacity =
file-size + closure overhead) so the byte vector and closure vector
do not reallocate. No `new` / `malloc` calls escape the `eastl::*`
container types; the build's `-Wglibre-no-raw-alloc` flag is
satisfied by construction.

The per-TU arena is **transient** (drains after the cooker hands off
to the `CompilationPipeline`). Per `perf-budget.md` Allocator Rule §4,
transient arenas do not count against the cell ceiling provided they
drain before the cell's drain phase; the cooker's drain point is
"after `IShaderBackend::compile` returns and `ShaderArtifact` is
inserted into CAS".

### 9.4 Init / cold-start contribution

`ShaderSource::open` does not run during runtime init in shipping
(§5). In editor / dev builds, the editor's first-load path may
ingest every project TU sequentially or in parallel; the §9.1
per-TU budget × project TU count is the editor's first-load
ingestion budget, not a frame-budget cell.

## 10. Failure Modes

The SPEC §10 closed enum is authoritative. This section pins the
arms attributable to `ShaderSource` and their per-arm operator
actions. All severities are `refuse` per SPEC §10.1 unless noted.

### 10.1 `ShaderSource`-emitted `shader::Error` arms

| Arm                          | Step (§3) | Trigger                                                                                                                                | Operator action                                                                                                          | Severity (SPEC §10.1) |
|------------------------------|-----------|----------------------------------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------|-----------------------|
| `SourceNotFound`             | §3.2 / §3.4 | The entry-file path or any include-target path does not exist or is not a regular file.                                                  | Re-check the project-relative path; common cause is a deleted include or a stale watcher event after a rename.            | `refuse`              |
| `IncludeEscape`              | §3.2      | An `#include` resolved to an absolute path, contained `..`-escapes leaving the project root, or pointed at a symlink whose canonical target leaves the root. | Re-author the include using a project-relative form. Path layout is a project policy, not negotiable.                    | `refuse`              |
| `IncludeCycle`               | §3.4      | The include graph contains a cycle. The cycle path is attached to the diagnostic detail.                                                | Break the cycle (introduce a header-shared declaration or split the file). The detail string is `a -> b -> c -> a`.        | `refuse`              |
| `EncodingInvalid`            | §3.5      | A file is not valid UTF-8 after BOM strip, or is empty.                                                                                  | Re-save the file as UTF-8. Editors that default to UTF-8-BOM are accepted; non-UTF-8 encodings are rejected.              | `refuse`              |
| `SourceParseFailed`          | §3.6      | An attribute `[shader(...)]` is followed by no function declaration (orphaned attribute). SPEC §10.2 scopes `SourceParseFailed` to "malformed `[shader(...)]` annotation" — an orphaned attribute is exactly that. Note: zero stage attributes on a function is not a refusal; it produces an empty `entry_points()` and is a valid library TU. | Remove the orphaned attribute or add the function declaration immediately after it.                                    | `refuse`              |
| `EntryPointStageAmbiguous`   | §3.6      | An entry-point function carries more than one `[shader(...)]` attribute, or carries an unrecognised stage string.                        | Use exactly one of the §5 `Stage` values.                                                                                  | `refuse`              |

The shader-error sibling spike is **not separately filed**; SPEC §10
is the closed-sum source of truth for the entire context.
`ShaderSource`'s arms are a subset of SPEC §5's `enum class
shader::Error`; this design transcribes them with operator-action
detail and binds each to its §3 step.

### 10.2 Refuse semantics

Every refusal is "atomic": the `ShaderSource` value is never
constructed past the failing step. Specifically:

- `SourceNotFound` / `IncludeEscape` / `IncludeCycle`: the partially-
  built `out_bytes` and `closure_order` are dropped; no
  `PreprocessedSource` is published; the prior `ShaderSource` (if
  the watcher tick reaction is the caller) remains live.
- `EncodingInvalid`: the normalizer fails before §3.6 / §3.7 run; no
  closure or hash is finalised.
- `SourceParseFailed` (orphaned-attribute trigger) / `EntryPointStageAmbiguous`:
  the closure walk has completed and `out_bytes` is well-formed, but
  the scanner's refusal aborts `open` before §3.7 runs. The
  `total_hash` is never finalised; the `ShaderSource` value never
  becomes addressable. Note: `EntryPointMissing` is a
  `CompilationPipeline`-emitted arm (SPEC §10.2), not a
  `ShaderSource::open`-emitted arm; ingestion-time orphaned-attribute
  failures use `SourceParseFailed` per the §10.1 table above.

This matches SPEC §8.4 ("the new artifact is not published; the
prior cache state remains live"). The editor's viewport overlay
displays the refusal; `render`'s PSO cache continues to bind the
prior artifact deterministically.

### 10.3 Logging

Per `reviews/decisions/error-model.md`, every refusal logs **once at
the boundary that handles it**. The handling boundary is:

- The watcher adapter (in editor / dev-build) for hot-reload-driven
  ingestions — `warn` level.
- The cooker's per-TU loop for cook-time ingestions — `error` level
  with the offending `(SourceId, project_relative_path)` attached.
- The test fixture for `GLIBRE_E2E` test ingestions — captured as a
  `Result<>`; not logged through `spdlog` from the test code itself.

`ShaderSource::open` itself does not call `spdlog`; it returns the
error and lets the caller's boundary log it. This preserves the
"one log per error" rule (error-model.md §"Logging / Telemetry" #1).

### 10.4 Cross-references

- Closed enum source-of-truth: SPEC §5 (`enum class shader::Error`).
- Hot-reload refusal projection: SPEC §8.4 cases 2-4 (this
  aggregate's arms surface as case 3 / 4 / shipping-refused-1
  depending on phase).
- Engine-wide error policy: `reviews/decisions/error-model.md`.
- Sibling reflection / descriptor / cache refusal: SPEC §10.2 (those
  arms are not `ShaderSource`'s; cited for completeness).

## 11. Test Plan

### 11.1 Unit tests (Catch2, `tests/shader/source/`)

Each row of §10.1 maps to one or more unit tests. The harness uses
**fixture-built Slang sources** generated by a
`tests/shader/source/fixtures/` Make-recipe (deterministic byte
content; checked-in goldens for the resulting `ShaderHash`).

| Test name                                            | Drives arm / property             | Fixture                                                            |
|------------------------------------------------------|-----------------------------------|--------------------------------------------------------------------|
| `source.open_resolves_project_relative`              | (positive)                        | `foo.slang` at project root with no includes.                       |
| `source.open_includes_quoted_form`                   | (positive)                        | `a.slang` containing `#include "b.slang"`.                          |
| `source.open_includes_bracket_form`                  | (positive)                        | `a.slang` containing `#include <lib/b.slang>`.                      |
| `source.open_normalizes_crlf_to_lf`                  | (positive, determinism)           | One CRLF file, one LF file with identical visible content; assert byte-equal `total_hash`. |
| `source.open_strips_utf8_bom`                        | (positive, determinism)           | One BOM-prefixed file vs same content without BOM; assert byte-equal `total_hash`. |
| `source.open_appends_trailing_newline`               | (positive, determinism)           | Two files differing only by trailing LF presence; assert byte-equal `total_hash`. |
| `source.open_rejects_absolute_include`               | `IncludeEscape`                   | `a.slang` containing `#include "/etc/passwd"`.                      |
| `source.open_rejects_dotdot_escape`                  | `IncludeEscape`                   | `a.slang` containing `#include "../outside.slang"`.                 |
| `source.open_rejects_symlink_escape`                 | `IncludeEscape`                   | A symlink under the project root pointing outside.                  |
| `source.open_rejects_missing_root`                   | `SourceNotFound`                  | `open(project_root, "missing.slang")`.                              |
| `source.open_rejects_missing_include`                | `SourceNotFound`                  | `a.slang` includes `b.slang`; `b.slang` absent.                     |
| `source.open_detects_self_cycle`                     | `IncludeCycle`                    | `a.slang` includes `a.slang`. Detail = `a.slang -> a.slang`.        |
| `source.open_detects_three_cycle`                    | `IncludeCycle`                    | `a → b → c → a`. Detail = `a -> b -> c -> a`.                       |
| `source.open_detects_diamond_no_cycle`               | (positive)                        | `a → b`, `a → c`, `b → d`, `c → d`. `d` ingested once.              |
| `source.open_rejects_invalid_utf8`                   | `EncodingInvalid`                 | `a.slang` containing a stray `0xFF` byte.                           |
| `source.open_rejects_zero_stage_annotated_function`  | (positive: not enumerated)        | Helper function with no `[shader(...)]`; `entry_points()` is empty. |
| `source.open_rejects_two_stage_attribute`            | `EntryPointStageAmbiguous`        | A function with two `[shader("vertex")]` `[shader("pixel")]`.       |
| `source.open_rejects_unknown_stage_string`           | `EntryPointStageAmbiguous`        | `[shader("ray-something")]`.                                        |
| `source.open_rejects_attribute_without_function`     | `SourceParseFailed`               | `[shader("vertex")]` followed by EOF.                                |
| `source.hash_is_byte_stable_across_runs`             | (positive, determinism)           | Run `open` 100 times; assert all `total_hash` equal.                |
| `source.hash_is_byte_stable_across_hosts`            | (positive, determinism)           | Cross-host CI matrix: macOS arm64 vs macOS x86_64; assert equal hash. |
| `source.hash_changes_on_byte_change`                 | (positive)                        | Two files differing by one byte; assert different hashes.            |
| `source.hash_changes_on_include_path_rename`         | (positive)                        | Move a shared header to a different project-relative path; assert different `total_hash`. |
| `source.hash_changes_on_include_content_change`      | (positive)                        | Edit a shared header's bytes; assert different `total_hash` for every TU including it. |
| `source.closure_order_is_topological`                | (positive)                        | `a → b → c`; assert `closure_order = [c, b, a]`.                    |
| `source.entry_points_collected_from_includes`        | (positive)                        | Entry point declared in `b.slang`, included by `a.slang`; assert enumerated by `open(a)`. |
| `source.preprocessor_directives_unconditionally_included` | (positive)                   | `#if 0\n#include "x.slang"\n#endif` ingests `x.slang` regardless.    |

### 11.2 Integration tests (Catch2, `tests/shader/integration/`)

- `integration.full_tu_ingestion_round_trip` — author a 32-include
  `.slang` TU; assert `open` completes inside §9.1's 5 ms target;
  assert `total_hash` matches a checked-in golden.
- `integration.watcher_driven_invalidation` — combine #719's
  `FileWatcher` with `ShaderSource::open` in a fixture; modify a
  file under the watch; assert the next `open` reports a different
  `total_hash`; assert the cooker's `ShaderArtifactReplaced` event
  carries exactly the affected `(PermutationKey, target)` set
  (driven by the inverse-index from §8.3). Cross-references SPEC
  §8.6's `inject_source_diff` test hook.
- `integration.parallel_open_no_shared_state` — invoke `open` on 16
  TUs concurrently; assert no data race (TSAN-clean); assert all 16
  hashes byte-equal a sequential reference run.
- `integration.refusal_preserves_prior_state` — author a TU; ingest
  it (success); edit the TU to introduce an `IncludeCycle`; ingest
  again (refused); assert the prior `ShaderSource` is still
  resolvable through the cooker's index; assert no
  `ShaderArtifactReplaced` event was published.

### 11.3 Performance microbenchmarks

Catch2 `BENCHMARK` blocks under `tests/shader/perf/source_bench.cpp`:

- `bench.open_small_tu_no_includes` — single 4 KiB file, 0 includes;
  assert <0.5 ms (§9.1).
- `bench.open_typical_tu` — 256 KiB total bytes across 8 includes;
  assert <3.0 ms (§9.1).
- `bench.open_max_tu_mvp` — 256 KiB total bytes across 32 includes;
  assert <5.0 ms (§9.1 target).
- `bench.hash_throughput` — assert BLAKE3 throughput ≥3 GiB/s on the
  M1 reference (§9.1 derivation).

CI gates per `reviews/decisions/perf-budget.md` §"CI Gate Spec" #1:
any benchmark exceeding its budget fails the PR. The 5 ms-per-TU
target is a cooker-side budget, not a per-frame budget, but the same
gate harness asserts it.

### 11.4 E2E coverage

The E2E trace under `tests/e2e/shader/source/` ships three fixture
projects:

- `happy-path` — 12 `.slang` files, 6 includes, 3 entry points;
  asserts `open` produces a recorded golden hash + closure.
- `cycle-recovery` — author-time edit introduces a cycle; assert
  refusal + prior-state preservation across an editor reload cycle.
- `unicode-paths` — UTF-8 paths inside the project root with
  non-ASCII characters; assert `total_hash` is host-stable.

Each fixture has a recorded `.glibre-trace` golden; CI replays
asserts byte-equal trace output across runs (deterministic-replay
obligation, PHILOSOPHY §7).

## 12. Open Questions

- **[OPEN] Slang preprocessor directive ownership boundary.** The
  §3.3 parser intentionally owns only `#include`; everything else
  (`#define`, `#if`, `#ifdef`) is forwarded to slangc as Slang
  bytes. If a future Slang version introduces a directive that
  affects include resolution (e.g. a `#pragma include_path`), we
  must decide whether `ShaderSource` interprets it or refuses it.
  Defer until the first concrete need; the bias is to refuse and
  force authoring into the project-rooted form.

- **[OPEN] `#pragma once` semantics vs explicit include guards.** The
  §3.4 walker visits each file at most once per closure (de facto
  `#pragma once`). Slang source authored with C-style include guards
  is admitted; the guards remain inert because the second visit is
  skipped. Should `ShaderSource` *forbid* C-style guards (lint-style
  warning) to enforce `#pragma once`-style authoring? Defer; lint is
  editor-tooling territory.

- **[OPEN] Build-scoped vs project-scoped paths.** The resolver pins
  the project source root at `open()` time. A multi-project workspace
  (e.g. an engine + game project pair sharing common Slang
  libraries) would need either (a) a wider root that contains both
  trees or (b) a project-aware include-resolution policy. MVP punts
  to option (a); the engine and game share a parent directory that is
  the project root for both. Re-visit when multi-project workspaces
  become a concrete need.

- **[OPEN] Editor-side per-file `content_hash` exposure for diff
  rendering.** `IncludeNode.content_hash` is the BLAKE3 of pre-
  normalization bytes. This is intentional (§7.3) and lets the editor
  show "this include changed" in a diff view without re-running the
  closure walk. But the SPEC §5 `IncludeNode.content_hash` field name
  is not explicit about pre- vs post-normalization. Decide whether to
  rename it or to document the contract more loudly when the editor
  panel lands.

- **[OPEN] Maximum include depth and TU byte size.** §9.1's MVP
  bound is "≤32 includes per TU, ≤256 KiB total post-include bytes".
  Should `open` enforce these as hard limits with explicit refusal
  arms (`IncludeDepthExceeded`, `SourceSizeExceeded`)? Defer; in
  practice deep includes manifest as `IncludeCycle` or as
  performance-budget violations caught by §11.3 benchmarks. Add an
  explicit arm only if real-world authoring blows past the bounds
  silently.

- **[OPEN] `weakly_canonical` vs `canonical` for include targets.**
  §3.2 uses `weakly_canonical` for include targets to admit not-yet-
  saved editor buffers (the `#include` resolves the canonical-form
  even when the bytes have not flushed to disk). `canonical` for the
  project root is required (the root must exist). Verify that
  `weakly_canonical`'s symlink-resolution semantics match the
  "symlink target must lie inside the root" rule when the symlink
  itself resolves but its target is missing — likely a corner
  case to test in `source.open_rejects_missing_include`.
