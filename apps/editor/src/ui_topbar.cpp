#include "ui_topbar.hpp"

#include "clay.h"
#include "clay_raylib.hpp"  // StyleTag
#include "raylib.h"         // IsMouseButtonReleased
#include "ui_fonts.hpp"     // FontId

#include <cstdint>
#include <cstdio>

namespace x4sb::editor {
namespace {

constexpr float kBarHeight = 36.0f;

constexpr Clay_Color kLabel{159, 220, 229, 255};
constexpr Clay_Color kValue{237, 232, 218, 255};
constexpr Clay_Color kAmber{242, 169, 59, 255};
constexpr Clay_Color kDisabled{90, 100, 108, 255};
// Styled elements MUST have backgroundColor.a > 0: Clay emits a RECTANGLE
// render command only when alpha > 0 (clay.h:2780), and clay_raylib paints the
// gloss/recessed fill inside that case (keyed by StyleTag; the renderer overrides
// the exact color, but the alpha gates emission).
constexpr Clay_Color kGlossFill{26, 32, 38, 255};
constexpr Clay_Color kRecessedFill{10, 11, 14, 153};

// One clickable button. Hover -> amber label + brighter recessed fill; disabled ->
// dimmed label and ignored clicks. Records into `clicked` if released over it.
// `id` and `label` must be string literals (outlive the frame; see clayStr).
void button(const char* id, const char* label, bool enabled, TopBarAction action,
            TopBarAction& clicked) {
  // CLAY_ID requires a string literal (enforced at compile time); use CLAY_SID
  // with a Clay_String for the const-char* id passed in at runtime.
  const Clay_ElementId eid = CLAY_SID(clayStr(id));
  const bool hovered = enabled && Clay_PointerOver(eid);
  if (hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) clicked = action;
  const Clay_Color text = enabled ? (hovered ? kAmber : kLabel) : kDisabled;
  // Field order matches Clay_TextElementConfig declaration order (C++20).
  CLAY({.id = eid,
        .layout = {.padding = {10, 10, 6, 6}},
        .backgroundColor = kRecessedFill,
        .userData = styleTag(StyleTag::Recessed)}) {
    CLAY_TEXT(clayStr(label),
              CLAY_TEXT_CONFIG({.textColor = text,
                                .fontId = static_cast<std::uint16_t>(FontId::Display),
                                .fontSize = 18,
                                .letterSpacing = 1}));
  }
}

// `text` must outlive the frame (literal or function-static buffer; see clayStr).
void readout(const char* text) {
  CLAY({.layout = {.padding = {10, 0, 8, 6}}}) {
    CLAY_TEXT(clayStr(text), CLAY_TEXT_CONFIG({.textColor = kValue,
                                               .fontId = static_cast<std::uint16_t>(FontId::Value),
                                               .fontSize = 16,
                                               .letterSpacing = 0}));
  }
}

}  // namespace

float topBarHeight() { return kBarHeight; }

TopBarAction topBar(const EditorState& state, double fps) {
  TopBarAction clicked = TopBarAction::None;

  // static: Clay references these bytes until renderClayCommands runs AFTER this
  // function returns (clay.h:2855). Per-call stack buffers would dangle. Single
  // UI thread + immediate-mode redraw makes overwrite-next-frame safe.
  static char placed[32];
  std::snprintf(placed, sizeof(placed), "Placed: %zu", state.station().size());
  static char fpsText[24];
  std::snprintf(fpsText, sizeof(fpsText), "%.0f fps", fps);

  // The whole strip is one gloss bar (tag drives the styled fill in clay_raylib).
  CLAY(
      {.id = CLAY_ID("TopBar"),
       .layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(kBarHeight)},
                  .padding = {6, 6, 0, 0},
                  .childGap = 4,
                  .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
       .backgroundColor = kGlossFill,
       .userData = styleTag(StyleTag::GlossBar)}) {
    button("BtnOpen", "Open", true, TopBarAction::Open, clicked);
    button("BtnSave", "Save", true, TopBarAction::Save, clicked);
    button("BtnUndo", "Undo", state.canUndo(), TopBarAction::Undo, clicked);
    button("BtnRedo", "Redo", state.canRedo(), TopBarAction::Redo, clicked);
    // Spacer pushes the readouts toward the right.
    CLAY({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
    readout("Plot: 20x20x20 km");
    readout(placed);
    readout(fpsText);
  }
  return clicked;
}

}  // namespace x4sb::editor
