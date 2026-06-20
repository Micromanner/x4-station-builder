#include "x4sb/assetpipe/meshconvert.hpp"

#include "x4sb/assetpipe/catalogbuild.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

using namespace x4sb;

namespace {
namespace fs = std::filesystem;

// Same self-authored synthetic XUMF tetrahedron used by xmf_test (NOT extracted
// game data — spec §12): 4 float3 verts + 12 uint16 indices, both buffers real
// zlib streams, so parseXmf actually succeeds on it.
const unsigned char kTetraXmf[] = {
    0x58, 0x55, 0x4d, 0x46, 0x03, 0x00, 0x40, 0x00, 0x02, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x0c, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
    0x0c, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x78, 0xda, 0x63, 0x60, 0x40, 0x06, 0x0a, 0x8e,
    0x0c, 0x04, 0xf8, 0x00, 0x13, 0xe3, 0x01, 0x24, 0x78, 0xda, 0x63, 0x60, 0x60, 0x64, 0x60, 0x62,
    0x60, 0x00, 0x92, 0xcc, 0x40, 0x92, 0x09, 0x48, 0x32, 0x82, 0x49, 0x00, 0x00, 0xd0, 0x00, 0x13,
};

std::string tetraXmf() {
  return std::string(reinterpret_cast<const char*>(kTetraXmf), sizeof(kTetraXmf));
}

// An ExtractFn backed by an in-memory logical-path -> bytes map (nullopt on miss).
using FileMap = std::unordered_map<std::string, std::string>;
ExtractFn extractFrom(std::shared_ptr<FileMap> files) {
  return [files = std::move(files)](const std::string& path) -> std::optional<std::string> {
    const auto it = files->find(path);
    if (it == files->end()) return std::nullopt;
    return it->second;
  };
}

// A fake install with one fully-resolvable module that has two visual parts, plus
// the real .xmf bytes for each part at the source geometry path the converter and
// the pipeline derive (geometryFolder + "/" + part.name + "-lod0.xmf").
ExtractFn makeFakeInstall() {
  auto files = std::make_shared<FileMap>();
  (*files)["libraries/wares.xml"] = R"(<wares>
    <ware id="module_arg_prod_foodrations_01" name="{20104,13401}" tags="module">
      <component ref="prod_arg_foodrations_macro"/>
    </ware>
  </wares>)";
  (*files)["index/macros.xml"] = R"(<index>
    <entry name="prod_arg_foodrations_macro" value="m/prod_macro"/>
  </index>)";
  (*files)["index/components.xml"] = R"(<index>
    <entry name="prod_arg_foodrations" value="c/prod_comp"/>
  </index>)";
  (*files)["m/prod_macro.xml"] = R"(<macros>
    <macro name="prod_arg_foodrations_macro" class="production">
      <component ref="prod_arg_foodrations"/>
      <properties><identification name="{20104,13401}" makerrace="argon"/></properties>
    </macro>
  </macros>)";
  (*files)["c/prod_comp.xml"] = R"(<components>
    <component name="prod_arg_foodrations" class="production">
      <source geometry="assets/structures/production/prod_data"/>
      <connections>
        <connection name="Connection01" tags="part">
          <parts><part name="part_main">
            <size><max x="10" y="20" z="30"/><center x="0" y="0" z="0"/></size>
          </part></parts>
        </connection>
        <connection name="Connection02" tags="part">
          <parts><part name="part_extra">
            <size><max x="5" y="5" z="5"/><center x="0" y="0" z="0"/></size>
          </part></parts>
        </connection>
      </connections>
    </component>
  </components>)";
  // Real mesh bytes at the two source paths the converter resolves.
  (*files)["assets/structures/production/prod_data/part_main-lod0.xmf"] = tetraXmf();
  (*files)["assets/structures/production/prod_data/part_extra-lod0.xmf"] = tetraXmf();
  return extractFrom(std::move(files));
}

// Unique scratch dir under the system temp; removed by the RAII guard.
struct TempDir {
  fs::path path;
  TempDir() {
    std::error_code ec;
    const std::string unique = std::to_string(reinterpret_cast<std::uintptr_t>(this));
    path = fs::temp_directory_path(ec) / fs::path("x4sb_meshconvert_test_" + unique);
    fs::remove_all(path, ec);
    fs::create_directories(path, ec);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;
};
}  // namespace

