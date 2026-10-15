#pragma once

#include "x4sb/assetpipe/material_library.hpp"
#include "x4sb/data/catalog.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace x4sb {

class Archive;

struct CachedMetadata {
  ModuleCatalog catalog;
  MaterialLibrary materials;
};

// Identity includes ordered CAT/DAT paths, sizes, timestamps and the schema.
// Missing/unreadable files disable caching rather than accepting stale metadata.
[[nodiscard]] std::optional<std::string> metadataFingerprint(const Archive& archive);
[[nodiscard]] std::optional<CachedMetadata> readMetadataCache(const std::filesystem::path& file,
                                                              const std::string& fingerprint);
[[nodiscard]] bool writeMetadataCache(const std::filesystem::path& file,
                                      const std::string& fingerprint,
                                      const CachedMetadata& metadata);

struct InstallMetadata {
  std::shared_ptr<const Archive> archive;  // fully indexed; safe for concurrent reads
  CachedMetadata metadata;
  bool cacheHit{};
  bool cacheWriteFailed{};
  std::size_t skippedModules{};  // only counted on a rebuild
};

// Index an X4 install and load its module/material metadata: from `cacheFile`
// when the fingerprint matches, else rebuilt from the game XML and written back
// (empty path = no caching). nullopt when the folder has no catalogs or modules.
[[nodiscard]] std::optional<InstallMetadata> loadInstallMetadata(
    const std::string& gameDirectory, const std::filesystem::path& cacheFile);

}  // namespace x4sb
