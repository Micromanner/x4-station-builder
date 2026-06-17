#include "render.hpp"

#include "input.hpp"  // gizmoScaleFor
#include "mesh_cache.hpp"
#include "profiling.hpp"
#include "raylib_convert.hpp"
#include "rlgl.h"

#include "x4sb/data/types.hpp"
#include "x4sb/editorcore/display_flip.hpp"
#include "x4sb/editorcore/gizmo.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <unordered_map>
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

// Decide whether `def` can render as solid meshes (else the caller boxes it). True
// only when at least one part is loaded AND no oversized part (>u16 indices, which
// would render as wrapped-index garbage) is the structural hull — a hull-less module
// reads as broken, so it boxes; an oversized detail/anim greeble is merely skipped so
// the hull still draws (most oversized parts are lod0-only greebles the LOD pass can't
// shrink). Shared by the per-module and instanced paths so their boxing policy can't
// drift apart.
[[nodiscard]] bool canRenderAsMeshes(const ModuleDef& def, MeshCache& meshes) {
  bool anyPresent = false;
  for (const MeshRef& ref : def.meshRefs) {
    if (meshes.get(ref.gltfPath) != nullptr)
      anyPresent = true;
    else if (meshes.isOversized(ref.gltfPath) && meshRefIsStructural(ref.gltfPath))
      return false;
  }
  return anyPresent;
}

