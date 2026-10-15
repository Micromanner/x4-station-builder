#include "mesh_load.hpp"

#include "raylib.h"
#include "x4sb/data/catalog.hpp"
#include "x4sb/editorcore/mesh_paths.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace x4sb::editor {

void loadStationMeshes(const Station& station, const ModuleCatalog& catalog, MeshCache& meshes) {
  // Every part's coarser LOD siblings too: the per-part LOD pick swaps files as the
  // camera moves, and a first-use upload mid-frame is a visible hitch.
  std::vector<std::string> paths;
  for (const std::string& base : meshPathsFor(station, catalog)) {
    paths.push_back(base);
    for (int lod = 1; lod < 4; ++lod) paths.push_back(meshKeyLod(base, lod));
  }
  const int total = static_cast<int>(paths.size());
  if (total == 0) return;

  // Workers decode in parallel; this thread uploads for up to one redraw interval
  // at a time, so the bar refreshes ~20/s regardless of mesh count.
  constexpr double kRedrawInterval = 0.05;

  const auto drawProgress = [&](int done) {
    BeginDrawing();
    ClearBackground(::Color{30, 30, 38, 255});
    const int w = GetScreenWidth();
    const int h = GetScreenHeight();
    const int barW = w / 2;
    constexpr int barH = 22;
    const int barX = (w - barW) / 2;
    const int barY = h / 2;
    const float frac = static_cast<float>(done) / static_cast<float>(total);
    DrawRectangleLines(barX - 1, barY - 1, barW + 2, barH + 2, ::Color{150, 188, 230, 255});
    DrawRectangle(barX, barY, static_cast<int>(static_cast<float>(barW) * frac), barH,
                  ::Color{86, 112, 150, 255});
    const std::string label =
        "Loading meshes  " + std::to_string(done) + " / " + std::to_string(total);
    DrawText(label.c_str(), barX, barY - 30, 20, ::Color{200, 220, 240, 255});
    EndDrawing();
  };

  for (int i = 0; i < total; ++i)
    meshes.request(paths[static_cast<std::size_t>(i)], /*optional=*/i % 4 != 0);
  for (;;) {
    meshes.pumpStreaming(kRedrawInterval, std::chrono::milliseconds{5});
    const auto done = static_cast<int>(std::count_if(
        paths.begin(), paths.end(), [&](const auto& path) { return meshes.isResolved(path); }));
    drawProgress(done);
    if (done == total || WindowShouldClose()) return;
  }
}

}  // namespace x4sb::editor
