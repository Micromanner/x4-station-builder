#pragma once
// Clay -> raylib translation layer (Approach A: only the primitives the top bar
// needs — flat/gloss/recessed fills, 1px edge, drop shadow, text). Styling lives
// HERE; UI declaration stays declarative and tags elements via Clay userData.
// Later step-6 slices extend this layer; the render-free libs never see Clay.
#include "clay.h"  // ⚠ VERIFY-CLAY: vendored header path is third_party/clay/clay.h
#include "ui_fonts.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace x4sb::editor {

// Carried on a Clay element's userData and read back per render command to choose
// how a rectangle is painted. Flat (default) paints the element's own backgroundColor.
enum class StyleTag : std::uintptr_t {
  Flat = 0,
  GlossBar = 1,
  Recessed = 2,
  Panel = 3,          // translucent docked side-panel body (palette / inspector)
  ListRow = 4,        // palette module row — hover wash
  ListRowActive = 5,  // the active/selected module row — cyan wash + accent
  Chip = 6,           // category filter chip (idle)
  ChipActive = 7,     // the selected category chip
};

// Non-owning Clay string over `s`. Clay references these bytes until the frame's
// render runs (after Clay_EndLayout), so `s` must be a string literal or a
// function-static buffer — never a per-call stack buffer. Shared by every ui_* unit.
[[nodiscard]] Clay_String clayStr(const char* s);
// Pack a StyleTag into a Clay element's userData void*.
[[nodiscard]] void* styleTag(StyleTag t);

// A throwaway opaque fill for any StyleTag-painted element: Clay emits a RECTANGLE
// command only when backgroundColor.a > 0, and the renderer then overrides the RGB
// by tag. Give every tagged element this one fill so "tagged" reliably means
// "painted" — drop it and the element silently renders nothing.
constexpr Clay_Color kStyledGate{20, 24, 30, 255};

[[nodiscard]] std::vector<std::uint8_t> clayInit(int width, int height, const UiFonts& fonts);

void renderClayCommands(const Clay_RenderCommandArray& commands, const UiFonts& fonts);

}  // namespace x4sb::editor
