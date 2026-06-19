#include "x4sb/assetpipe/component.hpp"

#include "strutil.hpp"
#include "x4sb/data/math.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <sstream>
#include <string_view>

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

// Dock keep-clear box half-extents (metres) per ship class, from X4's
// libraries/parameters.xml <exclusionzones>: there x/y are the full cross-section and
// z the full outward length, so each is halved here. The module's exclusionzone
// connection carries the matching ship_<size> tag; dock_<size> is accepted too for
// the launch->todock fallback. L/XL run 100 km (the whole capital-ship flight path
// out of the plot) while M and below are short — matching in-game measurement.
struct ClearanceBox {
  double halfW;
  double halfH;
  double halfDepth;
  std::string label;
};
ClearanceBox clearanceBox(const std::string& tags) {
  if (detail::hasToken(tags, "ship_xl") || detail::hasToken(tags, "dock_xl"))
    return {2000, 2000, 50000, "xl"};  // 4000 x 4000 x 100000
  if (detail::hasToken(tags, "ship_l") || detail::hasToken(tags, "dock_l"))
    return {600, 600, 50000, "l"};  // 1200 x 1200 x 100000
  if (detail::hasToken(tags, "ship_m") || detail::hasToken(tags, "dock_m"))
    return {90, 90, 250, "m"};  // 180 x 180 x 500
  if (detail::hasToken(tags, "ship_s") || detail::hasToken(tags, "dock_s"))
    return {35, 35, 200, "s"};  // 70 x 70 x 400
  if (detail::hasToken(tags, "ship_xs") || detail::hasToken(tags, "dock_xs"))
    return {35, 35, 200, "xs"};  // 70 x 70 x 400
  return {90, 90, 250, "m"};  // default to M-class when unclassified
}

// Unit quaternion rotating local +Z onto `dir` (unit). The clearance OBB's local
// Z is its corridor (long) axis, so this orients a computed launch->dock corridor.
Quat quatFromUnitZTo(Vec3 dir) {
  const double d = dir.z;                          // dot({0,0,1}, dir)
  if (d > 1.0 - 1e-9) return Quat{};               // already +Z
  if (d < -1.0 + 1e-9) return Quat{0, 1, 0, 0};    // 180deg about X flips +Z to -Z
  const Vec3 c = cross(Vec3{0, 0, 1}, dir);
  const Quat q{1.0 + d, c.x, c.y, c.z};
  const double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  return {q.w / n, q.x / n, q.y / n, q.z / n};
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

bool isVisualPart(std::string_view name) {
  const auto startsWith = [name](std::string_view p) { return name.substr(0, p.size()) == p; };
  const auto contains = [name](std::string_view s) {
    return name.find(s) != std::string_view::npos;
  };

  // Effects (decals/glows/fieldlines/lightcones), particle emitters, and
  // scale/editor dummies never render as solid geometry. The "fx" token can sit
  // under a visual prefix too ("part_fx_lightcones", "anim_fx_fire"), so match it
  // anywhere as well as the plain "fx_" prefix.
  if (startsWith("fx_") || contains("_fx_") || startsWith("emitterfx") ||
      startsWith("scaledummy") || startsWith("editor")) {
    return false;
  }
  // Pure non-visual volumes (collision/bounds/trigger/darkening). The token can
  // sit under a visual prefix (e.g. "detail_l_radiator_collisionbox"), so match
  // it anywhere in the name rather than only as a prefix.
  if (contains("collisionbox") || contains("bounding") || contains("damagearea") ||
      contains("triggerpart") || contains("darkening") || name == "geometryinvisible") {
    return false;
  }
  return true;
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
        cp.ref = part.attribute("ref").as_string();  // xref to a shared sub-assembly, if any
        cp.offset = offset;
        // Drop effects and non-visual helpers here, the one chokepoint both the
        // catalog (mesh-refs) and the converter walk, so neither references them.
        if (!cp.name.empty() && isVisualPart(cp.name)) geo.parts.push_back(std::move(cp));
      });
  return geo;
}

