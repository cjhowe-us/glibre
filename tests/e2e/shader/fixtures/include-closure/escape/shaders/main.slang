// SPDX-License-Identifier: Apache-2.0
// glibre — e2e fixture for shader story #321 (escape rejection).
//
// Drives the *escape* Gherkin scenario in #321:
//
//   Given shaders/main.slang includes "../../etc/passwd"
//   When I call ShaderSource::open(...)
//   Then the call returns Error::IncludeEscape
//
// `specs/shader/SPEC.md` §4.1 invariant 3 forbids upward-escaping or
// absolute include paths; the include resolver
// (`shader/source/include_resolver.{hpp,cpp}` per §6.1) MUST reject
// at construction time before any byte is read from the target.
//
// Do NOT "fix" this file — it is the load-bearing counterexample for
// the escape scenario. Removing the `../../` prefix would silently
// turn the trace green for the wrong reason.

#include "../../etc/passwd"

[shader("vertex")]
float4 vs_main(float3 pos : POSITION) : SV_Position {
    return float4(pos, 1.0);
}
