#include "x4sb/assetpipe/catalogbuild.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

using namespace x4sb;

namespace {
// A fake install: one good module fully resolvable, plus one whose macro is
// missing from the index (must be skipped, not fatal).
ExtractFn makeFakeInstall() {
  auto files = std::make_shared<std::unordered_map<std::string, std::string>>();
  (*files)["libraries/wares.xml"] = R"(<wares>
    <ware id="module_arg_prod_foodrations_01" name="{20104,13401}" tags="module">
      <component ref="prod_arg_foodrations_macro"/>
    </ware>
    <ware id="module_ghost_01" name="{1,1}" tags="module">
      <component ref="ghost_macro"/>
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
        <connection name="ConnectionSnap001" tags="snap">
          <offset><position x="0" y="0" z="200"/></offset>
        </connection>
        <connection name="Connection01" tags="part">
          <parts><part name="part_main">
            <size><max x="10" y="20" z="30"/><center x="0" y="0" z="0"/></size>
          </part></parts>
        </connection>
      </connections>
    </component>
  </components>)";
  (*files)["t/0001-l044.xml"] = R"(<language id="44">
    <page id="20104"><t id="13401">(Food Rations comment){20202,101} Food Rations</t></page>
    <page id="20202"><t id="101">Argon</t></page>
  </language>)";
  return [files](const std::string& path) -> std::optional<std::string> {
    const auto it = files->find(path);
    if (it == files->end()) return std::nullopt;
    return it->second;
  };
}
}  // namespace

TEST_CASE("buildModuleCatalog resolves a module and skips unresolvable ones") {
  const CatalogBuildResult res = buildModuleCatalog(makeFakeInstall(), {""});
  REQUIRE(res.modules.size() == 1);
  CHECK(res.skipped.size() == 1);

  const ModuleDef& d = res.modules[0];
  CHECK(d.id == "prod_arg_foodrations_macro");  // macro name is the catalog key
  CHECK(d.wareId == "module_arg_prod_foodrations_01");
  CHECK(d.nameRef == "{20104,13401}");
  CHECK(d.faction == "argon");
  CHECK(d.category == Category::Production);
  REQUIRE(d.connectionPoints.size() == 1);
  CHECK(d.connectionPoints[0].id == "ConnectionSnap001");
  CHECK(d.aabb.max.z == doctest::Approx(30));
  REQUIRE(d.meshRefs.size() == 1);
  CHECK(d.meshRefs[0].gltfPath == "meshes/prod_arg_foodrations_macro__part_main.gltf");
}

TEST_CASE("buildModuleCatalog returns empty when wares.xml is missing") {
  const ExtractFn none = [](const std::string&) -> std::optional<std::string> {
    return std::nullopt;
  };
  CHECK(buildModuleCatalog(none, {""}).modules.empty());
}

namespace {
// Base install + one DLC overlay. The DLC wares.xml is a <diff>; the DLC index
// values are self-qualified (extensions\ego_dlc_test\...).
ExtractFn makeMultiSourceInstall() {
  auto files = std::make_shared<std::unordered_map<std::string, std::string>>();
  // base
  (*files)["libraries/wares.xml"] = R"(<wares>
    <ware id="module_arg_prod_01" name="{1,1}" tags="module"><component ref="prod_arg_macro"/></ware>
  </wares>)";
  (*files)["index/macros.xml"] =
      R"(<index><entry name="prod_arg_macro" value="m/prod_arg_macro"/></index>)";
  (*files)["index/components.xml"] =
      R"(<index><entry name="prod_arg" value="c/prod_arg"/></index>)";
  (*files)["m/prod_arg_macro.xml"] =
      R"(<macros><macro name="prod_arg_macro" class="production"><component ref="prod_arg"/><properties><identification makerrace="argon"/></properties></macro></macros>)";
  (*files)["c/prod_arg.xml"] =
      R"(<components><component name="prod_arg"><connections><connection name="s" tags="snap"><offset><position x="0" y="0" z="1"/></offset></connection></connections></component></components>)";
  // DLC overlay (prefix extensions/ego_dlc_test/)
  (*files)["extensions/ego_dlc_test/libraries/wares.xml"] = R"(<diff><add sel="/wares">
    <ware id="module_bor_prod_01" name="{2,2}" tags="module"><component ref="prod_bor_macro"/></ware>
  </add></diff>)";
  (*files)["extensions/ego_dlc_test/index/macros.xml"] =
      R"(<index><entry name="prod_bor_macro" value="extensions\ego_dlc_test\m\prod_bor_macro"/></index>)";
  (*files)["extensions/ego_dlc_test/index/components.xml"] =
      R"(<index><entry name="prod_bor" value="extensions\ego_dlc_test\c\prod_bor"/></index>)";
  (*files)["extensions/ego_dlc_test/m/prod_bor_macro.xml"] =
      R"(<macros><macro name="prod_bor_macro" class="production"><component ref="prod_bor"/><properties><identification makerrace="boron"/></properties></macro></macros>)";
  (*files)["extensions/ego_dlc_test/c/prod_bor.xml"] =
      R"(<components><component name="prod_bor"><connections><connection name="s" tags="snap"><offset/></connection></connections></component></components>)";
  return [files](const std::string& path) -> std::optional<std::string> {
    const auto it = files->find(path);
    if (it == files->end()) return std::nullopt;
    return it->second;
  };
}
}  // namespace

