#include "x4sb/assetpipe/component.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace x4sb {
namespace {

// Collapse internal runs of whitespace and trim ends: "snap  " -> "snap",
// "part detail_m  " -> "part detail_m".
std::string normalizeTags(const std::string& raw) {
  std::istringstream in(raw);
  std::string tok, out;
  while (in >> tok) {
    if (!out.empty()) out += ' ';
    out += tok;
  }
  return out;
}

bool hasToken(const std::string& tags, const char* token) {
  std::istringstream in(tags);
  std::string tok;
  while (in >> tok)
    if (tok == token) return true;
  return false;
}

Transform parseOffset(const pugi::xml_node& conn) {
  Transform t;  // identity (position 0, rotation 1,0,0,0)
  const pugi::xml_node off = conn.child("offset");
  if (!off) return t;
  if (const pugi::xml_node p = off.child("position")) {
    t.position = {p.attribute("x").as_double(), p.attribute("y").as_double(),
                  p.attribute("z").as_double()};
  }
  if (const pugi::xml_node q = off.child("quaternion")) {
    // X4 attribute order is qx,qy,qz,qw; our Quat is {w,x,y,z}.
    t.rotation = {q.attribute("qw").as_double(1.0), q.attribute("qx").as_double(),
                  q.attribute("qy").as_double(), q.attribute("qz").as_double()};
  }
  return t;
}

}  // namespace

std::vector<ComponentConnection> parseComponentConnections(const std::string& componentXml) {
  std::vector<ComponentConnection> out;
  pugi::xml_document doc;
  if (!doc.load_string(componentXml.c_str())) return out;

  // <components> may hold several <component>s; collect connections from each.
  for (const pugi::xml_node comp : doc.child("components").children("component")) {
    for (const pugi::xml_node c : comp.child("connections").children("connection")) {
      ComponentConnection cc;
      cc.name = c.attribute("name").as_string();
      cc.tags = normalizeTags(c.attribute("tags").as_string());
      cc.offset = parseOffset(c);
      if (!cc.name.empty()) out.push_back(std::move(cc));
    }
  }
  return out;
}

bool isSnapConnection(const ComponentConnection& c) { return hasToken(c.tags, "snap"); }

std::vector<ConnectionPoint> snapConnectionPoints(const std::string& componentXml) {
  std::vector<ConnectionPoint> out;
  for (const auto& c : parseComponentConnections(componentXml)) {
    if (!isSnapConnection(c)) continue;
    ConnectionPoint cp;
    cp.id = c.name;
    cp.localPosition = c.offset.position;
    cp.localRotation = c.offset.rotation;
    cp.type = c.tags;
    out.push_back(std::move(cp));
  }
  return out;
}

ComponentGeometry parseComponentGeometry(const std::string& componentXml) {
  ComponentGeometry geo;
  pugi::xml_document doc;
  if (!doc.load_string(componentXml.c_str())) return geo;

  const pugi::xml_node comp = doc.child("components").child("component");
  std::string folder = comp.child("source").attribute("geometry").as_string();
  std::replace(folder.begin(), folder.end(), '\\', '/');  // X4 uses backslashes
  geo.geometryFolder = folder;

  for (const pugi::xml_node c : comp.child("connections").children("connection")) {
    const std::string tags = normalizeTags(c.attribute("tags").as_string());
    if (!hasToken(tags, "part")) continue;
    const Transform offset = parseOffset(c);
    for (const pugi::xml_node part : c.child("parts").children("part")) {
      ComponentPart cp;
      cp.name = part.attribute("name").as_string();
      cp.offset = offset;
      if (!cp.name.empty()) geo.parts.push_back(std::move(cp));
    }
  }
  return geo;
}

Category categoryFromClass(const std::string& className) {
  if (className == "production") return Category::Production;
  if (className == "storage" || className == "storagemodule") return Category::Storage;
  if (className == "habitation") return Category::Habitat;
  if (className == "dockarea" || className == "pier") return Category::Dock;
  if (className == "defence") return Category::Defense;
  if (className == "connectionmodule") return Category::Connector;
  return Category::Other;
}

MacroInfo parseMacro(const std::string& macroXml) {
  MacroInfo info;
  pugi::xml_document doc;
  if (!doc.load_string(macroXml.c_str())) return info;

  const pugi::xml_node macro = doc.child("macros").child("macro");
  info.macroName = macro.attribute("name").as_string();
  info.className = macro.attribute("class").as_string();
  info.category = categoryFromClass(info.className);
  info.componentRef = macro.child("component").attribute("ref").as_string();
  info.makerRace =
      macro.child("properties").child("identification").attribute("makerrace").as_string();
  return info;
}

}  // namespace x4sb
