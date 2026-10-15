#include "x4sb/assetpipe/component.hpp"

#include "strutil.hpp"
#include "x4sb/data/math.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>

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
  return {n.attribute("x").as_double(), n.attribute("y").as_double(), n.attribute("z").as_double()};
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

// Real parts author a few dozen keys; the cap bounds a hostile file.
constexpr std::size_t kMaxUvKeys = 1024;

std::vector<UvKey> parseUvKeys(const pugi::xml_node& prop) {
  const std::string controller = prop.attribute("controller").as_string();
  const bool bezier = controller == "bezier_float";
  if (!bezier && controller != "linear_float") return {};
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  std::vector<UvKey> keys;
  for (const pugi::xml_node k : prop.children("key")) {
    if (keys.size() == kMaxUvKeys) return {};
    const double frame = k.attribute("frame").as_double(kNan);
    const double value = k.attribute("value").as_double(kNan);
    if (!(std::abs(frame) <= kMaxUvAnimationMagnitude) ||
        !(std::abs(value) <= kMaxUvAnimationMagnitude))
      return {};
    UvInterp interp = UvInterp::Linear;
    if (bezier) {
      const std::string_view out = k.attribute("out_tangent_type").as_string();
      interp = out == "step"     ? UvInterp::Step
               : out == "linear" ? UvInterp::Linear
                                 : UvInterp::Smooth;
    }
    keys.push_back({frame, value, interp});
  }
  std::stable_sort(keys.begin(), keys.end(),
                   [](const UvKey& a, const UvKey& b) { return a.frame < b.frame; });
  return keys;
}

// A part's <uv_animations>; the frame range comes from its connection's "loop"
// animation, else the keys' own span.
UvAnimation parseUvAnimation(const pugi::xml_node& part, const std::string& connectionTags) {
  UvAnimation anim;
  // Cargo fill, not time, drives these (build storage tiling): at rest they are the identity.
  if (detail::hasToken(connectionTags, "cargodependent")) return anim;
  for (const pugi::xml_node a : part.child("uv_animations").children("uv_animation")) {
    const int uvSet = a.attribute("mapchannel").as_int(0) - 1;  // mapchannel is 1-based
    if (uvSet < 0 || uvSet >= kUvSets) continue;
    if (std::any_of(anim.channels.begin(), anim.channels.end(),
                    [uvSet](const UvChannel& c) { return c.uvSet == uvSet; }))
      continue;
    UvChannel c;
    c.uvSet = uvSet;
    c.aroundCenter = a.attribute("rotaroundcenter").as_bool();  // "1" and "true" both occur
    for (const pugi::xml_node prop : a.children("uv_property")) {
      const std::string_view name = prop.attribute("name").as_string();
      if (name == "u_offset") c.u = parseUvKeys(prop);
      if (name == "v_offset") c.v = parseUvKeys(prop);
      if (name == "rotation_angle") c.rotation = parseUvKeys(prop);
      if (name == "u_tile") c.uTile = parseUvKeys(prop);
      if (name == "v_tile") c.vTile = parseUvKeys(prop);
    }
    if (!c.u.empty() || !c.v.empty() || !c.rotation.empty() || !c.uTile.empty() || !c.vTile.empty())
      anim.channels.push_back(std::move(c));
  }
  if (anim.channels.empty()) return anim;

  for (const pugi::xml_node a : part.parent().parent().child("animations").children("animation")) {
    if (std::string_view(a.attribute("name").as_string()) != "loop") continue;
    const double start = a.attribute("start").as_double();
    const double end = a.attribute("end").as_double();
    if (std::isfinite(start) && std::isfinite(end) && end > start) {
      anim.loopStart = start;
      anim.loopEnd = end;
      return anim;
    }
  }
  anim.loopStart = std::numeric_limits<double>::max();
  anim.loopEnd = std::numeric_limits<double>::lowest();
  for (const UvChannel& c : anim.channels) {
    for (const std::vector<UvKey>* keys : {&c.u, &c.v, &c.rotation, &c.uTile, &c.vTile}) {
      if (keys->empty()) continue;
      anim.loopStart = std::min(anim.loopStart, keys->front().frame);
      anim.loopEnd = std::max(anim.loopEnd, keys->back().frame);
    }
  }
  return anim;
}

// Hostile data could make parents cycle.
constexpr int kMaxParentDepth = 8;

struct PartFrame {
  pugi::xml_node part;
  std::string tags;  // the connection's, normalized
  Transform frame;   // component space
};

