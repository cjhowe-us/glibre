// Fixture: middle_b.slang
// Middle include file. Includes leaf_c.slang.
// Used by: shader_source_resolves_includes (A -> B -> C chain)

#include "leaf_c.slang"

[shader("vertex")]
void b_vertex() {}
