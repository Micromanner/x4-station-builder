#include "mesh_load.hpp"

#include "x4sb/editorcore/mesh_paths.hpp"

#include "raylib.h"

#include <string>
#include <vector>

namespace x4sb::editor {

void loadStationMeshes(const Station& station, const ModuleCatalog& catalog, MeshCache& meshes) {
  const std::vector<std::string> paths = meshPathsFor(station, catalog);
  const int total = static_cast<int>(paths.size());
  if (total == 0) return;

  // Upload as fast as the GPU allows; redraw the progress bar on a time interval
  // (not per mesh) so the bar's refresh rate is decoupled from mesh count and from
  // any v-sync wait in EndDrawing — the uploads themselves are the wait being shown.
  constexpr double kRedrawInterval = 0.05;  // ~20 progress updates/sec
  double lastDraw = -1.0;

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

  for (int i = 0; i < total; ++i) {
    meshes.warm(paths[static_cast<std::size_t>(i)]);
    const double now = GetTime();
    if (lastDraw < 0.0 || now - lastDraw >= kRedrawInterval || i + 1 == total) {
      drawProgress(i + 1);
      lastDraw = GetTime();
      if (WindowShouldClose()) break;
    }
  }
}

}  // namespace x4sb::editor
