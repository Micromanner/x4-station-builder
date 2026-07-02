#pragma once
// Peripheral role classification (spec §6). Category alone cannot tell a turret
// (Category::Other, "defence_" id) or solar (Category::Production, energy id) from a
// plain functional module, so the role is derived from clearance-presence + id.
#include "x4sb/data/types.hpp"

namespace x4sb {

enum class Role { Interior, Dock, Turret, Solar };

// Interior modules go to the optimizer; Dock/Turret/Solar go to the peripheral pass.
[[nodiscard]] Role classifyRole(const ModuleDef& def);
[[nodiscard]] inline bool isPeripheral(Role r) { return r != Role::Interior; }

}  // namespace x4sb
