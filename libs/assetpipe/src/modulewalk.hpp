#pragma once
// Internal, render-free module-resolution walk shared by the catalog builder and
// the batch mesh converter (spec §4A). Both consumers MUST derive the same module
// id and component XML by construction — that is what eliminates the path-scheme
// drift between catalog.json's meshRefs[].gltfPath and the converter's output
// paths. This header is private to libs/assetpipe (not installed in include/).
#include "x4sb/assetpipe/catalogbuild.hpp"
#include "x4sb/assetpipe/component.hpp"
#include "x4sb/assetpipe/wares.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace x4sb {
namespace detail {

// One fully-resolved buildable module: its catalog id, the component XML bytes,
// and the source ware/macro identity the catalog needs to fill a ModuleDef.
struct ResolvedModule {
  std::string id;            // catalog key = lower(macroName ? macroName : ware.macroRef)
  std::string componentXml;  // bytes of the resolved component XML
  const WareModule* ware{};  // the originating ware (non-owning; valid during visit)
  const MacroInfo* macro{};  // parsed macro identity (non-owning; valid during visit)
  // Resolve a referenced component's geometry folder, for xref parts whose mesh
  // lives in another component. nullopt if it isn't indexed or has no geometry.
  // Backed by the walk's component index; valid only during the visit call.
  std::function<std::optional<std::string>(const std::string&)> resolveComponentFolder;
  std::string macroXml;  // bytes of the resolved macro XML (for dock-clearance extraction)
  // Resolve a referenced macro's XML bytes by macro name (for child dockingbay
  // macros). Backed by the walk's macro index; valid only during the visit call.
  std::function<std::optional<std::string>(const std::string&)> resolveMacroXml;
};

// Resolve every buildable station module across the given source overlays
// (sourcePrefixes: "" for the base game, "extensions/<dlc>/" per DLC) and invoke
// `visit` once per resolved module, in ware order. A module that fails to resolve
// is appended to `skipped` ("wareId: reason") and skipped — never fatal. All file
// access goes through `extract`.
void forEachResolvedModule(const ExtractFn& extract,
                           const std::vector<std::string>& sourcePrefixes,
                           const std::function<void(const ResolvedModule&)>& visit,
                           std::vector<std::string>& skipped);

}  // namespace detail
}  // namespace x4sb
