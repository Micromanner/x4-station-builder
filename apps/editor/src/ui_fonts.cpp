#include "ui_fonts.hpp"

#include "app_paths.hpp"

#include <cstdio>
#include <optional>
#include <string>

namespace x4sb::editor {
namespace {

// Glyph atlas baked at a generous size; raylib bilinear-filters down to the small
// HUD sizes (the aesthetic's small-size floor is applied at draw time, not here).
constexpr int kBakeSize = 48;

::Font loadOrFallback(const char* relPath, bool& fromFallback) {
  const std::optional<std::string> path = findAsset(relPath);
  if (path) {
    const ::Font f = LoadFontEx(path->c_str(), kBakeSize, nullptr, 0);
    if (f.texture.id != 0) {
      SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
      fromFallback = false;
      return f;
    }
  }
  std::fprintf(stderr, "ui_fonts: '%s' not found/loadable; using raylib default font\n", relPath);
  fromFallback = true;
  return GetFontDefault();
}

}  // namespace

UiFonts loadUiFonts() {
  UiFonts fonts;
  fonts.display =
      loadOrFallback("apps/editor/assets/fonts/Cinzel-Regular.ttf", fonts.displayFromFallback);
  fonts.value =
      loadOrFallback("apps/editor/assets/fonts/JetBrainsMono-Regular.ttf", fonts.valueFromFallback);
  std::printf("ui_fonts: display=%s value=%s\n", fonts.displayFromFallback ? "DEFAULT" : "Cinzel",
              fonts.valueFromFallback ? "DEFAULT" : "JetBrainsMono");
  return fonts;
}

void unloadUiFonts(UiFonts& fonts) {
  if (!fonts.displayFromFallback) UnloadFont(fonts.display);
  if (!fonts.valueFromFallback) UnloadFont(fonts.value);
}

}  // namespace x4sb::editor
