#include "x4sb/editorcore/mesh_paths.hpp"

#include <doctest/doctest.h>

using namespace x4sb;

namespace {
ModuleDef moduleWithMeshes(const std::string& id, const std::vector<std::string>& paths) {
  ModuleDef d;
  d.id = id;
  for (const auto& p : paths) {
    MeshRef r;
    r.gltfPath = p;
    d.meshRefs.push_back(r);
  }
  return d;
}
}  // namespace

TEST_CASE("meshPathsFor returns unique sorted mesh paths, collapsing repeated modules") {
  ModuleCatalog catalog;
  catalog.add(moduleWithMeshes("A", {"b.gltf", "a.gltf"}));  // two parts
  catalog.add(moduleWithMeshes("B", {"a.gltf", "c.gltf"}));  // shares a.gltf with A

  Station station;
  PlacedModule m1;
  m1.defId = "A";
  station.add(m1);
  PlacedModule m2;
  m2.defId = "A";  // a second copy of A -> must not duplicate paths
  station.add(m2);
  PlacedModule m3;
  m3.defId = "B";
  station.add(m3);

  const std::vector<std::string> paths = meshPathsFor(station, catalog);
  CHECK(paths == std::vector<std::string>{"a.gltf", "b.gltf", "c.gltf"});
}

TEST_CASE("meshPathsFor skips missing defs and empty paths") {
  ModuleCatalog catalog;
  ModuleDef a = moduleWithMeshes("A", {"a.gltf"});
  a.meshRefs.push_back(MeshRef{});  // empty gltfPath -> ignored
  catalog.add(a);

  Station station;
  PlacedModule m1;
  m1.defId = "A";
  station.add(m1);
  PlacedModule m2;
  m2.defId = "MISSING";  // no such def -> contributes nothing
  station.add(m2);

  CHECK(meshPathsFor(station, catalog) == std::vector<std::string>{"a.gltf"});
}

TEST_CASE("meshPathsFor on an empty station is empty") {
  ModuleCatalog catalog;
  Station station;
  CHECK(meshPathsFor(station, catalog).empty());
}
