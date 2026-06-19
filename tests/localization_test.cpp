#include "x4sb/assetpipe/localization.hpp"

#include <doctest/doctest.h>

#include <optional>

using namespace x4sb;

namespace {
// A small synthetic l044: a module name entry with a translator comment + nested
// faction/type/size refs, an escaped-paren entry, an all-comment entry, and a
// whitespace-heavy entry.
const char* kXml = R"(<?xml version="1.0" encoding="utf-8"?>
<language id="44">
  <page id="20104">
    <t id="1">OK</t>
    <t id="51401">(Argon Cross Connection Structure 01){20202,101} {20104,51301} {20111,5101}</t>
    <t id="51301">Cross Connection Structure</t>
    <t id="900">$KEY$ \(Left\)(controller hint)</t>
    <t id="901">(only a comment)</t>
    <t id="902">  spaced   out   text  </t>
  </page>
  <page id="20202"><t id="101">Argon</t></page>
  <page id="20111"><t id="5101">L</t></page>
</language>)";
}  // namespace

TEST_CASE("resolveRef does a flat lookup") {
  TextDatabase db;
  db.merge(kXml);
  CHECK(db.resolveRef("{20104,1}") == "OK");
}

TEST_CASE("resolveRef strips comments and resolves nested refs faithfully") {
  TextDatabase db;
  db.merge(kXml);
  CHECK(db.resolveRef("{20104,51401}") == "Argon Cross Connection Structure L");
}

TEST_CASE("resolveRef preserves escaped parens and drops the translator comment") {
  TextDatabase db;
  db.merge(kXml);
  CHECK(db.resolveRef("{20104,900}") == "$KEY$ (Left)");
}

TEST_CASE("resolveRef returns nullopt for an all-comment entry") {
  TextDatabase db;
  db.merge(kXml);
  CHECK(db.resolveRef("{20104,901}") == std::nullopt);
}

TEST_CASE("resolveRef collapses and trims whitespace") {
  TextDatabase db;
  db.merge(kXml);
  CHECK(db.resolveRef("{20104,902}") == "spaced out text");
}

TEST_CASE("resolveRef is nullopt for missing or malformed refs") {
  TextDatabase db;
  db.merge(kXml);
  CHECK(db.resolveRef("{20104,99999}") == std::nullopt);  // missing id
  CHECK(db.resolveRef("{999,1}") == std::nullopt);        // missing page
  CHECK(db.resolveRef("{}") == std::nullopt);
  CHECK(db.resolveRef("{1}") == std::nullopt);
  CHECK(db.resolveRef("{a,b}") == std::nullopt);
  CHECK(db.resolveRef("garbage") == std::nullopt);
}

TEST_CASE("resolveRef terminates on reference cycles") {
  TextDatabase db;
  db.merge(R"(<language id="44"><page id="1">
    <t id="1">{1,2}</t><t id="2">{1,1}</t></page></language>)");
  CHECK(db.resolveRef("{1,1}") == std::nullopt);  // a->b->a... must not hang
}

TEST_CASE("merge overlays later entries over earlier ones") {
  TextDatabase db;
  db.merge(R"(<language id="44"><page id="1"><t id="1">base</t></page></language>)");
  db.merge(R"(<language id="44"><page id="1"><t id="1">dlc</t></page></language>)");
  CHECK(db.resolveRef("{1,1}") == "dlc");
}

TEST_CASE("merge is fail-safe on malformed XML") {
  TextDatabase db;
  db.merge("<not xml");
  db.merge("");
  CHECK(db.size() == 0);
  CHECK(db.resolveRef("{1,1}") == std::nullopt);
}

TEST_CASE("out-of-range text id does not collide into adjacent page") {
  // kPageStride == 1'000'000: id >= kPageStride from page N maps to the same raw
  // key as a valid entry on page N+1, violating isolation. Both paths must guard.
  TextDatabase db;
  // Legitimate entry: page 1, id 0 -> key 1'000'000.
  db.merge(R"(<language id="44"><page id="1"><t id="0">legit</t></page></language>)");
  // A ref with page=0,id=1000000 must not alias to key 1'000'000 (page 1 / id 0).
  CHECK(db.resolveRef("{0,1000000}") == std::nullopt);
  // An entry with id >= kPageStride must be silently dropped (not stored).
  db.merge(R"(<language id="44"><page id="0"><t id="1000000">forged</t></page></language>)");
  CHECK(db.resolveRef("{0,1000000}") == std::nullopt);
  // The original legitimate entry is unaffected.
  CHECK(db.resolveRef("{1,0}") == "legit");
}
