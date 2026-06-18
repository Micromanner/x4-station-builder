#include "x4sb/editorcore/mesh_paths.hpp"

#include <algorithm>
#include <set>

namespace x4sb {

std::vector<std::string> meshPathsFor(const Station& station, const ModuleCatalog& catalog) {
  std::set<std::string> unique;  // dedups + orders in one pass
  for (const PlacedModule& pm : station.modules()) {
    const ModuleDef* def = catalog.find(pm.defId);
    if (def == nullptr) continue;
    for (const MeshRef& ref : def->meshRefs) {
      if (!ref.gltfPath.empty()) unique.insert(ref.gltfPath);
    }
  }
  return {unique.begin(), unique.end()};
}

}  // namespace x4sb
