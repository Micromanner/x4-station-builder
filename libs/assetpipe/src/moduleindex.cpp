#include "x4sb/assetpipe/moduleindex.hpp"

#include "strutil.hpp"

#include <pugixml.hpp>

#include <algorithm>

namespace x4sb {

std::unordered_map<std::string, std::string> parseModuleIndex(const std::string& indexXml) {
  std::unordered_map<std::string, std::string> out;
  pugi::xml_document doc;
  if (!doc.load_string(indexXml.c_str())) return out;

  for (const pugi::xml_node e : doc.child("index").children("entry")) {
    std::string name = e.attribute("name").as_string();
    std::string value = e.attribute("value").as_string();
    if (name.empty() || value.empty()) continue;
    std::replace(value.begin(), value.end(), '\\', '/');
    // Some index values use doubled separators ("\\"); collapse "//" -> "/".
    value = detail::collapseSlashes(std::move(value));
    out[detail::toLower(std::move(name))] = detail::toLower(value + ".xml");
  }
  return out;
}

}  // namespace x4sb
