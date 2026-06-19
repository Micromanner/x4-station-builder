#include "x4sb/assetpipe/component.hpp"

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <vector>

using namespace x4sb;

namespace {
// extractClearanceVolumes no longer needs the macro/dockingbay chain — the corridor
// geometry comes from the module component's keep-clear markers — so tests pass an
// empty macro and a no-op resolver.
std::optional<std::string> noResolve(const std::string&) { return std::nullopt; }
}  // namespace

TEST_CASE("largestDockSize picks the biggest dock class token") {
  CHECK(largestDockSize("dock_l dock_xl") == "dock_xl");
  CHECK(largestDockSize("dock_s") == "dock_s");
  CHECK(largestDockSize("dockingbay") == "");
}

// Piers/dockareas: each exclusionzone marker becomes a keep-clear box that starts AT
// the marker (the dock connection centre) and extends OUTWARD along the marker's local
// +Z (where ships dock). Dimensions come from libraries/parameters.xml <exclusionzones>
// keyed by the ship_<size> tag: ship_xl = 4000 x 4000 x 100000 (full), so half-extents
// are {2000, 2000, 50000} and the box centres 50 km outward of the marker.
TEST_CASE("extractClearanceVolumes: exclusionzone boxes use parameters.xml dims, extend outward") {
  const char* comp = R"(<?xml version="1.0"?>
<components><component name="c" class="pier">
  <connections>
    <connection name="Con_exclusionzone001" tags="exclusionzone ship_xl">
      <offset><position x="0" y="0" z="1887"/></offset>
    </connection>
    <connection name="Con_exclusionzone002" tags="exclusionzone ship_xl">
      <offset><position x="-1887" y="0" z="0"/>
        <quaternion qx="0" qy="0.7071068" qz="0" qw="-0.7071068"/></offset>
    </connection>
  </connections>
</component></components>)";
  const std::vector<ClearanceVolume> vols = extractClearanceVolumes("", comp, noResolve);
  REQUIRE(vols.size() == 2);

  CHECK(vols[0].shipSize == "xl");
  CHECK(vols[0].halfExtents.z == doctest::Approx(50000));  // 100 km depth (ship_xl z=100000)
  CHECK(vols[0].halfExtents.x == doctest::Approx(2000));   // 4000 full width
  CHECK(vols[0].center.z == doctest::Approx(51887));       // 1887 + 50000, outward of the marker

  // -X marker: outward is -X (via the quat), so the box centres further out at -X.
  CHECK(vols[1].center.x == doctest::Approx(-51887));      // -1887 - 50000
  CHECK(vols[1].halfExtents.z == doctest::Approx(50000));
}

// M-class corridors are SHORT (parameters.xml ship_m z=500), not the 100 km of L/XL —
// matches the in-game observation that M and below are much shorter than capital docks.
TEST_CASE("extractClearanceVolumes: M-class exclusionzone is short (500m), not 100km") {
  const char* comp = R"(<?xml version="1.0"?>
<components><component name="c" class="dockarea">
  <connections>
    <connection name="Con_exclusionzone001" tags="exclusionzone ship_m">
      <offset><position x="0" y="0" z="0"/></offset>
    </connection>
  </connections>
</component></components>)";
  const std::vector<ClearanceVolume> vols = extractClearanceVolumes("", comp, noResolve);
  REQUIRE(vols.size() == 1);
  CHECK(vols[0].shipSize == "m");
  CHECK(vols[0].halfExtents.z == doctest::Approx(250));  // 500 m depth
  CHECK(vols[0].halfExtents.x == doctest::Approx(90));   // 180 full width
  CHECK(vols[0].center.z == doctest::Approx(250));       // centred 250 m outward
}

// Build modules carry BOTH launchpos/todock (ship-pathing markers) AND exclusionzone
// keep-clear markers. The exclusionzone is authoritative for the no-build box, so it
// wins: the module emits one box per exclusionzone marker (ship_l = 1200 x 1200 x
// 100000), and the launchpos->todock corridor is NOT emitted.
TEST_CASE("extractClearanceVolumes: build module uses its exclusionzone boxes, not launch->todock") {
  const char* comp = R"(<?xml version="1.0"?>
<components><component name="c" class="buildmodule">
  <connections>
    <connection name="connection_launchpos01" tags="launchpos ">
      <offset><position x="-55" y="-97" z="-101"/></offset>
    </connection>
    <connection name="connection_todock01" tags="todock ">
      <offset><position x="-55" y="-97" z="2399"/></offset>
    </connection>
    <connection name="Con_exclusionzone001" tags="exclusionzone ship_l">
      <offset><position x="0" y="0" z="1099"/></offset>
    </connection>
    <connection name="Con_exclusionzone002" tags="exclusionzone ship_l">
      <offset><position x="0" y="0" z="1200"/></offset>
    </connection>
  </connections>
</component></components>)";
  const std::vector<ClearanceVolume> vols = extractClearanceVolumes("", comp, noResolve);
  REQUIRE(vols.size() == 2);  // the two exclusionzones, not one launch->todock corridor
  CHECK(vols[0].shipSize == "l");
  CHECK(vols[0].halfExtents.x == doctest::Approx(600));    // 1200 full width
  CHECK(vols[0].halfExtents.z == doctest::Approx(50000));  // 100 km depth
  CHECK(vols[0].center.z == doctest::Approx(51099));       // 1099 + 50000, outward
}

// Docks with only launchpos/todock and NO exclusionzone (small surface docks, S/M
// dockareas) fall back to a modeled flight corridor spanning launchpos -> todock.
TEST_CASE("extractClearanceVolumes: dock with only launchpos/todock emits a modeled corridor") {
  const char* comp = R"(<?xml version="1.0"?>
<components><component name="c" class="dockarea">
  <connections>
    <connection name="connection_launchpos01" tags="launchpos ">
      <offset><position x="-55" y="-97" z="-101"/></offset>
    </connection>
    <connection name="connection_todock01" tags="todock ">
      <offset><position x="-55" y="-97" z="2399"/></offset>
    </connection>
  </connections>
</component></components>)";
  const std::vector<ClearanceVolume> vols = extractClearanceVolumes("", comp, noResolve);
  REQUIRE(vols.size() == 1);  // one corridor from the launch/todock pair
  // span = 2399 - (-101) = 2500 -> half 1250; center z = (2399 + -101)/2 = 1149.
  CHECK(vols[0].halfExtents.z == doctest::Approx(1250));
  CHECK(vols[0].center.z == doctest::Approx(1149));
  CHECK(vols[0].center.x == doctest::Approx(-55));
}

// A module with no keep-clear markers (e.g. production/storage) gets no clearance.
TEST_CASE("extractClearanceVolumes: no markers -> no clearance volumes") {
  const char* comp = R"(<?xml version="1.0"?>
<components><component name="c" class="production">
  <connections>
    <connection name="ConnectionSnap001" tags="snap"><offset/></connection>
  </connections>
</component></components>)";
  CHECK(extractClearanceVolumes("", comp, noResolve).empty());
}
