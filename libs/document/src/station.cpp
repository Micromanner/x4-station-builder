#include "x4sb/document/station.hpp"

namespace x4sb {

InstanceId Station::add(const PlacedModule& m) {
  PlacedModule copy = m;
  if (copy.instanceId == 0) copy.instanceId = nextId_++;
  if (copy.instanceId >= nextId_) nextId_ = copy.instanceId + 1;
  const InstanceId id = copy.instanceId;
  placed_.push_back(std::move(copy));
  index_[id] = placed_.size() - 1;  // append never shifts existing positions
  return id;
}

bool Station::remove(InstanceId id) {
  const auto it = index_.find(id);
  if (it == index_.end()) return false;
  placed_.erase(placed_.begin() + static_cast<std::ptrdiff_t>(it->second));
  reindex();  // erase shifts every later position; remove is not on the hot path
  return true;
}

PlacedModule* Station::find(InstanceId id) {
  const auto it = index_.find(id);
  return it == index_.end() ? nullptr : &placed_[it->second];
}

const PlacedModule* Station::find(InstanceId id) const {
  const auto it = index_.find(id);
  return it == index_.end() ? nullptr : &placed_[it->second];
}

void Station::reindex() {
  index_.clear();
  index_.reserve(placed_.size());
  for (std::size_t i = 0; i < placed_.size(); ++i) index_[placed_[i].instanceId] = i;
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
