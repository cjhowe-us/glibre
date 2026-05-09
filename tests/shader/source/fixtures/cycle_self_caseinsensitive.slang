// Fixture: cycle_self_caseinsensitive.slang
// MED-2 regression: self-include with a different case in the include path.
// On macOS APFS (case-insensitive by default) "CYCLE_SELF_CASEINSENSITIVE.slang"
// and "cycle_self_caseinsensitive.slang" resolve to the same file.
// The preprocessor must detect this as a cycle (IncludeCycle) even though
// the exact path string differs.
//
// Used by: shader_source_cycle_detection_is_case_insensitive

#include "CYCLE_SELF_CASEINSENSITIVE.slang"
