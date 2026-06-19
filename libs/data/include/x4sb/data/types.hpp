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

// A synthesized dock/cradle clearance volume (design §4.1): an oriented box in
// module-local space whose long (local Z) axis is the ship approach/launch
// corridor. X4 ships no such box — it is reconstructed in the pipeline from the
// dock connection pose, ship-size class, and corridor markers.
struct ClearanceVolume {
  Vec3 center{};
  Quat rotation{};
  Vec3 halfExtents{};
  std::string shipSize;  // "dock_xs".."dock_xl" (provenance / future per-size rules)
};

struct ModuleDef {
  std::string id;       // macro name (catalog key; plans reference modules by macro)
  std::string name;     // display name (empty until the localization pass)
  std::string wareId;   // e.g. "module_arg_prod_foodrations_01"
  std::string nameRef;  // raw localization ref "{page,id}"
  std::string faction;  // makerrace
  Category category{Category::Other};
  bool playerBuildable{true};
  std::vector<ConnectionPoint> connectionPoints;
  std::vector<MeshRef> meshRefs;
  std::vector<ClearanceVolume> clearanceVolumes;  // dock/cradle clear volumes (design §3.1)
  AABB aabb{};  // derived bounding box, in module-local space
};

}  // namespace x4sb
