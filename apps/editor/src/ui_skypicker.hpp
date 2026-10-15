#pragma once
// Sector-sky picker: a topbar control + floating dropdown ("Random" + all baked
// sectors). Render-only; emits picks, main.cpp owns the switch + persistence.
#include "x4sb/assetpipe/sectorindex.hpp"

#include <vector>

namespace x4sb::editor {

inline constexpr int kSkyPickNone = -2;
inline constexpr int kSkyPickRandom = -1;

struct SkyPickerState {
  const std::vector<assetpipe::SectorIndexEntry>* entries = nullptr;  // index order
  const char* label = "Sky: Random";  // must outlive the frame (clayStr rule)
  bool open = false;
};

struct SkyPickerResult {
  bool toggleOpen = false;
  bool clickAway = false;     // released outside ctl+panel while open -> close
  int picked = kSkyPickNone;  // kSkyPickRandom or an index into *entries
};

// Declare the control (call inside the top bar's CLAY block) and, when open,
// the floating dropdown beneath it.
void skyPicker(const SkyPickerState& s, SkyPickerResult& out);

}  // namespace x4sb::editor
