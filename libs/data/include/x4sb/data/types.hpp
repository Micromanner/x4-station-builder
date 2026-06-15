#pragma once
// Domain data types loaded from the asset cache (spec §5). Pure data.
#include "x4sb/data/math.hpp"

#include <string>
#include <vector>

namespace x4sb {

enum class Category {
  Production,
  Storage,
  Habitat,
  Dock,
  Defense,
  Connector,
  Other,
};

struct ConnectionPoint {
  std::string id;
  Vec3 localPosition{};  // relative to module origin
  Quat localRotation{};  // connector facing / normal
  std::string type;      // compatibility tag from the component XML
};

struct MeshRef {
  std::string gltfPath;
  Transform localTransform{};  // a module may assemble several parts
};

struct ModuleDef {
  std::string id;  // macro id
  std::string name;
  std::string faction;
  Category category{Category::Other};
  std::vector<ConnectionPoint> connectionPoints;
  std::vector<MeshRef> meshRefs;
  AABB aabb{};  // derived bounding box, in module-local space
};

}  // namespace x4sb
