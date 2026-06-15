#include "x4sb/assetpipe/catalogbuild.hpp"

#include "strutil.hpp"

#include "x4sb/assetpipe/component.hpp"
#include "x4sb/assetpipe/moduleindex.hpp"
#include "x4sb/assetpipe/wares.hpp"

#include <optional>
#include <unordered_map>
#include <utility>

namespace x4sb {

CatalogBuildResult buildModuleCatalog(const ExtractFn& extract,
                                      const std::vector<std::string>& sourcePrefixes) {
  CatalogBuildResult res;

  // Accumulate wares and merge the macro/component indexes across the base game
  // and every DLC overlay. DLC index values are already fully-qualified archive
  // paths, so one merged map resolves modules from any source.
  std::vector<WareModule> wares;
  std::unordered_map<std::string, std::string> macroIdx;
  std::unordered_map<std::string, std::string> compIdx;
  for (const std::string& prefix : sourcePrefixes) {
    if (const std::optional<std::string> waresXml = extract(prefix + "libraries/wares.xml")) {
      const std::vector<WareModule> srcWares = parseModuleWares(*waresXml);
      wares.insert(wares.end(), srcWares.begin(), srcWares.end());
    }
    if (const std::optional<std::string> macrosXml = extract(prefix + "index/macros.xml")) {
      for (const auto& entry : parseModuleIndex(*macrosXml)) macroIdx[entry.first] = entry.second;
    }
    if (const std::optional<std::string> compsXml = extract(prefix + "index/components.xml")) {
      for (const auto& entry : parseModuleIndex(*compsXml)) compIdx[entry.first] = entry.second;
    }
  }
  if (wares.empty()) return res;

  for (const WareModule& w : wares) {
    const auto skip = [&](const std::string& why) { res.skipped.push_back(w.wareId + ": " + why); };

    const auto macroIt = macroIdx.find(detail::toLower(w.macroRef));
    if (macroIt == macroIdx.end()) {
      skip("macro not in index (" + w.macroRef + ")");
      continue;
    }
    const std::optional<std::string> macroXml = extract(macroIt->second);
    if (!macroXml) {
      skip("macro file missing (" + macroIt->second + ")");
      continue;
    }
    const MacroInfo mi = parseMacro(*macroXml);
    if (mi.componentRef.empty()) {
      skip("macro has no component ref");
      continue;
    }
    const auto compIt = compIdx.find(detail::toLower(mi.componentRef));
    if (compIt == compIdx.end()) {
      skip("component not in index (" + mi.componentRef + ")");
      continue;
    }
    const std::optional<std::string> componentXml = extract(compIt->second);
    if (!componentXml) {
      skip("component file missing (" + compIt->second + ")");
      continue;
    }

    ModuleDef d;
    d.id = detail::toLower(mi.macroName.empty() ? w.macroRef : mi.macroName);
    d.wareId = w.wareId;
    d.nameRef = w.nameRef;
    d.faction = mi.makerRace;
    d.category = mi.category;
    d.playerBuildable = w.playerBuildable;
    d.connectionPoints = snapConnectionPoints(*componentXml);
    d.aabb = moduleAabb(*componentXml);

    const ComponentGeometry geo = parseComponentGeometry(*componentXml);
    for (const ComponentPart& p : geo.parts) {
      MeshRef mr;
      mr.gltfPath = "meshes/" + d.id + "__" + p.name + ".gltf";
      mr.localTransform = p.offset;
      d.meshRefs.push_back(std::move(mr));
    }
    res.modules.push_back(std::move(d));
  }
  return res;
}

}  // namespace x4sb
