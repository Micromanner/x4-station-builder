#pragma once

#include "x4sb/assetpipe/catalogbuild.hpp"

#include <optional>
#include <string>

namespace x4sb {
class Archive;
class MaterialLibrary;

// Reads raw DDS payloads without transcoding, including compressed .gz siblings
// and source .dds/.tga extensions omitted from X4's stored paths.
[[nodiscard]] std::optional<std::string> readTextureDds(const ExtractFn& extract,
                                                        const std::string& path);
// Non-owning callback; archive must outlive all calls.
[[nodiscard]] ExtractFn archiveAssetReader(const Archive& archive);

namespace assetpipe {
[[nodiscard]] std::optional<std::string> clusterComponentPath(const Archive& archive,
                                                              const std::string& cluster);
// In-memory metadata only. Geometry and texture fields contain archive addresses.
// The sun and planets are seen from `sectorMacro` inside the cluster; empty or
// unresolvable = the cluster origin.
[[nodiscard]] std::optional<std::string> nativeBackgroundManifest(
    const Archive& archive, const MaterialLibrary& materials, const std::string& cluster,
    const std::string& sectorMacro = {});
// A sector's position in its cluster, via index/macros.xml -> the cluster macro
// file. nullopt when any link is missing.
[[nodiscard]] std::optional<Vec3> nativeSectorOffset(const Archive& archive, int clusterId,
                                                     const std::string& sectorMacro);
[[nodiscard]] std::string nativeSectorIndex(const Archive& archive);
}  // namespace assetpipe
}  // namespace x4sb
