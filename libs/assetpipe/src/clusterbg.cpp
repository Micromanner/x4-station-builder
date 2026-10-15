#include "x4sb/assetpipe/clusterbg.hpp"

#include "strutil.hpp"
#include "x4sb/archive/archive.hpp"
#include "x4sb/assetpipe/component.hpp"
#include "x4sb/assetpipe/material_library.hpp"
#include "x4sb/data/math.hpp"

#include <nlohmann/json.hpp>
#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace x4sb {
namespace assetpipe {
namespace {

RgbColor parseColor(const pugi::xml_node& n) {
  return RgbColor{static_cast<uint8_t>(n.attribute("r").as_uint()),
                  static_cast<uint8_t>(n.attribute("g").as_uint()),
                  static_cast<uint8_t>(n.attribute("b").as_uint()), 255};
}

Vec3 readPos(const pugi::xml_node& n) {
  if (n.attribute("x")) {
    return {n.attribute("x").as_double(), n.attribute("y").as_double(),
            n.attribute("z").as_double()};
  } else if (pugi::xml_node p = n.child("position")) {
    return {p.attribute("x").as_double(), p.attribute("y").as_double(),
            p.attribute("z").as_double()};
  }
  return {0, 0, 0};
}

Vec3 parseDirFlipped(const pugi::xml_node& n) {
  Vec3 v = readPos(n);
  double len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  if (len > 1e-9) {
    v.x /= len;
    v.y /= len;
    v.z /= len;
  } else {
    v = {0, 0, 1};
  }
  v.z = -v.z;
  return v;
}

double largest(const Vec3& v) { return std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)}); }

double bodyRadius(const ComponentPart& part) {
  const double raw = largest(part.sizeRaw);
  return raw > 0.0 ? raw : largest(part.sizeMax);
}

// The planet/moon whose connection sits at `pos`: attachments share the body's
// position exactly; 1% of the radius absorbs float formatting only.
bool isBodyKind(const std::optional<PlanetPartKind>& kind) {
  return kind == PlanetPartKind::Planet || kind == PlanetPartKind::Moon ||
         kind == PlanetPartKind::Sun;
}

// `reach`: how far from a body's centre, in body radii, `pos` may sit.
int bodyAt(const ComponentGeometry& geo, const std::vector<std::optional<PlanetPartKind>>& kinds,
           const Vec3& pos, double reach = 0.01) {
  for (std::size_t i = 0; i < geo.parts.size(); ++i) {
    if (!isBodyKind(kinds[i])) continue;
    const Vec3& b = geo.parts[i].offset.position;
    const double dx = b.x - pos.x;
    const double dy = b.y - pos.y;
    const double dz = b.z - pos.z;
    if (std::sqrt(dx * dx + dy * dy + dz * dz) <= reach * bodyRadius(geo.parts[i]))
      return static_cast<int>(i);
  }
  return -1;
}

// Per-part planet classification (nullopt = not a planet-system part). Rings need
// the bodies first: they are only recognisable by sitting on one.
std::vector<std::optional<PlanetPartKind>> classifyPlanetParts(const ComponentGeometry& geo,
                                                               const MaterialLibrary& matLib) {
  std::vector<std::optional<PlanetPartKind>> kinds(geo.parts.size());
  std::vector<std::size_t> ringCandidates;
  std::vector<std::size_t> flareCandidates;
  for (std::size_t i = 0; i < geo.parts.size(); ++i) {
    const std::optional<MaterialDef> def = matLib.getMaterial(geo.parts[i].materialRef);
    if (!def) continue;
    const std::string& shader = def->shader;
    if (shader == "p1_planet_atmosphere_scattering")
      kinds[i] = PlanetPartKind::Atmosphere;
    else if (shader == "p1_planet_sun")
      kinds[i] = PlanetPartKind::Sun;
    else if (shader == "p1_planet_moon")
      kinds[i] = PlanetPartKind::Moon;
    else if (shader.rfind("p1_planet_", 0) == 0)
      kinds[i] = PlanetPartKind::Planet;
    else if (shader == "p1_aurora")
      kinds[i] = PlanetPartKind::Aurora;
    else if (shader == "p1_complex_unlit" && def->blendMode == "ALPHA8")
      ringCandidates.push_back(i);
    else if (shader == "p1_complex_unlit" && def->blendMode == "ADDITIVE")
      flareCandidates.push_back(i);
  }
  for (const std::size_t i : ringCandidates)
    if (bodyAt(geo, kinds, geo.parts[i].offset.position) >= 0) kinds[i] = PlanetPartKind::Ring;
  // cluster_05's flare lofts pivot up to ~0.1 sun radii off its centre.
  for (const std::size_t i : flareCandidates)
    if (bodyAt(geo, kinds, geo.parts[i].offset.position, 1.0) >= 0)
      kinds[i] = PlanetPartKind::Flare;
  return kinds;
}

}  // namespace

