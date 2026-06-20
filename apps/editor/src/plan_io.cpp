#include "plan_io.hpp"

#include "modifier_keys.hpp"
#include "raylib.h"
#include "x4sb/planio/plan.hpp"
#include "x4sb/platform/platform.hpp"

#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <system_error>
#include <utility>

namespace x4sb::editor {
namespace {

constexpr const char* kPlanFile = "x4sb_plan.xml";

}  // namespace

std::string savePlan(EditorState& state) {
  const std::string xml = exportPlanXml(state.station(), state.catalog());
  SetClipboardText(xml.c_str());

  std::error_code ec;
  std::filesystem::path dir = platform::defaultConstructionPlanDir();
  bool toX4 = false;
  if (!dir.empty()) {
    std::filesystem::create_directories(dir, ec);
    toX4 = !ec;
  }
  // Fall back to the cwd so a save never silently no-ops when X4 isn't installed.
  if (!toX4) dir = std::filesystem::current_path(ec);

  const std::filesystem::path path = dir / kPlanFile;
  std::ofstream out(path, std::ios::binary);
  out << xml;
  out.close();  // flush+close now so a flush/close failure is visible to the check below
  if (!out) return "Clipboard copied; FILE WRITE FAILED: " + path.string();
  if (!toX4) return "Saved to working dir (X4 not found): " + path.string() + " (clipboard copied)";
  return "Saved " + path.string() + " (clipboard copied)";
}

std::string loadPlan(EditorState& state, bool& didLoad) {
  didLoad = false;
  std::error_code ec;
  std::filesystem::path dir = platform::defaultConstructionPlanDir();
  if (dir.empty()) dir = std::filesystem::current_path(ec);

  std::filesystem::path newest;
  std::filesystem::file_time_type best{};
  std::filesystem::directory_iterator it(dir, ec);
  const std::filesystem::directory_iterator end;
  for (; !ec && it != end; it.increment(ec)) {
    if (it->path().extension() != ".xml") continue;
    std::error_code rec;
    if (!it->is_regular_file(rec) || rec) continue;
    std::error_code tec;
    const std::filesystem::file_time_type t = std::filesystem::last_write_time(it->path(), tec);
    if (tec) continue;
    if (newest.empty() || t > best) {
      best = t;
      newest = it->path();
    }
  }
  if (newest.empty()) return "No plan (*.xml) found in " + dir.string();

  std::ifstream in(newest, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  std::optional<Station> loaded = importPlanXml(ss.str());
  if (!loaded) return "Parse FAILED: " + newest.filename().string();

  state.loadStation(std::move(*loaded));
  didLoad = true;
  return "Loaded " + newest.filename().string();
}

PlanIoOutcome handlePlanIoKeys(EditorState& state) {
  if (!isCtrlDown()) return {};
  if (IsKeyPressed(KEY_S)) return {savePlan(state), false};
  if (IsKeyPressed(KEY_O)) {
    bool loaded = false;
    std::string msg = loadPlan(state, loaded);
    return {std::move(msg), loaded};
  }
  return {};
}

}  // namespace x4sb::editor