// Draw `def`'s glTF parts as SOLID flat-shaded meshes under `xf`, each part nested
// in its own localTransform. Returns true if at least one mesh drew; false (boxing
// rule failed, see canRenderAsMeshes) signals the caller to fall back to the AABB
// box. The caller must have backface culling ENABLED: under the global (1,1,-1) flip
// the winding is exactly what default back-face culling expects (confirmed
// empirically), so this both renders correctly and is cheaper than drawing every
// triangle's back side.
bool drawModuleMeshes(const ModuleDef& def, const Transform& xf, MeshCache& meshes, ::Color tint) {
  if (!canRenderAsMeshes(def, meshes)) return false;
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

// `xf` is the pose to draw at — normally pm.worldTransform, but the live drag
// preview pose for the module being dragged, so its markers track the mesh.
void drawConnectors(const ModuleDef& def, const PlacedModule& pm, const Transform& xf) {
  for (const auto& cp : def.connectionPoints) {
    const Vec3 world = apply(xf, cp.localPosition);
    const bool linked = pointIsLinked(pm, cp.id);
    // Orange (free) / gray (occupied) so markers read distinctly from the blue
    // orientation-gizmo axis/tip.
    const ::Color c = linked ? GRAY : ORANGE;
    DrawSphere(toRl(world), 20.0f, c);  // X4 scale: ~0.6 was sub-pixel
    // Draw the connector's local +Z as its facing normal. This MUST stay the
    // same axis the snap solver mates about (snap.cpp `kMate`, spec §3.1); if
    // that convention is corrected against real data, update this axis too, or
    // the markers will mislead the eyeball check of a snap.
    const Vec3 n = rotate(xf.rotation * cp.localRotation, Vec3{0, 0, 1});
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

// Bucket one module's drawable parts for instanced submission. Shares the boxing
// decision with the per-module path via canRenderAsMeshes (false -> nothing bucketed,
// caller boxes). On success each present part's model matrix is appended to its
// per-path bucket, keyed by gltfPath (`<defId>__<part>`), so every placed copy of a
// module shares a bucket — the repetition an X4 station is built from, collapsed into
// one draw call per part.
[[nodiscard]] bool collectModuleInstances(
    const ModuleDef& def, const Transform& xf, MeshCache& meshes,
    std::unordered_map<std::string, std::vector<::Matrix>>& buckets) {
  if (!canRenderAsMeshes(def, meshes)) return false;
  for (const MeshRef& ref : def.meshRefs) {
    if (meshes.get(ref.gltfPath) == nullptr) continue;
    buckets[ref.gltfPath].push_back(toRlMatrix(compose(xf, ref.localTransform)));
  }
  return true;
}

// Draw every placed module at a cost matched to its on-screen size, in two passes
// so each gets the culling state it needs: solid meshes (backface-culled) first,
// then the opaque LOD/fallback boxes (unculled), then connectors+gizmo for the
// selected module ONLY (drawing them for every near module was the bulk of the
// clutter and draw-call cost on a big station). `selected` also forces its module
// to full detail; pass nullopt for none.
void drawPlacedModules(const Station& station, const ModuleCatalog& catalog,
                       std::optional<InstanceId> selected, MeshCache& meshes, bool showGizmos,
                       bool showMeshes, const ::Camera3D& camera, bool allConnectors,
                       std::optional<Transform> selectedPreview) {
  const double projF = projFactor(camera);
  const ViewCull vc = viewCull(camera);

  // While the selected module is being gizmo-dragged, draw it at the live preview
  // pose (so the real mesh moves/rotates with the drag) instead of its committed
  // transform — the AABB-box-only preview gave no real feedback. Cull/LOD in pass 1a
  // still use the committed pose, which is fine: the selected module is never culled
  // and always drawn at full detail.
  const auto xfFor = [&](const PlacedModule& pm, bool sel) -> const Transform& {
    return (sel && selectedPreview) ? *selectedPreview : pm.worldTransform;
  };

  // Declared before pass 1 (which fills it) so pass 2 can read it from its own
  // profiler zone scope. Most modules collapse to a box; size up front.
  std::vector<std::pair<const ModuleDef*, const PlacedModule*>> boxes;
  boxes.reserve(station.modules().size());

  // Pass 1a: cull + LOD select, bucketing detailed parts by mesh for instancing.
  // Pure CPU (no draw calls) so its Tracy zone isolates the per-module cull/LOD math
  // from the mesh-submission cost in pass 1b. The selected module is held out (it
  // wants a yellow tint and there is only one) and drawn the per-module way.
  std::unordered_map<std::string, std::vector<::Matrix>> instances;
  const ModuleDef* selDef = nullptr;
  const PlacedModule* selPm = nullptr;
  {
    ZoneScopedN("modules: cull+lod");
    for (const auto& pm : station.modules()) {
      const ModuleDef* def = catalog.find(pm.defId);
      if (def == nullptr) continue;
      const bool sel = selected.has_value() && *selected == pm.instanceId;
      const LodMetrics lod = lodMetrics(*def, pm, camera, projF);
      if (!sel && frustumCull(lod, vc)) continue;

      const bool detailed = sel || lod.pixelSize >= static_cast<double>(kMeshPx);
      if (!(detailed && showMeshes)) {
        boxes.emplace_back(def, &pm);
      } else if (sel) {
        selDef = def;
        selPm = &pm;
      } else if (!collectModuleInstances(*def, pm.worldTransform, meshes, instances)) {
        boxes.emplace_back(def, &pm);  // oversized hull / no geometry -> box
      }
    }
  }

  // Pass 1b: instanced mesh submission — one DrawMeshInstanced per unique part across
  // all its placed copies. Backface culling on (correct + cheaper under the flip).
  // The selected module draws separately for its tint; a structural-oversized one
  // still boxes.
  {
    ZoneScopedN("modules: mesh");
    rlEnableBackfaceCulling();
    for (const auto& [path, xforms] : instances) meshes.drawInstanced(path, xforms, LIGHTGRAY);
    if (selDef != nullptr && selPm != nullptr &&
        !drawModuleMeshes(*selDef, xfFor(*selPm, true), meshes, YELLOW))
      boxes.emplace_back(selDef, selPm);
    rlDisableBackfaceCulling();
  }

  // Pass 2: opaque boxes (batched, unculled) for distant + mesh-less modules.
  {
    ZoneScopedN("modules: boxes");
    for (const auto& [def, pm] : boxes) {
      const bool sel = selected.has_value() && *selected == pm->instanceId;
      drawModuleBox(*def, xfFor(*pm, sel), sel ? ::Color{180, 160, 40, 255} : kBoxFill,
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
      drawConnectors(*def, pm, xfFor(pm, sel));
      if (showGizmos) drawAxisGizmo(*def, xfFor(pm, sel));
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
  return boundsOf(box);
}

StationBounds boundsOf(const AABB& box) {
  return {box, (box.min + box.max) * 0.5, length(box.max - box.min) * 0.5};
}

void drawScene(const EditorState& state, const ::Camera3D& camera, MeshCache& meshes,
               bool showGizmos, bool showMeshes) {
  ZoneScoped;
  beginScene(camera);

  drawPlacedModules(state.station(), state.catalog(), state.selected(), meshes, showGizmos,
                    showMeshes, camera, /*allConnectors=*/false, state.dragPreview());

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

  // Translate gizmo + drag preview last, inside the scene's global flip (its
  // axes are X4-native, like every other primitive here), before endScene().
  drawTranslateGizmo(state, camera);

  endScene();
}

void drawTranslateGizmo(const EditorState& state, const ::Camera3D& camera) {
  if (!state.selected()) return;
  const PlacedModule* m = state.station().find(*state.selected());
  if (m == nullptr) return;
  // Gizmo follows the live drag preview when dragging, else the module origin.
  const std::optional<Transform> preview = state.dragPreview();
  const Vec3 origin = preview ? preview->position : m->worldTransform.position;
  const double scale = gizmoScaleFor(camera, state);
  const GizmoModel g = gizmoModel(origin, scale);

  const std::optional<GizmoHandle> hl = state.highlightHandle();
  const ::Color kHi{255, 255, 160, 255};  // hovered/active handle lights up
  const ::Vector3 o = toRl(g.origin);

  // Axis arrows: a solid cylinder shaft + a cone tip, so the handle reads in 3D
  // and the click target is obvious (thin lines were the clunk). The hovered or
  // dragged axis highlights.
  const double headLen = 0.28 * g.axisLength;
  const float shaftR = static_cast<float>(0.022 * g.axisLength);
  const float headR = static_cast<float>(0.065 * g.axisLength);
  struct AxisVis {
    GizmoHandle handle;
    ::Color color;
  };
  const std::array<AxisVis, 3> axes{{
      {GizmoHandle::AxisX, RED},
      {GizmoHandle::AxisY, GREEN},
      {GizmoHandle::AxisZ, BLUE},
  }};
  for (const AxisVis& a : axes) {
    const ::Color col = (hl && *hl == a.handle) ? kHi : a.color;
    const Vec3 dir = gizmoAxisDir(a.handle);
    const ::Vector3 neck = toRl(g.origin + dir * (g.axisLength - headLen));
    const ::Vector3 tip = toRl(g.origin + dir * g.axisLength);
    DrawCylinderEx(o, neck, shaftR, shaftR, 10, col);  // shaft
    DrawCylinderEx(neck, tip, headR, 0.0F, 12, col);   // arrowhead
  }

  // Plane handles: a small filled translucent quad over the pickable 0..planeSize
  // region (the recognizable plane-drag affordance), coloured by the axis it is
  // normal to; the hovered one highlights. Only the two OUTER edges are outlined —
  // the inner two would lie ON the X/Y/Z axes and just double the arrows (the
  // user's "redundant axis lines"). Backface culling is disabled at gizmo-draw
  // time, so a single winding per triangle shows from both sides.
  struct PlaneVis {
    GizmoHandle handle;
    Vec3 u;
    Vec3 v;
    ::Color color;
  };
  const std::array<PlaneVis, 3> planes{{
      {GizmoHandle::PlaneXY, {1, 0, 0}, {0, 1, 0}, BLUE},
      {GizmoHandle::PlaneYZ, {0, 1, 0}, {0, 0, 1}, RED},
      {GizmoHandle::PlaneZX, {0, 0, 1}, {1, 0, 0}, GREEN},
  }};
  for (const PlaneVis& p : planes) {
    const bool hot = hl && *hl == p.handle;
    const ::Color edge = hot ? kHi : p.color;
    const ::Color fill{edge.r, edge.g, edge.b, static_cast<unsigned char>(hot ? 130 : 60)};
    const ::Vector3 a = toRl(g.origin + p.u * g.planeSize);
    const ::Vector3 corner = toRl(g.origin + (p.u + p.v) * g.planeSize);
    const ::Vector3 c = toRl(g.origin + p.v * g.planeSize);
    DrawTriangle3D(o, a, corner, fill);
    DrawTriangle3D(o, corner, c, fill);
    DrawLine3D(a, corner, edge);
    DrawLine3D(corner, c, edge);
  }

  // Rotation rings: one clean circle per axis, in the plane normal to that axis,
  // at ringRadius. The rings are large, so a single line reads fine; the hovered/
  // active ring highlights. (DrawCircle3D draws the default XY circle tilted by
  // axis+angle.)
  struct RingVis {
    GizmoHandle handle;
    ::Vector3 tiltAxis;
    float tiltDeg;
    ::Color color;
  };
  const std::array<RingVis, 3> rings{{
      {GizmoHandle::RotX, ::Vector3{0, 1, 0}, 90.0F, RED},    // ring in YZ plane
      {GizmoHandle::RotY, ::Vector3{1, 0, 0}, 90.0F, GREEN},  // ring in XZ plane
      {GizmoHandle::RotZ, ::Vector3{0, 0, 1}, 0.0F, BLUE},    // ring in XY plane
  }};
  for (const RingVis& rv : rings) {
    const ::Color col = (hl && *hl == rv.handle) ? kHi : rv.color;
    DrawCircle3D(o, static_cast<float>(g.ringRadius), rv.tiltAxis, rv.tiltDeg, col);
  }

  // The selected module's real mesh now tracks the drag preview pose live (see
  // drawPlacedModules' selectedPreview), so no AABB-box ghost is drawn here — the
  // mesh itself IS the feedback.
}

void drawScene(const Station& station, const ModuleCatalog& catalog, const ::Camera3D& camera,
               MeshCache& meshes, bool showGizmos, bool showMeshes, bool allConnectors) {
  ZoneScoped;
  beginScene(camera);
  drawPlacedModules(station, catalog, std::nullopt, meshes, showGizmos, showMeshes, camera,
                    allConnectors, std::nullopt);
  endScene();
}

void drawHud(const EditorState& state, int screenWidth, int /*screenHeight*/, bool showGizmos) {
  const ModuleDef* def = state.activeDef();
  char line[256];

  if (state.placementEnabled()) {
    std::snprintf(line, sizeof(line), "BUILD  Active: %s",
                  def != nullptr ? def->id.c_str() : "(none)");
    DrawText(line, 12, 10, 20, RAYWHITE);
  } else {
    DrawText("SELECT  (Q to build)  -  click a placed module to select it", 12, 10, 20, GOLD);
  }

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
  DrawText(
      "LMB=place/select/drag-gizmo   Q=build/select   R/Shift+R/Ctrl+R=rotate   Alt=free   Del/X=delete   Ctrl+Z/Y=undo/redo   RMB=orbit   wheel=zoom   F=frame",
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
