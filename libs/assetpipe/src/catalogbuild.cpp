#include "x4sb/assetpipe/catalogbuild.hpp"

#include "modulewalk.hpp"
#include "strutil.hpp"
#include "x4sb/assetpipe/component.hpp"
#include "x4sb/assetpipe/localization.hpp"
#include "x4sb/assetpipe/moduleindex.hpp"
#include "x4sb/assetpipe/wares.hpp"

#include <optional>
#include <unordered_map>
#include <utility>

namespace x4sb {

namespace detail {

void forEachResolvedModule(const ExtractFn& extract, const std::vector<std::string>& sourcePrefixes,
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

  // Resolve a referenced component's geometry folder for xref parts (cached: pier
  // modules instance the same sub-assembly many times). Shared across all visits.
  std::unordered_map<std::string, std::optional<std::string>> folderCache;
  const auto resolveFolder = [&compIdx, &extract, &folderCache](
                                 const std::string& component) -> std::optional<std::string> {
    const std::string key = detail::toLower(component);
    if (const auto cached = folderCache.find(key); cached != folderCache.end())
      return cached->second;
    std::optional<std::string> folder;
    if (const auto it = compIdx.find(key); it != compIdx.end()) {
      if (const std::optional<std::string> xml = extract(it->second)) {
        std::string f = parseComponentGeometry(*xml).geometryFolder;
        if (!f.empty()) folder = std::move(f);
      }
    }
    folderCache.emplace(key, folder);
    return folder;
  };

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
    rm.resolveComponentFolder = resolveFolder;
    rm.macroXml = *macroXml;
    rm.resolveMacroXml = [&macroIdx,
                          &extract](const std::string& name) -> std::optional<std::string> {
      const auto it = macroIdx.find(detail::toLower(name));
      if (it == macroIdx.end()) return std::nullopt;
      return extract(it->second);
    };
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

  // Merge the English localization across base + every DLC overlay (later wins).
  TextDatabase text;
  for (const std::string& prefix : sourcePrefixes)
    if (const std::optional<std::string> xml = extract(prefix + "t/0001-l044.xml"))
      text.merge(*xml);

  detail::forEachResolvedModule(
      extract, sourcePrefixes,
      [&res, &text](const detail::ResolvedModule& rm) {
        ModuleDef d;
        d.id = rm.id;
        d.wareId = rm.ware->wareId;
        d.nameRef = rm.ware->nameRef;
        d.name = text.resolveRef(d.nameRef).value_or("");
        if (d.name.empty())
          ++res.namesUnresolved;
        else
          ++res.namesResolved;
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
        d.clearanceVolumes =
            extractClearanceVolumes(rm.macroXml, rm.componentXml, rm.resolveMacroXml);
        res.modules.push_back(std::move(d));
      },
      res.skipped);
  return res;
}

}  // namespace x4sb
