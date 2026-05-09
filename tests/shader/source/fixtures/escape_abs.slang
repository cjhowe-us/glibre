// Fixture: escape_abs.slang
// Includes an absolute path — should trigger IncludeEscape.
// Used by: include_resolver_rejects_absolute_path_with_IncludeEscape

#include "/etc/passwd"
