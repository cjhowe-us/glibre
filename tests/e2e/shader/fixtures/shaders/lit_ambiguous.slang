// SPDX-License-Identifier: Apache-2.0
// glibre — e2e fixture for shader story #319 (ambiguous case).
//
// `vs_main` carries TWO conflicting stage attributes. This violates
// `specs/shader/SPEC.md` §4.1 invariant 1 ("Every emitted EntryPoint
// has exactly one stage attribute") and must cause
// `ShaderSource::open` to return
// `shader::Error::EntryPointStageAmbiguous`.
//
// Used by `tests/e2e/shader/slang-authoring.glibre-trace` to exercise
// the negative path. Do NOT clean this up — it is the load-bearing
// counterexample for the second Gherkin scenario in issue #319.

struct VsIn {
    float3 position : POSITION;
};

struct VsOut {
    float4 position : SV_Position;
};

// Deliberately ambiguous: two stage attributes on one entry point.
[shader("vertex")]
[shader("pixel")]
VsOut vs_main(VsIn input) {
    VsOut output;
    output.position = float4(input.position, 1.0);
    return output;
}

[shader("pixel")]
float4 ps_main() : SV_Target {
    return float4(1.0, 1.0, 1.0, 1.0);
}
