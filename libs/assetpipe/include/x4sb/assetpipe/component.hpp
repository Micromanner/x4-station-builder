#pragma once
// Asset-pipeline XML parsing (spec §4A / §10.3): turn an X4 module's component &
// macro XML (extracted from the archives) into the connector transforms and
// identity the data layer needs. Render-free; the pipeline.exe and tests share it.
#include "x4sb/data/types.hpp"

#include <string>
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
  std::string name;  // e.g. "part_main" -> mesh file "<geometry>/part_main-lod0.xmf"
  Transform offset{};
};

// Where a component's meshes live and which parts assemble it.
struct ComponentGeometry {
  std::string geometryFolder;        // <source geometry=...>, '/'-normalized
  std::vector<ComponentPart> parts;  // connections tagged "part"
};

// Parse the geometry source folder and visual part list from a component XML.
ComponentGeometry parseComponentGeometry(const std::string& componentXml);

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

}  // namespace x4sb
