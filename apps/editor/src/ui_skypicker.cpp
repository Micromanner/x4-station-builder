#include "ui_skypicker.hpp"

#include "clay.h"
#include "clay_raylib.hpp"  // StyleTag, clayStr
#include "raylib.h"         // IsMouseButtonReleased
#include "ui_fonts.hpp"

#include <cstddef>
#include <cstdint>

namespace x4sb::editor {
namespace {

constexpr Clay_Color kLabel{159, 220, 229, 255};
constexpr Clay_Color kAmber{242, 169, 59, 255};
constexpr Clay_Color kRecessedFill{10, 11, 14, 153};
constexpr Clay_Color kPanelFill{16, 19, 23, 240};

// One dropdown row; rowIndex is kSkyPickRandom or an entries index. Idle rows
// paint nothing (alpha 0 -> no RECTANGLE), matching the palette-list pattern.
void row(int rowIndex, const char* label, SkyPickerResult& out) {
  const Clay_ElementId eid = CLAY_SIDI(clayStr("SkyRow"), static_cast<std::uint32_t>(rowIndex + 2));
  const bool hovered = Clay_PointerOver(eid);
  if (hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) out.picked = rowIndex;
  CLAY({.id = eid,
        .layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .padding = {10, 10, 4, 4}},
        .backgroundColor = hovered ? kRecessedFill : Clay_Color{0, 0, 0, 0},
        .userData = styleTag(StyleTag::Recessed)}) {
    CLAY_TEXT(clayStr(label), CLAY_TEXT_CONFIG({.textColor = hovered ? kAmber : kLabel,
                                                .fontId = static_cast<std::uint16_t>(FontId::Value),
                                                .fontSize = 16,
                                                .letterSpacing = 0}));
  }
}

}  // namespace

void skyPicker(const SkyPickerState& s, SkyPickerResult& out) {
  const Clay_ElementId ctl = CLAY_ID("SkyPickerCtl");
  const bool hovered = Clay_PointerOver(ctl);
  if (hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) out.toggleOpen = true;
  // Click-away must be decided HERE, in the layout phase: main.cpp's dispatch
  // runs after EndDrawing, where raylib's PollInputEvents has already advanced
  // input state — a release checked there fires one frame BEFORE these row
  // handlers see it, closing the panel and eating the pick.
  if (s.open && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !hovered &&
      !Clay_PointerOver(CLAY_ID("SkyPickerPanel"))) {
    out.clickAway = true;
  }
  CLAY({.id = ctl,
        .layout = {.padding = {10, 10, 6, 6}},
        .backgroundColor = kRecessedFill,
        .userData = styleTag(StyleTag::Recessed)}) {
    CLAY_TEXT(clayStr(s.label),
              CLAY_TEXT_CONFIG({.textColor = hovered ? kAmber : kLabel,
                                .fontId = static_cast<std::uint16_t>(FontId::Display),
                                .fontSize = 18,
                                .letterSpacing = 1}));
    if (s.open) {
      CLAY({.id = CLAY_ID("SkyPickerPanel"),
            .layout = {.sizing = {.width = CLAY_SIZING_FIXED(280.0f),
                                  .height = CLAY_SIZING_FIT(0, 420.0f)},
                       .layoutDirection = CLAY_TOP_TO_BOTTOM},
            .backgroundColor = kPanelFill,
            .floating = {.offset = {0.0f, 2.0f},
                         .zIndex = 10,
                         .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP,
                                          .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM},
                         .attachTo = CLAY_ATTACH_TO_PARENT},
            .userData = styleTag(StyleTag::Recessed)}) {
        CLAY({.id = CLAY_ID("SkyPickerScroll"),
              .layout = {.sizing = {.width = CLAY_SIZING_GROW(0),
                                    .height = CLAY_SIZING_FIT(0, 416.0f)},
                         .layoutDirection = CLAY_TOP_TO_BOTTOM},
              .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}}) {
          row(kSkyPickRandom, "Random", out);
          if (s.entries != nullptr) {
            for (std::size_t i = 0; i < s.entries->size(); ++i)
              row(static_cast<int>(i), (*s.entries)[i].name.c_str(), out);
          }
        }
      }
    }
  }
}

}  // namespace x4sb::editor