// Every <part> of every connection with its component-space frame: a connection
// with a `parent` part is offset relative to that part, so walk up the chain (the
// refined-metals lava sits in containers mounted on plates; an untagged helper
// part can be a link).
std::vector<PartFrame> componentPartFrames(const pugi::xml_node& comp) {
  struct Link {
    Transform offset;
    std::string parent;
  };
  std::vector<PartFrame> out;
  std::vector<Link> links;
  std::unordered_map<std::string, std::size_t> byName;
  for (const pugi::xml_node c : comp.child("connections").children("connection")) {
    const std::string tags = normalizeTags(c.attribute("tags").as_string());
    const Transform offset = parseOffset(c);
    for (const pugi::xml_node part : c.child("parts").children("part")) {
      byName.emplace(part.attribute("name").as_string(), links.size());
      links.push_back({offset, c.attribute("parent").as_string()});
      out.push_back({part, tags, offset});
    }
  }
  for (std::size_t i = 0; i < out.size(); ++i) {
    std::string parent = links[i].parent;
    for (int depth = 0; depth < kMaxParentDepth && !parent.empty(); ++depth) {
      const auto it = byName.find(parent);
      if (it == byName.end()) break;
      const Link& up = links[it->second];
      out[i].frame = compose(up.offset, out[i].frame);
      parent = up.parent;
    }
  }
  return out;
}

// Visit every <part> of the first <component>'s "part"-tagged connections,
// invoking fn(part, frame, connectionTags). Shared by the geometry and
// bounding-box passes so their connection/part traversal can't diverge.
template <typename Fn>
void forEachComponentPart(const pugi::xml_node& comp, Fn fn) {
  for (const PartFrame& pf : componentPartFrames(comp))
    if (detail::hasToken(pf.tags, "part")) fn(pf.part, pf.frame, pf.tags);
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
  return {90, 90, 250, "m"};     // default to M-class when unclassified
}

// Unit quaternion rotating local +Z onto `dir` (unit). The clearance OBB's local
// Z is its corridor (long) axis, so this orients a computed launch->dock corridor.
Quat quatFromUnitZTo(Vec3 dir) {
  const double d = dir.z;                        // dot({0,0,1}, dir)
  if (d > 1.0 - 1e-9) return Quat{};             // already +Z
  if (d < -1.0 + 1e-9) return Quat{0, 1, 0, 0};  // 180deg about X flips +Z to -Z
  const Vec3 c = cross(Vec3{0, 0, 1}, dir);
  const Quat q{1.0 + d, c.x, c.y, c.z};
  const double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  return {q.w / n, q.x / n, q.y / n, q.z / n};
}

// Real components author at most a few hundred; the caps bound a hostile file.
constexpr std::size_t kMaxLights = 4096;
constexpr std::size_t kMaxMacroChildren = 1024;
constexpr double kMaxLightValue = 1e6;

std::optional<LocalLight> parseLight(const pugi::xml_node& n) {
  const std::string_view tag = n.name();
  LocalLight l;
  if (tag == "omni") {
    l.kind = LightKind::Omni;
  } else if (tag == "spot") {
    l.kind = LightKind::Spot;
  } else if (tag == "box") {
    l.kind = LightKind::Box;
  } else if (tag == "arealight") {
    l.kind = LightKind::Area;
  } else {
    return std::nullopt;  // <ambient> and anything unknown
  }
  l.local = parseOffset(n);
  l.color = {n.attribute("r").as_double(255.0), n.attribute("g").as_double(255.0),
             n.attribute("b").as_double(255.0)};
  l.intensity = n.attribute("intensity").as_double(1.0);
  l.specularIntensity = n.attribute("specularintensity").as_double(1.0);
  l.range = n.attribute("range").as_double(0.0);
  l.spotAngle = n.attribute("angle").as_double(0.0);
  l.boxSize = readXyz(n.child("size"));
  l.areaX = n.attribute("areax").as_double(0.0);
  l.areaY = n.attribute("areay").as_double(0.0);
  l.areaRadius = n.attribute("radius").as_double(0.0);
  l.spotAttenuation = n.attribute("spotattenuation").as_double(0.0);

  const Vec3& p = l.local.position;
  const Quat& q = l.local.rotation;
  const std::array<double, 21> values{p.x,         p.y,          p.z,
                                      q.w,         q.x,          q.y,
                                      q.z,         l.color.x,    l.color.y,
                                      l.color.z,   l.intensity,  l.specularIntensity,
                                      l.range,     l.spotAngle,  l.boxSize.x,
                                      l.boxSize.y, l.boxSize.z,  l.areaX,
                                      l.areaY,     l.areaRadius, l.spotAttenuation};
  // The negated test also rejects NaN.
  if (!std::all_of(values.begin(), values.end(),
                   [](double v) { return std::abs(v) <= kMaxLightValue; }))
    return std::nullopt;
  return l;
}

}  // namespace

std::vector<LocalLight> parseComponentLights(const std::string& componentXml) {
  std::vector<LocalLight> out;
  pugi::xml_document doc;
  if (!doc.load_string(componentXml.c_str())) return out;
  const pugi::xml_node comp = doc.child("components").child("component");

  const auto add = [&out](const pugi::xml_node& lights, const Transform& frame) {
    for (const pugi::xml_node n : lights.children()) {
      if (out.size() == kMaxLights) return;
      if (std::optional<LocalLight> l = parseLight(n)) {
        l->local = compose(frame, l->local);
        out.push_back(*l);
      }
    }
  };
  for (const pugi::xml_node layer : comp.child("layers").children("layer"))
    add(layer.child("lights"), Transform{});

  for (const PartFrame& pf : componentPartFrames(comp)) add(pf.part.child("lights"), pf.frame);
  return out;
}

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

