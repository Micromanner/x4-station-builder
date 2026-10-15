#pragma once
// Sector index for the sky picker (spec 2026-07-19-cluster-picker): sector
// datasets from mapdefaults.xml -> the background/index.json the editor loads.
// All inputs are untrusted (game cats / user disk).
#include "x4sb/data/types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace x4sb::assetpipe {

// One sector dataset from a mapdefaults.xml.
struct MapSector {
  std::string macro;    // as shipped, e.g. "Cluster_11_Sector001_macro"
  std::string nameRef;  // "{20004,110011}"; empty when no identification node
  int clusterId{0};     // parsed from the macro name
};

// Sector datasets from one mapdefaults.xml (base or DLC). X4 keeps the owning
// cluster in the sector macro name, so no galaxy.xml walk is needed. Non-sector
// datasets and malformed macros are skipped; malformed XML -> empty.
[[nodiscard]] std::vector<MapSector> parseMapDefaultsSectors(std::string_view xml);

// One sky-picker entry: a sector and the cluster whose background it shows.
struct SectorIndexEntry {
  std::string macro;
  std::string name;     // localized display name (the macro when unresolvable)
  std::string cluster;  // background dir, e.g. "cluster_11"
};

// Serialize the index the editor loads, sorted by name then macro.
[[nodiscard]] std::string sectorIndexJson(std::vector<SectorIndexEntry> entries);

// Hostile-safe reader: malformed JSON -> empty; malformed entries skipped.
[[nodiscard]] std::vector<SectorIndexEntry> readSectorIndex(std::string_view json);

// One index/macros.xml lookup. X4 maps a macro name to the file defining it,
// either exactly or by a trailing-'*' prefix ("cluster_*"); an exact entry beats
// a wildcard, so the DLC's own cluster_600_macro wins over the base "cluster_*".
struct MacroIndexHit {
  std::string path;  // as shipped: backslashes, no ".xml"
  bool exact{false};
};
[[nodiscard]] std::optional<MacroIndexHit> findMacroPath(std::string_view macrosXml,
                                                         std::string_view macro);

// Where a sector sits inside its cluster (the `sectors` connection offset in the
// cluster macro file). Cluster backgrounds are placed at the cluster origin, so
// planets and the sun must be seen from here. No <offset> = the origin; nullopt
// when the cluster macro or the sector connection is absent.
[[nodiscard]] std::optional<Vec3> parseSectorOffset(std::string_view clustersXml,
                                                    std::string_view clusterMacro,
                                                    std::string_view sectorMacro);

}  // namespace x4sb::assetpipe
