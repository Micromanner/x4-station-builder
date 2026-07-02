#include "x4sb/autolayout/roles.hpp"

#include <array>
#include <string_view>

namespace x4sb {
namespace {

bool startsWith(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}
bool contains(std::string_view s, std::string_view sub) {
  return s.find(sub) != std::string_view::npos;
}

// The real catalog has no solar category; energy modules are Category::Production and
// only their id distinguishes them. One table so the heuristic is editable in one place
// (spec §6 risk: id heuristics break on renames — long-term fix is a catalog-gen tag).
constexpr std::array<std::string_view, 3> kSolarMarkers{
    {"energycells", "solarpanel", "sunriseflowers"}};

}  // namespace

Role classifyRole(const ModuleDef& def) {
  if (!def.clearanceVolumes.empty()) return Role::Dock;     // docks/piers carry corridors
  if (startsWith(def.id, "defence_")) return Role::Turret;  // turrets are Category::Other
  for (const std::string_view marker : kSolarMarkers)
    if (contains(def.id, marker)) return Role::Solar;
  return Role::Interior;
}

}  // namespace x4sb
