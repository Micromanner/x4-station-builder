#include "render.hpp"

#include "mesh_cache.hpp"
#include "profiling.hpp"
#include "raylib_convert.hpp"
#include "rlgl.h"

#include "x4sb/data/types.hpp"
#include "x4sb/editorcore/display_flip.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace x4sb::editor {
namespace {

bool pointIsLinked(const PlacedModule& m, const std::string& pointId) {
  for (const auto& l : m.links)
    if (l.thisPointId == pointId) return true;
  return false;
}

// A mesh-ref is the structural hull when its part name (after "__" in the cache
// path) begins "part". An oversized hull can't render, so the whole module boxes;
// an oversized detail/anim greeble is merely skipped so the hull still draws.
bool meshRefIsStructural(const std::string& gltfPath) {
  const std::size_t sep = gltfPath.rfind("__");
  return sep != std::string::npos && gltfPath.compare(sep + 2, 4, "part") == 0;
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

// Opaque so a distant module collapsed to its box still reads against the dark
// background (a translucent fill vanished — the whole-station "missing modules"
// bug). Edge brighter than fill so adjacent boxes stay distinguishable.
constexpr ::Color kBoxFill{86, 112, 150, 255};
constexpr ::Color kBoxEdge{150, 188, 230, 255};

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

// Draw `def`'s glTF parts as SOLID flat-shaded meshes under `xf`, each part nested
// in its own localTransform. Returns true if at least one mesh drew; false (no mesh
// loaded) signals the caller to fall back to the AABB box. The caller must have
// backface culling ENABLED: under the global (1,1,-1) flip the winding is exactly
// what default back-face culling expects (confirmed empirically), so this both
// renders correctly and is cheaper than drawing every triangle's back side.
bool drawModuleMeshes(const ModuleDef& def, const Transform& xf, MeshCache& meshes, ::Color tint) {
  // Resolve all parts first (get() caches). An oversized part (>u16 indices) would
  // render as wrapped-index garbage, so it can't draw: box the whole module only
  // when the oversized part is the structural hull (a hull-less module reads as
  // broken); skip an oversized detail/anim greeble and let the hull still draw.
  // Most oversized parts are lod0-only detail greebles the LOD pass can't shrink.
  bool anyPresent = false;
  for (const MeshRef& ref : def.meshRefs) {
    if (meshes.get(ref.gltfPath) != nullptr)
      anyPresent = true;
    else if (meshes.isOversized(ref.gltfPath) && meshRefIsStructural(ref.gltfPath))
      return false;
  }
  if (!anyPresent) return false;

  for (const MeshRef& ref : def.meshRefs) {
    const ::Model* model = meshes.get(ref.gltfPath);
    if (model == nullptr) continue;
    rlPushMatrix();
    rlMultMatrixf(MatrixToFloatV(toRlMatrix(compose(xf, ref.localTransform))).v);
    DrawModel(*model, ::Vector3{0, 0, 0}, 1.0f, tint);
    rlPopMatrix();
  }
  return true;
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

// Depth-precision is the near/far RATIO, not the absolute near plane: a fixed
// (10, 2e6) wrecked precision when zoomed out AND clipped nearby modules when
// zoomed in. Scale both to the camera's orbit distance so the ratio stays ~modest
// at every zoom (near small enough never to clip, far large enough to cover the
// whole station from inside it).
struct ClipRange {
  double nearC;
  double farC;
};
[[nodiscard]] ClipRange sceneClip(const ::Camera3D& cam) {
  const double d = static_cast<double>(Vector3Distance(cam.position, cam.target));
  // 60km far floor covers the 20km plot's diagonal from any inside vantage with
  // margin (so nothing clips when zoomed in), while staying far tighter than the
  // old fixed 2e6 — the near/far ratio, hence depth precision, is what matters.
  return {std::clamp(d * 0.02, 1.5, 30.0), std::max(d * 4.0, 60000.0)};
}

// Begin the 3D scene: distance-scaled clip planes, the global (1,1,-1) handedness
// flip, and the translucent 20km build-volume plot box. Backface culling is left
// DISABLED here as the base state so the batched raylib primitives (plot box, LOD
// boxes, connector spheres) — whose winding the flip inverts — render whole; the
// mesh pass enables culling only around its own immediate draws. Pairs with
// endScene().
void beginScene(const ::Camera3D& camera) {
  const ClipRange clip = sceneClip(camera);
  rlSetClipPlanes(clip.nearC, clip.farC);  // must precede BeginMode3D (builds the projection)

  BeginMode3D(camera);
  rlPushMatrix();
  rlScalef(1.0f, 1.0f, -1.0f);  // X4 left-handed -> raylib right-handed (parent §4)
  rlDisableBackfaceCulling();

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
  rlDisableBackfaceCulling();  // batched primitives flush at EndMode3D — keep them unculled
  rlPopMatrix();
  EndMode3D();
  rlEnableBackfaceCulling();  // restore raylib's default for whatever draws next
}

// Per-module level-of-detail + culling metrics, all in raylib DISPLAY space. The
// camera lives in display space (the scene applies rlScalef(1,1,-1)), so the
// module's X4-space center has its Z negated to match before any camera-relative
// math.
struct LodMetrics {
  double along;      // depth along the view direction (negative = behind eye)
  double lateral;    // distance of the center from the view axis
  double radius;     // bounding-sphere radius
  double pixelSize;  // projected radius in screen pixels (0 if behind)
};

// screenH / (2*tan(fovy/2)): maps a world radius at unit depth to projected
// pixels. Identical for every module, so it's computed once per frame and threaded
// into lodMetrics, keeping the per-module path free of trig + global-state queries.
[[nodiscard]] double projFactor(const ::Camera3D& cam) {
  return static_cast<double>(GetScreenHeight()) /
         (2.0 * std::tan(static_cast<double>(cam.fovy) * static_cast<double>(DEG2RAD) * 0.5));
}

// `projF` is projFactor(cam), precomputed once per frame by the caller.
[[nodiscard]] LodMetrics lodMetrics(const ModuleDef& def, const PlacedModule& pm,
                                    const ::Camera3D& cam, double projF) {
  const AABB wbox = worldAabb(def.aabb, pm.worldTransform);  // X4 space
  const Vec3 c = (wbox.min + wbox.max) * 0.5;
  const double r = length(wbox.max - wbox.min) * 0.5;
  const Vec3 dc = flipZ(c);  // X4 -> display space

  const Vec3 camPos{cam.position.x, cam.position.y, cam.position.z};
  const Vec3 camTgt{cam.target.x, cam.target.y, cam.target.z};
  const Vec3 view = camTgt - camPos;
  const double viewLen = length(view);
  const Vec3 fwd = viewLen > 0 ? view * (1.0 / viewLen) : Vec3{0, 0, 1};

  const Vec3 rel = dc - camPos;
  const double along = dot(rel, fwd);
  const double lateral = length(rel - fwd * along);
  const double pixelSize = along > 0 ? (r / along) * projF : 0.0;
  return {along, lateral, r, pixelSize};
}

// Per-frame view constants shared by the cull test (so it stays free of trig).
struct ViewCull {
  double tanDiag;  // tangent of the half-angle to a frustum CORNER (circumscribed cone)
  double farC;     // far clip distance
};
[[nodiscard]] ViewCull viewCull(const ::Camera3D& cam) {
  const double tanV = std::tan(static_cast<double>(cam.fovy) * static_cast<double>(DEG2RAD) * 0.5);
  const double aspect =
      static_cast<double>(GetScreenWidth()) / static_cast<double>(GetScreenHeight());
  const double tanH = tanV * aspect;
  return {std::sqrt(tanH * tanH + tanV * tanV), sceneClip(cam).farC};
}

// Conservative cull: drop a module only when its bounding sphere is wholly behind
// the eye, wholly past the far plane, or wholly outside a cone that CIRCUMSCRIBES
// the view frustum (so a corner-of-screen module is never wrongly removed). This
// replaces the old behind-only cull with a real off-screen reject (the win when
// zoomed into a big station), without the over-cull risk of an inscribed cone.
[[nodiscard]] bool frustumCull(const LodMetrics& m, const ViewCull& v) {
  if (m.along < -m.radius) return true;
  if (m.along - m.radius > v.farC) return true;
  return m.along > 0 && m.lateral > m.along * v.tanDiag + m.radius;
}

// Tunable pixel threshold: a module whose projected radius is under kMeshPx draws
// as a cheap opaque box instead of its full mesh. The selected module always draws
// full detail.
constexpr float kMeshPx = 12.0f;

// Draw every placed module at a cost matched to its on-screen size, in two passes
// so each gets the culling state it needs: solid meshes (backface-culled) first,
// then the opaque LOD/fallback boxes (unculled), then connectors+gizmo for the
// selected module ONLY (drawing them for every near module was the bulk of the
// clutter and draw-call cost on a big station). `selected` also forces its module
// to full detail; pass nullopt for none.
void drawPlacedModules(const Station& station, const ModuleCatalog& catalog,
                       std::optional<InstanceId> selected, MeshCache& meshes, bool showGizmos,
                       bool showMeshes, const ::Camera3D& camera, bool allConnectors) {
  const double projF = projFactor(camera);
  const ViewCull vc = viewCull(camera);

  // Declared before pass 1 (which fills it) so pass 2 can read it from its own
  // profiler zone scope. Most modules collapse to a box; size up front.
  std::vector<std::pair<const ModuleDef*, const PlacedModule*>> boxes;
  boxes.reserve(station.modules().size());

  // Pass 1a: cull + LOD select. Pure CPU (no draw calls) so its Tracy zone isolates
  // the per-module cull/LOD math from the mesh-submission cost measured in pass 1b —
  // resolving whether the big-station bottleneck is the math loop or the draw calls.
  struct DetailDraw {
    const ModuleDef* def;
    const PlacedModule* pm;
    ::Color tint;
  };
  std::vector<DetailDraw> detail;
  detail.reserve(station.modules().size());
  {
    ZoneScopedN("modules: cull+lod");
    for (const auto& pm : station.modules()) {
      const ModuleDef* def = catalog.find(pm.defId);
      if (def == nullptr) continue;
      const bool sel = selected.has_value() && *selected == pm.instanceId;
      const LodMetrics lod = lodMetrics(*def, pm, camera, projF);
      if (!sel && frustumCull(lod, vc)) continue;

      const bool detailed = sel || lod.pixelSize >= static_cast<double>(kMeshPx);
      if (detailed && showMeshes)
        detail.push_back({def, &pm, sel ? YELLOW : LIGHTGRAY});
      else
        boxes.emplace_back(def, &pm);
    }
  }

  // Pass 1b: solid-mesh submission for the detailed set. Backface culling on —
  // correct + cheaper under the flip. A mesh that fails to draw (no cached geometry)
  // falls back to a box.
  {
    ZoneScopedN("modules: mesh");
    rlEnableBackfaceCulling();
    for (const auto& d : detail) {
      if (!drawModuleMeshes(*d.def, d.pm->worldTransform, meshes, d.tint))
        boxes.emplace_back(d.def, d.pm);
    }
    rlDisableBackfaceCulling();
  }

  // Pass 2: opaque boxes (batched, unculled) for distant + mesh-less modules.
  {
    ZoneScopedN("modules: boxes");
    for (const auto& [def, pm] : boxes) {
      const bool sel = selected.has_value() && *selected == pm->instanceId;
      drawModuleBox(*def, pm->worldTransform, sel ? ::Color{180, 160, 40, 255} : kBoxFill,
                    sel ? YELLOW : kBoxEdge);
    }
  }

  // Pass 3: detail markers. Interactive editor shows them for the SELECTED module
  // only (the forest of every-module connectors was the bulk of big-station
  // clutter + cost); the snap-eyeball harness opts into all of them.
  {
    ZoneScopedN("modules: markers");
    for (const auto& pm : station.modules()) {
      const bool sel = selected.has_value() && *selected == pm.instanceId;
      if (!allConnectors && !sel) continue;
      const ModuleDef* def = catalog.find(pm.defId);
      if (def == nullptr) continue;
      drawConnectors(*def, pm);
      if (showGizmos) drawAxisGizmo(*def, pm.worldTransform);
    }
  }
}

}  // namespace

RenderStats lodStats(const Station& station, const ModuleCatalog& catalog,
                     const ::Camera3D& camera) {
  const double projF = projFactor(camera);
  const ViewCull vc = viewCull(camera);
  RenderStats s;
  for (const auto& pm : station.modules()) {
    const ModuleDef* def = catalog.find(pm.defId);
    if (def == nullptr) continue;
    ++s.total;
    const LodMetrics lod = lodMetrics(*def, pm, camera, projF);
    if (frustumCull(lod, vc)) {
      ++s.culled;
      continue;
    }
    if (lod.pixelSize >= static_cast<double>(kMeshPx))
      ++s.drawnDetailed;
    else
      ++s.drawnBox;
  }
  return s;
}

StationBounds stationBounds(const Station& station, const ModuleCatalog& catalog) {
  const auto& mods = station.modules();
  if (mods.empty()) return {AABB{Vec3{0, 0, 0}, Vec3{0, 0, 0}}, Vec3{0, 0, 0}, 0.0};
  AABB box{mods.front().worldTransform.position, mods.front().worldTransform.position};
  for (const auto& pm : mods) {
    const ModuleDef* def = catalog.find(pm.defId);
    if (def == nullptr) continue;
    box = merge(box, worldAabb(def->aabb, pm.worldTransform));
  }
  const Vec3 center = (box.min + box.max) * 0.5;
  return {box, center, length(box.max - box.min) * 0.5};
}

void drawScene(const EditorState& state, const ::Camera3D& camera, MeshCache& meshes,
               bool showGizmos, bool showMeshes) {
  ZoneScoped;
  beginScene(camera);

  drawPlacedModules(state.station(), state.catalog(), state.selected(), meshes, showGizmos,
                    showMeshes, camera, /*allConnectors=*/false);

  if (state.ghost()) {
    const ModuleDef* gdef = state.defFor(state.ghost()->defId);
    if (gdef != nullptr) {
      const ::Color fill =
          state.ghost()->valid ? ::Color{0, 220, 0, 90} : ::Color{220, 0, 0, 90};
      const ::Color edge = state.ghost()->valid ? GREEN : RED;
      rlEnableBackfaceCulling();
      const bool drew =
          showMeshes && drawModuleMeshes(*gdef, state.ghost()->worldTransform, meshes, edge);
      rlDisableBackfaceCulling();
      if (!drew) drawModuleBox(*gdef, state.ghost()->worldTransform, fill, edge);
      if (showGizmos) drawAxisGizmo(*gdef, state.ghost()->worldTransform);
    }
  }

  endScene();
}

void drawScene(const Station& station, const ModuleCatalog& catalog, const ::Camera3D& camera,
               MeshCache& meshes, bool showGizmos, bool showMeshes, bool allConnectors) {
  ZoneScoped;
  beginScene(camera);
  drawPlacedModules(station, catalog, std::nullopt, meshes, showGizmos, showMeshes, camera,
                    allConnectors);
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
