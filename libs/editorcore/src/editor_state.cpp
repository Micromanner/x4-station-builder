#include "x4sb/editorcore/editor_state.hpp"

#include "x4sb/document/commands.hpp"
#include "x4sb/snap/pick.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

namespace x4sb {

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

void EditorState::updateGhost(Vec3 rayOriginX4, Vec3 rayDirX4) {
  ghost_.reset();
  const ModuleDef* def = activeDef();
  if (def == nullptr) return;

  if (station_.empty()) {
    if (std::abs(rayDirX4.y) < 1e-9) return;  // parallel to the ground plane
    const double t = (groundY_ - rayOriginX4.y) / rayDirX4.y;
    if (t < 0.0) return;  // ground is behind the ray origin
    Transform root;       // identity rotation
    root.position = rayOriginX4 + rayDirX4 * t;
    ghost_ = Ghost{def->id, root, /*valid=*/true, std::nullopt};
    return;
  }
  // Non-empty station: the new module snaps to the nearest free connector near
  // where the ray strikes the nearest module's box (parent §6).
  const std::optional<InstanceId> hitId = pickModule(station_, catalog_, rayOriginX4, rayDirX4);
  if (!hitId) return;
  const PlacedModule* pm = station_.find(*hitId);
  if (pm == nullptr) return;
  const ModuleDef* hitDef = catalog_.find(pm->defId);
  if (hitDef == nullptr) return;

  const AABB worldBox = worldAabb(hitDef->aabb, pm->worldTransform);
  const std::optional<double> t = rayIntersectsAabb(rayOriginX4, rayDirX4, worldBox);
  if (!t) return;
  const Vec3 cursor = rayOriginX4 + rayDirX4 * (*t);

  const std::optional<SnapCandidate> cand =
      findSnapCandidate(*def, cursor, station_, catalog_, snapRadius_);
  if (!cand) return;

  const Transform xf = computeSnapTransform(station_, catalog_, cand->instanceId,
                                            cand->targetPointId, *def, cand->newPointId);
  // No collision gating in the box-proxy phase: AABB boxes are far larger than the
  // real meshes, so neighbours overlap at correct joints and an AABB collision test
  // would reject almost every placement in a multi-module station. A snap is valid
  // whenever a compatible free connector is found. (Revisit with OBB/real-mesh
  // collision once meshes land — collidesWithStation/makeSnapPlacement still exist.)
  ghost_ = Ghost{def->id, xf, /*valid=*/true, cand};
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

}  // namespace x4sb
