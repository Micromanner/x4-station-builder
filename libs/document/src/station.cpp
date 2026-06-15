#include "x4sb/document/station.hpp"

namespace x4sb {

InstanceId Station::add(const PlacedModule& m) {
  PlacedModule copy = m;
  if (copy.instanceId == 0) copy.instanceId = nextId_++;
  if (copy.instanceId >= nextId_) nextId_ = copy.instanceId + 1;
  placed_.push_back(std::move(copy));
  return placed_.back().instanceId;
}

bool Station::remove(InstanceId id) {
  for (auto it = placed_.begin(); it != placed_.end(); ++it) {
    if (it->instanceId == id) {
      placed_.erase(it);
      return true;
    }
  }
  return false;
}

PlacedModule* Station::find(InstanceId id) {
  for (auto& p : placed_)
    if (p.instanceId == id) return &p;
  return nullptr;
}

const PlacedModule* Station::find(InstanceId id) const {
  for (const auto& p : placed_)
    if (p.instanceId == id) return &p;
  return nullptr;
}

void UndoStack::execute(Station& s, std::unique_ptr<Command> cmd) {
  cmd->apply(s);
  done_.push_back(std::move(cmd));
  undone_.clear();
}

void UndoStack::undo(Station& s) {
  if (done_.empty()) return;
  std::unique_ptr<Command> cmd = std::move(done_.back());
  done_.pop_back();
  cmd->undo(s);
  undone_.push_back(std::move(cmd));
}

void UndoStack::redo(Station& s) {
  if (undone_.empty()) return;
  std::unique_ptr<Command> cmd = std::move(undone_.back());
  undone_.pop_back();
  cmd->apply(s);
  done_.push_back(std::move(cmd));
}

}  // namespace x4sb
