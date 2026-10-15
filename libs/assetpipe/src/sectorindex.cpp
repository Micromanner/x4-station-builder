#include "x4sb/assetpipe/sectorindex.hpp"

#include "strutil.hpp"

#include <nlohmann/json.hpp>
#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>

namespace x4sb::assetpipe {
namespace {

// "Cluster_<N>_Sector<M>_macro" (any case) -> N; nullopt for anything else.
std::optional<int> clusterIdFromSectorMacro(std::string_view macro) {
  std::string m(macro);
  std::transform(m.begin(), m.end(), m.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  constexpr std::string_view kCluster = "cluster_";
  if (m.rfind(kCluster, 0) != 0) return std::nullopt;
  std::size_t i = kCluster.size();
  int id = 0;
  std::size_t digits = 0;
  // 4-digit bound keeps hostile input from overflowing int.
  while (i < m.size() && std::isdigit(static_cast<unsigned char>(m[i])) != 0 && digits < 4) {
    id = id * 10 + (m[i] - '0');
    ++i;
    ++digits;
  }
  if (digits == 0) return std::nullopt;
  constexpr std::string_view kSector = "_sector";
  if (m.compare(i, kSector.size(), kSector) != 0) return std::nullopt;
  i += kSector.size();
  std::size_t sectorDigits = 0;
  while (i < m.size() && std::isdigit(static_cast<unsigned char>(m[i])) != 0) {
    ++i;
    ++sectorDigits;
  }
  if (sectorDigits == 0) return std::nullopt;
  if (m.compare(i, std::string_view::npos, "_macro") != 0) return std::nullopt;
  return id;
}

}  // namespace

std::vector<MapSector> parseMapDefaultsSectors(std::string_view xml) {
  std::vector<MapSector> out;
  pugi::xml_document doc;
  if (!doc.load_buffer(xml.data(), xml.size())) return out;
  for (const pugi::xml_node ds : doc.child("defaults").children("dataset")) {
    const char* macro = ds.attribute("macro").as_string();
    const std::optional<int> id = clusterIdFromSectorMacro(macro);
    if (!id) continue;
    MapSector s;
    s.macro = macro;
    s.clusterId = *id;
    s.nameRef = ds.child("properties").child("identification").attribute("name").as_string();
    out.push_back(std::move(s));
  }
  return out;
}

std::string sectorIndexJson(std::vector<SectorIndexEntry> entries) {
  std::sort(entries.begin(), entries.end(),
            [](const SectorIndexEntry& a, const SectorIndexEntry& b) {
              return a.name != b.name ? a.name < b.name : a.macro < b.macro;
            });
  nlohmann::json arr = nlohmann::json::array();
  for (const SectorIndexEntry& e : entries)
    arr.push_back({{"macro", e.macro}, {"name", e.name}, {"cluster", e.cluster}});
  return arr.dump(2);
}

std::vector<SectorIndexEntry> readSectorIndex(std::string_view json) {
  std::vector<SectorIndexEntry> out;
  const nlohmann::json doc = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
  if (!doc.is_array()) return out;
  for (const nlohmann::json& e : doc) {
    if (!e.is_object()) continue;
    const auto str = [&e](const char* key) -> std::string {
      const auto it = e.find(key);
      return it != e.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    SectorIndexEntry s{str("macro"), str("name"), str("cluster")};
    if (s.macro.empty() || s.name.empty() || s.cluster.empty()) continue;
    out.push_back(std::move(s));
  }
  return out;
}

std::optional<MacroIndexHit> findMacroPath(std::string_view macrosXml, std::string_view macro) {
  pugi::xml_document doc;
  if (!doc.load_buffer(macrosXml.data(), macrosXml.size())) return std::nullopt;
  const std::string wanted = detail::toLower(std::string(macro));
  std::optional<MacroIndexHit> best;
  std::size_t bestPrefix = 0;
  for (const pugi::xml_node entry : doc.child("index").children("entry")) {
    const std::string name = detail::toLower(entry.attribute("name").as_string());
    const std::string value = entry.attribute("value").as_string();
    if (name.empty() || value.empty()) continue;
    if (name == wanted) return MacroIndexHit{value, true};
    if (name.back() != '*') continue;
    const std::size_t prefix = name.size() - 1;
    if (wanted.compare(0, prefix, name, 0, prefix) == 0 && (!best || prefix > bestPrefix)) {
      best = MacroIndexHit{value, false};
      bestPrefix = prefix;
    }
  }
  return best;
}

std::optional<Vec3> parseSectorOffset(std::string_view clustersXml, std::string_view clusterMacro,
                                      std::string_view sectorMacro) {
  pugi::xml_document doc;
  if (!doc.load_buffer(clustersXml.data(), clustersXml.size())) return std::nullopt;
  const std::string cluster = detail::toLower(std::string(clusterMacro));
  const std::string sector = detail::toLower(std::string(sectorMacro));
  for (const pugi::xml_node macro : doc.child("macros").children("macro")) {
    if (detail::toLower(macro.attribute("name").as_string()) != cluster) continue;
    for (const pugi::xml_node conn : macro.child("connections").children("connection")) {
      if (std::string_view(conn.attribute("ref").as_string()) != "sectors") continue;
      if (detail::toLower(conn.child("macro").attribute("ref").as_string()) != sector) continue;
      const pugi::xml_node pos = conn.child("offset").child("position");
      const Vec3 offset{pos.attribute("x").as_double(), pos.attribute("y").as_double(),
                        pos.attribute("z").as_double()};
      if (!std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z))
        return std::nullopt;
      return offset;
    }
  }
  return std::nullopt;
}

}  // namespace x4sb::assetpipe
