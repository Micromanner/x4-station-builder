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

// One connector marker: a sphere at `world` plus its facing-normal stub. Colour
// encodes free/occupied and near/far (lit). Shared by the per-module path and the
// grid-culled path so both render identically.
void drawOneConnector(Vec3 world, Vec3 normal, bool linked, bool lit) {
  ::Color c;
  if (linked) {
    c = lit ? ::Color{120, 120, 120, 100} : ::Color{120, 120, 120, 50};
  } else {
    c = lit ? ORANGE : ::Color{255, 161, 0, 90};
  }
  DrawSphere(toRl(world), 20.0f, c);  // X4 scale: ~0.6 was sub-pixel
  // The connector's local +Z is its facing normal. This MUST stay the same axis
  // the snap solver mates about (snap.cpp `kMate`, spec §3.1).
  DrawLine3D(toRl(world), toRl(world + normal * 80.0), c);  // X4 scale normal stub
}

// Dashed 3D segment: alternate dash/gap along a->b. The dash count is capped so a
// long span can't explode the draw-call count.
void drawDashedLine3D(Vec3 a, Vec3 b, ::Color color, double dashLen, double gapLen) {
  const Vec3 span = b - a;
  const double total = length(span);
  if (total < 1e-6) return;
  const Vec3 dir = span * (1.0 / total);
  const double stride = dashLen + gapLen;
  constexpr int kMaxDashes = 64;
  double s = 0.0;
  for (int i = 0; i < kMaxDashes && s < total; ++i, s += stride) {
    const double e = std::min(s + dashLen, total);
    DrawLine3D(toRl(a + dir * s), toRl(a + dir * e), color);
  }
}

// The about-to-snap connector pair: a dashed guide-line + endpoint spheres shown
// during the approach (before the instant magnetic snap coincides them). Skipped
// once coincident. Drawn inside the scene flip, like the gizmo/connectors.
void drawSnapLink(const EditorState& state) {
  // One dashed cyan guide-line per possible snap (active connector -> target), each
  // skipped once coincident (already snapped). Distinct cyan vs the orange connectors.
  constexpr ::Color kSnap{80, 230, 255, 255};
  for (const SnapLink& link : state.activeSnapLinks()) {
    if (length(link.toWorld - link.fromWorld) < 1.0) continue;  // coincident: snapped
    drawDashedLine3D(link.fromWorld, link.toWorld, kSnap, 60.0, 40.0);
    DrawSphere(toRl(link.fromWorld), 28.0F, kSnap);
    DrawSphere(toRl(link.toWorld), 28.0F, kSnap);
  }
}

// A connector is bright (lit) if it belongs to the selected module, if there is no
// active ghost/selection context (tests/debug), or if it is within 1000 units of
// the active module's AABB.
bool connectorLit(Vec3 world, std::optional<AABB> activeAabb, bool isSelected) {
  if (isSelected) return true;
  if (!activeAabb) return true;
  const Vec3 q{std::clamp(world.x, activeAabb->min.x, activeAabb->max.x),
               std::clamp(world.y, activeAabb->min.y, activeAabb->max.y),
               std::clamp(world.z, activeAabb->min.z, activeAabb->max.z)};
  return length(world - q) < 1000.0;
}

// `xf` is the pose to draw at — normally pm.worldTransform, but the live drag
// preview pose for the module being dragged so its markers track the mesh.
void drawConnectors(const ModuleDef& def, const PlacedModule& pm, const Transform& xf,
                    std::optional<AABB> activeAabb = std::nullopt,
                    std::optional<InstanceId> selected = std::nullopt) {
  const bool isSelected = selected.has_value() && *selected == pm.instanceId;
  for (const auto& cp : def.connectionPoints) {
    const Vec3 world = apply(xf, cp.localPosition);
    const Vec3 n = rotate(xf.rotation * cp.localRotation, Vec3{0, 0, 1});
    drawOneConnector(world, n, pointIsLinked(pm, cp.id), connectorLit(world, activeAabb, isSelected));
  }
}

