#include "x4sb/platform/platform.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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

std::filesystem::path appConfigDir() {
#ifdef _WIN32
  const char* base = std::getenv("APPDATA");
  if (base == nullptr || *base == '\0') return {};
  return std::filesystem::path(base) / "x4sb";
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0')
    return std::filesystem::path(xdg) / "x4sb";
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') return {};
  return std::filesystem::path(home) / ".config" / "x4sb";
#endif
}

std::filesystem::path settingsFilePath() {
  const std::filesystem::path dir = appConfigDir();
  return dir.empty() ? dir : dir / "settings.json";
}

std::filesystem::path appCacheDir() {
#ifdef _WIN32
  const auto base = fromEnv("LOCALAPPDATA");
  return base.empty() ? base : base / "x4sb";
#else
  auto base = fromEnv("XDG_CACHE_HOME");
  if (base.empty()) {
    base = fromEnv("HOME");
    if (base.empty()) return {};
    base /= ".cache";
  }
  return base / "x4sb";
#endif
}

bool writeFileAtomically(const std::filesystem::path& file, const std::string& bytes) {
  if (file.empty()) return false;
  std::error_code ec;
  if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path(), ec);
  if (ec) return false;
  auto temporary = file;
  temporary += "." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
               "." + std::to_string(std::random_device{}()) + ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << bytes;
    out.close();
    if (!out) {
      std::filesystem::remove(temporary, ec);
      return false;
    }
  }
#ifdef _WIN32
  const bool replaced = MoveFileExW(temporary.c_str(), file.c_str(),
                                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  std::filesystem::rename(temporary, file, ec);
  const bool replaced = !ec;
#endif
  if (!replaced) std::filesystem::remove(temporary, ec);
  return replaced;
}

std::string loadSkySetting(const std::filesystem::path& file) {
  const std::ifstream in(file, std::ios::binary);
  if (!in) return "random";
  std::stringstream ss;
  ss << in.rdbuf();
  const nlohmann::json doc = nlohmann::json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
  if (!doc.is_object()) return "random";
  const auto it = doc.find("sky");
  if (it == doc.end() || !it->is_string()) return "random";
  const std::string v = it->get<std::string>();
  return v.empty() ? "random" : v;
}

bool saveSkySetting(const std::filesystem::path& file, const std::string& value) {
  std::ifstream input(file, std::ios::binary);
  auto json = input ? nlohmann::json::parse(input, nullptr, false) : nlohmann::json::object();
  if (!json.is_object()) json = nlohmann::json::object();
  json["sky"] = value;
  input.close();
  return writeFileAtomically(file, json.dump(2));
}

std::string loadGameDirectory(const std::filesystem::path& file) {
  std::ifstream input(file, std::ios::binary);
  if (!input) return {};
  const auto json = nlohmann::json::parse(input, nullptr, false);
  if (!json.is_object() || !json.contains("x4Install") || !json["x4Install"].is_string()) return {};
  return json["x4Install"].get<std::string>();
}

bool saveGameDirectory(const std::filesystem::path& file, const std::string& directory) {
  std::ifstream input(file, std::ios::binary);
  auto json = input ? nlohmann::json::parse(input, nullptr, false) : nlohmann::json::object();
  if (!json.is_object()) json = nlohmann::json::object();
  json["x4Install"] = directory;
  input.close();
  return writeFileAtomically(file, json.dump(2));
}

}  // namespace x4sb::platform
