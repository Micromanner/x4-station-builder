#include "x4sb/assetpipe/native_background.hpp"

#include "x4sb/archive/archive.hpp"
#include "x4sb/assetpipe/clusterbg.hpp"
#include "x4sb/assetpipe/gunzip.hpp"
#include "x4sb/assetpipe/localization.hpp"
#include "x4sb/assetpipe/material_library.hpp"
#include "x4sb/assetpipe/sectorindex.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <map>

namespace x4sb {

ExtractFn archiveAssetReader(const Archive& archive) {
  return [&archive](const std::string& path) -> std::optional<std::string> {
    if (auto bytes = archive.extract(path)) return bytes;
    for (const auto& prefix : archive.sources()) {
      if (prefix.empty()) continue;
      if (auto bytes = archive.extract(prefix + path)) return bytes;
    }
    return std::nullopt;
  };
}

std::optional<std::string> readTextureDds(const ExtractFn& extract, const std::string& path) {
  if (path.empty()) return std::nullopt;
  std::string normalized = path;
  for (auto& ch : normalized)
    ch = ch == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  std::vector<std::string> candidates{normalized, normalized + ".gz"};
  if (normalized.size() > 4) {
    const auto extension = normalized.substr(normalized.size() - 4);
    if (extension == ".dds" || extension == ".tga") {
      const auto stem = normalized.substr(0, normalized.size() - 4);
      candidates.push_back(stem);
      candidates.push_back(stem + ".gz");
    }
  }
  for (const auto& candidate : candidates) {
    if (auto bytes = extract(candidate)) {
      if (bytes->size() >= 2 && static_cast<unsigned char>((*bytes)[0]) == 0x1FU &&
          static_cast<unsigned char>((*bytes)[1]) == 0x8BU) {
        const auto decoded = gunzip(*bytes);
        if (!decoded) return std::nullopt;
        return std::string(decoded->begin(), decoded->end());
      }
      return bytes;
    }
  }
  return std::nullopt;
}

namespace assetpipe {
namespace {
std::optional<int> clusterIdOf(const std::string& cluster) {
  if (cluster.rfind("cluster_", 0) != 0) return std::nullopt;
  int id = 0;
  const auto parsed = std::from_chars(cluster.data() + 8, cluster.data() + cluster.size(), id);
  if (parsed.ec != std::errc{} || parsed.ptr != cluster.data() + cluster.size() || id < 0)
    return std::nullopt;
  return id;
}

std::string clusterName(int id) {
  return "cluster_" + std::string(id < 10 ? "0" : "") + std::to_string(id);
}
}  // namespace

std::optional<std::string> clusterComponentPath(const Archive& archive,
                                                const std::string& cluster) {
  const auto id = clusterIdOf(cluster);
  if (!id) return std::nullopt;
  for (const auto* folder : {"cluster", "clusters"}) {
    const std::string relative =
        "assets/environments/" + std::string(folder) + "/" + clusterName(*id) + ".xml";
    for (const auto& prefix : archive.sources())
      if (archive.contains(prefix + relative)) return prefix + relative;
  }
  return std::nullopt;
}

namespace {
// The macro file that defines the cluster's sectors and regions (maps/*/clusters.xml
// or a DLC's own), via index/macros.xml.
std::optional<std::string> clusterMacroXml(const Archive& archive,
                                           const std::string& clusterMacro) {
  std::optional<MacroIndexHit> best;
  for (const auto& prefix : archive.sources()) {
    const auto index = archive.extract(prefix + "index/macros.xml");
    if (!index) continue;
    // Later sources override earlier ones, but never an exact entry with a wildcard.
    if (auto hit = findMacroPath(*index, clusterMacro);
        hit && (!best || hit->exact || !best->exact))
      best = std::move(hit);
  }
  if (!best) return std::nullopt;
  std::string file = best->path + ".xml";
  std::replace(file.begin(), file.end(), '\\', '/');
  return archive.extract(file);
}

// The cluster's regions with every source's region and medium library merged in.
FogField nativeFogField(const Archive& archive, std::string_view macroXml,
                        const std::string& clusterMacro, Vec3 origin) {
  RegionLibrary regions;
  MediumLibrary media;
  for (const auto& source : archive.sources()) {
    if (const auto defs = archive.extract(source + "libraries/region_definitions.xml"))
      parseRegionDefinitions(*defs, regions);
    if (const auto library = archive.extract(source + "libraries/medium_library.xml"))
      parseMediumLibrary(*library, media);
  }
  return buildFogField(parseClusterRegions(macroXml, clusterMacro), regions, media, origin);
}
}  // namespace

std::optional<Vec3> nativeSectorOffset(const Archive& archive, int clusterId,
                                       const std::string& sectorMacro) {
  const std::string clusterMacro = clusterName(clusterId) + "_macro";
  const auto xml = clusterMacroXml(archive, clusterMacro);
  if (!xml) return std::nullopt;
  return parseSectorOffset(*xml, clusterMacro, sectorMacro);
}

std::optional<std::string> nativeBackgroundManifest(const Archive& archive,
                                                    const MaterialLibrary& materials,
                                                    const std::string& cluster,
                                                    const std::string& sectorMacro) {
  const auto path = clusterComponentPath(archive, cluster);
  if (!path) return std::nullopt;
  const auto xml = archive.extract(*path);
  if (!xml) return std::nullopt;
  const auto prefix = path->substr(0, path->find("assets/"));
  const int clusterId = *clusterIdOf(cluster);
  const std::string clusterMacro = clusterName(clusterId) + "_macro";
  const auto macroXml = clusterMacroXml(archive, clusterMacro);
  Vec3 sectorOffset{};
  if (macroXml && !sectorMacro.empty())
    sectorOffset = parseSectorOffset(*macroXml, clusterMacro, sectorMacro).value_or(Vec3{});
  auto background = extractClusterBackground(archive, clusterId, prefix, sectorOffset);
  if (!background) return std::nullopt;
  const auto geometry = parseComponentGeometry(*xml);
  const auto xmfAddress = [&](const std::string& part) {
    auto address = partXmfPath(geometry.geometryFolder, part);
    if (!archive.contains(address) && archive.contains(prefix + address))
      address = prefix + address;
    return address;
  };
  auto domes = selectSkyDomes(geometry, &materials, sectorOffset);
  for (auto& dome : domes) dome.xmfPath = xmfAddress(dome.part);
  auto planets = selectPlanetParts(geometry, materials, sectorOffset);
  for (auto& planet : planets) planet.xmfPath = xmfAddress(planet.part);
  const auto extract = archiveAssetReader(archive);

  LensEffectMap effects;
  bool first = true;
  for (const auto& source : archive.sources()) {
    if (const auto library = archive.extract(source + "libraries/lens_effects.xml")) {
      if (first) {
        effects = parseLensEffects(*library);
        first = false;
      } else
        mergeLensEffectsDiff(effects, *library);
    }
  }
  std::vector<SunSprite> sprites;
  const auto flares = archive.extract("assets/fx/lensflares/lensflares.xml");
  const auto effect = effects.find(background->sun.lenseffect);
  if (flares && effect != effects.end()) {
    const auto flareGeometry = parseComponentGeometry(*flares);
    std::map<std::string, FlareMesh> meshes;
    for (const auto& element : effect->second.elements) {
      const auto part =
          std::find_if(flareGeometry.parts.begin(), flareGeometry.parts.end(),
                       [&](const auto& candidate) { return candidate.name == element.part; });
      if (part == flareGeometry.parts.end()) continue;
      const auto material = materials.getMaterial(part->materialRef);
      if (!material || material->diffuseMap.empty()) continue;
      auto found = meshes.find(element.part);
      if (found == meshes.end()) {
        const auto bytes = extract(partXmfPath(flareGeometry.geometryFolder, element.part));
        if (!bytes) continue;
        const auto mesh = parseXmf(*bytes);
        if (!mesh) continue;
        found = meshes.emplace(element.part, flareMeshFromXmf(*mesh)).first;
      }
      if (found->second.index.empty()) continue;
      auto sprite = makeSunSprite(element, effect->second.scale, background->sun.distance,
                                  material->diffuseMap, material->diffuseStr);
      if (!sprite) continue;
      sprite->mesh = found->second;
      sprite->screenBlend = material->blendMode == "SCREEN";
      sprite->emissive = material->colorEmissive;
      // sun_corona_v1's red rim comes from this grade (path to profit sun.rdc 3816).
      if (material->shader == "p1_complex_unlit") {
        const auto& floats = material->properties.floats;
        const auto prop = [&floats](const char* name, float fallback) {
          const auto it = floats.find(name);
          return it != floats.end() ? it->second : fallback;
        };
        sprite->grade = {prop("base_brightness_shift", 1.0F), prop("base_contrast_shift", 1.0F),
                         prop("base_saturation_shift", 1.0F), prop("base_hue_shift", 0.0F)};
      }
      sprites.push_back(std::move(*sprite));
    }
  }
  return backgroundManifestJson(
      *background, domes, sprites, planets,
      macroXml ? nativeFogField(archive, *macroXml, clusterMacro, sectorOffset) : FogField{});
}

std::string nativeSectorIndex(const Archive& archive) {
  TextDatabase text;
  for (const auto& prefix : archive.sources())
    if (const auto xml = archive.extract(prefix + "t/0001-l044.xml")) text.merge(*xml);
  std::vector<SectorIndexEntry> entries;
  for (const auto& prefix : archive.sources()) {
    const auto xml = archive.extract(prefix + "libraries/mapdefaults.xml");
    if (!xml) continue;
    for (const auto& sector : parseMapDefaultsSectors(*xml)) {
      const auto cluster = clusterName(sector.clusterId);
      if (!clusterComponentPath(archive, cluster)) continue;
      entries.push_back(
          {sector.macro, text.resolveRef(sector.nameRef).value_or(sector.macro), cluster});
    }
  }
  return sectorIndexJson(std::move(entries));
}
}  // namespace assetpipe
}  // namespace x4sb
