#include "x4sb/assetpipe/catalogbuild.hpp"

#include "modulewalk.hpp"
#include "strutil.hpp"

#include "x4sb/assetpipe/component.hpp"
#include "x4sb/assetpipe/moduleindex.hpp"
#include "x4sb/assetpipe/wares.hpp"

#include <optional>
#include <unordered_map>
#include <utility>

namespace x4sb {

namespace detail {

void forEachResolvedModule(const ExtractFn& extract,
                           const std::vector<std::string>& sourcePrefixes,
                           const std::function<void(const ResolvedModule&)>& visit,
                           std::vector<std::string>& skipped) {
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
  if (wares.empty()) return;

  for (const WareModule& w : wares) {
    const auto skip = [&](const std::string& why) { skipped.push_back(w.wareId + ": " + why); };

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

    ResolvedModule rm;
    rm.id = detail::toLower(mi.macroName.empty() ? w.macroRef : mi.macroName);
    rm.componentXml = *componentXml;
    rm.ware = &w;
    rm.macro = &mi;
    visit(rm);
  }
}

}  // namespace detail

std::string meshGltfPath(const std::string& moduleId, const std::string& partName) {
  return "meshes/" + moduleId + "__" + partName + ".gltf";
}

CatalogBuildResult buildModuleCatalog(const ExtractFn& extract,
                                      const std::vector<std::string>& sourcePrefixes) {
  CatalogBuildResult res;
  detail::forEachResolvedModule(
      extract, sourcePrefixes,
      [&res](const detail::ResolvedModule& rm) {
        ModuleDef d;
        d.id = rm.id;
        d.wareId = rm.ware->wareId;
        d.nameRef = rm.ware->nameRef;
        d.faction = rm.macro->makerRace;
        d.category = rm.macro->category;
        d.playerBuildable = rm.ware->playerBuildable;
        d.connectionPoints = snapConnectionPoints(rm.componentXml);
        d.aabb = moduleAabb(rm.componentXml);

        const ComponentGeometry geo = parseComponentGeometry(rm.componentXml);
        for (const ComponentPart& p : geo.parts) {
          MeshRef mr;
          mr.gltfPath = meshGltfPath(d.id, p.name);
          mr.localTransform = p.offset;
          d.meshRefs.push_back(std::move(mr));
        }
        res.modules.push_back(std::move(d));
      },
      res.skipped);
  return res;
}

}  // namespace x4sb
