#include "x4sb/platform/platform.hpp"

#include <cstdlib>

namespace x4sb::platform {
namespace {

std::filesystem::path fromEnv(const char* var) {
  const char* v = std::getenv(var);
  return (v && *v) ? std::filesystem::path(v) : std::filesystem::path{};
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

}  // namespace x4sb::platform
