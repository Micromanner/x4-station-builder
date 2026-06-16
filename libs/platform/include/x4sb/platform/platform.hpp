#pragma once
// OS-specific integration, isolated behind one module so the rest of the app
// stays platform-agnostic (same "isolate the boundary" approach as coords).
// Windows is the primary target; Linux is supported (X4 has a native Linux
// build) and kept a flip-the-switch away rather than a later refactor.
#include <filesystem>
#include <string>
#include <vector>

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

// Numeric account-profile ids directly under userDataDir, newest
// directory-mtime first (X4 creates one per account, e.g. "118356972").
// Error-safe: a missing or unreadable dir yields an empty vector, never throws.
// Exposed separately from x4ProfileIds so it can be unit-tested against a temp
// directory.
[[nodiscard]] std::vector<std::string> profileIdsIn(const std::filesystem::path& userDataDir);

// Account-profile ids under the resolved X4 user-data dir (see profileIdsIn).
[[nodiscard]] std::vector<std::string> x4ProfileIds();

// Construction-plan dir of the newest profile, or empty if none exist. Does NOT
// create the directory.
[[nodiscard]] std::filesystem::path defaultConstructionPlanDir();

}  // namespace x4sb::platform