TEST_CASE("meshGltfPath composes the cache-relative path") {
  CHECK(meshGltfPath("foo_macro", "part_main") == "meshes/foo_macro__part_main.gltf");
}

TEST_CASE("chooseMeshLod picks the most-detailed LOD within budget") {
  using A = std::array<std::optional<std::uint32_t>, 4>;
  // lod0 already fits -> keep full detail.
  CHECK(chooseMeshLod(A{40000u, 10000u, 5000u, 1000u}, 65535) == 0);
  // lod0 oversized; lod2 is the most detailed that fits (mirrors pier_spl_harbor_03).
  CHECK(chooseMeshLod(A{600000u, 250000u, 63000u, 6000u}, 65535) == 2);
  // only lod3 fits (mirrors storage_ter_l_container_01).
  CHECK(chooseMeshLod(A{419000u, 309000u, 113000u, 30000u}, 65535) == 3);
  // lod0 missing but lod1 fits -> fall through to it.
  CHECK(chooseMeshLod(A{std::nullopt, 50000u, 10000u, std::nullopt}, 65535) == 1);
}

TEST_CASE("chooseMeshLod falls back to the fewest-vertex LOD when none fit") {
  using A = std::array<std::optional<std::uint32_t>, 4>;
  // None fit the budget: pick the smallest (it will still box, but best effort).
  CHECK(chooseMeshLod(A{900000u, 800000u, 700000u, 70000u}, 65535) == 3);
  // Nothing shipped at all -> no choice.
  CHECK_FALSE(
      chooseMeshLod(A{std::nullopt, std::nullopt, std::nullopt, std::nullopt}, 65535).has_value());
}

TEST_CASE("convertModuleMeshes writes exactly the paths buildModuleCatalog records") {
  const ExtractFn extract = makeFakeInstall();
  TempDir out;

  // The catalog and the converter walk the SAME fake data.
  const CatalogBuildResult cat = buildModuleCatalog(extract, {""});
  REQUIRE(cat.modules.size() == 1);
  REQUIRE(cat.modules[0].meshRefs.size() == 2);

  const MeshConvertResult conv =
      convertModuleMeshes(extract, {""}, out.path.string(), /*force=*/false, nullptr);
  CHECK(conv.modules == 1);
  CHECK(conv.converted == 2);
  CHECK(conv.skipped == 0);
  CHECK(conv.failed == 0);

  // RECONCILIATION: every gltfPath the catalog records must exist on disk.
  for (const ModuleDef& m : cat.modules)
    for (const MeshRef& mr : m.meshRefs) {
      const fs::path written = out.path / mr.gltfPath;
      CHECK_MESSAGE(fs::exists(written), "missing converted mesh: " << mr.gltfPath);
    }

  // A second non-forced run must skip the existing outputs, not re-convert.
  const MeshConvertResult again =
      convertModuleMeshes(extract, {""}, out.path.string(), /*force=*/false, nullptr);
  CHECK(again.modules == 1);
  CHECK(again.converted == 0);
  CHECK(again.skipped == 2);
  CHECK(again.failed == 0);

  // force=true re-converts them.
  const MeshConvertResult forced =
      convertModuleMeshes(extract, {""}, out.path.string(), /*force=*/true, nullptr);
  CHECK(forced.converted == 2);
  CHECK(forced.skipped == 0);
}

