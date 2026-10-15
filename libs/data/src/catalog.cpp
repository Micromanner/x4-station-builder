#include "x4sb/data/catalog.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <optional>
#include <string_view>
#include <utility>

namespace x4sb {

using nlohmann::json;

namespace {

Category parseCategory(const std::string& s) {
  if (s == "production") return Category::Production;
  if (s == "storage") return Category::Storage;
  if (s == "habitat") return Category::Habitat;
  if (s == "dock") return Category::Dock;
  if (s == "defense") return Category::Defense;
  if (s == "connector") return Category::Connector;
  return Category::Other;
}

Vec3 readVec3(const json& j) {
  if (j.is_array() && j.size() >= 3)
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
  return {};
}

Quat readQuat(const json& j) {
  if (j.is_array() && j.size() >= 4)
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>(), j[3].get<double>()};
  return {};
}

std::string categoryToString(Category c) {
  switch (c) {
    case Category::Production:
      return "production";
    case Category::Storage:
      return "storage";
    case Category::Habitat:
      return "habitat";
    case Category::Dock:
      return "dock";
    case Category::Defense:
      return "defense";
    case Category::Connector:
      return "connector";
    case Category::Other:
      break;
  }
  return "other";
}

json writeVec3(const Vec3& v) { return json::array({v.x, v.y, v.z}); }
json writeQuat(const Quat& q) { return json::array({q.w, q.x, q.y, q.z}); }

constexpr std::array<std::pair<LightKind, std::string_view>, 4> kLightKinds{{
    {LightKind::Omni, "omni"},
    {LightKind::Spot, "spot"},
    {LightKind::Box, "box"},
    {LightKind::Area, "area"},
}};

std::optional<LightKind> parseLightKind(std::string_view s) {
  for (const auto& [kind, name] : kLightKinds)
    if (name == s) return kind;
  return std::nullopt;
}

std::string lightKindName(LightKind k) {
  for (const auto& [kind, name] : kLightKinds)
    if (kind == k) return std::string(name);
  return "omni";
}

}  // namespace

// Tolerant parse of the catalog schema. The exact schema is finalized once the
// asset pipeline lands (spec §4A); missing fields default rather than throw.
ModuleCatalog ModuleCatalog::loadFromJson(const std::string& text) {
  ModuleCatalog cat;
  const json root = json::parse(text);
  const json& mods = root.contains("modules") ? root.at("modules") : root;

  for (const auto& m : mods) {
    ModuleDef d;
    d.id = m.value("id", std::string{});
    d.name = m.value("name", std::string{});
    d.faction = m.value("faction", std::string{});
    d.category = parseCategory(m.value("category", std::string{"other"}));
    d.wareId = m.value("wareId", std::string{});
    d.nameRef = m.value("nameRef", std::string{});
    d.playerBuildable = m.value("playerBuildable", true);

    if (m.contains("aabb")) {
      const json& bb = m.at("aabb");
      d.aabb.min = readVec3(bb.value("min", json::array()));
      d.aabb.max = readVec3(bb.value("max", json::array()));
    }
    if (m.contains("connectionPoints")) {
      for (const auto& c : m.at("connectionPoints")) {
        ConnectionPoint cp;
        cp.id = c.value("id", std::string{});
        cp.localPosition = readVec3(c.value("localPosition", json::array()));
        cp.localRotation = readQuat(c.value("localRotation", json::array()));
        cp.type = c.value("type", std::string{});
        d.connectionPoints.push_back(std::move(cp));
      }
    }
    if (m.contains("meshRefs")) {
      for (const auto& r : m.at("meshRefs")) {
        MeshRef mr;
        mr.meshKey = r.value("meshKey", std::string{});
        mr.radius = r.value("radius", 0.0);
        mr.xmfPath = r.value("xmfPath", std::string{});
        mr.materialRef = r.value("materialRef", std::string{});
        if (r.contains("uvAnimation")) mr.uvAnimation = uvAnimationFromJson(r.at("uvAnimation"));
        const int detail = r.value("detail", 0);
        if (detail > 0 && detail <= static_cast<int>(DetailClass::XL))
          mr.detail = static_cast<DetailClass>(detail);
        mr.effect = r.value("effect", false);
        if (r.contains("localTransform")) {
          const json& t = r.at("localTransform");
          mr.localTransform.position = readVec3(t.value("position", json::array()));
          mr.localTransform.rotation = readQuat(t.value("rotation", json::array()));
        }
        d.meshRefs.push_back(std::move(mr));
      }
    }
    if (m.contains("clearanceVolumes")) {
      for (const auto& v : m.at("clearanceVolumes")) {
        ClearanceVolume cv;
        cv.center = readVec3(v.value("center", json::array()));
        cv.rotation = readQuat(v.value("rotation", json::array()));
        cv.halfExtents = readVec3(v.value("halfExtents", json::array()));
        cv.shipSize = v.value("shipSize", std::string{});
        d.clearanceVolumes.push_back(std::move(cv));
      }
    }
    if (m.contains("lights")) {
      for (const auto& v : m.at("lights")) {
        const std::optional<LightKind> kind = parseLightKind(v.value("kind", std::string{}));
        if (!kind) continue;
        LocalLight l;
        l.kind = *kind;
        l.local.position = readVec3(v.value("position", json::array()));
        l.local.rotation = readQuat(v.value("rotation", json::array()));
        l.color = readVec3(v.value("color", json::array()));
        l.intensity = v.value("intensity", 1.0);
        l.specularIntensity = v.value("specularIntensity", 1.0);
        l.range = v.value("range", 0.0);
        l.spotAngle = v.value("spotAngle", 0.0);
        l.boxSize = readVec3(v.value("boxSize", json::array()));
        l.areaX = v.value("areaX", 0.0);
        l.areaY = v.value("areaY", 0.0);
        l.areaRadius = v.value("areaRadius", 0.0);
        l.spotAttenuation = v.value("spotAttenuation", 0.0);
        d.lights.push_back(l);
      }
    }
    if (!d.id.empty()) cat.add(d);
  }
  return cat;
}