std::optional<PartRef> parsePartRef(std::string_view ref) {
  const auto dot = ref.find('.');  // "<component>.<part>"; names hold no '.', so first split is exact
  if (dot == std::string_view::npos || dot == 0 || dot + 1 >= ref.size()) return std::nullopt;
  return PartRef{std::string(ref.substr(0, dot)), std::string(ref.substr(dot + 1))};
}

std::string partXmfPath(const std::string& geometryFolder, const std::string& partName) {
  return partXmfPathLod(geometryFolder, partName, 0);
}

std::string partXmfPathLod(const std::string& geometryFolder, const std::string& partName,
                           int lod) {
  return geometryFolder + "/" + partName + "-lod" + std::to_string(lod) + ".xmf";
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

std::string largestDockSize(const std::string& docksizeTags) {
  static const std::array<const char*, 5> order{{"dock_xl", "dock_l", "dock_m", "dock_s", "dock_xs"}};
  for (const char* s : order)
    if (detail::hasToken(docksizeTags, s)) return s;
  return {};
}

std::vector<ClearanceVolume> extractClearanceVolumes(
    const std::string& macroXml, const std::string& componentXml,
    const std::function<std::optional<std::string>(const std::string&)>& resolveMacroXml) {
  // X4's authoritative no-build box is the module component's <connection
  // tags="exclusionzone ship_<size>"> marker (piers, dockareas, build modules): the
  // marker gives the pose, and libraries/parameters.xml <exclusionzones> gives the
  // box dimensions per ship_<size> (see clearanceBox). Build modules ALSO carry
  // launchpos/todock ship-pathing markers, but those are not the keep-clear box, so
  // exclusionzone takes priority; the launch->todock corridor is only a fallback for
  // docks that declare no exclusionzone. The macro/dockingbay chain isn't needed.
  (void)macroXml;
  (void)resolveMacroXml;  // reserved for a future surface-dock (no-marker) fallback
  std::vector<ClearanceVolume> out;
  const std::vector<ComponentConnection> conns = parseComponentConnections(componentXml);

  // Authoritative path: one box per exclusionzone marker. The no-build zone starts AT
  // the connection centre (the marker) and extends OUTWARD along the connection's
  // local +Z — the side ships dock/approach from. (v1 ran origin->marker, i.e. inward
  // over the structure; that was the wrong side.)
  for (const ComponentConnection& c : conns) {
    if (!detail::hasToken(c.tags, "exclusionzone")) continue;
    const ClearanceBox cb = clearanceBox(c.tags);
    const Vec3 outward = rotate(c.offset.rotation, Vec3{0, 0, 1});
    ClearanceVolume cv;
    cv.rotation = c.offset.rotation;
    cv.halfExtents = {cb.halfW, cb.halfH, cb.halfDepth};
    cv.center = c.offset.position + outward * cb.halfDepth;
    cv.shipSize = cb.label;
    out.push_back(std::move(cv));
  }
  if (!out.empty()) return out;

  // Fallback for docks with no exclusionzone marker (small surface docks, S/M
  // dockareas/dockingbays): a single modeled corridor spanning launchpos -> todock.
  // These components carry no ship_/dock_ tag, so clearanceBox defaults to M; the
  // corridor length is the modeled launch->dock distance rather than a class depth.
  const ComponentConnection* todock = nullptr;
  const ComponentConnection* launch = nullptr;
  for (const ComponentConnection& c : conns) {
    if (detail::hasToken(c.tags, "todock")) todock = &c;
    if (detail::hasToken(c.tags, "launchpos")) launch = &c;
  }
  if (todock != nullptr && launch != nullptr) {
    const ClearanceBox cb = clearanceBox("");
    const Vec3 a = todock->offset.position;
    const Vec3 b = launch->offset.position;
    const Vec3 d = a - b;
    const double len = length(d);
    if (len > 1.0) {
      ClearanceVolume cv;
      cv.rotation = quatFromUnitZTo(d * (1.0 / len));
      cv.halfExtents = {cb.halfW, cb.halfH, len * 0.5};
      cv.center = (a + b) * 0.5;
      cv.shipSize = cb.label;
      out.push_back(std::move(cv));
    }
  }
  return out;
}

}  // namespace x4sb
