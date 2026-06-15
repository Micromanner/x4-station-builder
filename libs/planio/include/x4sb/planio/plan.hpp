#pragma once
// Plan I/O (spec §8): Station <-> X4 construction-plan XML. All coordinate
// conversion is delegated to the coords module at this boundary.
#include "x4sb/data/catalog.hpp"
#include "x4sb/document/station.hpp"

#include <optional>
#include <string>

namespace x4sb {

// Serialize a Station to X4 construction-plan XML text.
std::string exportPlanXml(const Station& station, const ModuleCatalog& catalog);

// Parse a construction-plan XML (saved in-game or produced here) into a Station.
// Returns nullopt if the document fails to parse.
std::optional<Station> importPlanXml(const std::string& xml);

// Convenience: write exportPlanXml() to a file. Returns false on IO error.
bool writePlanFile(const Station& station, const ModuleCatalog& catalog, const std::string& path);

}  // namespace x4sb
