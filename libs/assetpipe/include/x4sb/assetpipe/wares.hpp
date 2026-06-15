#pragma once
// Parse libraries/wares.xml for buildable station modules (spec §2). A module is
// a <ware> whose `tags` attribute contains the token "module" and which has a
// <component ref=...> (the module's macro).
#include <string>
#include <vector>

namespace x4sb {

struct WareModule {
  std::string wareId;    // <ware id=...>
  std::string nameRef;   // raw "{page,id}" from <ware name=...>
  std::string macroRef;  // <component ref=...>, the module macro
  bool playerBuildable{true};  // false if tags include noplayerblueprint/noblueprint
};

[[nodiscard]] std::vector<WareModule> parseModuleWares(const std::string& waresXml);

}  // namespace x4sb