std::optional<ClusterBackground> extractClusterBackground(const Archive& archive, int clusterId,
                                                          const std::string& prefix,
                                                          Vec3 sectorOffset) {
  ClusterBackground bg;

  // Base + most DLC ship "cluster/" (singular); the Boron DLC alone ships
  // "clusters/" (plural) — try both, first hit wins.
  char pathBuf[128];
  std::optional<std::string> xmlBytes;
  for (const char* folder : {"cluster", "clusters"}) {
    std::snprintf(pathBuf, sizeof(pathBuf), "assets/environments/%s/Cluster_%02d.xml", folder,
                  clusterId);
    xmlBytes = archive.extract(prefix + pathBuf);
    if (xmlBytes) break;
  }
  if (!xmlBytes) return std::nullopt;

  pugi::xml_document doc;
  if (!doc.load_buffer(xmlBytes->data(), xmlBytes->size())) return std::nullopt;

  pugi::xml_node comp = doc.child("components").child("component");
  if (!comp) return std::nullopt;

  pugi::xml_node env = comp.child("layers").child("layer").child("lights");
  if (!env) return std::nullopt;

  for (pugi::xml_node star = env.child("star"); star; star = star.next_sibling("star")) {
    Star s;
    s.dir = parseDirFlipped(star);
    s.color = parseColor(star);
    s.brightness = star.attribute("intensity").as_float(1.0f);
    s.range = star.attribute("range").as_float(1.0f);
    s.specularIntensity = star.attribute("specularintensity").as_float(0.0f);
    bg.stars.push_back(s);
  }

  if (const pugi::xml_node ambient = env.child("ambient")) {
    bg.ambient.color = parseColor(ambient);
    const float intensity = ambient.attribute("intensity").as_float(1.0F);
    bg.ambient.intensity = std::isfinite(intensity) ? std::max(intensity, 0.0F) : 0.0F;
  }

  pugi::xml_node omni = env.child("omni");
  if (omni) {
    bg.sun.color = parseColor(omni);
    bg.sun.intensity = omni.attribute("intensity").as_float(1.0f);
    bg.sun.lenseffect = omni.attribute("lenseffect").as_string();
    const Vec3 raw = readPos(omni.child("offset"));
    const Vec3 off{raw.x - sectorOffset.x, raw.y - sectorOffset.y, raw.z - sectorOffset.z};
    const double len = std::sqrt(off.x * off.x + off.y * off.y + off.z * off.z);
    bg.sun.dir = len > 1e-9 && std::isfinite(len) ? Vec3{off.x / len, off.y / len, -off.z / len}
                                                  : Vec3{0, 0, 1};
    bg.sun.distance = std::isfinite(len) ? len : 0.0;
  }

  return bg;
}

std::vector<ClusterDome> selectSkyDomes(const ComponentGeometry& geo, const MaterialLibrary* matLib,
                                        Vec3 sectorOffset) {
  constexpr double kSkyExtent = 1e8;
  const std::vector<std::optional<PlanetPartKind>> planetKinds =
      matLib != nullptr ? classifyPlanetParts(geo, *matLib)
                        : std::vector<std::optional<PlanetPartKind>>(geo.parts.size());
  std::vector<ClusterDome> out;
  for (std::size_t i = 0; i < geo.parts.size(); ++i) {
    const ComponentPart& part = geo.parts[i];
    const double extent = largest(part.sizeMax);
    const bool skyNamed = part.name.find("bg") != std::string::npos ||
                          part.name.find("nebula") != std::string::npos ||
                          part.name.find("background") != std::string::npos;
    if ((extent < kSkyExtent && !skyNamed) || planetKinds[i]) continue;
    ClusterDome d;
    d.part = part.name;
    d.materialRef = part.materialRef;
    if (matLib != nullptr) {
      if (const std::optional<MaterialDef> def = matLib->getMaterial(part.materialRef))
        d.additive = def->blendMode == "ADDITIVE";
    }
    // The (1,1,-1) display flip, applied here so the editor consumes the manifest
    // without coordinate work: positions negate z; a rotation conjugated by the
    // z-reflection negates its x,y quaternion components.
    const Vec3& pos = part.offset.position;
    d.pos = {pos.x - sectorOffset.x, pos.y - sectorOffset.y, -(pos.z - sectorOffset.z)};
    d.quat = {part.offset.rotation.w, -part.offset.rotation.x, -part.offset.rotation.y,
              part.offset.rotation.z};
    d.extent = extent;
    out.push_back(std::move(d));
  }
  return out;
}

