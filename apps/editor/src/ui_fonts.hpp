#pragma once
// Loads the two Gunmetal Cherenkov type roles (engraved display + tabular value)
// as TTFs with bilinear filtering. Placeholder faces (Cinzel + JetBrains Mono);
// final picks are deferred (aesthetic spec §4). Falls back to raylib's default
// font per role if a file is missing, so the UI always renders.
#include "raylib.h"

#include <cstdint>

namespace x4sb::editor {

enum class FontId : std::uint16_t { Display = 0, Value = 1 };

struct UiFonts {
  ::Font display{};
  ::Font value{};
  bool displayFromFallback{false};
  bool valueFromFallback{false};
};

[[nodiscard]] UiFonts loadUiFonts();
void unloadUiFonts(UiFonts& fonts);

}  // namespace x4sb::editor