const ModuleDef* ModuleCatalog::find(const std::string& macroId) const {
  const auto it = modules_.find(macroId);
  return it == modules_.end() ? nullptr : &it->second;
}

std::string toCatalogJson(const std::vector<ModuleDef>& modules) {
  json mods = json::array();
  for (const ModuleDef& d : modules) {
    json m;
    m["id"] = d.id;
    m["name"] = d.name;
    m["wareId"] = d.wareId;
    m["nameRef"] = d.nameRef;
    m["faction"] = d.faction;
    m["category"] = categoryToString(d.category);
    m["playerBuildable"] = d.playerBuildable;
    m["aabb"] = {{"min", writeVec3(d.aabb.min)}, {"max", writeVec3(d.aabb.max)}};

    json pts = json::array();
    for (const ConnectionPoint& cp : d.connectionPoints) {
      pts.push_back({{"id", cp.id},
                     {"localPosition", writeVec3(cp.localPosition)},
                     {"localRotation", writeQuat(cp.localRotation)},
                     {"type", cp.type}});
    }
    m["connectionPoints"] = std::move(pts);

    json refs = json::array();
    for (const MeshRef& r : d.meshRefs) {
      json ref = {{"meshKey", r.meshKey},
                  {"xmfPath", r.xmfPath},
                  {"materialRef", r.materialRef},
                  {"radius", r.radius},
                  {"localTransform",
                   {{"position", writeVec3(r.localTransform.position)},
                    {"rotation", writeQuat(r.localTransform.rotation)}}}};
      if (!r.uvAnimation.channels.empty()) ref["uvAnimation"] = uvAnimationToJson(r.uvAnimation);
      if (r.detail != DetailClass::None) ref["detail"] = static_cast<int>(r.detail);
      if (r.effect) ref["effect"] = true;
      refs.push_back(std::move(ref));
    }
    m["meshRefs"] = std::move(refs);

    json vols = json::array();
    for (const ClearanceVolume& v : d.clearanceVolumes) {
      vols.push_back({{"center", writeVec3(v.center)},
                      {"rotation", writeQuat(v.rotation)},
                      {"halfExtents", writeVec3(v.halfExtents)},
                      {"shipSize", v.shipSize}});
    }
    m["clearanceVolumes"] = std::move(vols);

    json lights = json::array();
    for (const LocalLight& l : d.lights) {
      lights.push_back({{"kind", lightKindName(l.kind)},
                        {"position", writeVec3(l.local.position)},
                        {"rotation", writeQuat(l.local.rotation)},
                        {"color", writeVec3(l.color)},
                        {"intensity", l.intensity},
                        {"specularIntensity", l.specularIntensity},
                        {"range", l.range},
                        {"spotAngle", l.spotAngle},
                        {"boxSize", writeVec3(l.boxSize)},
                        {"areaX", l.areaX},
                        {"areaY", l.areaY},
                        {"areaRadius", l.areaRadius},
                        {"spotAttenuation", l.spotAttenuation}});
    }
    m["lights"] = std::move(lights);
    mods.push_back(std::move(m));
  }
  return json{{"modules", std::move(mods)}}.dump(2);
}

std::string meshKeyLod(const std::string& meshKey, int lod) {
  if (lod <= 0) return meshKey;
  return meshKey + "-lod" + std::to_string(lod);
}

}  // namespace x4sb
