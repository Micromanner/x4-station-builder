#include "render.hpp"

#include "mesh_cache.hpp"
#include "raylib_convert.hpp"
#include "rlgl.h"

#include "x4sb/data/types.hpp"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>

namespace x4sb::editor {
namespace {

bool pointIsLinked(const PlacedModule& m, const std::string& pointId) {
  for (const auto& l : m.links)
    if (l.thisPointId == pointId) return true;
  return false;
}

const char* categoryName(Category c) {
  switch (c) {
    case Category::Production: return "Production";
    case Category::Storage: return "Storage";
    case Category::Habitat: return "Habitat";
    case Category::Dock: return "Dock";
    case Category::Defense: return "Defense";
    case Category::Connector: return "Connector";
    case Category::Other: return "Other";
  }
  return "?";
}

// Draw a box for `def`'s local AABB under `xf`, nested in the current matrix.
void drawModuleBox(const ModuleDef& def, const Transform& xf, ::Color fill, ::Color edge) {
  const Vec3 center = (def.aabb.min + def.aabb.max) * 0.5;
  const Vec3 size = def.aabb.max - def.aabb.min;
  rlPushMatrix();
  rlMultMatrixf(MatrixToFloatV(toRlMatrix(xf)).v);
  DrawCubeV(toRl(center), toRl(size), fill);
  DrawCubeWiresV(toRl(center), toRl(size), edge);
  rlPopMatrix();
}

// Draw `def`'s glTF parts as wireframes under `xf`, each part nested in its own
// localTransform. Returns true if at least one mesh drew; false (no mesh loaded)
// signals the caller to fall back to the AABB box. Wireframe is winding-
// independent, so the global culling state need not change.
bool drawModuleMeshes(const ModuleDef& def, const Transform& xf, MeshCache& meshes, ::Color tint) {
  bool drew = false;
  for (const MeshRef& ref : def.meshRefs) {
    const ::Model* model = meshes.get(ref.gltfPath);
    if (model == nullptr) continue;
    rlPushMatrix();
    rlMultMatrixf(MatrixToFloatV(toRlMatrix(compose(xf, ref.localTransform))).v);
    DrawModelWires(*model, ::Vector3{0, 0, 0}, 1.0f, tint);
    rlPopMatrix();
    drew = true;
  }
  return drew;
}

void drawConnectors(const ModuleDef& def, const PlacedModule& pm) {
  for (const auto& cp : def.connectionPoints) {
    const Vec3 world = apply(pm.worldTransform, cp.localPosition);
    const bool linked = pointIsLinked(pm, cp.id);
    // Orange (free) / gray (occupied) so markers read distinctly from the blue
    // orientation-gizmo axis/tip.
    const ::Color c = linked ? GRAY : ORANGE;
    DrawSphere(toRl(world), 20.0f, c);  // X4 scale: ~0.6 was sub-pixel
    // Draw the connector's local +Z as its facing normal. This MUST stay the
    // same axis the snap solver mates about (snap.cpp `kMate`, spec §3.1); if
    // that convention is corrected against real data, update this axis too, or
    // the markers will mislead the eyeball check of a snap.
    const Vec3 n = rotate(pm.worldTransform.rotation * cp.localRotation, Vec3{0, 0, 1});
    DrawLine3D(toRl(world), toRl(world + n * 80.0), c);  // X4 scale normal stub
  }
}

// Local-frame gizmo at the module origin so orientation is visible at a glance:
// +X red, +Y green, +Z blue (the conventional "forward", capped with a sphere).
// Scaled to the module, so a wrongly-rotated snap shows up as a tilted triad
// rather than an indistinguishable box.
void drawAxisGizmo(const ModuleDef& def, const Transform& xf) {
  const Vec3 ext = def.aabb.max - def.aabb.min;
  const double maxExt = std::max(ext.x, std::max(ext.y, ext.z));
  const double len = 0.6 * maxExt;
  const Vec3 o = xf.position;
  DrawLine3D(toRl(o), toRl(o + rotate(xf.rotation, Vec3{1, 0, 0}) * len), RED);
  DrawLine3D(toRl(o), toRl(o + rotate(xf.rotation, Vec3{0, 1, 0}) * len), GREEN);
  const Vec3 fwd = o + rotate(xf.rotation, Vec3{0, 0, 1}) * len;
  DrawLine3D(toRl(o), toRl(fwd), BLUE);
  DrawSphere(toRl(fwd), static_cast<float>(maxExt) * 0.04f, BLUE);  // +Z forward tip
}

// Begin the 3D scene: X4-scale clip planes, the global (1,1,-1) handedness flip
// + culling-off, and the translucent 20km build-volume plot box. Pairs with
// endScene(). Shared by both drawScene overloads so the interactive path and the
// --snaptest harness render through identical setup.
void beginScene(const ::Camera3D& camera) {
  // X4 modules span tens to ~1500 units, far past raylib's default 1000-unit far
  // plane. Push the clip planes out to X4 scale so geometry isn't clipped (must
  // be set before BeginMode3D, which builds the projection from these).
  rlSetClipPlanes(0.5, 2000000.0);

  BeginMode3D(camera);
  rlPushMatrix();
  rlScalef(1.0f, 1.0f, -1.0f);  // X4 left-handed -> raylib right-handed (parent §4)
  rlDisableBackfaceCulling();   // the -1 scale inverts winding; boxes/wires don't cull

  // The 20x20x20 km station build volume (X4's max plot), centered on the origin.
  // Hardcoded for now (later adjustable); purely visual, no collision. Drawn like
  // the in-game plot: a faint translucent blue volume with brighter blue edges.
  constexpr float kPlotSize = 20000.0f;  // 20 km in X4 units (meters)
  const ::Vector3 plotCenter{0.0f, 0.0f, 0.0f};
  const ::Vector3 plotExtent{kPlotSize, kPlotSize, kPlotSize};
  DrawCubeV(plotCenter, plotExtent, ::Color{50, 120, 210, 16});
  DrawCubeWiresV(plotCenter, plotExtent, ::Color{70, 150, 240, 220});
}

void endScene() {
  rlEnableBackfaceCulling();
  rlPopMatrix();
  EndMode3D();
}

// Draw every placed module (mesh wireframe with box fallback, connectors, gizmo).
// `selected` highlights one instance in yellow; pass nullopt for none.
void drawPlacedModules(const Station& station, const ModuleCatalog& catalog,
                       std::optional<InstanceId> selected, MeshCache& meshes, bool showGizmos,
                       bool showMeshes) {
  for (const auto& pm : station.modules()) {
    const ModuleDef* def = catalog.find(pm.defId);
    if (def == nullptr) continue;
    const bool sel = selected.has_value() && *selected == pm.instanceId;
    // Mesh wireframe when enabled, else (or if no mesh loads) the AABB box.
    const ::Color wire = sel ? YELLOW : LIGHTGRAY;
    if (!showMeshes || !drawModuleMeshes(*def, pm.worldTransform, meshes, wire)) {
      drawModuleBox(*def, pm.worldTransform, ::Color{120, 160, 200, 90}, sel ? YELLOW : DARKBLUE);
    }
    drawConnectors(*def, pm);
    if (showGizmos) drawAxisGizmo(*def, pm.worldTransform);
  }
}

}  // namespace

void drawScene(const EditorState& state, const ::Camera3D& camera, MeshCache& meshes,
               bool showGizmos, bool showMeshes) {
  beginScene(camera);

  drawPlacedModules(state.station(), state.catalog(), state.selected(), meshes, showGizmos,
                    showMeshes);

  if (state.ghost()) {
    const ModuleDef* gdef = state.defFor(state.ghost()->defId);
    if (gdef != nullptr) {
      const ::Color fill =
          state.ghost()->valid ? ::Color{0, 220, 0, 90} : ::Color{220, 0, 0, 90};
      const ::Color edge = state.ghost()->valid ? GREEN : RED;
      if (!showMeshes ||
          !drawModuleMeshes(*gdef, state.ghost()->worldTransform, meshes, edge)) {
        drawModuleBox(*gdef, state.ghost()->worldTransform, fill, edge);
      }
      if (showGizmos) drawAxisGizmo(*gdef, state.ghost()->worldTransform);
    }
  }

  endScene();
}

void drawScene(const Station& station, const ModuleCatalog& catalog, const ::Camera3D& camera,
               MeshCache& meshes, bool showGizmos, bool showMeshes) {
  beginScene(camera);
  drawPlacedModules(station, catalog, std::nullopt, meshes, showGizmos, showMeshes);
  endScene();
}

void drawHud(const EditorState& state, int screenWidth, int /*screenHeight*/, bool showGizmos) {
  const ModuleDef* def = state.activeDef();
  char line[256];

  std::snprintf(line, sizeof(line), "Active: %s", def != nullptr ? def->id.c_str() : "(none)");
  DrawText(line, 12, 10, 20, RAYWHITE);

  const char* filt = state.filter().has_value() ? categoryName(*state.filter()) : "All";
  std::snprintf(line, sizeof(line), "Filter: %s    %zu/%zu", filt,
                def != nullptr ? state.activeIndex() + 1 : 0, state.activeCount());
  DrawText(line, 12, 34, 18, SKYBLUE);

  std::snprintf(line, sizeof(line), "Placed: %zu    Undo:%s  Redo:%s    Gizmos:%s",
                state.station().size(), state.canUndo() ? "on" : "-", state.canRedo() ? "on" : "-",
                showGizmos ? "on" : "off");
  DrawText(line, 12, 56, 16, LIGHTGRAY);

  DrawText(
      "[ / ]=cycle  1=Prod 2=Stor 3=Hab 4=Dock 5=Def 6=Conn 7=Other  0=all  G=gizmos  M=mesh/box",
      12, 78, 14, GRAY);
  DrawText("LMB=place/select   Del or X=delete   Ctrl+Z/Y=undo/redo   RMB=orbit   wheel=zoom   F=frame",
           12, 96, 14, GRAY);

  DrawFPS(screenWidth - 90, 10);
}

void drawToast(const std::string& message, int screenHeight) {
  if (message.empty()) return;
  const int fontSize = 20;
  const int pad = 10;
  const int x = 20;
  const int bottomMargin = 12;
  const int y = screenHeight - fontSize - pad - bottomMargin;
  const int textW = MeasureText(message.c_str(), fontSize);
  DrawRectangle(x - pad, y - pad, textW + 2 * pad, fontSize + 2 * pad, ::Color{0, 0, 0, 190});
  DrawText(message.c_str(), x, y, fontSize, ::Color{120, 230, 140, 255});
}

}  // namespace x4sb::editor
