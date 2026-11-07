// process_filter.hpp - decides whether the overlay may act inside THIS process.
//
// Why this is not just "compare /proc/self/comm at load time":
//   Under Proton every Wine process (game, launcher, services.exe, wineserver helpers ...) loads
//   an LD_PRELOADed library. At load time a Wine process still carries the name of its loader
//   ("wine64-preload"); it only becomes "GTA5.exe" later, when Wine sets its final name. A strict
//   name test at load time would therefore reject the game itself.
//   So: at load time a Wine loader is let through when a filter is set, and the filter is
//   enforced for real at the first presented frame, when the name is final.
#pragma once

namespace vko {

// at_load == true : called from the library constructor (lenient for Wine loaders).
// at_load == false: called once the process has its final name (strict).
//
// VKOVERLAY_DISABLE=1          never active
// VKOVERLAY_PROCESS=a,b,c      only processes whose name matches one entry (case-insensitive;
//                              the kernel keeps 15 characters of a name, so longer entries
//                              are compared on their first 15, e.g. "GeometryDash.exe")
// (no filter)                  everything except known launcher/shell/Steam/Wine helper processes
bool process_allowed(bool at_load);

// The kernel's short name of this process (at most 15 characters).
void current_comm(char (&out)[32]);

// Exposed for tests.
bool process_name_matches_list(const char* comm, const char* comma_separated);

}  // namespace vko
