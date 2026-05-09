// Fixture: same_stage_dup.slang
// The same entry-point function name appears twice with the same stage
// (simulates include duplication).  The scanner must collapse duplicates
// and emit exactly ONE EntryPoint, not EntryPointStageAmbiguous.
// Used by: shader_source_same_stage_dup_is_collapsed_not_ambiguous

[shader("vertex")]
void vs_main() {}

// Simulated re-declaration (e.g. from a double-included header).
[shader("vertex")]
void vs_main() {}
