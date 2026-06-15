#include "x4sb/data/catalog.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace x4sb {
namespace {

using nlohmann::json;

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
        mr.gltfPath = r.value("gltfPath", std::string{});
        if (r.contains("localTransform")) {
          const json& t = r.at("localTransform");
          mr.localTransform.position = readVec3(t.value("position", json::array()));
          mr.localTransform.rotation = readQuat(t.value("rotation", json::array()));
        }
        d.meshRefs.push_back(std::move(mr));
      }
    }
    if (!d.id.empty()) cat.add(d);
  }
  return cat;
}

std::optional<ModuleCatalog> ModuleCatalog::loadFromFile(const std::string& path) {
  const std::ifstream in(path);
  if (!in) return std::nullopt;
  std::stringstream ss;
  ss << in.rdbuf();
  try {
    return loadFromJson(ss.str());
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

const ModuleDef* ModuleCatalog::find(const std::string& macroId) const {
  const auto it = modules_.find(macroId);
  return it == modules_.end() ? nullptr : &it->second;
}

}  // namespace x4sb
