// SPDX-License-Identifier: Apache-2.0
// glibre — e2e fixture for shader story #319 (valid case).
//
// One Slang translation unit declaring exactly two stage-tagged entry
// points, one [shader("vertex")] and one [shader("pixel")]. Used by
// `tests/e2e/shader/slang-authoring.glibre-trace` to exercise the
// `ShaderSource::open` happy path:
//
//   * `ShaderSource::id().project_relative_path == "shaders/lit.slang"`
//   * `ShaderSource::entry_points() == { {"vs_main", Stage::Vertex},
//                                        {"ps_main", Stage::Pixel} }`
//     in source-declaration order.
//   * `ShaderSource::preprocessed().total_hash` is byte-stable across
//     two consecutive calls.
//
// This file is intentionally trivial — the trace asserts metadata
// extraction, not shading correctness. Keep changes review-gated:
// touching it perturbs the byte-stable preprocessed hash and
// invalidates the recorded `EnvHash` in the trace per
// `specs/e2e/SPEC.md` §7.1.2 inv 1.

struct VsIn {
    float3 position : POSITION;
};

struct VsOut {
    float4 position : SV_Position;
};

[shader("vertex")]
VsOut vs_main(VsIn input) {
    VsOut output;
    output.position = float4(input.position, 1.0);
    return output;
}

[shader("pixel")]
float4 ps_main() : SV_Target {
    return float4(1.0, 1.0, 1.0, 1.0);
}
