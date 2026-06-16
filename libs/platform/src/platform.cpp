#include "x4sb/platform/platform.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <system_error>
#include <utility>
#include <vector>

namespace x4sb::platform {
namespace {

std::filesystem::path fromEnv(const char* var) {
  const char* v = std::getenv(var);
  return (v && *v) ? std::filesystem::path(v) : std::filesystem::path{};
}

bool allDigits(const std::string& s) {
  if (s.empty()) return false;
  return std::all_of(s.begin(), s.end(),
                     [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

}  // namespace

std::filesystem::path x4UserDataDir() {
#if defined(_WIN32)
  // TODO(platform): prefer SHGetKnownFolderPath(FOLDERID_Documents) — the
  // Documents folder can be redirected; the env-based path is a baseline.
  const std::filesystem::path home = fromEnv("USERPROFILE");
  if (home.empty()) return {};
  return home / "Documents" / "Egosoft" / "X4";
#else
  // Linux native install. TODO(platform): confirm exact folder casing against a
  // real install during the plan round-trip spike (§10.2), and also detect a
  // Proton prefix, where the path is
  //   <prefix>/drive_c/users/steamuser/Documents/Egosoft/X4.
  std::filesystem::path config = fromEnv("XDG_CONFIG_HOME");
  if (config.empty()) {
    const std::filesystem::path home = fromEnv("HOME");
    if (home.empty()) return {};
    config = home / ".config";
  }
  return config / "EgoSoft" / "X4";
#endif
}

std::filesystem::path constructionPlanDir(const std::string& profileId) {
  const std::filesystem::path base = x4UserDataDir();
  if (base.empty()) return {};
  return base / profileId / "constructionplan";
}

std::vector<std::string> profileIdsIn(const std::filesystem::path& userDataDir) {
  std::error_code ec;
  std::filesystem::directory_iterator it(userDataDir, ec);
  if (ec) return {};  // missing/unreadable dir: no profiles, no throw

  std::vector<std::pair<std::filesystem::file_time_type, std::string>> found;
  const std::filesystem::directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec) break;
    const std::string name = it->path().filename().string();
    if (!allDigits(name)) continue;
    if (!it->is_directory(ec) || ec) continue;
    const auto mtime = it->last_write_time(ec);
    if (ec) continue;
    found.emplace_back(mtime, name);
  }

  std::sort(found.begin(), found.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });

  std::vector<std::string> ids;
  ids.reserve(found.size());
  for (auto& entry : found) ids.push_back(std::move(entry.second));
  return ids;
}

std::vector<std::string> x4ProfileIds() { return profileIdsIn(x4UserDataDir()); }

std::filesystem::path defaultConstructionPlanDir() {
  const std::vector<std::string> ids = x4ProfileIds();
  if (ids.empty()) return {};
  return constructionPlanDir(ids.front());
}

}  // namespace x4sb::platform
