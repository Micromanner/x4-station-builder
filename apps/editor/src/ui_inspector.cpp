#include "ui_inspector.hpp"

#include "clay.h"
#include "clay_raylib.hpp"  // StyleTag, clayStr, styleTag
#include "ui_category.hpp"
#include "ui_fonts.hpp"  // FontId
#include "x4sb/data/types.hpp"
#include "x4sb/document/station.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <set>
#include <string>

namespace x4sb::editor {
namespace {

constexpr float kPanelWidth = 260.0f;

constexpr Clay_Color kLabel{159, 220, 229, 255};
constexpr Clay_Color kValue{237, 232, 218, 255};
constexpr Clay_Color kMuted{150, 160, 168, 255};

void line(const char* text, Clay_Color color, FontId font, std::uint16_t size) {
  CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {
    CLAY_TEXT(clayStr(text), CLAY_TEXT_CONFIG({.textColor = color,
                                               .fontId = static_cast<std::uint16_t>(font),
                                               .fontSize = size,
                                               .letterSpacing = 0}));
  }
}

}  // namespace

float inspectorWidth() { return kPanelWidth; }

void inspector(const EditorState& state) {
  // Context: a resolvable selection wins; otherwise the active build module.
  const ModuleDef* selDef = nullptr;
  if (const std::optional<InstanceId> sel = state.selected()) {
    const PlacedModule* pm = state.station().find(*sel);
    if (pm != nullptr) selDef = state.defFor(pm->defId);
  }
  const bool showingSelection = selDef != nullptr;
  const ModuleDef* def = showingSelection ? selDef : state.activeDef();

  // static: Clay holds these pointers until render; persistent storage, per-frame read.
  // The buffers depend only on `def`, so rebuild them only when the inspected module
  // changes — not every frame (a dock's connectors would otherwise churn a std::set
  // per frame). Catalog defs live for the program, so pointer identity is stable.
  static std::string nameBuf;
  static char metaBuf[96];
  static char dimBuf[64];
  static char connBuf[48];
  static const ModuleDef* builtFor = nullptr;
  if (def != nullptr && def != builtFor) {
    builtFor = def;
    nameBuf = displayName(*def);
    std::snprintf(metaBuf, sizeof(metaBuf), "%s  -  %s",
                  def->faction.empty() ? "unknown" : def->faction.c_str(),
                  categoryLabel(def->category));
    const Vec3 dim = def->aabb.max - def->aabb.min;
    std::snprintf(dimBuf, sizeof(dimBuf), "%.0f x %.0f x %.0f m", dim.x, dim.y, dim.z);
    std::set<std::string> types;
    for (const auto& cp : def->connectionPoints) types.insert(cp.type);
    std::snprintf(connBuf, sizeof(connBuf), "%zu connectors, %zu types",
                  def->connectionPoints.size(), types.size());
  }

  CLAY({.id = CLAY_ID("Inspector"),
        .layout = {.sizing = {.width = CLAY_SIZING_FIXED(kPanelWidth),
                              .height = CLAY_SIZING_GROW(0)},
                   .padding = {12, 12, 10, 10},
                   .childGap = 6,
                   .layoutDirection = CLAY_TOP_TO_BOTTOM},
        .backgroundColor = kStyledGate,
        .userData = styleTag(StyleTag::Panel)}) {
    if (def == nullptr) {
      line("No module selected", kMuted, FontId::Value, 15);
    } else {
      line(showingSelection ? "SELECTED" : "BUILD", kLabel, FontId::Value, 13);
      line(nameBuf.c_str(), kValue, FontId::Display, 20);
      line(metaBuf, kMuted, FontId::Value, 14);
      line(dimBuf, kValue, FontId::Value, 14);
      line(connBuf, kValue, FontId::Value, 14);
      if (!def->clearanceVolumes.empty())
        line("dock clearance corridor", kLabel, FontId::Value, 14);
    }
  }
}

}  // namespace x4sb::editor
