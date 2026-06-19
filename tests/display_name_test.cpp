#include "x4sb/data/types.hpp"

#include <doctest/doctest.h>

using namespace x4sb;

TEST_CASE("displayName prefers the localized name") {
  ModuleDef d;
  d.id = "struct_arg_cross_01_macro";
  d.name = "Argon Cross Connection Structure";
  CHECK(displayName(d) == "Argon Cross Connection Structure");
}

TEST_CASE("displayName falls back to the macro id when name is empty") {
  ModuleDef d;
  d.id = "struct_arg_cross_01_macro";
  CHECK(displayName(d) == "struct_arg_cross_01_macro");
}
