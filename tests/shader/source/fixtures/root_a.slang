// Fixture: root_a.slang
// Root shader file. Includes middle_b.slang which includes leaf_c.slang.
// Used by: shader_source_resolves_includes (A -> B -> C chain)

#include "middle_b.slang"

[shader("pixel")]
void a_pixel() {}
