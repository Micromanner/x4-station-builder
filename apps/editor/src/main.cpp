// x4sb-editor — the interactive station builder (spec §4B).
//
// PLACEHOLDER: opens a raylib window with an orbit camera and a reference grid,
// proving the renderer/camera wiring. Real module rendering, picking, snapping,
// and the raygui UI panels build on top of the libs/ units.
#include "raylib.h"

int main() {
  const int screenWidth = 1280;
  const int screenHeight = 720;
  InitWindow(screenWidth, screenHeight, "X4 Station Builder");
  SetTargetFPS(60);

  Camera3D camera = {0};
  camera.position = (Vector3){20.0f, 20.0f, 20.0f};
  camera.target = (Vector3){0.0f, 0.0f, 0.0f};
  camera.up = (Vector3){0.0f, 1.0f, 0.0f};
  camera.fovy = 45.0f;
  camera.projection = CAMERA_PERSPECTIVE;

  while (!WindowShouldClose()) {
    UpdateCamera(&camera, CAMERA_ORBITAL);

    BeginDrawing();
    ClearBackground(RAYWHITE);

    BeginMode3D(camera);
    DrawGrid(20, 1.0f);
    DrawCubeWires((Vector3){0, 0, 0}, 2, 2, 2, DARKBLUE);
    EndMode3D();

    DrawText("X4 Station Builder \xE2\x80\x94 scaffold", 12, 12, 20, DARKGRAY);
    DrawFPS(screenWidth - 90, 12);
    EndDrawing();
  }

  CloseWindow();
  return 0;
}
