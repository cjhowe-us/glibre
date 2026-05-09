// Fixture: cycle_b.slang
// Cycle detection: B includes A (which included B — cycle!).
// Used by: shader_source_include_closure_rejects_escape_and_cycle

#include "cycle_a.slang"
