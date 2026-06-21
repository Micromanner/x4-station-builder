#pragma once
// Shared category presentation for the Clay chrome (palette chips + inspector).
// Pure formatting — no raylib, no state. Long labels for the inspector, short for
// chips, an accent color per category for the palette row swatches.
#include "clay.h"
#include "x4sb/data/types.hpp"

namespace x4sb::editor {

[[nodiscard]] inline const char* categoryLabel(Category c) {
  switch (c) {
    case Category::Production:
      return "Production";
    case Category::Storage:
      return "Storage";
    case Category::Habitat:
      return "Habitat";
    case Category::Dock:
      return "Dock";
    case Category::Defense:
      return "Defense";
    case Category::Connector:
      return "Connector";
    case Category::Other:
      return "Other";
  }
  return "Other";
}

[[nodiscard]] inline const char* categoryShort(Category c) {
  switch (c) {
    case Category::Production:
      return "Prod";
    case Category::Storage:
      return "Stor";
    case Category::Habitat:
      return "Hab";
    case Category::Dock:
      return "Dock";
    case Category::Defense:
      return "Def";
    case Category::Connector:
      return "Conn";
    case Category::Other:
      return "Other";
  }
  return "Other";
}

[[nodiscard]] inline Clay_Color categoryColor(Category c) {
  switch (c) {
    case Category::Production:
      return Clay_Color{210, 140, 60, 255};
    case Category::Storage:
      return Clay_Color{90, 160, 210, 255};
    case Category::Habitat:
      return Clay_Color{110, 200, 130, 255};
    case Category::Dock:
      return Clay_Color{200, 200, 90, 255};
    case Category::Defense:
      return Clay_Color{210, 90, 90, 255};
    case Category::Connector:
      return Clay_Color{160, 160, 175, 255};
    case Category::Other:
      return Clay_Color{140, 130, 160, 255};
  }
  return Clay_Color{140, 130, 160, 255};
}

}  // namespace x4sb::editor
