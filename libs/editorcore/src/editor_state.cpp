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
              findSnapCandidate(*def, cursor, station_, catalog_, snapRadius_);
          if (cand) {
            const Transform xf = computeSnapTransform(station_, catalog_, cand->instanceId,
                                                      cand->targetPointId, *def, cand->newPointId);
            const AABB snapWorldBox = worldAabb(def->aabb, xf);
            ghost_ = Ghost{def->id, xf, isInsidePlot(snapWorldBox), cand};
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
  ghost_ = Ghost{def->id, xf, isInsidePlot(worldBox), std::nullopt};
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
  undo_.execute(station_, std::move(cmd));
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
}

std::optional<InstanceId> EditorState::selectByRay(Vec3 rayOriginX4, Vec3 rayDirX4) {
  selected_ = pickModule(station_, catalog_, rayOriginX4, rayDirX4);
  return selected_;
}

bool EditorState::deleteSelected() {
  if (!selected_) return false;
  undo_.execute(station_, std::make_unique<DeleteModuleCommand>(*selected_));
  selected_.reset();
  return true;
}

bool EditorState::beginGizmoDrag(Vec3 rayOriginX4, Vec3 rayDirX4, double gizmoScale) {
  if (!selected_) return false;
  const PlacedModule* m = station_.find(*selected_);
  if (m == nullptr) return false;
  const GizmoModel g = gizmoModel(m->worldTransform.position, gizmoScale);
  const std::optional<GizmoHandle> handle = gizmoPick(g, rayOriginX4, rayDirX4);
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
    const std::optional<SnapCandidate> cand = findSnapCandidate(
        *def, freePose.position, station_, catalog_, dragSnapRadius_, drag_->id, freePose);
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
  }

  if (d.snap) {
    undo_.execute(station_, std::make_unique<SnapMoveCommand>(
                                d.id, d.preview, d.snap->instanceId, d.snap->newPointId,
                                d.snap->targetPointId));
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
  undo_.execute(station_, std::make_unique<MoveModuleCommand>(d.id, d.preview));
  return true;
}

std::optional<Transform> EditorState::dragPreview() const {
  if (!drag_) return std::nullopt;
  return drag_->preview;
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
  undo_.execute(station_, std::make_unique<MoveModuleCommand>(*selected_, t));
  return true;
}

void EditorState::updateGizmoHover(Vec3 rayOriginX4, Vec3 rayDirX4, double gizmoScale) {
  hoveredHandle_.reset();
  if (drag_ || !selected_) return;  // a drag owns the highlight; nothing to hover otherwise
  const PlacedModule* m = station_.find(*selected_);
  if (m == nullptr) return;
  const GizmoModel g = gizmoModel(m->worldTransform.position, gizmoScale);
  hoveredHandle_ = gizmoPick(g, rayOriginX4, rayDirX4);
}

std::optional<GizmoHandle> EditorState::highlightHandle() const {
  if (drag_) return drag_->handle;
  return hoveredHandle_;
}

}  // namespace x4sb
