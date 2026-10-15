#include "render.hpp"

#include "input.hpp"  // gizmoScaleFor
#include "mesh_cache.hpp"
#include "profiling.hpp"
#include "raylib_convert.hpp"
#include "rlgl.h"
#include "ui_category.hpp"  // categoryLabel (shared with the Clay chrome)
#include "x4sb/data/catalog.hpp"
#include "x4sb/data/types.hpp"
#include "x4sb/editorcore/display_flip.hpp"
#include "x4sb/editorcore/gizmo.hpp"
#include "x4sb/snap/snap.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace x4sb::editor {
namespace {

std::function<void(ScenePhase)>& scenePhaseHook() {
  static std::function<void(ScenePhase)> hook;
  return hook;
}

std::function<double()>& lodClock() {
  static std::function<double()> clock;
  return clock;
}

[[nodiscard]] double lodNow() { return lodClock() ? lodClock()() : GetTime(); }

struct LodTrace {
  std::optional<InstanceId> module;
  std::string text;
  int frame{0};
};
LodTrace& lodTrace() {
  static LodTrace trace;
  return trace;
}

[[nodiscard]] bool tracing(InstanceId id) {
  const LodTrace& t = lodTrace();
  return t.module && *t.module == id;
}

[[gnu::format(printf, 1, 2)]] void traceLine(const char* format, ...) {
  std::array<char, 512> line{};
  va_list args;  // NOLINT(cppcoreguidelines-pro-type-vararg)
  va_start(args, format);
  std::vsnprintf(line.data(), line.size(), format, args);
  va_end(args);
  lodTrace().text += line.data();
}

void markPhase(ScenePhase phase) {
  if (const auto& hook = scenePhaseHook()) hook(phase);
}

bool pointIsLinked(const PlacedModule& m, const std::string& pointId) {
  for (const auto& l : m.links)
    if (l.thisPointId == pointId) return true;
  return false;
}

// Opaque so a distant module collapsed to its box still reads against the dark
// background (a translucent fill vanishes). Edge brighter than fill so adjacent
// boxes stay distinguishable.
constexpr ::Color kBoxFill{86, 112, 150, 255};
constexpr ::Color kBoxEdge{150, 188, 230, 255};

// Clearance-corridor tints: red = a body intrudes (blocked), green = clear. Shared by
// the placed-corridor pass and the ghost's own-corridor draw so they stay in sync.
constexpr ::Color kClearanceRed{230, 90, 90, 255};
constexpr ::Color kClearanceGreen{120, 230, 140, 255};

// Draw a box for `def`'s local AABB under `xf`, nested in the current matrix.
void drawModuleBox(const ModuleDef& def, const Transform& xf, ::Color fill, ::Color edge) {
  const Vec3 center = (def.aabb.min + def.aabb.max) * 0.5;
  const Vec3 size = def.aabb.max - def.aabb.min;
  rlPushMatrix();
  rlMultMatrixf(MatrixToFloatV(toRlMatrix(xf)).v);
  DrawCubeV(toRl(flipZ(center)), toRl(size), fill);
  DrawCubeWiresV(toRl(flipZ(center)), toRl(size), edge);
  rlPopMatrix();
}

// Clip segment [a,b] to the +/-L plot cube (parametric slab clip), writing the
// clipped endpoints back. Returns false if the segment lies entirely outside.
[[nodiscard]] bool clipSegmentToPlot(Vec3& a, Vec3& b, double L) {
  const Vec3 d = b - a;
  const double o[3] = {a.x, a.y, a.z};
  const double dd[3] = {d.x, d.y, d.z};
  double t0 = 0.0;
  double t1 = 1.0;
  for (int i = 0; i < 3; ++i) {
    if (std::abs(dd[i]) < 1e-9) {
      if (o[i] < -L || o[i] > L) return false;  // parallel to slab and outside it
      continue;
    }
    double tn = (-L - o[i]) / dd[i];
    double tf = (L - o[i]) / dd[i];
    if (tn > tf) std::swap(tn, tf);
    t0 = std::max(t0, tn);
    t1 = std::min(t1, tf);
    if (t0 > t1) return false;
  }
  const Vec3 na = a + d * t0;
  const Vec3 nb = a + d * t1;
  a = na;
  b = nb;
  return true;
}

// Draw a clearance OBB (local-space ClearanceVolume placed by a module transform)
// as a translucent box + wireframe. Render-only; `color` carries its own alpha.
// The true keep-clear box runs up to 100 km out (the capital-ship flight path out of
// the plot) — drawing that literally swamps the scene and overruns the far plane.
// Modules can't exist outside the +/-10km plot, so the DRAWN box is clipped to the
// plot along its corridor axis (collision still uses the full untruncated volume).
void drawClearanceVolume(const ClearanceVolume& cv, const Transform& moduleXf, ::Color color) {
  const Quat worldRot = moduleXf.rotation * cv.rotation;
  const Vec3 worldCenter = apply(moduleXf, cv.center);
  const Vec3 axis = rotate(worldRot, Vec3{0, 0, 1});  // corridor (long) axis in world
  Vec3 nearEnd = worldCenter - axis * cv.halfExtents.z;
  Vec3 farEnd = worldCenter + axis * cv.halfExtents.z;
  constexpr double kPlotLimit = 10000.0;
  if (!clipSegmentToPlot(nearEnd, farEnd, kPlotLimit)) return;

  const Transform world{(nearEnd + farEnd) * 0.5, worldRot};
  const Vec3 size{cv.halfExtents.x * 2.0, cv.halfExtents.y * 2.0, length(farEnd - nearEnd)};
  rlPushMatrix();
  rlMultMatrixf(MatrixToFloatV(toRlMatrix(world)).v);
  DrawCubeV(toRl(flipZ(Vec3{0, 0, 0})), toRl(size), ::Color{color.r, color.g, color.b, 40});
  DrawCubeWiresV(toRl(flipZ(Vec3{0, 0, 0})), toRl(size), color);
  rlPopMatrix();
}

// Decide whether `def` can render as solid meshes (else the caller boxes it): true
// once at least one part is loaded.
[[nodiscard]] bool canRenderAsMeshes(const ModuleDef& def, MeshCache& meshes) {
  bool anyPresent = false;
  // No early exit: partFiles() also queues every part for streaming.
  for (const MeshRef& ref : def.meshRefs)
    if (meshes.partFiles(ref).lod[0] != nullptr) anyPresent = true;
  return anyPresent;
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
  // Low-res sphere: a connector marker is a small on-screen dot, but DrawSphere's
  // default 16x16 mesh is regenerated on the CPU per call — at ~80-280 markers/frame
  // on a dense station that was ~3ms. 6x6 is visually identical here, ~7x cheaper.
  DrawSphereEx(toRl(flipZ(world)), 20.0f, 6, 6, c);
  // The connector's local +Z is its facing normal. This MUST stay the same axis
  // the snap solver mates about (snap.cpp `kMate`, spec §3.1).
  DrawLine3D(toRl(flipZ(world)), toRl(flipZ(world + normal * 80.0)), c);  // X4 scale normal stub
}

// Dashed 3D segment: alternate dash/gap along a->b. The dash count is capped so a
// long span can't explode the draw-call count.
void drawDashedLine3D(Vec3 a, Vec3 b, ::Color color, double dashLen, double gapLen) {
  const Vec3 span = b - a;
  const double total = length(span);
  if (total < 1e-6) return;
  const Vec3 dir = span * (1.0 / total);
  const double stride = dashLen + gapLen;
  constexpr int kMaxDashes = 24;
  double s = 0.0;
  for (int i = 0; i < kMaxDashes && s < total; ++i, s += stride) {
    const double e = std::min(s + dashLen, total);
    DrawLine3D(toRl(flipZ(a + dir * s)), toRl(flipZ(a + dir * e)), color);
  }
}

// The about-to-snap connector pair: a dashed guide-line + endpoint spheres shown
// during the approach (before the instant magnetic snap coincides them). Skipped
// once coincident. Drawn inside the scene flip, like the gizmo/connectors.
void drawSnapLink(const EditorState& state) {
  constexpr ::Color kSnap{80, 230, 255, 255};
  for (const SnapLink& link : state.activeSnapLinks()) {
    if (length(link.toWorld - link.fromWorld) < 1.0) continue;  // coincident: snapped
    drawDashedLine3D(link.fromWorld, link.toWorld, kSnap, 60.0, 40.0);
    DrawSphereEx(toRl(flipZ(link.fromWorld)), 28.0F, 6, 6, kSnap);  // low-res: see drawOneConnector
    DrawSphereEx(toRl(flipZ(link.toWorld)), 28.0F, 6, 6, kSnap);
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
    drawOneConnector(world, n, pointIsLinked(pm, cp.id),
                     connectorLit(world, activeAabb, isSelected));
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
  DrawLine3D(toRl(flipZ(o)), toRl(flipZ(o + rotate(xf.rotation, Vec3{1, 0, 0}) * len)), RED);
  DrawLine3D(toRl(flipZ(o)), toRl(flipZ(o + rotate(xf.rotation, Vec3{0, 1, 0}) * len)), GREEN);
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
  // margin (so nothing clips when zoomed in).
  return {std::clamp(d * 0.02, 1.5, 30.0), std::max(d * 4.0, 60000.0)};
}

void drawDottedLine(::Vector3 start, ::Vector3 end, ::Color color) {
  drawDashedLine3D(toVec3(start), toVec3(end), color, 300.0, 300.0);
}

void drawPlotDottedEdges() {
  constexpr float h = 10000.0f;               // Half size
  const ::Color edgeColor{34, 187, 255, 60};  // Faint Cherenkov blue

  // 4 edges along X
  drawDottedLine(::Vector3{-h, -h, -h}, ::Vector3{h, -h, -h}, edgeColor);
  drawDottedLine(::Vector3{-h, -h, h}, ::Vector3{h, -h, h}, edgeColor);
  drawDottedLine(::Vector3{-h, h, -h}, ::Vector3{h, h, -h}, edgeColor);
  drawDottedLine(::Vector3{-h, h, h}, ::Vector3{h, h, h}, edgeColor);

  // 4 edges along Y
  drawDottedLine(::Vector3{-h, -h, -h}, ::Vector3{-h, h, -h}, edgeColor);
  drawDottedLine(::Vector3{-h, -h, h}, ::Vector3{-h, h, h}, edgeColor);
  drawDottedLine(::Vector3{h, -h, -h}, ::Vector3{h, h, -h}, edgeColor);
  drawDottedLine(::Vector3{h, -h, h}, ::Vector3{h, h, h}, edgeColor);

  // 4 edges along Z
  drawDottedLine(::Vector3{-h, -h, -h}, ::Vector3{-h, -h, h}, edgeColor);
  drawDottedLine(::Vector3{-h, h, -h}, ::Vector3{-h, h, h}, edgeColor);
  drawDottedLine(::Vector3{h, -h, -h}, ::Vector3{h, -h, h}, edgeColor);
  drawDottedLine(::Vector3{h, h, -h}, ::Vector3{h, h, h}, edgeColor);
}

void beginScene(const ::Camera3D& camera) {
  const ClipRange clip = sceneClip(camera);
  rlSetClipPlanes(clip.nearC, clip.farC);  // must precede BeginMode3D (builds the projection)

  BeginMode3D(camera);
  rlPushMatrix();

  // Draw the faint dotted edges of the 20x20x20 km build volume
  drawPlotDottedEdges();
}

void endScene() {
  rlPopMatrix();
  EndMode3D();
}

// Per-module level-of-detail + culling metrics, all in raylib DISPLAY space. The
// camera lives in display space (the scene applies rlScalef(1,1,-1)), so the
// module's X4-space center has its Z negated to match before any camera-relative
// math.
struct LodMetrics {
  double along;      // depth along the view direction (negative = behind eye)
  double lateral;    // distance of the center from the view axis
  double radius;     // bounding-sphere radius
  double pixelSize;  // projected radius in screen pixels
  // pixelSize x this = the size X4's module lod rule sees (kX4ModuleSizeContrib).
  double ruleScale;
};

// screenH / (2*tan(fovy/2)): maps a world radius at unit depth to projected
// pixels. Identical for every module, so it's computed once per frame and threaded
// into lodMetrics, keeping the per-module path free of trig + global-state queries.
[[nodiscard]] double projFactor(const ::Camera3D& cam) {
  return static_cast<double>(GetScreenHeight()) /
         (2.0 * std::tan(static_cast<double>(cam.fovy) * static_cast<double>(DEG2RAD) * 0.5));
}

// X4's station-module lod rule (renderparam_library.xml dbglodrule_1) has
// sizecontrib="50": a module is sized half by its own radius, half by its station's,
// so a module of a big station keeps detail much farther out. Fitted in-game: an Argon
// L habitat steps to lod1 at ~7.5 km on a ~35-module station, ~15.4 km on the
// 584-module one, where its own radius alone gives ~2.7 km.
constexpr double kX4ModuleSizeContrib = 0.5;

// `projF` is projFactor(cam), precomputed once per frame by the caller.
[[nodiscard]] LodMetrics lodMetrics(const ModuleDef& def, const PlacedModule& pm,
                                    const ::Camera3D& cam, double projF, double stationRadius) {
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
  // A module whose centre is behind the eye can still fill the view (frustumCull drops
  // the wholly-behind ones): it must read as huge, not as sub-pixel, or every part
  // falls under kPartCullPx and the module vanishes as the camera closes in.
  const double pixelSize = (r / std::max(along, 1.0)) * projF;
  const double ruleScale =
      r > 0.0 ? (kX4ModuleSizeContrib * r + (1.0 - kX4ModuleSizeContrib) * stationRadius) / r : 1.0;
  return {along, lateral, r, pixelSize, ruleScale};
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
// the view frustum (so a corner-of-screen module is never wrongly removed).
[[nodiscard]] bool frustumCull(const LodMetrics& m, const ViewCull& v) {
  if (m.along < -m.radius) return true;
  if (m.along - m.radius > v.farC) return true;
  return m.along > 0 && m.lateral > m.along * v.tanDiag + m.radius;
}

// The view frustum's side and near planes, display space, for shadow-caster culling.
struct ViewFrustum {
  Vec3 eye;
  Vec3 fwd;
  Vec3 right;
  Vec3 up;
  double tanH;
  double tanV;
};
[[nodiscard]] ViewFrustum viewFrustum(const ::Camera3D& cam) {
  const ::Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
  const ::Vector3 right = Vector3Normalize(Vector3CrossProduct(fwd, cam.up));
  ViewFrustum f{};
  f.eye = toVec3(cam.position);
  f.fwd = toVec3(fwd);
  f.right = toVec3(right);
  f.up = cross(f.right, f.fwd);
  f.tanV = std::tan(static_cast<double>(cam.fovy) * static_cast<double>(DEG2RAD) * 0.5);
  f.tanH = f.tanV * static_cast<double>(GetScreenWidth()) / static_cast<double>(GetScreenHeight());
  return f;
}

// A caster only matters if its shadow can land on something in view. Its bounding
// sphere swept away from the sun to the cascade's far face is a capsule; the capsule
// misses the frustum if one plane has both segment ends more than `radius` outside
// (exact per plane, since the signed distance is linear along the segment).
[[nodiscard]] bool shadowMissesView(Vec3 center, double radius, const ShadowRegion& region,
                                    const ViewFrustum& f) {
  const double sweep = std::max(dot(center - region.center, region.toSun) + region.halfExtent, 0.0);
  const std::array<Vec3, 2> ends{center - f.eye, center - region.toSun * sweep - f.eye};
  const auto outside = [&](auto signedDistance) {
    return signedDistance(ends[0]) < -radius && signedDistance(ends[1]) < -radius;
  };
  const double normH = std::sqrt(1.0 + f.tanH * f.tanH);
  const double normV = std::sqrt(1.0 + f.tanV * f.tanV);
  return outside([&](Vec3 p) { return dot(p, f.fwd); }) ||
         outside([&](Vec3 p) { return (dot(p, f.fwd) * f.tanH - dot(p, f.right)) / normH; }) ||
         outside([&](Vec3 p) { return (dot(p, f.fwd) * f.tanH + dot(p, f.right)) / normH; }) ||
         outside([&](Vec3 p) { return (dot(p, f.fwd) * f.tanV - dot(p, f.up)) / normV; }) ||
         outside([&](Vec3 p) { return (dot(p, f.fwd) * f.tanV + dot(p, f.up)) / normV; });
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

// A level switch dissolves over time, as X4's lod rules do (renderparam_library.xml
// dbglodrule_1 speed="2.0"): both levels on complementary screen-door masks, then the
// new one alone. A band of distance instead left a camera parked inside it showing
// both levels for good, the coarse triangles cutting through the fine ones.
constexpr double kLodFadeSeconds = 0.5;
// A switch needs the projected size this far past the cut, so a camera resting on a
// cut doesn't flip levels.
constexpr double kLodHysteresis = 0.05;
// A part unseen this long (culled, off screen) takes its level without a fade.
constexpr double kLodStaleSeconds = 0.25;
constexpr int kFadeSteps = 16;  // Bayer 4x4 resolution; also bounds the bucket count
// Texture scroll stops below this projected MODULE radius (window px): blinking
// lights and scanline waves minified to a few pixels only read as flicker. Cut per
// module, not per part, so all of a module's animations stop together (part boxes
// range from a sign's few metres to position lights spanning the whole module).
constexpr double kAnimStillPx = 24.0;
// Parts below this projected radius (window px) are skipped outright, as X4 drops
// sub-pixel parts: a shipyard's hundreds of docking-bay parts per module otherwise
// cost full vertex work for nothing visible.
constexpr double kPartCullPx = 1.5;

// X4 hides detail-class parts (connection tag detail_xs..detail_xl, see DetailClass)
// by its lod rules (libraries/renderparam_library.xml, dbglodrule_detail_*): shown
// down to a projected radius of lodradius3, faded out by 2/3 of it, in pixels "for
// a 1280:720 mainscene". A detail part's box spans its whole module, so a looser cut
// kept tens of millions of sub-pixel triangles a frame (known-issues 1.5).
constexpr double kX4LodScreenHeight = 720.0;
// X4's lod rule for station modules (dbglodrule_1: production, storage, dockarea,
// pier, ...): the module steps to lod1/2/3 below these projected radii, 720p pixels.
// Without it the per-part cuts kept ~7 triangles per pixel, which MSAA then shades
// per covered triangle (fragment invocations 2-2.7x at mid range).
constexpr std::array<double, 3> kX4ModuleLodRadius{200.0, 100.0, 35.0};
[[nodiscard]] std::optional<double> detailLodRadius3(DetailClass detail) {
  switch (detail) {
    case DetailClass::XS:
      return 400.0;
    case DetailClass::S:
      return 300.0;
    case DetailClass::M:
      return 200.0;
    case DetailClass::L:
      return 50.0;
    case DetailClass::XL:
      return 20.0;
    case DetailClass::None:
      break;
  }
  return std::nullopt;
}

// One part's draw for this frame: the level to show, plus (mid-dissolve) the
// outgoing level on the complementary mask. `fade` is the incoming coverage.
struct PartLod {
  const std::string* in{nullptr};
  const std::string* out{nullptr};
  float fade{1.0F};
  bool still{false};  // draw the texture scroll at its rest frame
};

// One station part's level over time: the level shown, the one it dissolves from.
struct PartLodState {
  int level{-1};  // -1 = never picked
  int from{-1};   // -1 = settled
  double start{0.0};
  double seen{0.0};
};
// Indexed by station instance; instance ids move when the station changes.
struct PartLodStates {
  std::uint64_t revision{~std::uint64_t{0}};
  std::vector<PartLodState> parts;
};
PartLodStates& partLodStates() {
  static PartLodStates states;
  return states;
}

// Which level(s) to draw `ref` at for a module of on-screen size `lod`. `lod`
// nullptr = full detail (the station-centred far shadow cascade, cached across views).
// `state` carries the part's level across frames (dissolves, hysteresis); nullptr =
// the level for this frame alone.
[[nodiscard]] PartLod partLod(const MeshRef& ref, const LodMetrics* lod, MeshCache& meshes,
                              PartLodState* state) {
  PartLod r;
  if (lod == nullptr) {
    r.in = meshes.partFiles(ref).lod[0];
    return r;
  }
  const double px = ref.radius > 0.0 && lod->radius > 0.0
                        ? lod->pixelSize * ref.radius / lod->radius
                        : lod->pixelSize;
  if (px < kPartCullPx) return r;
  r.still = lod->pixelSize < kAnimStillPx;
  if (const std::optional<double> full = detailLodRadius3(ref.detail)) {
    const double p720 = px * kX4LodScreenHeight / static_cast<double>(GetScreenHeight());
    const double gone = *full * 2.0 / 3.0;
    if (p720 < gone) return r;
    r.fade = static_cast<float>(std::clamp((p720 - gone) / (*full - gone), 0.0, 1.0));
  }
  // Every part takes its module's level, as in X4: coarser levels are probed by the
  // meshKeyLod convention and a missing one falls back toward lod0.
  const auto levelAt = [&](double scale) {
    const double p720 =
        lod->pixelSize * lod->ruleScale * scale * kX4LodScreenHeight / GetScreenHeight();
    int level = 0;
    while (level < 3 && p720 < kX4ModuleLodRadius[static_cast<std::size_t>(level)]) ++level;
    return level;
  };
  int level = levelAt(1.0);
  const MeshCache::PartFiles& files = meshes.partFiles(ref);
  if (state != nullptr) {
    const double now = lodNow();
    if (state->level < 0 || now - state->seen > kLodStaleSeconds) {
      state->level = level;
      state->from = -1;
    } else if (level != state->level) {
      // Larger sizes pick finer (lower) levels: keep the shown one while it is still a
      // pick within the hysteresis either way.
      const bool keep = state->level >= levelAt(1.0 + kLodHysteresis) &&
                        state->level <= levelAt(1.0 - kLodHysteresis);
      if (keep) {
        level = state->level;
      } else {
        state->from = state->level;
        state->level = level;
        state->start = now;
      }
    }
    state->seen = now;
    // A detail part fading by distance already uses the screen-door: it switches cold.
    if (state->from >= 0 && r.fade >= 1.0F) {
      const double t = (now - state->start) / kLodFadeSeconds;
      if (t >= 1.0) {
        state->from = -1;
      } else {
        r.out = files.lod[static_cast<std::size_t>(state->from)];
        r.fade = static_cast<float>(t);
      }
    }
  }
  r.in = files.lod[static_cast<std::size_t>(level)];
  if (r.out != nullptr && r.out == r.in) {  // same file at both levels: nothing to blend
    r.out = nullptr;
    r.fade = 1.0F;
  }
  return r;
}

// Instances bucketed by (part file, fade step): quantizing the fade keeps the
// bucket count bounded while a whole station of one part dissolves at once.
// Keyed by the cache's own key pointer, so bucketing hashes no strings.
struct BucketKey {
  const std::string* path{nullptr};
  int step{0};
  bool invert{false};
  bool still{false};
  bool operator==(const BucketKey&) const = default;
};
struct BucketKeyHash {
  std::size_t operator()(const BucketKey& k) const {
    const std::size_t bits = (static_cast<std::size_t>(k.step) << 2) |
                             (static_cast<std::size_t>(k.invert) << 1) |
                             static_cast<std::size_t>(k.still);
    return std::hash<const void*>{}(k.path) ^ (bits * 0x9E3779B97F4A7C15ULL);
  }
};
struct InstanceBucket {
  std::vector<std::uint32_t> instances;  // into MeshCache's instance buffer
  float fade{1.0F};
  std::size_t first{0};    // offset in the uploaded batch (uploadBuckets)
  std::uint32_t index{0};  // its place in the batch's cull buckets
};
using InstanceBuckets = std::unordered_map<BucketKey, InstanceBucket, BucketKeyHash>;

void addInstance(InstanceBuckets& buckets, const std::string& path, float fade, bool invert,
                 bool still, std::uint32_t instance) {
  const int step = std::max(1, static_cast<int>(std::lround(fade * kFadeSteps)));
  // The outgoing level's mask is empty at a full step, but a fade of 1 skips the mask in
  // the shaders: drawn, it would cover the incoming level solid for the dissolve's last frames.
  if (invert && step == kFadeSteps) return;
  InstanceBucket& b = buckets[BucketKey{&path, step, invert, still}];
  b.fade = static_cast<float>(step) / kFadeSteps;
  b.instances.push_back(instance);
}

// One GPU upload for every bucket's visible list; drawBuckets then draws ranges of
// it. Every pass drawing from these buckets must follow this upload: the next one
// replaces it. `occlusion`: the main view's batch, culled against its own depth.
void uploadBuckets(MeshCache& meshes, std::initializer_list<InstanceBuckets*> sets,
                   bool occlusion = false) {
  std::vector<std::uint32_t> all;
  std::vector<MeshCache::VisibleBucket> cull;
  for (InstanceBuckets* buckets : sets) {
    for (auto& [key, b] : *buckets) {
      b.first = all.size();
      b.index = static_cast<std::uint32_t>(cull.size());
      if (occlusion)
        cull.push_back({key.path, b.first, static_cast<std::uint32_t>(b.instances.size())});
      all.insert(all.end(), b.instances.begin(), b.instances.end());
    }
  }
  meshes.uploadVisible(all, cull);
}

void drawBuckets(MeshCache& meshes, const InstanceBuckets& buckets, ::Color tint, DrawPass pass) {
  std::vector<MeshCache::InstancedDraw> draws;
  draws.reserve(buckets.size());
  for (const auto& [key, b] : buckets)
    draws.push_back({key.path, b.first, static_cast<std::uint32_t>(b.instances.size()), b.fade,
                     key.invert, key.still, b.index});
  meshes.drawInstanced(draws, tint, pass);
}

// The world matrix of each of `def`'s parts placed at `xf`, in meshRefs order.
[[nodiscard]] std::vector<::Matrix> partTransforms(const ModuleDef& def, const Transform& xf) {
  std::vector<::Matrix> xforms;
  xforms.reserve(def.meshRefs.size());
  for (const MeshRef& ref : def.meshRefs)
    xforms.push_back(toRlMatrix(compose(xf, ref.localTransform)));
  return xforms;
}

// MeshCache's station instances follow `station`: one per (module, mesh ref) in
// station order, rebuilt only when the station changed. Every module loop that
// indexes them advances its first instance by def->meshRefs.size().
void syncStationInstances(const Station& station, const ModuleCatalog& catalog, MeshCache& meshes) {
  if (PartLodStates& lods = partLodStates(); lods.revision != station.revision()) {
    lods.revision = station.revision();
    lods.parts.clear();
  }
  if (meshes.stationInstancesCurrent(station.revision())) return;
  ZoneScopedN("instances: rebuild");
  std::vector<::Matrix> xforms;
  for (const auto& pm : station.modules()) {
    const ModuleDef* def = catalog.find(pm.defId);
    if (def == nullptr) continue;
    const std::vector<::Matrix> parts = partTransforms(*def, pm.worldTransform);
    xforms.insert(xforms.end(), parts.begin(), parts.end());
  }
  meshes.uploadStationInstances(station.revision(), xforms);
}

// Buckets `def`'s parts, whose instances start at `firstInstance`, by part file: every
// placed copy of a module shares a bucket, the repetition an X4 station is built from
// collapsed into one sub-draw per part range. False (nothing loaded yet) = the caller boxes.
[[nodiscard]] bool collectModuleInstances(const ModuleDef& def, std::uint32_t firstInstance,
                                          MeshCache& meshes, InstanceBuckets& buckets,
                                          const LodMetrics* lod = nullptr,
                                          double minPartRadius = 0.0,
                                          const char* tracePass = nullptr) {
  if (!canRenderAsMeshes(def, meshes)) return false;
  for (std::size_t i = 0; i < def.meshRefs.size(); ++i) {
    const MeshRef& ref = def.meshRefs[i];
    if (ref.radius > 0.0 && ref.radius < minPartRadius) continue;
    const auto instance = firstInstance + static_cast<std::uint32_t>(i);
    PartLodState* state = nullptr;
    if (lod != nullptr) {
      std::vector<PartLodState>& states = partLodStates().parts;
      if (instance >= states.size()) states.resize(instance + 1);
      state = &states[instance];
    }
    const PartLod pl = partLod(ref, lod, meshes, state);
    if (tracePass != nullptr)
      traceLine("  %-6s %-52s level %d from %d fade %.3f in %s out %s\n", tracePass,
                ref.meshKey.c_str(), state ? state->level : -2, state ? state->from : -2,
                static_cast<double>(pl.fade), pl.in ? pl.in->c_str() : "-",
                pl.out ? pl.out->c_str() : "-");
    if (!pl.in) continue;
    // Only animated parts split off a still bucket; the rest keep one sub-draw.
    const bool still = pl.still && meshes.partFiles(ref).animated;
    addInstance(buckets, *pl.in, pl.fade, false, still, instance);
    if (pl.out) addInstance(buckets, *pl.out, pl.fade, true, still, instance);
  }
  return true;
}

// Buckets `def` at full detail at a pose the station doesn't hold (the selection's
// drag preview, the ghost), through transient instance `slot`. False = box it.
[[nodiscard]] bool collectTransient(const ModuleDef& def, const Transform& xf,
                                    MeshCache::TransientSlot slot, MeshCache& meshes,
                                    InstanceBuckets& buckets) {
  if (!canRenderAsMeshes(def, meshes)) return false;
  const auto first = meshes.uploadTransientInstances(slot, partTransforms(def, xf));
  return first && collectModuleInstances(def, *first, meshes, buckets);
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
                       const ConnectorGrid* grid = nullptr, bool lodEnabled = true,
                       NebulaFog* fog = nullptr) {
  const double projF = projFactor(camera);
  const ViewCull vc = viewCull(camera);

  // While the selected module is being gizmo-dragged, draw it at the live preview
  // pose (so the real mesh moves/rotates with the drag) instead of its committed
  // transform. Cull/LOD in pass 1a still use the committed pose, which is fine: the
  // selected module is never culled and always drawn at full detail.
  const auto xfFor = [&](const PlacedModule& pm, bool sel) -> const Transform& {
    return (sel && selectedPreview) ? *selectedPreview : pm.worldTransform;
  };

  // Declared before pass 1 (which fills it) so pass 2 can read it from its own
  // profiler zone scope.
  std::vector<std::pair<const ModuleDef*, const PlacedModule*>> boxes;
  boxes.reserve(station.modules().size());

  syncStationInstances(station, catalog, meshes);

  // Pass 1a: cull + LOD select, bucketing detailed parts by mesh for instancing.
  // Pure CPU (no draw calls) so its Tracy zone isolates the per-module cull/LOD math
  // from the mesh-submission cost in pass 1b. The selected module is held out (it
  // wants a yellow tint and its drag-preview pose) and bucketed on its own.
  InstanceBuckets instances;
  InstanceBuckets selection;
  const ModuleDef* selDef = nullptr;
  const PlacedModule* selPm = nullptr;
  LocalLights& lights = meshes.localLights();
  lights.begin();
  // The selected module's lights follow its drag-preview pose, added below.
  lights.addStation(station, catalog, selected);
  {
    ZoneScopedN("modules: cull+lod");
    const double stationRadius = stationBounds(station, catalog).radius;
    std::uint32_t nextInstance = 0;
    for (const auto& pm : station.modules()) {
      const ModuleDef* def = catalog.find(pm.defId);
      if (def == nullptr) continue;
      const std::uint32_t firstInstance = nextInstance;
      nextInstance += static_cast<std::uint32_t>(def->meshRefs.size());
      const bool sel = selected.has_value() && *selected == pm.instanceId;
      const LodMetrics lod = lodMetrics(*def, pm, camera, projF, stationRadius);
      const bool traced = tracing(pm.instanceId);
      if (traced)
        traceLine("frame %d t=%.3f cam=(%.1f %.1f %.1f) modulePx %.1f rulePx %.1f along %.1f%s\n",
                  lodTrace().frame++, lodNow(), static_cast<double>(camera.position.x),
                  static_cast<double>(camera.position.y), static_cast<double>(camera.position.z),
                  lod.pixelSize, lod.pixelSize * lod.ruleScale, lod.along,
                  sel ? " SELECTED (full detail)" : (frustumCull(lod, vc) ? " CULLED" : ""));
      if (!sel && frustumCull(lod, vc)) continue;

      const bool detailed = sel || !lodEnabled || lod.pixelSize >= static_cast<double>(kMeshPx);
      if (traced && !detailed) traceLine("  BOX\n");
      if (!detailed) {
        boxes.emplace_back(def, &pm);
      } else if (sel) {
        selDef = def;
        selPm = &pm;
        lights.add(*def, xfFor(pm, true));  // the drag-preview pose its mesh draws at
      } else if (!collectModuleInstances(*def, firstInstance, meshes, instances, &lod, 0.0,
                                         traced ? "main" : nullptr)) {
        boxes.emplace_back(def, &pm);  // oversized hull / no geometry -> box
      }
    }
  }
  {
    ZoneScopedN("lights: commit");
    meshes.commitLocalLights();
  }

  // The selected module's preview-pose parts; a structural-oversized one still boxes.
  const bool selMeshes =
      selDef != nullptr && collectTransient(*selDef, xfFor(*selPm, true),
                                            MeshCache::TransientSlot::Selection, meshes, selection);
  if (selDef != nullptr && !selMeshes) boxes.emplace_back(selDef, selPm);

  // Pass 1b: instanced mesh submission, one multi-draw per state class with a sub-draw
  // per unique part range across all its placed copies. Backface culling on (correct + cheaper
  // under the flip). The selected module draws separately for its tint.
  {
    ZoneScopedN("modules: mesh");
    rlEnableBackfaceCulling();
    if (!showMeshes) rlEnableWireMode();
    // Opaque + masked first (depth-writing), THEN all additive/blend fx with
    // depth-write off — fx drawn interleaved would punch holes in later hulls.
    {
      ZoneScopedN("mesh: opaque");
      uploadBuckets(meshes, {&instances, &selection}, /*occlusion=*/true);
      markPhase(ScenePhase::Prep);
      if (showMeshes) {
        ZoneScopedN("mesh: prepass+ssao+resolve");
        meshes.setCullStepHook([](MeshCache::CullStep step) {
          markPhase(static_cast<ScenePhase>(static_cast<int>(ScenePhase::GeometryA) +
                                            static_cast<int>(step)));
        });
        meshes.setDepthPrepass(true);
        drawBuckets(meshes, instances, WHITE, DrawPass::Opaque);
        meshes.setDepthPrepass(false);
        markPhase(ScenePhase::Geometry);
        meshes.computeSsao();
        markPhase(ScenePhase::Ssao);
        meshes.resolveVisibility();
      }
      markPhase(ScenePhase::Resolve);
      drawBuckets(meshes, instances, WHITE, DrawPass::Opaque);
      meshes.setAoEnabled(false);
    }
    // Not in the prepass: it writes its own depth.
    meshes.endPrepassDepth();
    drawBuckets(meshes, selection, YELLOW, DrawPass::Opaque);
    markPhase(ScenePhase::Forward);

    // Pass 2: opaque boxes (batched, unculled, solid even in wire mode) for distant +
    // mesh-less modules, before the depth copy so the fog and the fx see them.
    rlDisableBackfaceCulling();
    if (!showMeshes) rlDisableWireMode();
    {
      ZoneScopedN("modules: boxes");
      for (const auto& [def, pm] : boxes) {
        const bool sel = selected.has_value() && *selected == pm->instanceId;
        drawModuleBox(*def, xfFor(*pm, sel), sel ? ::Color{180, 160, 40, 255} : kBoxFill,
                      sel ? YELLOW : kBoxEdge);
      }
      rlDrawRenderBatchActive();
    }
    if (!showMeshes) rlEnableWireMode();
    {
      ZoneScopedN("mesh: depthcopy");
      meshes.captureSceneDepth();
    }
    markPhase(ScenePhase::Boxes);
    // Wire mode would draw the fog's fullscreen triangle as three lines.
    if (fog != nullptr && showMeshes) {
      if (const SceneDepthCopy* depth = meshes.sceneDepth()) fog->apply(camera, *depth);
    }
    markPhase(ScenePhase::Fog);
    // X4 draws its additive/blended fx two-sided (X4_SHOWROOM.rdc: NoCull on every
    // complex_unlit/simple_unlit draw): a lightcone the camera sits inside shows its far
    // walls, which the station then occludes.
    if (!meshes.materialTermActive()) {
      ZoneScopedN("mesh: transparent");
      drawBuckets(meshes, instances, WHITE, DrawPass::Transparent);
      drawBuckets(meshes, selection, YELLOW, DrawPass::Transparent);
    }
    markPhase(ScenePhase::Transparent);
    if (!showMeshes) rlDisableWireMode();
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
        const double reach =
            std::min(kConnectorDrawRadius + length(activeAabb->max - activeAabb->min) * 0.5,
                     kMaxConnectorReach);
        for (const ConnectorGrid::Entry& e : grid->queryRadius(center, reach)) {
          if (selected.has_value() && e.instanceId == *selected) continue;  // drawn above
          const PlacedModule* pm = station.find(e.instanceId);
          const ModuleDef* def = pm ? catalog.find(pm->defId) : nullptr;
          if (def == nullptr || e.connectorIndex >= def->connectionPoints.size()) continue;
          const ConnectionPoint& cp = def->connectionPoints[e.connectorIndex];
          const Vec3 n = rotate(pm->worldTransform.rotation * cp.localRotation, Vec3{0, 0, 1});
          drawOneConnector(e.world, n, pointIsLinked(*pm, cp.id),
                           connectorLit(e.world, activeAabb, false));
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
    const LodMetrics lod = lodMetrics(*def, pm, camera, projF, 0.0);
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

void drawShadowCasters(const Station& station, const ModuleCatalog& catalog, MeshCache& meshes,
                       double minPartRadius, const ShadowRegion* region,
                       const ::Camera3D* lodCamera) {
  ZoneScoped;
  syncStationInstances(station, catalog, meshes);
  InstanceBuckets instances;
  {
    ZoneScopedN("shadow: collect");
    const double projF = lodCamera != nullptr ? projFactor(*lodCamera) : 0.0;
    std::optional<ViewFrustum> frustum;
    if (lodCamera != nullptr) frustum = viewFrustum(*lodCamera);
    const double stationRadius = stationBounds(station, catalog).radius;
    std::uint32_t nextInstance = 0;
    for (const auto& pm : station.modules()) {
      const ModuleDef* def = catalog.find(pm.defId);
      if (def == nullptr) continue;
      const std::uint32_t firstInstance = nextInstance;
      nextInstance += static_cast<std::uint32_t>(def->meshRefs.size());
      if (region != nullptr) {
        const StationBounds b = boundsOf(worldAabb(def->aabb, pm.worldTransform));
        const Vec3 d = flipZ(b.center) - region->center;
        const double reach = region->halfExtent + b.radius;
        if (std::abs(dot(d, region->right)) > reach || std::abs(dot(d, region->up)) > reach)
          continue;
        if (frustum && shadowMissesView(flipZ(b.center), b.radius, *region, *frustum)) continue;
      }
      std::optional<LodMetrics> lod;
      if (lodCamera != nullptr) {
        lod = lodMetrics(*def, pm, *lodCamera, projF, stationRadius);
        // A caster the view culls as wholly behind the eye still shadows hulls in view:
        // size it by distance. Nearer ones keep the view's size, so both passes agree on
        // the shared PartLodState.
        if (lod->along < -lod->radius)
          lod->pixelSize =
              lod->radius / std::max(std::hypot(lod->along, lod->lateral), 1e-3) * projF;
      }
      const bool traced = tracing(pm.instanceId);
      if (traced)
        traceLine(" shadow cascade re-render at t=%.3f%s\n", lodNow(),
                  lod ? "" : " (full detail, no view LOD)");
      (void)collectModuleInstances(*def, firstInstance, meshes, instances, lod ? &*lod : nullptr,
                                   minPartRadius, traced ? "shadow" : nullptr);
    }
  }
  {
    ZoneScopedN("shadow: draw");
    uploadBuckets(meshes, {&instances});
    drawBuckets(meshes, instances, WHITE, DrawPass::Opaque);
  }
}

StationBounds boundsOf(const AABB& box) {
  return {box, (box.min + box.max) * 0.5, length(box.max - box.min) * 0.5};
}

void drawScene(const EditorState& state, const ::Camera3D& camera, MeshCache& meshes,
               bool showGizmos, bool showMeshes, bool lodEnabled, const SceneSun& sun,
               NebulaFog* fog) {
  ZoneScoped;
  meshes.setLighting(sun.dir, sun.color, sun.intensity, sun.ambientColor, sun.ambientIntensity,
                     camera.position);
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
                    connGrid, lodEnabled, fog);

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
    ZoneScopedN("iact: facegrids");
    auto drawFaceGrid = [](int axis, double value, double uVal, double vVal, ::Color gridColor) {
      constexpr double gridHalfSize = 1500.0;
      constexpr int steps = 6;
      const double stepSize = (gridHalfSize * 2.0) / steps;

      auto drawClippedLine = [](Vec3 p1, Vec3 p2, ::Color color) {
        constexpr double L = 10000.0;
        if ((p1.x > L && p2.x > L) || (p1.x < -L && p2.x < -L)) return;
        if ((p1.y > L && p2.y > L) || (p1.y < -L && p2.y < -L)) return;
        if ((p1.z > L && p2.z > L) || (p1.z < -L && p2.z < -L)) return;

        p1.x = std::clamp(p1.x, -L, L);
        p1.y = std::clamp(p1.y, -L, L);
        p1.z = std::clamp(p1.z, -L, L);

        p2.x = std::clamp(p2.x, -L, L);
        p2.y = std::clamp(p2.y, -L, L);
        p2.z = std::clamp(p2.z, -L, L);

        DrawLine3D(toRl(flipZ(p1)), toRl(flipZ(p2)), color);
      };

      for (int i = 0; i <= steps; ++i) {
        double offset = -gridHalfSize + i * stepSize;
        Vec3 p1{}, p2{}, q1{}, q2{};

        if (axis == 0) {  // X plane
          p1 = {value, uVal + offset, vVal - gridHalfSize};
          p2 = {value, uVal + offset, vVal + gridHalfSize};
          q1 = {value, uVal - gridHalfSize, vVal + offset};
          q2 = {value, uVal + gridHalfSize, vVal + offset};
        } else if (axis == 1) {  // Y plane
          p1 = {uVal + offset, value, vVal - gridHalfSize};
          p2 = {uVal + offset, value, vVal + gridHalfSize};
          q1 = {uVal - gridHalfSize, value, vVal + offset};
          q2 = {uVal + gridHalfSize, value, vVal + offset};
        } else {  // Z plane
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
    const ::Color normalColor{34, 187, 255, 180};  // Cherenkov Blue
    const ::Color gridColor = normalColor;

    if (std::abs(worldBox.max.x - kPlotLimit) < kThresh) {
      drawFaceGrid(0, kPlotLimit, centerPos.y, centerPos.z, gridColor);
    }
    if (std::abs(worldBox.min.x + kPlotLimit) < kThresh) {
      drawFaceGrid(0, -kPlotLimit, centerPos.y, centerPos.z, gridColor);
    }

    if (std::abs(worldBox.max.y - kPlotLimit) < kThresh) {
      drawFaceGrid(1, kPlotLimit, centerPos.x, centerPos.z, gridColor);
    }
    if (std::abs(worldBox.min.y + kPlotLimit) < kThresh) {
      drawFaceGrid(1, -kPlotLimit, centerPos.x, centerPos.z, gridColor);
    }

    if (std::abs(worldBox.max.z - kPlotLimit) < kThresh) {
      drawFaceGrid(2, kPlotLimit, centerPos.x, centerPos.y, gridColor);
    }
    if (std::abs(worldBox.min.z + kPlotLimit) < kThresh) {
      drawFaceGrid(2, -kPlotLimit, centerPos.x, centerPos.y, gridColor);
    }
  }

  // ── Flight-corridor clearance volumes ───────────────────────────────────────
  // Which existing dock corridor an in-progress placement/drag is intruding, so it
  // turns red exactly when a body enters it (even with "show all" off). Only computed
  // while a ghost or drag is active, so it stays O(docks) and off the idle path.
  std::vector<ClearanceHit> revealed;
  if (state.ghost()) {
    if (const ModuleDef* gdef = state.defFor(state.ghost()->defId))
      revealed = violatedClearance(*gdef, state.ghost()->worldTransform, 0, 0, state.station(),
                                   state.catalog());
  } else if (state.selected() && state.dragPreview()) {
    if (const PlacedModule* m = state.station().find(*state.selected()))
      if (const ModuleDef* sdef = state.defFor(m->defId))
        revealed = violatedClearance(*sdef, *state.dragPreview(), *state.selected(), 0,
                                     state.station(), state.catalog());
  }
  const auto isRevealed = [&revealed](InstanceId id, std::size_t i) {
    for (const ClearanceHit& h : revealed)
      if (h.blocker == id && h.volumeIndex == i) return true;
    return false;
  };

  {
    ZoneScopedN("iact: clearance");
    const ::Color kInfo{90, 180, 220, 200};  // neutral "show all" corridor
    const std::optional<InstanceId> sel = state.selected();
    const bool showAll = state.showAllClearance();
    // Nothing can draw without a reveal, a selection, or show-all — skip the whole
    // per-frame station walk (and its per-module catalog lookups) in the idle case.
    if (showAll || sel.has_value() || !revealed.empty()) {
      for (const PlacedModule& m : state.station().modules()) {
        const ModuleDef* mdef = state.defFor(m.defId);
        if (mdef == nullptr || mdef->clearanceVolumes.empty()) continue;
        const bool isSelected = sel.has_value() && *sel == m.instanceId;
        const bool selBlocked =
            isSelected && collidesClearance(*mdef, m.worldTransform, m.instanceId, 0,
                                            state.station(), state.catalog());
        for (std::size_t i = 0; i < mdef->clearanceVolumes.size(); ++i) {
          ::Color col{};
          if (isRevealed(m.instanceId, i)) {
            col = kClearanceRed;
          } else if (isSelected) {
            col = selBlocked ? kClearanceRed : kClearanceGreen;
          } else if (showAll) {
            col = kInfo;
          } else {
            continue;  // not selected, not intruded, show-all off -> hidden
          }
          drawClearanceVolume(mdef->clearanceVolumes[i], m.worldTransform, col);
        }
      }
    }
  }

  if (state.ghost()) {
    ZoneScopedN("iact: ghost+clearance");
    const ModuleDef* gdef = state.defFor(state.ghost()->defId);
    if (gdef != nullptr) {
      const ::Color fill = state.ghost()->valid ? ::Color{0, 220, 0, 90} : ::Color{220, 0, 0, 90};
      const ::Color edge = state.ghost()->valid ? GREEN : RED;
      // While any part is still streaming (usually already queued by the palette
      // hover) draw nothing: a box or a half-assembled module flashing for a few
      // frames reads as a glitch. The box stays for modules that genuinely lack meshes.
      bool streaming = false;
      for (const MeshRef& ref : gdef->meshRefs) meshes.request(ref.meshKey, false, /*urgent=*/true);
      for (const MeshRef& ref : gdef->meshRefs)
        if (meshes.get(ref.meshKey) == nullptr && !meshes.isResolved(ref.meshKey)) streaming = true;
      if (!streaming) {
        rlEnableBackfaceCulling();
        if (!showMeshes) rlEnableWireMode();
        InstanceBuckets ghost;
        const bool drew = collectTransient(*gdef, state.ghost()->worldTransform,
                                           MeshCache::TransientSlot::Ghost, meshes, ghost);
        if (drew) {
          uploadBuckets(meshes, {&ghost});
          drawBuckets(meshes, ghost, edge, DrawPass::Opaque);
          drawBuckets(meshes, ghost, edge, DrawPass::Transparent);
        }
        if (!showMeshes) rlDisableWireMode();
        rlDisableBackfaceCulling();
        if (!drew) drawModuleBox(*gdef, state.ghost()->worldTransform, fill, edge);
      }
      if (!gdef->clearanceVolumes.empty()) {
        const ::Color col = state.ghost()->valid ? kClearanceGreen : kClearanceRed;
        for (const ClearanceVolume& cv : gdef->clearanceVolumes)
          drawClearanceVolume(cv, state.ghost()->worldTransform, col);
      }
    }
  }

  // Gizmo + drag preview last, inside the scene's global flip (its axes are
  // X4-native, like every other primitive here), before endScene().
  {
    ZoneScopedN("iact: gizmo");
    drawGizmo(state, camera);
  }

  {
    ZoneScopedN("iact: snaplinks");
    drawSnapLink(state);
  }

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
  const ::Vector3 o = toRl(flipZ(g.origin));

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
      const ::Vector3 neck = toRl(flipZ(g.origin + dir * (g.axisLength - headLen)));
      const ::Vector3 tip = toRl(flipZ(g.origin + dir * g.axisLength));
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
      const ::Vector3 a = toRl(flipZ(g.origin + p.u * g.planeSize));
      const ::Vector3 corner = toRl(flipZ(g.origin + (p.u + p.v) * g.planeSize));
      const ::Vector3 c = toRl(flipZ(g.origin + p.v * g.planeSize));
      DrawTriangle3D(o, a, corner, fill);
      DrawTriangle3D(o, corner, c, fill);
      DrawLine3D(a, corner, edge);
      DrawLine3D(corner, c, edge);
    }

    // Center free-translate handle: semi-transparent so it doesn't hide the part origin.
    const ::Color centerCol =
        (hl && *hl == GizmoHandle::Center) ? kHi : ::Color{255, 255, 255, 120};
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
}

void drawScene(const Station& station, const ModuleCatalog& catalog, const ::Camera3D& camera,
               MeshCache& meshes, bool showGizmos, bool showMeshes, bool allConnectors,
               bool lodEnabled, const SceneSun& sun, NebulaFog* fog) {
  ZoneScoped;
  meshes.setLighting(sun.dir, sun.color, sun.intensity, sun.ambientColor, sun.ambientIntensity,
                     camera.position);
  meshes.beginFrame();  // per-frame upload budget reset — see the EditorState drawScene overload
  beginScene(camera);
  drawPlacedModules(station, catalog, std::nullopt, meshes, showGizmos, showMeshes, camera,
                    allConnectors, std::nullopt, std::nullopt, nullptr, lodEnabled, fog);
  endScene();
}

void drawHud(const EditorState& state, int /*screenWidth*/, int /*screenHeight*/, bool showGizmos) {
  const ModuleDef* def = state.activeDef();
  char line[256];

  if (state.placementEnabled()) {
    std::snprintf(line, sizeof(line), "BUILD  Active: %s",
                  def != nullptr ? displayName(*def).c_str() : "(none)");
    DrawText(line, 12, 10, 20, RAYWHITE);
  } else {
    DrawText("SELECT  (Tab to build)  -  click a placed module to select it", 12, 10, 20, GOLD);
  }

  const char* filt = state.filter().has_value() ? categoryLabel(*state.filter()) : "All";
  std::snprintf(line, sizeof(line), "Filter: %s    %zu/%zu", filt,
                def != nullptr ? state.activeIndex() + 1 : 0, state.activeCount());
  DrawText(line, 12, 34, 18, SKYBLUE);

  // Placed/Undo/Redo/FPS now live in the Clay top bar; keep the toggles here.
  std::snprintf(line, sizeof(line), "Gizmos:%s    Overlap:%s    Corridors:%s",
                showGizmos ? "on" : "off", state.allowOverlap() ? "ON" : "off",
                state.showAllClearance() ? "ON" : "off");
  DrawText(line, 12, 56, 16, LIGHTGRAY);

  DrawText(state.cartSummary().c_str(), 12, 76, 16, ::Color{200, 220, 255, 255});

  DrawText(
      "[ / ]=cycle  1=Prod 2=Stor 3=Hab 4=Dock 5=Def 6=Conn 7=Other  0=all  G=gizmos  M=mesh/box  "
      "K=sun fx",
      12, 96, 14, GRAY);
  DrawText(
      "LMB=place/select/drag-gizmo   Tab=build/select   T/Y=move/rotate gizmo   "
      "R/Shift+R/Ctrl+R=rotate   O=overlap   C=corridors   Alt=free   Del=delete   "
      "Ctrl+Z/Y=undo/redo",
      12, 114, 14, GRAY);
  DrawText("WASD=fly  Q/E=turn  Z/X=up/down  RMB=orbit  MMB=pan  wheel=zoom  F=frame", 12, 132, 14,
           GRAY);
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

void setScenePhaseHook(std::function<void(ScenePhase)> hook) { scenePhaseHook() = std::move(hook); }

void setLodClock(std::function<double()> clock) { lodClock() = std::move(clock); }

void startLodTrace(InstanceId module) { lodTrace() = {module, {}, 0}; }

std::string stopLodTrace() {
  std::string text = std::move(lodTrace().text);
  lodTrace() = {};
  return text;
}

}  // namespace x4sb::editor
