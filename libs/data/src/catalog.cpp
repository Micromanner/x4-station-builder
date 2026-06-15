#include "x4sb/data/catalog.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

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
    case Category::Production: return "production";
    case Category::Storage: return "storage";
    case Category::Habitat: return "habitat";
    case Category::Dock: return "dock";
    case Category::Defense: return "defense";
    case Category::Connector: return "connector";
    case Category::Other: break;
  }
  return "other";
}

json writeVec3(const Vec3& v) { return json::array({v.x, v.y, v.z}); }
json writeQuat(const Quat& q) { return json::array({q.w, q.x, q.y, q.z}); }

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
      refs.push_back({{"gltfPath", r.gltfPath},
                      {"localTransform",
                       {{"position", writeVec3(r.localTransform.position)},
                        {"rotation", writeQuat(r.localTransform.rotation)}}}});
    }
    m["meshRefs"] = std::move(refs);
    mods.push_back(std::move(m));
  }
  return json{{"modules", std::move(mods)}}.dump(2);
}

bool writeCatalogFile(const std::vector<ModuleDef>& modules, const std::string& path) {
  std::ofstream out(path);
  if (!out) return false;
  out << toCatalogJson(modules);
  out.flush();
  return out.good();
}

}  // namespace x4sb
