#include "x4sb/editorcore/editor_state.hpp"

#include "x4sb/document/commands.hpp"
#include "x4sb/snap/pick.hpp"
#include "x4sb/snap/snap.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

namespace x4sb {
namespace {
// pi/2: the 90-degree step shared by the rotate-in-place commands and the ghost.
constexpr double kHalfPi = 1.5707963267948966;

bool isInsidePlot(const AABB& box) {
  constexpr double kPlotLimit = 10000.0;  // 20km plot centered at origin => +/-10km
  return box.min.x >= -kPlotLimit && box.max.x <= kPlotLimit &&
         box.min.y >= -kPlotLimit && box.max.y <= kPlotLimit &&
         box.min.z >= -kPlotLimit && box.max.z <= kPlotLimit;
}

Transform clampToPlot(const AABB& localAabb, Transform t) {
  constexpr double L = 9999.0;  // 1m buffer inside the 10000.0 boundary
  AABB rotated = worldAabb(localAabb, Transform{Vec3{0, 0, 0}, t.rotation});

  double minX = -L - rotated.min.x;
  double maxX = L - rotated.max.x;
  if (maxX >= minX) t.position.x = std::clamp(t.position.x, minX, maxX);

  double minY = -L - rotated.min.y;
  double maxY = L - rotated.max.y;
  if (maxY >= minY) t.position.y = std::clamp(t.position.y, minY, maxY);

  double minZ = -L - rotated.min.z;
  double maxZ = L - rotated.max.z;
  if (maxZ >= minZ) t.position.z = std::clamp(t.position.z, minZ, maxZ);

  return t;
}
}  // namespace

EditorState::EditorState(const ModuleCatalog& catalog) : catalog_(catalog) {
  order_.reserve(catalog.all().size());
  for (const auto& entry : catalog.all()) order_.push_back(entry.first);
  std::sort(order_.begin(), order_.end());
}

void EditorState::execute(std::unique_ptr<Command> cmd) {
  undo_.execute(station_, std::move(cmd));
  gridDirty_ = true;
}

const ConnectorGrid& EditorState::connectorGrid() const {
  if (gridDirty_ || !connectorGrid_) {
    connectorGrid_.emplace(station_, catalog_, snapRadius_);
    gridDirty_ = false;
  }
  return *connectorGrid_;
}

std::vector<std::string> EditorState::filteredOrder() const {
  if (!filter_) return order_;
  std::vector<std::string> out;
  for (const auto& id : order_) {
    const ModuleDef* d = catalog_.find(id);
    if (d != nullptr && d->category == *filter_) out.push_back(id);
  }
  return out;
}

const ModuleDef* EditorState::activeDef() const {
  const std::vector<std::string> view = filteredOrder();
  if (view.empty()) return nullptr;
  const std::size_t idx = activeIndex_ < view.size() ? activeIndex_ : 0;
  return catalog_.find(view[idx]);
}

std::size_t EditorState::activeCount() const { return filteredOrder().size(); }

const ModuleDef* EditorState::defFor(const std::string& id) const { return catalog_.find(id); }

void EditorState::cycleActive(int delta) {
  const std::size_t n = filteredOrder().size();
  if (n == 0) {
    activeIndex_ = 0;
    return;
  }
  const long long m = static_cast<long long>(n);
  long long i = static_cast<long long>(activeIndex_) + delta;
  i = ((i % m) + m) % m;  // signed-safe wrap into [0, n)
  activeIndex_ = static_cast<std::size_t>(i);
}

void EditorState::setFilter(std::optional<Category> cat) {
  filter_ = cat;
  activeIndex_ = 0;
}

void EditorState::updateGhost(Vec3 rayOriginX4, Vec3 rayDirX4, bool forceFree) {
  ghost_.reset();
  if (!placementEnabled_) return;  // select mode: no ghost, so clicks select
  const ModuleDef* def = activeDef();
  if (def == nullptr) return;

  // Snap path: aim at a placed module and mate onto its nearest free connector.
  if (!forceFree && !station_.empty()) {
    const std::optional<InstanceId> hitId =
        pickModule(station_, catalog_, rayOriginX4, rayDirX4);
    if (hitId) {
      const PlacedModule* pm = station_.find(*hitId);
      const ModuleDef* hitDef = pm ? catalog_.find(pm->defId) : nullptr;
      if (hitDef) {
        const AABB worldBox = worldAabb(hitDef->aabb, pm->worldTransform);
        const std::optional<double> t = rayIntersectsAabb(rayOriginX4, rayDirX4, worldBox);
        if (t) {
          const Vec3 cursor = rayOriginX4 + rayDirX4 * (*t);
          const std::optional<SnapCandidate> cand =
              findSnapCandidate(*def, cursor, station_, catalog_, connectorGrid(), snapRadius_);
          if (cand) {
            const Transform xf = computeSnapTransform(station_, catalog_, cand->instanceId,
                                                      cand->targetPointId, *def, cand->newPointId);
            const AABB snapWorldBox = worldAabb(def->aabb, xf);
            // Clearance is always enforced regardless of allowOverlap_ — confirmed
            // in-game: the "Allow Module Overlap" toggle only relaxes body AABBs,
            // not dock/cradle flight corridors.
            const bool clear =
                (allowOverlap_ || !collidesWithStation(*def, xf, cand->instanceId, station_, catalog_)) &&
                !collidesClearance(*def, xf, cand->instanceId, 0, station_, catalog_);
            ghost_ = Ghost{def->id, xf, isInsidePlot(snapWorldBox) && clear, cand};
            return;
          }
        }
      }
    }
  }

  // Free-place fallback (also the empty-station root case): a view-facing standoff
  // a fixed distance in front of the camera along the cursor ray. This is the fix
  // for the "feels 2D" problem — projecting onto a ground plane made vertical mouse
  // motion sweep the hit point near/far; a standoff moves the ghost up/down on
  // screen instead. The mouse ray's direction is unit length (GetScreenToWorldRay).
  Transform xf;
  xf.position = rayOriginX4 + rayDirX4 * placeDistance_;
  xf.rotation = pendingRotation_;
  xf = clampToPlot(def->aabb, xf);
  const AABB worldBox = worldAabb(def->aabb, xf);
  // Same clearance-always rule as the snap path (see comment above).
  const bool clear =
      (allowOverlap_ || !collidesWithStation(*def, xf, 0, station_, catalog_)) &&
      !collidesClearance(*def, xf, 0, 0, station_, catalog_);
  ghost_ = Ghost{def->id, xf, isInsidePlot(worldBox) && clear, std::nullopt};
}

void EditorState::rotateGhost(Vec3 worldAxis) {
  pendingRotation_ = axisAngle(worldAxis, kHalfPi) * pendingRotation_;
}

std::optional<InstanceId> EditorState::commitGhost() {
  if (!ghost_ || !ghost_->valid) return std::nullopt;
  const Ghost g = *ghost_;
  std::unique_ptr<Command> cmd;
  if (g.candidate) {
    cmd = std::make_unique<PlaceModuleCommand>(g.defId, g.worldTransform, g.candidate->instanceId,
                                               g.candidate->newPointId, g.candidate->targetPointId);
  } else {
    cmd = std::make_unique<PlaceModuleCommand>(g.defId, g.worldTransform);
  }
  execute(std::move(cmd));
  ghost_.reset();
  pendingRotation_ = Quat{};  // spec §5: rotation resets on commit
  if (station_.modules().empty()) return std::nullopt;
  // PlaceModuleCommand appends the new module (Station::add push_backs), so the
  // just-placed one is at the back. Holds while place always appends.
  return station_.modules().back().instanceId;
}

void EditorState::loadStation(Station station) {
  station_ = std::move(station);
  undo_ = UndoStack{};  // UndoStack has no clear(); a fresh instance is the reset
  selected_.reset();
  ghost_.reset();
  placementEnabled_ = true;  // fresh document starts in build mode
  gridDirty_ = true;
}

std::optional<InstanceId> EditorState::selectByRay(Vec3 rayOriginX4, Vec3 rayDirX4) {
  selected_ = pickModule(station_, catalog_, rayOriginX4, rayDirX4);
  if (selected_) gizmoMode_ = GizmoMode::Translate;  // a fresh selection starts in move mode
  return selected_;
}

bool EditorState::deleteSelected() {
  if (!selected_) return false;
  execute(std::make_unique<DeleteModuleCommand>(*selected_));
  selected_.reset();
  return true;
}

bool EditorState::beginGizmoDrag(Vec3 rayOriginX4, Vec3 rayDirX4, double gizmoScale) {
  if (!selected_) return false;
  const PlacedModule* m = station_.find(*selected_);
  if (m == nullptr) return false;
  const GizmoModel g = gizmoModel(m->worldTransform.position, gizmoScale);
  const std::optional<GizmoHandle> handle = gizmoPick(g, rayOriginX4, rayDirX4, gizmoMode_);
  if (!handle) return false;

  ghost_.reset();  // a grab is not a placement
  GizmoDrag d;
  d.id = *selected_;
  d.handle = *handle;
  d.startRayOrigin = rayOriginX4;
  d.startRayDir = rayDirX4;
  d.startTransform = m->worldTransform;
  d.preview = m->worldTransform;
  drag_ = d;
  return true;
}

void EditorState::updateGizmoDrag(Vec3 rayOriginX4, Vec3 rayDirX4, bool forceFree) {
  if (!drag_) return;
  const PlacedModule* m = station_.find(drag_->id);
  if (m == nullptr) {
    drag_.reset();
    return;
  }
  const ModuleDef* def = catalog_.find(m->defId);
  if (def == nullptr) {
    drag_.reset();
    return;
  }

  // Rotation handles spin the module in place about the ring axis — no translation,
  // no snap-on-move.
  if (gizmoIsRotation(drag_->handle)) {
    const double angle = gizmoDragRotation(drag_->handle, drag_->startTransform.position,
                                           drag_->startRayOrigin, drag_->startRayDir, rayOriginX4,
                                           rayDirX4);
    Transform rotated = drag_->startTransform;
    rotated.rotation = axisAngle(gizmoAxisDir(drag_->handle), angle) * drag_->startTransform.rotation;
    rotated = clampToPlot(def->aabb, rotated);
    drag_->snap.reset();
    drag_->preview = rotated;
    return;
  }

  const Vec3 delta = gizmoDragDelta(drag_->handle, drag_->startTransform.position,
                                    drag_->startRayOrigin, drag_->startRayDir, rayOriginX4,
                                    rayDirX4);
  Transform freePose = drag_->startTransform;
  freePose.position = drag_->startTransform.position + delta;
  freePose = clampToPlot(def->aabb, freePose);
  drag_->snap.reset();
  drag_->preview = freePose;

  if (!forceFree) {
    const std::optional<SnapCandidate> cand =
        findSnapCandidate(*def, freePose.position, station_, catalog_, connectorGrid(),
                          dragSnapRadius_, drag_->id, freePose);
    if (cand) {
      drag_->snap = cand;
      drag_->preview = computeSnapTransform(station_, catalog_, cand->instanceId,
                                            cand->targetPointId, *def, cand->newPointId);
    }
  }
}

bool EditorState::endGizmoDrag() {
  if (!drag_) return false;
  const GizmoDrag d = *drag_;
  drag_.reset();

  // Validate boundary box before committing the drag
  const PlacedModule* pm = station_.find(d.id);
  const ModuleDef* def = pm ? catalog_.find(pm->defId) : nullptr;
  if (def) {
    if (!isInsidePlot(worldAabb(def->aabb, d.preview))) {
      return false;
    }
    const InstanceId partner = d.snap ? d.snap->instanceId : 0;
    // Body overlap is togglable; clearance (dock/cradle corridor) always blocks.
    const bool bodyBlocked =
        !allowOverlap_ && collidesWithStation(*def, d.preview, d.id, partner, station_, catalog_);
    const bool clearanceBlocked =
        collidesClearance(*def, d.preview, d.id, partner, station_, catalog_);
    if (bodyBlocked || clearanceBlocked) {
      return false;
    }
  }

  if (d.snap) {
    execute(std::make_unique<SnapMoveCommand>(d.id, d.preview, d.snap->instanceId,
                                             d.snap->newPointId, d.snap->targetPointId));
    return true;
  }
  // Commit if the pose actually changed in position OR orientation — a rotation
  // drag leaves the position fixed, so a position-only check would drop it.
  const Vec3 moved = d.preview.position - d.startTransform.position;
  const Quat& qp = d.preview.rotation;
  const Quat& qs = d.startTransform.rotation;
  const double qdot = std::abs(qp.w * qs.w + qp.x * qs.x + qp.y * qs.y + qp.z * qs.z);
  const bool changed = length(moved) > 1e-9 || qdot < 1.0 - 1e-9;
  if (!changed) return false;  // a click, not a drag
  execute(std::make_unique<MoveModuleCommand>(d.id, d.preview));
  return true;
}

std::optional<Transform> EditorState::dragPreview() const {
  if (!drag_) return std::nullopt;
  return drag_->preview;
}

std::vector<SnapLink> EditorState::activeSnapLinks() const {
  // A guide-line for EVERY compatible free connector pair within the approach radius
  // (not just the nearest) so the user sees all the places it could snap — like the
  // proximity glow, but precise. The lines collapse to the joint as the magnet engages.
  std::vector<SnapLink> out;
  const ModuleDef* def = nullptr;
  Transform pose;
  InstanceId ignore = 0;
  if (drag_) {
    const PlacedModule* m = station_.find(drag_->id);
    if (m == nullptr) return out;
    def = catalog_.find(m->defId);
    pose = drag_->preview;
    ignore = drag_->id;
  } else if (ghost_) {
    def = catalog_.find(ghost_->defId);
    pose = ghost_->worldTransform;
  } else {
    return out;
  }
  if (def == nullptr) return out;

  // Pair each of the active module's connectors with every nearby compatible free
  // target connector. Grid-accelerated, so the cost is local, not station-wide.
  const ConnectorGrid& grid = connectorGrid();
  for (const ConnectionPoint& np : def->connectionPoints) {
    const Vec3 npWorld = apply(pose, np.localPosition);
    for (const ConnectorGrid::Entry& e : grid.queryRadius(npWorld, lineRadius_)) {
      if (e.instanceId == ignore) continue;  // never snap a module to itself
      const PlacedModule* target = station_.find(e.instanceId);
      const ModuleDef* tdef = target ? catalog_.find(target->defId) : nullptr;
      if (tdef == nullptr || e.connectorIndex >= tdef->connectionPoints.size()) continue;
      const ConnectionPoint& tp = tdef->connectionPoints[e.connectorIndex];
      if (connectorIsLinked(*target, tp.id)) continue;  // target already occupied
      if (!connectorsCompatible(np, tp)) continue;       // type tags incompatible
      out.push_back(SnapLink{npWorld, e.world});
    }
  }
  return out;
}

bool EditorState::rotateSelected(Vec3 worldAxis) {
  if (!selected_) return false;
  const PlacedModule* m = station_.find(*selected_);
  if (m == nullptr) return false;
  Transform t = m->worldTransform;
  t.rotation = axisAngle(worldAxis, kHalfPi) * t.rotation;
  const ModuleDef* def = catalog_.find(m->defId);
  if (def) {
    t = clampToPlot(def->aabb, t);
  }
  execute(std::make_unique<MoveModuleCommand>(*selected_, t));
  return true;
}

void EditorState::updateGizmoHover(Vec3 rayOriginX4, Vec3 rayDirX4, double gizmoScale) {
  hoveredHandle_.reset();
  if (drag_ || !selected_) return;  // a drag owns the highlight; nothing to hover otherwise
  const PlacedModule* m = station_.find(*selected_);
  if (m == nullptr) return;
  const GizmoModel g = gizmoModel(m->worldTransform.position, gizmoScale);
  hoveredHandle_ = gizmoPick(g, rayOriginX4, rayDirX4, gizmoMode_);
}

std::optional<GizmoHandle> EditorState::highlightHandle() const {
  if (drag_) return drag_->handle;
  return hoveredHandle_;
}

}  // namespace x4sb
