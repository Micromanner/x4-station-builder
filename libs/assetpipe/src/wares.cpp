#include "x4sb/assetpipe/wares.hpp"

#include "strutil.hpp"

#include <pugixml.hpp>

#include <string>

namespace x4sb {
namespace {

// Evaluate a single <ware> as a candidate buildable module; append if it qualifies.
void evalWare(const pugi::xml_node& w, std::vector<WareModule>& out) {
  const std::string tags = w.attribute("tags").as_string();
  if (!detail::hasToken(tags, "module")) return;
  const std::string macroRef = w.child("component").attribute("ref").as_string();
  if (macroRef.empty()) return;
  WareModule m;
  m.wareId = w.attribute("id").as_string();
  m.nameRef = w.attribute("name").as_string();
  m.macroRef = macroRef;
  m.playerBuildable =
      !detail::hasToken(tags, "noplayerblueprint") && !detail::hasToken(tags, "noblueprint");
  if (!m.wareId.empty()) out.push_back(std::move(m));
}

// Walk the tree: evaluate every <ware> as a top-level candidate, but do NOT recurse
// into a ware's own subtree (its <production> ingredient <ware ware=.../> refs are
// not modules). Recurse through every other element (<wares>, <diff>, <add>, ...).
void walk(const pugi::xml_node& node, std::vector<WareModule>& out) {
  for (pugi::xml_node child : node.children()) {
    if (std::string(child.name()) == "ware")
      evalWare(child, out);
    else
      walk(child, out);
  }
}

}  // namespace

std::vector<WareModule> parseModuleWares(const std::string& waresXml) {
  std::vector<WareModule> out;
  pugi::xml_document doc;
  if (!doc.load_string(waresXml.c_str())) return out;
  walk(doc, out);
  return out;
}

}  // namespace x4sb
