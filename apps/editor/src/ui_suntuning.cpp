#include "ui_suntuning.hpp"

#include "clay.h"
#include "clay_raylib.hpp"  // StyleTag, clayStr
#include "raylib.h"         // IsMouseButtonReleased
#include "ui_fonts.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace x4sb::editor {
namespace {

constexpr Clay_Color kLabel{159, 220, 229, 255};
constexpr Clay_Color kAmber{242, 169, 59, 255};
constexpr Clay_Color kRecessedFill{10, 11, 14, 153};
constexpr Clay_Color kPanelFill{16, 19, 23, 240};

// One +/- button; returns -1/+1 into *delta on release (layout-phase decision,
// same rule as the sky picker rows).
void stepButton(int knob, int sign, int* delta) {
  const std::uint32_t idx = static_cast<std::uint32_t>(knob * 2 + (sign > 0 ? 1 : 0));
  const Clay_ElementId eid = CLAY_SIDI(clayStr("SunTuneBtn"), idx);
  const bool hovered = Clay_PointerOver(eid);
  if (hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) *delta = sign;
  CLAY({.id = eid,
        .layout = {.sizing = {.width = CLAY_SIZING_FIXED(26.0f)}, .padding = {8, 8, 2, 2}},
        .backgroundColor = hovered ? kRecessedFill : Clay_Color{0, 0, 0, 40},
        .userData = styleTag(StyleTag::Recessed)}) {
    CLAY_TEXT(clayStr(sign > 0 ? "+" : "-"),
              CLAY_TEXT_CONFIG({.textColor = hovered ? kAmber : kLabel,
                                .fontId = static_cast<std::uint16_t>(FontId::Value),
                                .fontSize = 16,
                                .letterSpacing = 0}));
  }
}

// Clay does not copy text: value strings live in static buffers that outlive
// the frame (rewritten each declaration, rendered after this function returns).
void knobRow(int knob, const char* name, float value, int decimals, int* delta) {
  static std::array<std::array<char, 16>, 1> bufs{};
  auto& buf = bufs.at(static_cast<std::size_t>(knob));
  std::snprintf(buf.data(), buf.size(), "%.*f", decimals, static_cast<double>(value));
  CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)},
                   .padding = {10, 10, 3, 3},
                   .childGap = 8,
                   .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(120.0f)}}}) {
      CLAY_TEXT(clayStr(name),
                CLAY_TEXT_CONFIG({.textColor = kLabel,
                                  .fontId = static_cast<std::uint16_t>(FontId::Value),
                                  .fontSize = 15,
                                  .letterSpacing = 0}));
    }
    stepButton(knob, -1, delta);
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(52.0f)},
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}}) {
      CLAY_TEXT(clayStr(buf.data()),
                CLAY_TEXT_CONFIG({.textColor = kAmber,
                                  .fontId = static_cast<std::uint16_t>(FontId::Value),
                                  .fontSize = 15,
                                  .letterSpacing = 0}));
    }
    stepButton(knob, +1, delta);
  }
}

}  // namespace

void sunTuningPanel(const SunTuneState& s, SunTuneResult& out) {
  if (!s.open) return;
  CLAY({.id = CLAY_ID("SunTunePanel"),
        .layout = {.sizing = {.width = CLAY_SIZING_FIT(260.0f)},
                   .padding = {0, 0, 6, 8},
                   .layoutDirection = CLAY_TOP_TO_BOTTOM},
        .backgroundColor = kPanelFill,
        .floating = {.offset = {-12.0f, 44.0f},
                     .zIndex = 10,
                     .attachPoints = {.element = CLAY_ATTACH_POINT_RIGHT_TOP,
                                      .parent = CLAY_ATTACH_POINT_RIGHT_TOP},
                     .attachTo = CLAY_ATTACH_TO_ROOT},
        .userData = styleTag(StyleTag::Recessed)}) {
    CLAY({.layout = {.padding = {10, 10, 2, 4}}}) {
      CLAY_TEXT(clayStr("FRAME TUNING (F10)"),
                CLAY_TEXT_CONFIG({.textColor = kLabel,
                                  .fontId = static_cast<std::uint16_t>(FontId::Display),
                                  .fontSize = 15,
                                  .letterSpacing = 1}));
    }
    knobRow(0, "Scene exposure", s.sceneExposure, 2, &out.sceneExpDelta);
  }
}

}  // namespace x4sb::editor
