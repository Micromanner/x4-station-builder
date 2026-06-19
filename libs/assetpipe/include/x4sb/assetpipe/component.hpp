#pragma once
// Asset-pipeline XML parsing (spec §4A / §10.3): turn an X4 module's component &
// macro XML (extracted from the archives) into the connector transforms and
// identity the data layer needs. Render-free; the pipeline.exe and tests share it.
#include "x4sb/data/types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace x4sb {

// One <connection> from a component XML, with its local offset transform.
// X4 stores rotation as a quaternion (qx,qy,qz,qw); position/quaternion default
// to identity when the <offset> is empty or absent.
struct ComponentConnection {
  std::string name;  // e.g. "ConnectionSnap001"
  std::string tags;  // trimmed, space-separated, e.g. "snap" or "part detail_m"
  Transform offset{};
};

// Parse every <connection> under <components>/<component>/<connections>.
std::vector<ComponentConnection> parseComponentConnections(const std::string& componentXml);

// True if the connection is a buildable module-to-module snap connector
// (tags contains the "snap" token) — the points the snap engine attaches to.
bool isSnapConnection(const ComponentConnection& c);

// Convenience: the snap connectors of a component XML as data-layer
// ConnectionPoints (id = connection name, type = tags).
std::vector<ConnectionPoint> snapConnectionPoints(const std::string& componentXml);

// One visual part mesh of a module (a <connection tags="part ...">/<parts>/<part>),
// with the local offset at which it is mounted on the module.
struct ComponentPart {
  std::string name;  // local instance name, e.g. "part_main" or "part_main01"
  std::string ref;   // cross-ref "<component>.<part>" when the geometry lives in
                     // another component (pier/dock sub-assemblies); else empty
  Transform offset{};
};

// A cross-reference part `ref` split into its component and part names.
struct PartRef {
  std::string component;
  std::string part;
};

// Split a `<part ref="...">` value ("<component>.<part>") used by pier/dock modules
// that instance a shared sub-assembly: the part's mesh lives in the referenced
// component's geometry folder, not the host module's. Returns nullopt if `ref` is
// empty or has no component/part separator.
[[nodiscard]] std::optional<PartRef> parsePartRef(std::string_view ref);

// Where a component's meshes live and which parts assemble it.
struct ComponentGeometry {
  std::string geometryFolder;        // <source geometry=...>, '/'-normalized
  std::vector<ComponentPart> parts;  // connections tagged "part"
};

// True if a part name denotes renderable hull geometry (structural `part_*`,
// greeble `detail_*`, or animated visual `anim_*`) rather than an effect or a
// non-visual helper. X4 modules also ship `fx_*` effects (decals, glows,
// galaxy-scale fieldlines) and collision/bounds/trigger/dummy/emitter volumes;
// drawing those as solid geometry produces sheets and spikes, so they are
// excluded from the visual part list. Matches by token, not just prefix, because
// a helper can hide under a visual prefix (e.g. `detail_l_radiator_collisionbox`).
[[nodiscard]] bool isVisualPart(std::string_view partName);

// Parse the geometry source folder and visual part list from a component XML.
ComponentGeometry parseComponentGeometry(const std::string& componentXml);

// The source .xmf path of a visual part's LOD0 mesh, e.g.
// "<geometryFolder>/part_main-lod0.xmf". The input-side mirror of meshGltfPath:
// the batch converter and the pipeline CLI derive their source paths through
// this so the LOD-suffix/join convention lives in exactly one place.
[[nodiscard]] std::string partXmfPath(const std::string& geometryFolder,
                                      const std::string& partName);

// Like partXmfPath but for an explicit LOD level (0 = highest detail), e.g.
// "<geometryFolder>/part_main-lod2.xmf". The converter probes successive LODs to
// find the most detailed one that fits raylib's 16-bit mesh-index limit.
[[nodiscard]] std::string partXmfPathLod(const std::string& geometryFolder,
                                         const std::string& partName, int lod);

// The module's overall AABB in module-local space: the union of each visual
// part's <size>/<max> half-extents about <size>/<center>, transformed by the
// part's mount offset. Returns a zero box if no part declares a <size>.
[[nodiscard]] AABB moduleAabb(const std::string& componentXml);

// Identity parsed from a macro XML (<macros>/<macro>).
struct MacroInfo {
  std::string macroName;               // e.g. "struct_arg_cross_01_macro"
  std::string componentRef;            // referenced component, e.g. "struct_arg_cross_01"
  std::string className;               // raw class attr, e.g. "connectionmodule"
  std::string makerRace;               // faction from <identification makerrace=...>, may be empty
  Category category{Category::Other};  // mapped from className
};
MacroInfo parseMacro(const std::string& macroXml);

// Map an X4 macro `class` string to our coarse Category.
Category categoryFromClass(const std::string& className);

// The largest dock-size class present in a <docksize tags="..."> string
// ("dock_xl">"dock_l">"dock_m">"dock_s">"dock_xs"), or "" if none. (design §4.2)
[[nodiscard]] std::string largestDockSize(const std::string& docksizeTags);

// Synthesize a module's dock/cradle clearance volumes (design §4.2). The keep-clear
// box is X4's <connection tags="exclusionzone ship_<size>"> marker: each marker's
// pose gives an OBB along its outward local +Z, sized per ship class from
// libraries/parameters.xml <exclusionzones>. Docks with no exclusionzone fall back to
// a corridor spanning the launchpos->todock markers. `macroXml`/`resolveMacroXml`
// (macro name -> XML bytes, nullopt if unindexed) are currently unused — reserved for
// a future surface-dock fallback. Render-free.
[[nodiscard]] std::vector<ClearanceVolume> extractClearanceVolumes(
    const std::string& macroXml, const std::string& componentXml,
    const std::function<std::optional<std::string>(const std::string&)>& resolveMacroXml);

}  // namespace x4sb
