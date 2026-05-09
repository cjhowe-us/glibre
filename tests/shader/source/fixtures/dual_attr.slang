// Fixture: dual_attr.slang
// A function with two [shader("...")] attributes — ambiguous stage.
// Used by: shader_source_open_returns_EntryPointStageAmbiguous_on_dual_attributes

[shader("vertex")]
[shader("pixel")]
void ambiguous_fn() {}