std::vector<PlanetPart> selectPlanetParts(const ComponentGeometry& geo,
                                          const MaterialLibrary& matLib, Vec3 sectorOffset) {
  const std::vector<std::optional<PlanetPartKind>> kinds = classifyPlanetParts(geo, matLib);
  std::vector<PlanetPart> out;
  std::vector<int> outIndex(geo.parts.size(), -1);
  // Bodies first so attachments can point at an already-emitted index.
  for (const bool bodies : {true, false}) {
    for (std::size_t i = 0; i < geo.parts.size(); ++i) {
      if (!kinds[i]) continue;
      const bool isBody = isBodyKind(kinds[i]);
      if (isBody != bodies) continue;
      const ComponentPart& part = geo.parts[i];
      PlanetPart p;
      p.part = part.name;
      p.materialRef = part.materialRef;
      p.kind = *kinds[i];
      if (!isBody) {
        const double reach = kinds[i] == PlanetPartKind::Flare ? 1.0 : 0.01;
        const int body = bodyAt(geo, kinds, part.offset.position, reach);
        p.body = body >= 0 ? outIndex[static_cast<std::size_t>(body)] : -1;
      }
      const Vec3& pos = part.offset.position;
      const Quat& rot = part.offset.rotation;
      p.pos = {pos.x - sectorOffset.x, pos.y - sectorOffset.y, -(pos.z - sectorOffset.z)};
      p.quat = {rot.w, -rot.x, -rot.y, rot.z};
      p.radius = bodyRadius(part);
      p.uvAnimation = part.uvAnimation;
      outIndex[i] = static_cast<int>(out.size());
      out.push_back(std::move(p));
    }
  }
  return out;
}

std::string backgroundManifestJson(const ClusterBackground& bg,
                                   const std::vector<ClusterDome>& domes,
                                   const std::vector<SunSprite>& sunSprites,
                                   const std::vector<PlanetPart>& planets, const FogField& fog) {
  nlohmann::json doc;
  doc["sun"] = {{"dir", {bg.sun.dir.x, bg.sun.dir.y, bg.sun.dir.z}},
                {"color", {bg.sun.color.r, bg.sun.color.g, bg.sun.color.b, bg.sun.color.a}},
                {"intensity", bg.sun.intensity}};
  doc["ambient"] = {{"color", {bg.ambient.color.r, bg.ambient.color.g, bg.ambient.color.b}},
                    {"intensity", bg.ambient.intensity}};
  if (!sunSprites.empty()) {
    doc["sun"]["lenseffect"] = bg.sun.lenseffect;
    doc["sun"]["distance"] = bg.sun.distance;
    nlohmann::json elements = nlohmann::json::array();
    for (const SunSprite& s : sunSprites) {
      elements.push_back(
          {{"tex", s.texFile},
           {"mesh",
            {{"xy", s.mesh.xy}, {"uv", s.mesh.uv}, {"rgb", s.mesh.rgb}, {"index", s.mesh.index}}},
           {"sizeMode", s.screenMode ? "screen" : "angular"},
           {"size", s.size},
           {"rotation", s.rotation},
           {"glow", s.glow},
           {"shineBehind", s.shineBehind},
           {"intensity", s.intensity},
           {"screenBlend", s.screenBlend},
           {"position", s.position},
           {"emissive", {s.emissive[0], s.emissive[1], s.emissive[2]}},
           {"grade", s.grade}});
    }
    doc["sun"]["elements"] = std::move(elements);
  }
  nlohmann::json stars = nlohmann::json::array();
  for (const Star& s : bg.stars) {
    stars.push_back({{"dir", {s.dir.x, s.dir.y, s.dir.z}},
                     {"color", {s.color.r, s.color.g, s.color.b, s.color.a}},
                     {"brightness", s.brightness},
                     {"range", s.range},
                     {"spec", s.specularIntensity}});
  }
  doc["stars"] = std::move(stars);
  doc["starAtlas"] = kStarAtlasSource;
  nlohmann::json jdomes = nlohmann::json::array();
  for (const ClusterDome& d : domes) {
    jdomes.push_back({{"xmf", d.xmfPath},
                      {"material", d.materialRef},
                      {"blend", d.additive ? "additive" : "opaque"},
                      {"pos", {d.pos.x, d.pos.y, d.pos.z}},
                      {"quat", {d.quat.x, d.quat.y, d.quat.z, d.quat.w}},  // [x,y,z,w]
                      {"extent", d.extent}});
  }
  doc["domes"] = std::move(jdomes);
  static constexpr std::array<const char*, 7> kKindNames{"planet", "moon", "atmosphere", "ring",
                                                         "aurora", "sun",  "flare"};
  nlohmann::json jplanets = nlohmann::json::array();
  for (const PlanetPart& p : planets) {
    nlohmann::json jp = {{"xmf", p.xmfPath},
                         {"material", p.materialRef},
                         {"kind", kKindNames[static_cast<std::size_t>(p.kind)]},
                         {"body", p.body},
                         {"pos", {p.pos.x, p.pos.y, p.pos.z}},
                         {"quat", {p.quat.x, p.quat.y, p.quat.z, p.quat.w}},  // [x,y,z,w]
                         {"radius", p.radius}};
    if (!p.uvAnimation.channels.empty()) jp["uvAnimation"] = uvAnimationToJson(p.uvAnimation);
    jplanets.push_back(std::move(jp));
  }
  doc["planets"] = std::move(jplanets);
  if (!fog.spheres.empty()) doc["fog"] = fogFieldToJson(fog);
  return doc.dump(2);
}

}  // namespace assetpipe
}  // namespace x4sb
