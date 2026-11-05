// Vitox U3 (approved M5A): containment-checked LEXICAL relative path, replacing fs::relative() at the
// known Vita3K call sites. fs::relative() goes through weakly_canonical() -> GetFinalPathNameByHandleW
// (VOLUME_NAME_DOS), which returns ERROR_ACCESS_DENIED for packaged apps on Xbox (UPSTREAM_FINDINGS U3).
//
// Rules (fail closed, never produce an unsafe relative path):
//   - child and base must both be absolute
//   - both are lexically_normal()-ised; any ".." left after normalisation is rejected (escape)
//   - root names (drives) and root directories must match (drive letters compared case-insensitively)
//   - every base element must be a prefix of the child's elements (case-insensitive, as NTFS);
//     a child equal to the base is rejected (not strictly contained)
#pragma once

#include <util/fs.h>

#include <string>

namespace vitox {

// true: `out` = child relative to base. false: `out` is empty and `why` (if given) explains.
bool lexical_relative(const fs::path &child, const fs::path &base, fs::path &out, std::string *why = nullptr);

// Optional trace hook (the probe logs every derivation made by the substituted Vita3K call sites).
using LexicalRelativeTrace = void (*)(const fs::path &child, const fs::path &base, const fs::path &out, bool ok, const std::string &why);
void set_lexical_relative_trace(LexicalRelativeTrace trace);

} // namespace vitox
