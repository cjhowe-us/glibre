// SPDX-License-Identifier: Apache-2.0
// glibre — e2e fixture for shader story #321 (valid include closure).
//
// Drives the *positive* Gherkin scenario in #321:
//
//   Given shaders/main.slang includes "common.slangi"
//   And shaders/common.slangi includes "math.slangi"
//   And every include resolves under <project_root>
//   When I call ShaderSource::open(project_root, "shaders/main.slang")
//   Then the call returns the value state
//   And PreprocessedSource::include_closure lists
//       [common.slangi, math.slangi] in resolution order
//   And every IncludeNode::content_hash is non-zero and reproducible
//
// The chain is exactly two hops deep so the trace can spot-check both
// the immediate include and the transitively resolved include without
// inflating the closure beyond what `tests/e2e/shader/include-closure
// .glibre-trace` asserts byte-for-byte.
//
// Touching this file perturbs every IncludeNode::content_hash and
// PreprocessedSource::total_hash, which invalidates the trace's
// recorded `EnvHash` per `specs/e2e/SPEC.md` §7.1.2 inv 1. Review-gate
// edits accordingly.

#include "common.slangi"

[shader("vertex")]
float4 vs_main(float3 pos : POSITION) : SV_Position {
    return float4(pos * kIdentityScale, 1.0);
}
