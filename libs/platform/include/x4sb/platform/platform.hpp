#pragma once
// OS-specific integration, isolated behind one module so the rest of the app
// stays platform-agnostic (same "isolate the boundary" approach as coords).
// Windows is the primary target; Linux is supported (X4 has a native Linux
// build) and kept a flip-the-switch away rather than a later refactor.
#include <filesystem>
#include <string>

namespace x4sb::platform {

// Root of X4's per-user data directory, OS-specific:
//   Windows: <Documents>\Egosoft\X4
//   Linux:   $XDG_CONFIG_HOME/EgoSoft/X4   (default ~/.config/EgoSoft/X4)
// Returns an empty path if the location can't be determined.
std::filesystem::path x4UserDataDir();

// Construction-plan directory for a given profile id (the numeric subfolder X4
// creates per account under the user-data dir). Plans are imported from and
// exported to here. Empty if the user-data dir can't be resolved.
std::filesystem::path constructionPlanDir(const std::string& profileId);

}  // namespace x4sb::platform
