#include "x4sb/autolayout/roles.hpp"

#include <doctest/doctest.h>

using namespace x4sb;

namespace {
ModuleDef def(const std::string& id, Category cat) {
  ModuleDef d;
  d.id = id;
  d.category = cat;
  return d;
}
}  // namespace

TEST_CASE("classifyRole: docks via clearance volumes") {
  ModuleDef d = def("dockarea_arg_m_station_01_macro", Category::Dock);
  d.clearanceVolumes.push_back(ClearanceVolume{});  // any corridor present => Dock
  CHECK(classifyRole(d) == Role::Dock);
  CHECK(isPeripheral(Role::Dock));
}

TEST_CASE("classifyRole: turrets via defence_ prefix (category is Other in real data)") {
  CHECK(classifyRole(def("defence_arg_disc_01_macro", Category::Other)) == Role::Turret);
  CHECK(classifyRole(def("defence_par_tube_01_macro", Category::Other)) == Role::Turret);
}

TEST_CASE("classifyRole: turret-component FACTORIES are interior, not turrets") {
  // prod_*turretcomponents builds turret parts; it is a normal production module.
  CHECK(classifyRole(def("prod_gen_turretcomponents_macro", Category::Production)) ==
        Role::Interior);
}

TEST_CASE("classifyRole: solar via id markers") {
  CHECK(classifyRole(def("prod_gen_energycells_macro", Category::Production)) == Role::Solar);
  CHECK(classifyRole(def("xenon_small_station_01_solarpanel_01_macro", Category::Production)) ==
        Role::Solar);
  CHECK(classifyRole(def("prod_tel_sunriseflowers_macro", Category::Production)) == Role::Solar);
}

TEST_CASE("classifyRole: plain functional modules are interior") {
  CHECK(classifyRole(def("prod_arg_foodrations_macro", Category::Production)) == Role::Interior);
  CHECK(classifyRole(def("hab_arg_l_01_macro", Category::Habitat)) == Role::Interior);
  CHECK(classifyRole(def("storage_arg_m_container_01_macro", Category::Storage)) == Role::Interior);
  CHECK_FALSE(isPeripheral(Role::Interior));
}