// Local-frame gizmo at the module origin so orientation is visible at a glance:
// +X red, +Y green. Scaled to the module, so a wrongly-rotated snap shows up as
// tilted axes rather than an indistinguishable box.
void drawAxisGizmo(const ModuleDef& def, const Transform& xf) {
  const Vec3 ext = def.aabb.max - def.aabb.min;
  const double maxExt = std::max(ext.x, std::max(ext.y, ext.z));
  const double len = 0.6 * maxExt;
  const Vec3 o = xf.position;
  DrawLine3D(toRl(o), toRl(o + rotate(xf.rotation, Vec3{1, 0, 0}) * len), RED);
  DrawLine3D(toRl(o), toRl(o + rotate(xf.rotation, Vec3{0, 1, 0}) * len), GREEN);
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

void drawDottedLine(::Vector3 start, ::Vector3 end, ::Color color) {
  drawDashedLine3D(toVec3(start), toVec3(end), color, 300.0, 300.0);
}

void drawPlotDottedEdges() {
  constexpr float h = 10000.0f; // Half size
  const ::Color edgeColor{34, 187, 255, 60}; // Faint Cherenkov blue for the dotted edges
  
  // 4 edges along X
  drawDottedLine(::Vector3{-h, -h, -h}, ::Vector3{h, -h, -h}, edgeColor);
  drawDottedLine(::Vector3{-h, -h,  h}, ::Vector3{h, -h,  h}, edgeColor);
  drawDottedLine(::Vector3{-h,  h, -h}, ::Vector3{h,  h, -h}, edgeColor);
  drawDottedLine(::Vector3{-h,  h,  h}, ::Vector3{h,  h,  h}, edgeColor);

  // 4 edges along Y
  drawDottedLine(::Vector3{-h, -h, -h}, ::Vector3{-h, h, -h}, edgeColor);
  drawDottedLine(::Vector3{-h, -h,  h}, ::Vector3{-h, h,  h}, edgeColor);
  drawDottedLine(::Vector3{ h, -h, -h}, ::Vector3{ h, h, -h}, edgeColor);
  drawDottedLine(::Vector3{ h, -h,  h}, ::Vector3{ h, h,  h}, edgeColor);

  // 4 edges along Z
  drawDottedLine(::Vector3{-h, -h, -h}, ::Vector3{-h, -h, h}, edgeColor);
  drawDottedLine(::Vector3{-h,  h, -h}, ::Vector3{-h,  h, h}, edgeColor);
  drawDottedLine(::Vector3{ h, -h, -h}, ::Vector3{ h, -h, h}, edgeColor);
  drawDottedLine(::Vector3{ h,  h, -h}, ::Vector3{ h,  h, h}, edgeColor);
}

void beginScene(const ::Camera3D& camera) {
  const ClipRange clip = sceneClip(camera);
  rlSetClipPlanes(clip.nearC, clip.farC);  // must precede BeginMode3D (builds the projection)

  BeginMode3D(camera);
  rlPushMatrix();
  rlScalef(1.0f, 1.0f, -1.0f);  // X4 left-handed -> raylib right-handed (parent §4)
  rlDisableBackfaceCulling();

  // Draw the faint dotted edges of the 20x20x20 km build volume
  drawPlotDottedEdges();
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

// Connectors farther than this from the active module are not drawn at all (at a
// large-station camera distance a 20m marker is sub-pixel anyway). The grid query
// pads by the active module's radius so nothing within the 1000-unit lit band of a
// large module's surface is missed — but capped (kMaxConnectorReach) so a huge
// module's AABB can't blow the query up to hundreds of neighbours and tank the frame
// rate in a dense station (known-issues 1.2, big-mesh case).
constexpr double kConnectorDrawRadius = 1000.0;
constexpr double kMaxConnectorReach = 2500.0;

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
                       std::optional<Transform> selectedPreview,
                       std::optional<AABB> activeAabb = std::nullopt,
                       const ConnectorGrid* grid = nullptr, bool lodEnabled = true) {
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

      const bool detailed = sel || !lodEnabled || lod.pixelSize >= static_cast<double>(kMeshPx);
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

  // Pass 3: connector markers. The selected module always shows its connectors and
  // gizmo. Otherwise, when a ghost or selection is active, show only connectors
  // NEAR the active module — queried from the grid instead of looping every module
  // (the per-frame draw-call explosion behind known-issues 1.2). `allConnectors`
  // keeps the full-station debug view.
  {
    ZoneScopedN("modules: markers");
    if (allConnectors) {
      for (const auto& pm : station.modules()) {
        const ModuleDef* def = catalog.find(pm.defId);
        if (def == nullptr) continue;
        const bool sel = selected.has_value() && *selected == pm.instanceId;
        drawConnectors(*def, pm, xfFor(pm, sel), activeAabb, selected);
        if (showGizmos) drawAxisGizmo(*def, xfFor(pm, sel));
      }
    } else {
      if (selected) {
        const PlacedModule* pm = station.find(*selected);
        const ModuleDef* def = pm ? catalog.find(pm->defId) : nullptr;
        if (def != nullptr) {
          drawConnectors(*def, *pm, xfFor(*pm, true), activeAabb, selected);
        }
      }
      if (activeAabb && grid != nullptr) {
        const Vec3 center = (activeAabb->min + activeAabb->max) * 0.5;
        const double reach = std::min(
            kConnectorDrawRadius + length(activeAabb->max - activeAabb->min) * 0.5, kMaxConnectorReach);
        for (const ConnectorGrid::Entry& e : grid->queryRadius(center, reach)) {
          if (selected.has_value() && e.instanceId == *selected) continue;  // drawn above
          const PlacedModule* pm = station.find(e.instanceId);
          const ModuleDef* def = pm ? catalog.find(pm->defId) : nullptr;
          if (def == nullptr || e.connectorIndex >= def->connectionPoints.size()) continue;
          const ConnectionPoint& cp = def->connectionPoints[e.connectorIndex];
          const Vec3 n = rotate(pm->worldTransform.rotation * cp.localRotation, Vec3{0, 0, 1});
          drawOneConnector(e.world, n, pointIsLinked(*pm, cp.id), connectorLit(e.world, activeAabb, false));
        }
      }
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
               bool showGizmos, bool showMeshes, bool lodEnabled) {
  ZoneScoped;
  // Reset the per-frame mesh-upload budget here, in drawScene, so EVERY render entry
  // point (the editor loop AND the offscreen --profile/--megashot/--snaptest/--gizmoshot
  // harnesses) gets it — none can forget and stall mesh streaming after the first frame.
  meshes.beginFrame();
  beginScene(camera);

  std::optional<AABB> activeAabb;
  if (state.ghost()) {
    const ModuleDef* gdef = state.defFor(state.ghost()->defId);
    if (gdef) {
      activeAabb = worldAabb(gdef->aabb, state.ghost()->worldTransform);
    }
  } else if (state.selected()) {
    const PlacedModule* pm = state.station().find(*state.selected());
    const ModuleDef* sdef = pm ? state.catalog().find(pm->defId) : nullptr;
    if (sdef) {
      Transform xf = state.dragPreview() ? *state.dragPreview() : pm->worldTransform;
      activeAabb = worldAabb(sdef->aabb, xf);
    }
  }

  // Neighbour connector markers are a snapping aid — only meaningful while placing a
  // ghost or dragging. On idle selection, skip them (only the selected module's own
  // connectors draw): a big selected module's AABB would otherwise flood the grid query
  // with hundreds of neighbours and tank the frame rate (known-issues 1.2, big-mesh case).
  const ConnectorGrid* connGrid =
      (state.ghost() || state.dragging()) ? &state.connectorGrid() : nullptr;
  drawPlacedModules(state.station(), state.catalog(), state.selected(), meshes, showGizmos,
                    showMeshes, camera, /*allConnectors=*/false, state.dragPreview(), activeAabb,
                    connGrid, lodEnabled);

  // Draw proximity warning grids on build boundary faces when a module is near them
  AABB worldBox{};
  bool hasRef = false;
  bool isRefValid = true;
  Vec3 centerPos{};

  if (state.ghost()) {
    const ModuleDef* def = state.defFor(state.ghost()->defId);
    if (def) {
      worldBox = worldAabb(def->aabb, state.ghost()->worldTransform);
      centerPos = (worldBox.min + worldBox.max) * 0.5;
      hasRef = true;
      isRefValid = state.ghost()->valid;
    }
  } else if (state.dragging() && state.dragPreview()) {
    if (state.selected()) {
      const PlacedModule* pm = state.station().find(*state.selected());
      const ModuleDef* def = pm ? state.catalog().find(pm->defId) : nullptr;
      if (def) {
        worldBox = worldAabb(def->aabb, *state.dragPreview());
        centerPos = (worldBox.min + worldBox.max) * 0.5;
        hasRef = true;
        constexpr double kPlotLimit = 10000.0;
        isRefValid = worldBox.min.x >= -kPlotLimit && worldBox.max.x <= kPlotLimit &&
                     worldBox.min.y >= -kPlotLimit && worldBox.max.y <= kPlotLimit &&
                     worldBox.min.z >= -kPlotLimit && worldBox.max.z <= kPlotLimit;
      }
    }
  }

  if (hasRef && isRefValid) {
    auto drawFaceGrid = [](int axis, double value, double uVal, double vVal, ::Color gridColor) {
      constexpr double gridHalfSize = 1500.0;
      constexpr int steps = 6;
      const double stepSize = (gridHalfSize * 2.0) / steps;

      auto drawClippedLine = [](Vec3 p1, Vec3 p2, ::Color color) {
        constexpr double L = 10000.0;
        // Check if completely outside
        if ((p1.x > L && p2.x > L) || (p1.x < -L && p2.x < -L)) return;
        if ((p1.y > L && p2.y > L) || (p1.y < -L && p2.y < -L)) return;
        if ((p1.z > L && p2.z > L) || (p1.z < -L && p2.z < -L)) return;

        // Clamp endpoints to the plot boundary box
        p1.x = std::clamp(p1.x, -L, L);
        p1.y = std::clamp(p1.y, -L, L);
        p1.z = std::clamp(p1.z, -L, L);

        p2.x = std::clamp(p2.x, -L, L);
        p2.y = std::clamp(p2.y, -L, L);
        p2.z = std::clamp(p2.z, -L, L);

        DrawLine3D(toRl(p1), toRl(p2), color);
      };

      for (int i = 0; i <= steps; ++i) {
        double offset = -gridHalfSize + i * stepSize;
        Vec3 p1{}, p2{}, q1{}, q2{};

        if (axis == 0) { // X plane
          p1 = {value, uVal + offset, vVal - gridHalfSize};
          p2 = {value, uVal + offset, vVal + gridHalfSize};
          q1 = {value, uVal - gridHalfSize, vVal + offset};
          q2 = {value, uVal + gridHalfSize, vVal + offset};
        } else if (axis == 1) { // Y plane
          p1 = {uVal + offset, value, vVal - gridHalfSize};
          p2 = {uVal + offset, value, vVal + gridHalfSize};
          q1 = {uVal - gridHalfSize, value, vVal + offset};
          q2 = {uVal + gridHalfSize, value, vVal + offset};
        } else { // Z plane
          p1 = {uVal + offset, vVal - gridHalfSize, value};
          p2 = {uVal + offset, vVal + gridHalfSize, value};
          q1 = {uVal - gridHalfSize, vVal + offset, value};
          q2 = {uVal + gridHalfSize, vVal + offset, value};
        }

        drawClippedLine(p1, p2, gridColor);
        drawClippedLine(q1, q2, gridColor);
      }
    };

    constexpr double kPlotLimit = 10000.0;
    constexpr double kThresh = 250.0;
    const ::Color normalColor{34, 187, 255, 180}; // Cherenkov Blue
    const ::Color gridColor = normalColor;

    // Check proximity to X planes (+/- 10000) using AABB bounds
    if (std::abs(worldBox.max.x - kPlotLimit) < kThresh) {
      drawFaceGrid(0, kPlotLimit, centerPos.y, centerPos.z, gridColor);
    }
    if (std::abs(worldBox.min.x + kPlotLimit) < kThresh) {
      drawFaceGrid(0, -kPlotLimit, centerPos.y, centerPos.z, gridColor);
    }

    // Check proximity to Y planes (+/- 10000) using AABB bounds
    if (std::abs(worldBox.max.y - kPlotLimit) < kThresh) {
      drawFaceGrid(1, kPlotLimit, centerPos.x, centerPos.z, gridColor);
    }
    if (std::abs(worldBox.min.y + kPlotLimit) < kThresh) {
      drawFaceGrid(1, -kPlotLimit, centerPos.x, centerPos.z, gridColor);
    }

    // Check proximity to Z planes (+/- 10000) using AABB bounds
    if (std::abs(worldBox.max.z - kPlotLimit) < kThresh) {
      drawFaceGrid(2, kPlotLimit, centerPos.x, centerPos.y, gridColor);
    }
    if (std::abs(worldBox.min.z + kPlotLimit) < kThresh) {
      drawFaceGrid(2, -kPlotLimit, centerPos.x, centerPos.y, gridColor);
    }
  }

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
    }
  }

  // Gizmo + drag preview last, inside the scene's global flip (its axes are
  // X4-native, like every other primitive here), before endScene().
  drawGizmo(state, camera);

  drawSnapLink(state);

  endScene();
}

void drawGizmo(const EditorState& state, const ::Camera3D& camera) {
  if (!state.selected()) return;
  const PlacedModule* m = state.station().find(*state.selected());
  if (m == nullptr) return;
  const std::optional<Transform> preview = state.dragPreview();
  const Vec3 origin = preview ? preview->position : m->worldTransform.position;
  const double scale = gizmoScaleFor(camera, state);
  const GizmoModel g = gizmoModel(origin, scale);
  const GizmoMode mode = state.gizmoMode();

  const std::optional<GizmoHandle> hl = state.highlightHandle();
  const ::Color kHi{255, 255, 160, 255};  // hovered/active handle lights up
  const ::Vector3 o = toRl(g.origin);

  // X-ray: handles always draw over module geometry so they are never buried in a
  // large mesh. Flush the batch before/after toggling so only gizmo primitives skip
  // the depth test; depth WRITE is untouched.
  rlDrawRenderBatchActive();
  rlDisableDepthTest();

  if (mode == GizmoMode::Translate) {
    // Axis arrows: solid cylinder shaft + cone tip; hovered/dragged axis highlights.
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

    // Plane handles: filled translucent quad over the pickable 0..planeSize region,
    // only the two OUTER edges outlined (inner two would double the arrows).
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

    // Center free-translate handle: semi-transparent sphere so it no longer blocks
    // the part origin (was a solid white ball).
    const ::Color centerCol = (hl && *hl == GizmoHandle::Center) ? kHi : ::Color{255, 255, 255, 120};
    DrawSphere(o, static_cast<float>(g.centerPickRadius), centerCol);
  } else {
    // Rotation rings: one clean circle per axis, in the plane normal to that axis.
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
  }

  rlDrawRenderBatchActive();
  rlEnableDepthTest();

  // The selected module's real mesh tracks the drag preview pose live (see
  // drawPlacedModules' selectedPreview), so no AABB-box ghost is drawn here.
}

void drawScene(const Station& station, const ModuleCatalog& catalog, const ::Camera3D& camera,
               MeshCache& meshes, bool showGizmos, bool showMeshes, bool allConnectors,
               bool lodEnabled) {
  ZoneScoped;
  meshes.beginFrame();  // per-frame upload budget reset — see the EditorState drawScene overload
  beginScene(camera);
  drawPlacedModules(station, catalog, std::nullopt, meshes, showGizmos, showMeshes, camera,
                    allConnectors, std::nullopt, std::nullopt, nullptr, lodEnabled);
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
      "LMB=place/select/drag-gizmo   Q=build/select   T/Y=move/rotate gizmo   R/Shift+R/Ctrl+R=rotate   Alt=free   Del/X=delete   Ctrl+Z/Y=undo/redo   RMB=orbit   wheel=zoom   F=frame",
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
