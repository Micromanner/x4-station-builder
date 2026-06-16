#include "x4sb/assetpipe/component.hpp"

#include "strutil.hpp"

#include <pugixml.hpp>

#include <algorithm>
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

Vec3 readXyz(const pugi::xml_node& n) {
  return {n.attribute("x").as_double(), n.attribute("y").as_double(),
          n.attribute("z").as_double()};
}

// Normalize a '/'-separated logical folder: collapse runs of '/' to one and
// strip a trailing '/'. Some component <source geometry> values are authored
// with a stray double-backslash (e.g. "...\landmarks\\foo_data"), which becomes
// "...landmarks//foo_data" after the '\'->'/' pass and no longer matches the
// archive entry (stored single-slash). Single-slash paths pass through unchanged.
std::string normalizeFolder(const std::string& raw) {
  std::string out = detail::collapseSlashes(raw);
  if (!out.empty() && out.back() == '/') out.pop_back();
  return out;
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

// Visit every <part> of the first <component>'s "part"-tagged connections,
// invoking fn(part, connectionOffset, connectionTags). Shared by the geometry
// and bounding-box passes so their connection/part traversal can't diverge.
template <typename Fn>
void forEachComponentPart(const pugi::xml_node& comp, Fn fn) {
  for (const pugi::xml_node c : comp.child("connections").children("connection")) {
    const std::string tags = normalizeTags(c.attribute("tags").as_string());
    if (!detail::hasToken(tags, "part")) continue;
    const Transform offset = parseOffset(c);
    for (const pugi::xml_node part : c.child("parts").children("part")) fn(part, offset, tags);
  }
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

bool isSnapConnection(const ComponentConnection& c) { return detail::hasToken(c.tags, "snap"); }

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
  // Collapse a stray '//' (double-backslash in the source) and trailing '/'.
  geo.geometryFolder = normalizeFolder(folder);

  forEachComponentPart(
      comp, [&geo](const pugi::xml_node& part, const Transform& offset, const std::string&) {
        ComponentPart cp;
        cp.name = part.attribute("name").as_string();
        cp.offset = offset;
        if (!cp.name.empty()) geo.parts.push_back(std::move(cp));
      });
  return geo;
}

std::string partXmfPath(const std::string& geometryFolder, const std::string& partName) {
  return geometryFolder + "/" + partName + "-lod0.xmf";
}

AABB moduleAabb(const std::string& componentXml) {
  AABB box;
  bool any = false;
  pugi::xml_document doc;
  if (!doc.load_string(componentXml.c_str())) return box;

  const pugi::xml_node comp = doc.child("components").child("component");
  forEachComponentPart(comp, [&](const pugi::xml_node& part, const Transform& offset,
                                 const std::string& tags) {
    if (detail::hasToken(tags, "nocollision")) return;  // detail/anim/fx parts overstate their size box
    const pugi::xml_node size = part.child("size");
    if (!size) return;
    const Vec3 half = readXyz(size.child("max"));
    const Vec3 ctr = readXyz(size.child("center"));
    for (int sx = -1; sx <= 1; sx += 2) {
      for (int sy = -1; sy <= 1; sy += 2) {
        for (int sz = -1; sz <= 1; sz += 2) {
          const Vec3 corner{ctr.x + half.x * static_cast<double>(sx),
                            ctr.y + half.y * static_cast<double>(sy),
                            ctr.z + half.z * static_cast<double>(sz)};
          const Vec3 world = apply(offset, corner);
          if (!any) {
            box.min = world;
            box.max = world;
            any = true;
          } else {
            expand(box, world);
          }
        }
      }
    }
  });
  return box;
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
