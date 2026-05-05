// SPDX-License-Identifier: Apache-2.0
// glibre — e2e fixture for shader story #321 (cycle rejection).
//
// Drives the *cycle* Gherkin scenario in #321 alongside `b.slangi`:
//
//   Given shaders/a.slang includes "b.slangi"
//   And shaders/b.slangi includes "a.slang"
//   When I call ShaderSource::open(project_root, "shaders/a.slang")
//   Then the call returns Error::IncludeCycle
//
// `specs/shader/SPEC.md` §4.1 invariant 3 also forbids cycles in the
// include graph. The resolver MUST detect this when it observes the
// same `project_relative_path` already on the active include stack
// during depth-first expansion, and emit `Error::IncludeCycle`
// without recursing.

#include "b.slangi"

[shader("vertex")]
float4 vs_main(float3 pos : POSITION) : SV_Position {
    return float4(pos, 1.0);
}
