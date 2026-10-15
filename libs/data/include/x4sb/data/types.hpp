#pragma once
// Domain data types loaded from the asset cache (spec §5). Pure data.
#include "x4sb/data/math.hpp"
#include "x4sb/data/uv_animation.hpp"

#include <cstdint>
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

// X4's detail class of a part, from its connection's detail_* tag: the engine
// hides small classes first as a part shrinks on screen. None = always drawn.
enum class DetailClass : std::uint8_t { None, XS, S, M, L, XL };

struct MeshRef {
  std::string meshKey;         // part identity "<moduleId>__<partName>" (meshKeyFor)
  Transform localTransform{};  // a module may assemble several parts
  // Half-diagonal of the part's own <size> box; the editor's per-part LOD pick
  // projects it. 0 when the component declares none (falls back to the module).
  double radius{};
  // lod0 XMF archive address and the material for XMFs without a material table.
  std::string xmfPath;
  std::string materialRef;
  UvAnimation uvAnimation;
  DetailClass detail{DetailClass::None};
  // An fx_* part: only its material ranges the renderer ports draw.
  bool effect{false};
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

// An authored local light (a component's <lights>), in module-local space. Values
// are X4's raw attributes; the renderer applies X4's decode (colour gamma, falloff
// per kind). Local axes: spot and area lights emit along -Y, an area light spans X
// (right) by Z (up), a box light's emitting line runs along X.
enum class LightKind { Omni, Spot, Box, Area };

struct LocalLight {
  LightKind kind{LightKind::Omni};
  Transform local{};
  Vec3 color{255, 255, 255};  // r, g, b in 0-255
  double intensity{1.0};
  double specularIntensity{1.0};
  double range{};      // 0 on box lights that author none
  double spotAngle{};  // spot: cone angle, degrees
  Vec3 boxSize{};      // box: full extents
  double areaX{};      // area: plane/tube extents and sphere radius
  double areaY{};
  double areaRadius{};
  double spotAttenuation{};  // area: directional cutoff, <= 0 = omnidirectional
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
  std::vector<LocalLight> lights;
  AABB aabb{};  // derived bounding box, in module-local space
};

// Player-facing label: the localized name when present, else the macro id. Name
// is empty for unresolved refs or catalogs built before the localization pass.
[[nodiscard]] inline const std::string& displayName(const ModuleDef& m) {
  return m.name.empty() ? m.id : m.name;
}

}  // namespace x4sb
