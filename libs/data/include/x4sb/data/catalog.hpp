#pragma once
// In-memory module catalog, loaded from the generated catalog.json (spec §4B.1).
#include "x4sb/data/types.hpp"

#include <optional>
#include <string>
#include <unordered_map>

namespace x4sb {

class ModuleCatalog {
 public:
  // Parse a generated catalog.json. Returns nullopt on IO/parse error.
  static std::optional<ModuleCatalog> loadFromFile(const std::string& path);
  // Parse catalog JSON text directly (throws nlohmann::json::exception on bad input).
  static ModuleCatalog loadFromJson(const std::string& jsonText);

  // Insert/overwrite a module (used by the loader and by tests).
  void add(const ModuleDef& def) { modules_[def.id] = def; }

  // Look up by macro id, or nullptr if absent.
  const ModuleDef* find(const std::string& macroId) const;

  std::size_t size() const { return modules_.size(); }
  const std::unordered_map<std::string, ModuleDef>& all() const { return modules_; }

 private:
  std::unordered_map<std::string, ModuleDef> modules_;
};

}  // namespace x4sb