namespace {
// A fake install: one dock module with a dockingbay child macro so the catalog
// builder exercises extractClearanceVolumes on the real walk path.
ExtractFn makeDockInstall() {
  auto files = std::make_shared<std::unordered_map<std::string, std::string>>();
  (*files)["libraries/wares.xml"] = R"(<wares>
    <ware id="module_arg_dock_01" name="{1,100}" tags="module">
      <component ref="dock_test_macro"/>
    </ware>
  </wares>)";
  (*files)["index/macros.xml"] = R"(<index>
    <entry name="dock_test_macro" value="m/dock_macro"/>
    <entry name="dockingbay_test_l_macro" value="m/db_macro"/>
  </index>)";
  (*files)["index/components.xml"] = R"(<index>
    <entry name="dock_test_comp" value="c/dock_comp"/>
  </index>)";
  (*files)["m/dock_macro.xml"] = R"(<macros>
    <macro name="dock_test_macro" class="buildmodule">
      <component ref="dock_test_comp"/>
      <properties><identification makerrace="argon"/></properties>
      <connections>
        <connection ref="connection_dockingbay01">
          <macro ref="dockingbay_test_l_macro" connection="con_component01"/>
        </connection>
      </connections>
    </macro>
  </macros>)";
  (*files)["c/dock_comp.xml"] = R"(<components>
    <component name="dock_test_comp" class="buildmodule">
      <connections>
        <connection name="connection_dockingbay01" tags="dock_l dockingbay">
          <offset><position x="0" y="0" z="0"/></offset>
        </connection>
        <connection name="connection_launchpos01" tags="launchpos">
          <offset><position x="0" y="0" z="0"/></offset>
        </connection>
        <connection name="connection_todock01" tags="todock">
          <offset><position x="0" y="0" z="1200"/></offset>
        </connection>
        <connection name="Con_exclusionzone001" tags="exclusionzone ship_l">
          <offset><position x="0" y="0" z="600"/></offset>
        </connection>
      </connections>
    </component>
  </components>)";
  // The dockingbay child macro carries the <docksize> that names the ship class.
  (*files)["m/db_macro.xml"] = R"(<macros>
    <macro name="dockingbay_test_l_macro" class="dockingbay">
      <properties><docksize tags="dock_l"/></properties>
    </macro>
  </macros>)";
  return [files](const std::string& path) -> std::optional<std::string> {
    const auto it = files->find(path);
    if (it == files->end()) return std::nullopt;
    return it->second;
  };
}
}  // namespace

TEST_CASE("buildModuleCatalog populates clearanceVolumes for dock modules") {
  const CatalogBuildResult res = buildModuleCatalog(makeDockInstall(), {""});
  REQUIRE(res.modules.size() == 1);
  const ModuleDef& d = res.modules[0];
  REQUIRE(d.clearanceVolumes.size() == 1);
  CHECK(d.clearanceVolumes[0].shipSize == "l");
  // Build module: the exclusionzone ship_l marker (z=600) is authoritative over the
  // launchpos->todock corridor. ship_l box = 1200 x 1200 x 100000 (parameters.xml) ->
  // half-depth 50000, centred 50 km outward of the marker (600 + 50000).
  CHECK(d.clearanceVolumes[0].halfExtents.z == doctest::Approx(50000));
  CHECK(d.clearanceVolumes[0].halfExtents.x == doctest::Approx(600));
  CHECK(d.clearanceVolumes[0].center.z == doctest::Approx(50600));
}

TEST_CASE("buildModuleCatalog merges base and DLC overlays") {
  const CatalogBuildResult res =
      buildModuleCatalog(makeMultiSourceInstall(), {"", "extensions/ego_dlc_test/"});
  REQUIRE(res.modules.size() == 2);
  CHECK(res.skipped.empty());

  const ModuleDef* bor = nullptr;
  const ModuleDef* arg = nullptr;
  for (const ModuleDef& m : res.modules) {
    if (m.id == "prod_bor_macro") bor = &m;
    if (m.id == "prod_arg_macro") arg = &m;
  }
  REQUIRE(arg != nullptr);
  REQUIRE(bor != nullptr);         // the DLC module resolved via the merged overlay index
  CHECK(bor->faction == "boron");  // identity came from the DLC macro XML
}

TEST_CASE("buildModuleCatalog resolves the localized module name") {
  const CatalogBuildResult res = buildModuleCatalog(makeFakeInstall(), {""});
  REQUIRE(res.modules.size() == 1);
  CHECK(res.modules[0].name == "Argon Food Rations");
  CHECK(res.namesResolved == 1);
  CHECK(res.namesUnresolved == 0);
}

TEST_CASE("buildModuleCatalog leaves name empty when no localization file is present") {
  // makeMultiSourceInstall ships no t/0001-l044.xml.
  const CatalogBuildResult res =
      buildModuleCatalog(makeMultiSourceInstall(), {"", "extensions/ego_dlc_test/"});
  REQUIRE(res.modules.size() == 2);
  for (const ModuleDef& m : res.modules) CHECK(m.name.empty());
  CHECK(res.namesResolved == 0);
  CHECK(res.namesUnresolved == 2);
}