TEST_CASE("convertModuleMeshes falls back to a lower LOD when lod0 is absent") {
  auto files = std::make_shared<FileMap>();
  (*files)["libraries/wares.xml"] = R"(<wares>
    <ware id="module_y" name="{1,1}" tags="module"><component ref="y_macro"/></ware>
  </wares>)";
  (*files)["index/macros.xml"] = R"(<index><entry name="y_macro" value="m/y"/></index>)";
  (*files)["index/components.xml"] = R"(<index><entry name="y_comp" value="c/y"/></index>)";
  (*files)["m/y.xml"] =
      R"(<macros><macro name="y_macro" class="production"><component ref="y_comp"/></macro></macros>)";
  (*files)["c/y.xml"] = R"(<components><component name="y_comp">
    <source geometry="g/y"/>
    <connections><connection name="C" tags="part"><parts><part name="part_main"/></parts></connection></connections>
  </component></components>)";
  // Only lod2 is shipped (no lod0/lod1): the converter must probe and use it.
  (*files)["g/y/part_main-lod2.xmf"] = tetraXmf();
  const ExtractFn extract = extractFrom(std::move(files));

  TempDir out;
  const MeshConvertResult conv =
      convertModuleMeshes(extract, {""}, out.path.string(), /*force=*/false, nullptr);
  CHECK(conv.converted == 1);
  CHECK(conv.failed == 0);
  CHECK(conv.reducedLod == 1);  // fell back below lod0
  CHECK(fs::exists(out.path / meshGltfPath("y_macro", "part_main")));
}

TEST_CASE("convertModuleMeshes resolves an xref part's mesh from the referenced component") {
  auto files = std::make_shared<FileMap>();
  (*files)["libraries/wares.xml"] = R"(<wares>
    <ware id="module_pier" name="{1,1}" tags="module"><component ref="pier_macro"/></ware>
  </wares>)";
  (*files)["index/macros.xml"] = R"(<index><entry name="pier_macro" value="m/pier"/></index>)";
  // The component index must resolve BOTH the module's component and the xref target.
  (*files)["index/components.xml"] = R"(<index>
    <entry name="pier_comp" value="c/pier"/>
    <entry name="xref_bay" value="c/xref_bay"/>
  </index>)";
  (*files)["m/pier.xml"] =
      R"(<macros><macro name="pier_macro" class="dockarea"><component ref="pier_comp"/></macro></macros>)";
  // The pier's own folder has no inst mesh; the geometry lives in xref_bay.
  (*files)["c/pier.xml"] = R"(<components><component name="pier_comp">
    <source geometry="g/pier"/>
    <connections><connection name="C" tags="part"><parts>
      <part ref="xref_bay.part_main" name="part_main01"/>
    </parts></connection></connections>
  </component></components>)";
  (*files)["c/xref_bay.xml"] =
      R"(<components><component name="xref_bay"><source geometry="g/baydata"/></component></components>)";
  (*files)["g/baydata/part_main-lod0.xmf"] = tetraXmf();
  const ExtractFn extract = extractFrom(std::move(files));

  TempDir out;
  const MeshConvertResult conv =
      convertModuleMeshes(extract, {""}, out.path.string(), /*force=*/false, nullptr);
  CHECK(conv.converted == 1);
  CHECK(conv.failed == 0);
  // Output is keyed by the LOCAL instance name but sourced from the xref geometry.
  CHECK(fs::exists(out.path / meshGltfPath("pier_macro", "part_main01")));
}

TEST_CASE("convertModuleMeshes counts a missing source mesh as failed") {
  auto files = std::make_shared<FileMap>();
  (*files)["libraries/wares.xml"] = R"(<wares>
    <ware id="module_x" name="{1,1}" tags="module"><component ref="x_macro"/></ware>
  </wares>)";
  (*files)["index/macros.xml"] = R"(<index><entry name="x_macro" value="m/x"/></index>)";
  (*files)["index/components.xml"] = R"(<index><entry name="x_comp" value="c/x"/></index>)";
  (*files)["m/x.xml"] =
      R"(<macros><macro name="x_macro" class="production"><component ref="x_comp"/></macro></macros>)";
  (*files)["c/x.xml"] = R"(<components><component name="x_comp">
    <source geometry="g/x"/>
    <connections><connection name="C" tags="part"><parts><part name="p"/></parts></connection></connections>
  </component></components>)";
  // NOTE: no g/x/p-lod0.xmf -> source missing.
  const ExtractFn extract = extractFrom(std::move(files));

  TempDir out;
  const MeshConvertResult conv =
      convertModuleMeshes(extract, {""}, out.path.string(), /*force=*/false, nullptr);
  CHECK(conv.modules == 1);
  CHECK(conv.converted == 0);
  CHECK(conv.failed == 1);
}
