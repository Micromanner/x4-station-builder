#include "x4sb/assetpipe/metadata_cache.hpp"

#include "x4sb/archive/archive.hpp"
#include "x4sb/assetpipe/catalogbuild.hpp"
#include "x4sb/platform/platform.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace x4sb {

std::optional<std::string> metadataFingerprint(const Archive& archive) {
  if (archive.catalogPaths().empty()) return std::nullopt;
  auto files = nlohmann::json::array();
  for (const auto& catalog : archive.catalogPaths()) {
    for (const auto* extension : {".cat", ".dat"}) {
      auto path = std::filesystem::path(catalog);
      path.replace_extension(extension);
      std::error_code ec;
      path = std::filesystem::canonical(path, ec);
      if (ec) return std::nullopt;
      const auto size = std::filesystem::file_size(path, ec);
      if (ec) return std::nullopt;
      const auto time = std::filesystem::last_write_time(path, ec);
      if (ec) return std::nullopt;
      files.push_back({path.generic_string(), size, time.time_since_epoch().count()});
    }
  }
  return nlohmann::json{{"schema", 13}, {"sources", archive.sources()}, {"files", files}}.dump();
}

std::optional<CachedMetadata> readMetadataCache(const std::filesystem::path& file,
                                                const std::string& fingerprint) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(file, ec);
  if (ec || size > 64U * 1024U * 1024U) return std::nullopt;
  std::ifstream input(file, std::ios::binary);
  if (!input) return std::nullopt;
  try {
    const auto json = nlohmann::json::parse(input);
    if (json.at("fingerprint") != fingerprint || !json.at("catalog").at("modules").is_array())
      return std::nullopt;
    auto materials = MaterialLibrary::fromCacheJson(json.at("materials").dump());
    if (!materials) return std::nullopt;
    auto catalog = ModuleCatalog::loadFromJson(json.at("catalog").dump());
    if (catalog.size() == 0) return std::nullopt;
    for (const auto& [id, module] : catalog.all()) {
      for (const auto& ref : module.meshRefs)
        if (ref.xmfPath.empty()) return std::nullopt;
    }
    return CachedMetadata{std::move(catalog), std::move(*materials)};
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
}

bool writeMetadataCache(const std::filesystem::path& file, const std::string& fingerprint,
                        const CachedMetadata& metadata) {
  std::vector<ModuleDef> modules;
  modules.reserve(metadata.catalog.size());
  for (const auto& [id, module] : metadata.catalog.all()) modules.push_back(module);
  const nlohmann::json json{{"fingerprint", fingerprint},
                            {"catalog", nlohmann::json::parse(toCatalogJson(modules))},
                            {"materials", nlohmann::json::parse(metadata.materials.toCacheJson())}};
  return platform::writeFileAtomically(file, json.dump());
}

std::optional<InstallMetadata> loadInstallMetadata(const std::string& gameDirectory,
                                                   const std::filesystem::path& cacheFile) {
  auto archive = std::make_shared<Archive>();
  if (archive->addInstall(gameDirectory) == 0) return std::nullopt;
  InstallMetadata result;
  result.archive = archive;
  const auto fingerprint = metadataFingerprint(*archive);
  if (fingerprint && !cacheFile.empty()) {
    if (auto cached = readMetadataCache(cacheFile, *fingerprint)) {
      result.metadata = std::move(*cached);
      result.cacheHit = true;
      return result;
    }
  }
  const auto built = buildModuleCatalog(
      [&archive](const std::string& path) { return archive->extract(path); }, archive->sources());
  if (built.modules.empty()) return std::nullopt;
  result.skippedModules = built.skipped.size();
  result.metadata.materials = mergedMaterialLibrary(*archive);
  for (const auto& module : built.modules) result.metadata.catalog.add(module);
  // A game update during metadata construction must not stamp a mixed snapshot
  // as current. Rebuild next launch if either filesystem observation differs.
  if (fingerprint && metadataFingerprint(*archive) == fingerprint && !cacheFile.empty())
    result.cacheWriteFailed = !writeMetadataCache(cacheFile, *fingerprint, result.metadata);
  return result;
}

}  // namespace x4sb
