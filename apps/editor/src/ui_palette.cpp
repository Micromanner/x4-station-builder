#include "ui_palette.hpp"

#include "clay.h"
#include "clay_raylib.hpp"  // StyleTag, clayStr, styleTag
#include "raylib.h"         // IsMouseButtonReleased
#include "ui_category.hpp"
#include "ui_fonts.hpp"  // FontId

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

namespace x4sb::editor {
namespace {

constexpr float kPanelWidth = 240.0f;

constexpr Clay_Color kLabel{159, 220, 229, 255};
constexpr Clay_Color kValue{237, 232, 218, 255};
constexpr Clay_Color kAmber{242, 169, 59, 255};
constexpr Clay_Color kRowText{210, 218, 224, 255};
constexpr Clay_Color kClear{0, 0, 0, 0};  // idle rows paint nothing (alpha gates emission)

struct ChipDef {
  const char* id;               // string literal (CLAY_SID needs the bytes to outlive the frame)
  std::optional<Category> cat;  // nullopt == the "All" chip (clears the filter)
};

constexpr std::array<ChipDef, 8> kChips{{
    {"ChipAll", std::nullopt},
    {"ChipProd", Category::Production},
    {"ChipStor", Category::Storage},
    {"ChipHab", Category::Habitat},
    {"ChipDock", Category::Dock},
    {"ChipDef", Category::Defense},
    {"ChipConn", Category::Connector},
    {"ChipOther", Category::Other},
}};

// One category chip. Active => highlighted; click => writes the intent into `out`.
void chip(const ChipDef& cd, const std::optional<Category>& filter, PaletteAction& out) {
  const bool active = filter == cd.cat;  // both nullopt -> the "All" chip is active
  const Clay_ElementId eid = CLAY_SID(clayStr(cd.id));
  const bool hovered = Clay_PointerOver(eid);
  if (hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
    out.kind = cd.cat ? PaletteActionKind::SetFilter : PaletteActionKind::ClearFilter;
    if (cd.cat) out.category = *cd.cat;
  }
  const char* label = cd.cat ? categoryShort(*cd.cat) : "All";
  CLAY({.id = eid,
        .layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .padding = {8, 8, 4, 4}},
        .backgroundColor = kStyledGate,
        .userData = styleTag(active ? StyleTag::ChipActive : StyleTag::Chip)}) {
    CLAY_TEXT(clayStr(label),
              CLAY_TEXT_CONFIG({.textColor = active ? kValue : (hovered ? kAmber : kLabel),
                                .fontId = static_cast<std::uint16_t>(FontId::Value),
                                .fontSize = 15,
                                .letterSpacing = 1}));
  }
}

}  // namespace

float paletteWidth() { return kPanelWidth; }

PaletteAction palette(const EditorState& state) {
  PaletteAction action;

  const std::optional<Category> filter = state.filter();
  const std::vector<const ModuleDef*>& view = state.filteredView();
  const std::size_t activeIndex = state.activeIndex();

  // static: Clay holds this pointer until render (after EndLayout); a per-call buffer
  // would dangle. Single UI thread + immediate redraw makes overwrite-next-frame safe.
  static char cartLine[48];
  std::snprintf(cartLine, sizeof(cartLine), "Cart: %d  -  Enter to build", state.cartTotal());

  CLAY({.id = CLAY_ID("Palette"),
        .layout = {.sizing = {.width = CLAY_SIZING_FIXED(kPanelWidth),
                              .height = CLAY_SIZING_GROW(0)},
                   .padding = {8, 8, 8, 8},
                   .childGap = 6,
                   .layoutDirection = CLAY_TOP_TO_BOTTOM},
        .backgroundColor = kStyledGate,
        .userData = styleTag(StyleTag::Panel)}) {
    // Clay has no flex-wrap, so lay the chips out in explicit rows of four; the row
    // count and last-row remainder derive from kChips.size() (no hard-coded count).
    constexpr std::size_t kChipCols = 4;
    for (std::size_t start = 0; start < kChips.size(); start += kChipCols) {
      CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = 4}}) {
        for (std::size_t i = start; i < start + kChipCols && i < kChips.size(); ++i)
          chip(kChips[i], filter, action);
      }
    }

    // Scrollable module list. childOffset = Clay_GetScrollOffset() applies this
    // element's accumulated wheel delta (fed by Clay_UpdateScrollContainers in main).
    CLAY({.id = CLAY_ID("PaletteList"),
          .layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
                     .childGap = 2,
                     .layoutDirection = CLAY_TOP_TO_BOTTOM},
          .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}}) {
      for (std::size_t i = 0; i < view.size(); ++i) {
        const ModuleDef* def = view[i];
        const Clay_ElementId rowId = CLAY_IDI("PaletteRow", static_cast<std::uint32_t>(i));
        const bool isActive = i == activeIndex;
        const bool hovered = Clay_PointerOver(rowId);
        if (hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
          action.kind = PaletteActionKind::SetActive;
          action.index = i;
        }
        // Idle, un-hovered rows paint nothing; only active/hovered rows get a fill.
        const bool paint = isActive || hovered;
        CLAY({.id = rowId,
              .layout = {.sizing = {.width = CLAY_SIZING_GROW(0)},
                         .padding = {6, 6, 4, 4},
                         .childGap = 6,
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
              .backgroundColor = paint ? kStyledGate : kClear,
              .userData = styleTag(isActive ? StyleTag::ListRowActive : StyleTag::ListRow)}) {
          // Category swatch: a Flat child whose exact backgroundColor the renderer
          // draws, so category color flows without touching clay_raylib.
          CLAY({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(4),
                                      .height = CLAY_SIZING_FIXED(14)}},
                .backgroundColor = categoryColor(def->category)}) {}
          CLAY_TEXT(clayStr(displayName(*def).c_str()),
                    CLAY_TEXT_CONFIG({.textColor = isActive ? kValue : kRowText,
                                      .fontId = static_cast<std::uint16_t>(FontId::Value),
                                      .fontSize = 15,
                                      .letterSpacing = 0}));
        }
      }
    }

    // Cart summary footer — display only; the real quantity editor is slice 3.
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .padding = {6, 6, 6, 6}},
          .backgroundColor = kStyledGate,
          .userData = styleTag(StyleTag::Recessed)}) {
      CLAY_TEXT(clayStr(cartLine),
                CLAY_TEXT_CONFIG({.textColor = kValue,
                                  .fontId = static_cast<std::uint16_t>(FontId::Value),
                                  .fontSize = 14,
                                  .letterSpacing = 0}));
    }
  }
  return action;
}

}  // namespace x4sb::editor
