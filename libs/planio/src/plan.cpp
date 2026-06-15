#include "x4sb/planio/plan.hpp"

#include "x4sb/coords/coords.hpp"

#include <pugixml.hpp>

#include <cmath>
#include <fstream>
#include <sstream>

namespace x4sb {
namespace {

// X4 plans omit rotation/position components that are zero; treat anything below
// this (degrees) as no rotation so a pure-yaw plan re-exports as pure yaw.
constexpr double kAngleEps = 1e-4;

// Locate the first <plan> element, whether the document root is <plan> itself or
// a <plans> aggregate (as in the in-game constructionplans.xml).
pugi::xml_node firstPlan(const pugi::xml_document& doc) {
  if (pugi::xml_node root = doc.child("plan")) return root;
  return doc.child("plans").child("plan");
}

Transform readOffset(const pugi::xml_node& entry) {
  Transform t;  // identity
  const pugi::xml_node off = entry.child("offset");
  if (!off) return t;
  if (const pugi::xml_node p = off.child("position")) {
    const Vec3 x4{p.attribute("x").as_double(), p.attribute("y").as_double(),
                  p.attribute("z").as_double()};
    t.position = x4ToApp(x4);
  }
  if (const pugi::xml_node r = off.child("rotation")) {
    t.rotation = quatFromX4Euler(r.attribute("yaw").as_double(), r.attribute("pitch").as_double(),
                                 r.attribute("roll").as_double());
  }
  return t;
}

void writeOffset(pugi::xml_node entry, const Transform& world) {
  const Transform x4 = appToX4(world);
  pugi::xml_node off = entry.append_child("offset");
  pugi::xml_node pos = off.append_child("position");
  pos.append_attribute("x") = x4.position.x;
  pos.append_attribute("y") = x4.position.y;
  pos.append_attribute("z") = x4.position.z;

  double yaw, pitch, roll;
  x4EulerFromQuat(x4.rotation, yaw, pitch, roll);
  if (std::abs(yaw) > kAngleEps || std::abs(pitch) > kAngleEps || std::abs(roll) > kAngleEps) {
    pugi::xml_node rot = off.append_child("rotation");
    if (std::abs(yaw) > kAngleEps) rot.append_attribute("yaw") = yaw;
    if (std::abs(pitch) > kAngleEps) rot.append_attribute("pitch") = pitch;
    if (std::abs(roll) > kAngleEps) rot.append_attribute("roll") = roll;
  }
}

}  // namespace

// Serialize a Station to a single-<plan> construction-plan document (the format
// X4 imports from the constructionplan folder). spec §8.
std::string exportPlanXml(const Station& station, const ModuleCatalog& catalog) {
  (void)catalog;  // reserved for macro validation / DLC <patches> emission later
  pugi::xml_document doc;
  pugi::xml_node plan = doc.append_child("plan");
  plan.append_attribute("id") = "x4sb_plan";
  plan.append_attribute("name") = "x4sb export";
  plan.append_attribute("description") = "";

  for (const auto& m : station.modules()) {
    pugi::xml_node entry = plan.append_child("entry");
    entry.append_attribute("index") = static_cast<long long>(m.instanceId);
    entry.append_attribute("macro") = m.defId.c_str();
    // This module's own connector + which earlier entry it attaches to.
    if (!m.links.empty()) {
      const Link& l = m.links.front();
      entry.append_attribute("connection") = l.thisPointId.c_str();
      pugi::xml_node pred = entry.append_child("predecessor");
      pred.append_attribute("index") = static_cast<long long>(l.otherInstanceId);
      pred.append_attribute("connection") = l.otherPointId.c_str();
    }
    writeOffset(entry, m.worldTransform);
  }

  std::ostringstream ss;
  doc.save(ss, "  ");
  return ss.str();
}

std::optional<Station> importPlanXml(const std::string& xml) {
  pugi::xml_document doc;
  if (!doc.load_string(xml.c_str())) return std::nullopt;
  const pugi::xml_node plan = firstPlan(doc);
  if (!plan) return std::nullopt;

  Station station;
  for (const pugi::xml_node entry : plan.children("entry")) {
    PlacedModule m;
    m.instanceId = entry.attribute("index").as_ullong();
    m.defId = entry.attribute("macro").as_string();
    m.worldTransform = readOffset(entry);

    // A <predecessor> + this entry's `connection` define one connection-graph link.
    const std::string conn = entry.attribute("connection").as_string();
    if (const pugi::xml_node pred = entry.child("predecessor"); pred && !conn.empty()) {
      Link l;
      l.thisPointId = conn;
      l.otherInstanceId = pred.attribute("index").as_ullong();
      l.otherPointId = pred.attribute("connection").as_string();
      m.links.push_back(std::move(l));
    }
    station.add(m);
  }
  return station;
}

bool writePlanFile(const Station& station, const ModuleCatalog& catalog, const std::string& path) {
  std::ofstream out(path);
  if (!out) return false;
  out << exportPlanXml(station, catalog);
  return static_cast<bool>(out);
}

}  // namespace x4sb
