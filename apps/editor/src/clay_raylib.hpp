#pragma once
// Clay -> raylib translation layer (Approach A: only the primitives the top bar
// needs — flat/gloss/recessed fills, 1px edge, drop shadow, text). Styling lives
// HERE; UI declaration stays declarative and tags elements via Clay userData.
// Later step-6 slices extend this layer; the render-free libs never see Clay.
#include "clay.h"  // ⚠ VERIFY-CLAY: vendored header path is third_party/clay/clay.h
#include "ui_fonts.hpp"

#include <cstdint>
#include <vector>

namespace x4sb::editor {

// Carried on a Clay element's userData and read back per render command to choose
// how a rectangle is painted. Flat is the default (plain panel fill).
enum class StyleTag : std::uintptr_t { Flat = 0, GlossBar = 1, Recessed = 2 };

[[nodiscard]] std::vector<std::uint8_t> clayInit(int width, int height, const UiFonts& fonts);

void renderClayCommands(const Clay_RenderCommandArray& commands, const UiFonts& fonts);

}  // namespace x4sb::editor
