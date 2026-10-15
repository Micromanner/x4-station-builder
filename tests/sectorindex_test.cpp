#include "x4sb/assetpipe/sectorindex.hpp"

#include <doctest/doctest.h>

using namespace x4sb::assetpipe;

TEST_CASE("parseMapDefaultsSectors keeps sector datasets, skips cluster/universe ones") {
  // Shapes copied from the real base + Boron mapdefaults.xml (survey 2026-07-19).
  const char* xml = R"(<?xml version="1.0"?>
<defaults>
  <dataset macro="xu_ep2_universe_macro"><properties/></dataset>
  <dataset macro="Cluster_11_macro"><properties/></dataset>
  <dataset macro="Cluster_11_Sector001_macro">
    <properties><identification name="{20004,110011}" description="{20004,110012}"/></properties>
  </dataset>
  <dataset macro="Cluster_601_Sector001_macro"><properties/></dataset>
  <dataset macro="Cluster_x_Sector001_macro"><properties/></dataset>
</defaults>)";
  const std::vector<MapSector> s = parseMapDefaultsSectors(xml);
  REQUIRE(s.size() == 2);
  CHECK(s[0].macro == "Cluster_11_Sector001_macro");
  CHECK(s[0].clusterId == 11);
  CHECK(s[0].nameRef == "{20004,110011}");
  CHECK(s[1].clusterId == 601);  // 3-digit DLC id
  CHECK(s[1].nameRef.empty());   // no identification -> caller falls back to the macro
}

TEST_CASE("parseMapDefaultsSectors: malformed XML -> empty") {
  CHECK(parseMapDefaultsSectors("not xml <").empty());
}

TEST_CASE("sectorIndexJson sorts by name; readSectorIndex round-trips") {
  std::vector<SectorIndexEntry> in = {
      {"Cluster_29_Sector002_macro", "Silent Witness XII", "cluster_29"},
      {"Cluster_14_Sector001_macro", "Argon Prime", "cluster_14"},
  };
  const std::vector<SectorIndexEntry> out = readSectorIndex(sectorIndexJson(std::move(in)));
  REQUIRE(out.size() == 2);
  CHECK(out[0].name == "Argon Prime");
  CHECK(out[0].macro == "Cluster_14_Sector001_macro");
  CHECK(out[1].cluster == "cluster_29");
}

TEST_CASE("readSectorIndex is hostile-safe") {
  CHECK(readSectorIndex("garbage").empty());
  CHECK(readSectorIndex(R"({"not":"array"})").empty());
  // Wrong-typed / incomplete entries are skipped, valid ones kept.
  CHECK(readSectorIndex(
            R"([{"macro":"m","name":123,"cluster":"c"},{"macro":"m2","name":"n","cluster":"c2"}])")
            .size() == 1);
}

TEST_CASE("findMacroPath: exact entry beats wildcard, longest wildcard wins") {
  // Shapes from the real base + Boron index/macros.xml.
  const char* xml = R"(<?xml version="1.0"?>
<index>
  <entry name="cluster_*" value="maps\XU_ep2_universe\clusters" />
  <entry name="cluster_01_sector*" value="maps\XU_ep2_universe\sectors" />
  <entry name="cluster_600_macro" value="extensions\ego_dlc_boron\maps\XU_ep2_universe\dlc_boron_clusters" />
</index>)";
  const auto base = findMacroPath(xml, "Cluster_01_macro");
  REQUIRE(base);
  CHECK(base->path == "maps\\XU_ep2_universe\\clusters");
  CHECK_FALSE(base->exact);
  const auto sector = findMacroPath(xml, "Cluster_01_Sector002_macro");
  REQUIRE(sector);
  CHECK(sector->path == "maps\\XU_ep2_universe\\sectors");
  const auto boron = findMacroPath(xml, "Cluster_600_macro");
  REQUIRE(boron);
  CHECK(boron->exact);
  CHECK(boron->path == "extensions\\ego_dlc_boron\\maps\\XU_ep2_universe\\dlc_boron_clusters");
  CHECK_FALSE(findMacroPath(xml, "ship_macro"));
  CHECK_FALSE(findMacroPath("<index", "cluster_01_macro"));
}

TEST_CASE("parseSectorOffset reads the sectors connection offset; absent offset = origin") {
  // Shape of the real Cluster_01_macro in maps/xu_ep2_universe/clusters.xml.
  const char* xml = R"(<?xml version="1.0"?>
<macros>
  <macro name="Cluster_01_macro" class="cluster">
    <connections>
      <connection name="Cluster_01_Sector001_connection" ref="sectors">
        <macro ref="Cluster_01_Sector001_macro" connection="cluster" />
      </connection>
      <connection name="Cluster_01_Sector002_connection" ref="sectors">
        <offset><position x="-21113958" y="0" z="7381814.5" /></offset>
        <macro ref="Cluster_01_Sector002_macro" connection="cluster" />
      </connection>
      <connection name="SuperHighway001_Cluster_01_connection" ref="sechighways">
        <offset><position x="1" y="2" z="3" /></offset>
        <macro ref="Cluster_01_Sector003_macro" connection="cluster" />
      </connection>
    </connections>
  </macro>
</macros>)";
  const auto second = parseSectorOffset(xml, "cluster_01_macro", "CLUSTER_01_SECTOR002_MACRO");
  REQUIRE(second);
  CHECK(second->x == doctest::Approx(-21113958.0));
  CHECK(second->z == doctest::Approx(7381814.5));
  const auto first = parseSectorOffset(xml, "Cluster_01_macro", "Cluster_01_Sector001_macro");
  REQUIRE(first);
  CHECK(first->x == doctest::Approx(0.0));
  // Only `sectors` connections count; unknown clusters/sectors and bad XML -> nullopt.
  CHECK_FALSE(parseSectorOffset(xml, "Cluster_01_macro", "Cluster_01_Sector003_macro"));
  CHECK_FALSE(parseSectorOffset(xml, "Cluster_02_macro", "Cluster_01_Sector001_macro"));
  CHECK_FALSE(parseSectorOffset("<macros", "Cluster_01_macro", "Cluster_01_Sector001_macro"));
}
