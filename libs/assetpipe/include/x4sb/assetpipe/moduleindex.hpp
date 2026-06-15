#pragma once
// Resolve X4 index files (index/macros.xml, index/components.xml): each
// <entry name=".." value=".."/> maps a macro/component name to the logical path
// of its XML. Keys are lower-cased; values are "<value>.xml", '/'-separated.
#include <string>
#include <unordered_map>

namespace x4sb {

[[nodiscard]] std::unordered_map<std::string, std::string> parseModuleIndex(
    const std::string& indexXml);

}  // namespace x4sb