namespace {

bool isEffectPart(std::string_view name) {
  // The "fx" token can sit under a visual prefix too ("part_fx_lightcones",
  // "anim_fx_fire"), so match it anywhere as well as the plain "fx_" prefix.
  return name.substr(0, 3) == "fx_" || name.find("_fx_") != std::string_view::npos;
}

bool isHelperPart(std::string_view name) {
  const auto startsWith = [name](std::string_view p) { return name.substr(0, p.size()) == p; };
  const auto contains = [name](std::string_view s) {
    return name.find(s) != std::string_view::npos;
  };
  // Particle emitters, scale/editor dummies and pure non-visual volumes
  // (collision/bounds/trigger/darkening). The volume token can sit under a visual
  // prefix (e.g. "detail_l_radiator_collisionbox"), so match it anywhere.
  return startsWith("emitterfx") || startsWith("scaledummy") || startsWith("editor") ||
         contains("collisionbox") || contains("collisiononly") || contains("bounding") ||
         contains("damagearea") || contains("triggerpart") || contains("darkening") ||
         name == "geometryinvisible";
}

}  // namespace

bool isVisualPart(std::string_view name) { return !isEffectPart(name) && !isHelperPart(name); }

DetailClass partDetailClass(const std::string& tags, std::string_view partName) {
  static constexpr std::array<std::pair<const char*, DetailClass>, 5> kClasses{{
      {"detail_xs", DetailClass::XS},
      {"detail_s", DetailClass::S},
      {"detail_m", DetailClass::M},
      {"detail_l", DetailClass::L},
      {"detail_xl", DetailClass::XL},
  }};
  for (const auto& [token, cls] : kClasses)
    if (detail::hasToken(tags, token)) return cls;
  for (const auto& [token, cls] : kClasses) {
    const std::string_view prefix{token};
    if (partName.size() > prefix.size() && partName.substr(0, prefix.size()) == prefix &&
        partName[prefix.size()] == '_')
      return cls;
  }
  return DetailClass::None;
}

ComponentGeometry parseComponentGeometry(const std::string& componentXml, bool withEffects) {
  ComponentGeometry geo;
  pugi::xml_document doc;
  if (!doc.load_string(componentXml.c_str())) return geo;

  const pugi::xml_node comp = doc.child("components").child("component");
  std::string folder = comp.child("source").attribute("geometry").as_string();
  std::replace(folder.begin(), folder.end(), '\\', '/');  // X4 uses backslashes
  // Collapse a stray '//' (double-backslash in the source) and trailing '/'.
  geo.geometryFolder = normalizeFolder(folder);

  forEachComponentPart(comp, [&geo, withEffects](const pugi::xml_node& part,
                                                 const Transform& offset, const std::string& tags) {
    ComponentPart cp;
    cp.name = part.attribute("name").as_string();
    cp.ref = part.attribute("ref").as_string();  // xref to a shared sub-assembly, if any
    // The <pivot> shifts the mesh within the connection frame (the <size> box already
    // includes it, so moduleAabb must not). hab_arg_l_01's details sit 145 m off without it.
    cp.offset = compose(offset, parseOffset(part.child("pivot")));
    if (const pugi::xml_node mat =
            part.child("lods").child("lod").child("materials").child("material")) {
      cp.materialRef = mat.attribute("ref").as_string();
    }
    if (const pugi::xml_node size = part.child("size")) cp.sizeMax = readXyz(size.child("max"));
    if (const pugi::xml_node raw = part.child("size_raw")) cp.sizeRaw = readXyz(raw.child("max"));
    cp.uvAnimation = parseUvAnimation(part, tags);
    cp.detail = partDetailClass(tags, cp.name);
    cp.effect = isEffectPart(cp.name);
    if (cp.name.empty() || isHelperPart(cp.name) || (cp.effect && !withEffects)) return;
    geo.parts.push_back(std::move(cp));
  });
  return geo;
}

std::optional<PartRef> parsePartRef(std::string_view ref) {
  const auto dot =
      ref.find('.');  // "<component>.<part>"; names hold no '.', so first split is exact
  if (dot == std::string_view::npos || dot == 0 || dot + 1 >= ref.size()) return std::nullopt;
  return PartRef{std::string(ref.substr(0, dot)), std::string(ref.substr(dot + 1))};
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
  forEachComponentPart(
      comp, [&](const pugi::xml_node& part, const Transform& offset, const std::string& tags) {
        if (detail::hasToken(tags, "nocollision"))
          return;  // detail/anim/fx parts overstate their size box
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
  for (const pugi::xml_node c : macro.child("connections").children("connection")) {
    if (info.children.size() == kMaxMacroChildren) break;
    MacroChild child{c.attribute("ref").as_string(), c.child("macro").attribute("ref").as_string()};
    if (!child.connectionRef.empty() && !child.macroRef.empty())
      info.children.push_back(std::move(child));
  }
  return info;
}

std::string largestDockSize(const std::string& docksizeTags) {
  static const std::array<const char*, 5> order{
      {"dock_xl", "dock_l", "dock_m", "dock_s", "dock_xs"}};
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
