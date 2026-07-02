#include "x4sb/autolayout/autolayout.hpp"
#include "x4sb/autolayout/metrics.hpp"
#include "x4sb/autolayout/search.hpp"
#include "x4sb/autolayout/seed.hpp"
#include "x4sb/data/math.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace x4sb;

namespace {
const AABB kUnit{{-0.5, -0.5, -0.5}, {0.5, 0.5, 0.5}};

// Connector-normal quaternion mapping the module-local +Z normal (snap convention) to `dir`, so
// a fixture's port points OUTWARD like the real catalog (whose ±X/±Y/±Z ports carry exactly this
// rotation). Fixtures with identity rotations make every normal +Z, which collapses directional
// layout onto the Z axis — faithful normals are required to exercise the fishbone's axes.
Quat zTo(Vec3 dir) {
  const Vec3 z{0, 0, 1};
  const Vec3 axis = cross(z, dir);
  const double d = dot(z, dir);
  if (length(axis) < 1e-9) return d > 0 ? Quat{} : axisAngle(Vec3{1, 0, 0}, std::acos(-1.0));
  return axisAngle(axis, std::acos(std::clamp(d, -1.0, 1.0)));
}

ModuleDef hubDef(const std::string& id) {
  ModuleDef d;
  d.id = id;
  d.category = Category::Connector;
  d.aabb = kUnit;
  for (const auto& [pid, pos] : std::vector<std::pair<std::string, Vec3>>{
           {"c0", {2, 0, 0}}, {"c1", {-2, 0, 0}}, {"c2", {0, 0, 2}}, {"c3", {0, 0, -2}}}) {
    ConnectionPoint cp;
    cp.id = pid;
    cp.localPosition = pos;
    cp.type = "snap";
    d.connectionPoints.push_back(cp);
  }
  return d;
}
ModuleDef spokeDef(const std::string& id, Category cat = Category::Production) {
  ModuleDef d;
  d.id = id;
  d.category = cat;
  d.aabb = kUnit;
  ConnectionPoint cp;
  cp.id = "s";
  cp.localPosition = {0, 0, 0};
  cp.type = "snap";
  d.connectionPoints.push_back(cp);
  return d;
}
// A functional module with TWO ports (like real modules) — so once a hub fills, the seed
// could either CHAIN the next module functional-to-functional (the bug) or synthesize a new
// hub (the fix). A 1-port spoke can't show this — it floats instead of chaining.
ModuleDef twoPortDef(const std::string& id, Category cat = Category::Production) {
  ModuleDef d;
  d.id = id;
  d.category = cat;
  d.aabb = kUnit;
  for (const auto& [pid, pos] :
       std::vector<std::pair<std::string, Vec3>>{{"a", {2, 0, 0}}, {"b", {-2, 0, 0}}}) {
    ConnectionPoint cp;
    cp.id = pid;
    cp.localPosition = pos;
    cp.type = "snap";
    d.connectionPoints.push_back(cp);
  }
  return d;
}
// Every functional (non-Connector) module attaches ONLY to hub (Connector) ports — never
// functional-to-functional. A floating module (no links) passes trivially.
bool functionalsAttachToHubsOnly(const Station& s, const ModuleCatalog& c) {
  for (const auto& m : s.modules()) {
    const ModuleDef* dm = c.find(m.defId);
    if (dm == nullptr || dm->category == Category::Connector) continue;
    for (const Link& l : m.links) {
      const PlacedModule* o = s.find(l.otherInstanceId);
      if (o == nullptr) continue;
      const ModuleDef* od = c.find(o->defId);
      if (od != nullptr && od->category != Category::Connector) return false;  // chained!
    }
  }
  return true;
}
}  // namespace

TEST_CASE(
    "compactSeed routes functional modules through hubs (no functional-to-functional chains)") {
  ModuleCatalog c;
  c.add(hubDef("struct_arg_cross_01_macro"));  // 4-port hub
  c.add(twoPortDef("prod_arg_x"));
  const std::vector<std::string> cart(8, "prod_arg_x");  // exceeds one 4-port hub

  Station green;
  const int added = compactSeed(green, cart, c, AutoLayoutOptions{});
  CHECK(added >= 2);  // 8 modules need more than one hub — not a single token hub
  CHECK(functionalsAttachToHubsOnly(green, c));  // every module sits on a hub port
  CHECK(layoutLegal(green, c, AutoLayoutOptions{}));

  // Additive: building onto a non-empty station must still synthesize hubs for the new
  // modules (the editor's usual case once anything is placed).
  Station pre;
  {
    PlacedModule m;
    m.defId = "prod_arg_x";
    pre.add(m);
  }
  const int addedAdd = compactSeed(pre, std::vector<std::string>(4, "prod_arg_x"), c, {});
  CHECK(addedAdd >= 1);  // hubs synthesized in additive mode, not zero
  CHECK(functionalsAttachToHubsOnly(pre, c));
}

TEST_CASE("compactSeed picks a compatible hub even when the faction hub is incompatible") {
  // Reproduces the real-catalog venture trap: the only faction-matching hub ("gen") has
  // "snap venture" ports that mate nothing normal, while a non-faction hub has plain "snap"
  // ports. The seed must host the modules on the compatible hub instead of breeding empty
  // venture hubs and floating every module.
  ModuleCatalog c;
  ModuleDef venture = hubDef("struct_gen_venturercross_01_macro");     // faction "gen"
  for (auto& cp : venture.connectionPoints) cp.type = "snap venture";  // incompatible with "snap"
  c.add(venture);
  c.add(hubDef("struct_arg_cross_01_macro"));  // faction "arg", plain "snap" ports
  c.add(spokeDef("prod_gen_hullparts"));       // faction "gen", "snap" port (like the real module)

  Station work;                                                      // greenfield
  const std::vector<std::string> interior(3, "prod_gen_hullparts");  // fit one 4-port hub
  const int connectorsAdded = compactSeed(work, interior, c, AutoLayoutOptions{});

  CHECK(connectorsAdded >= 1);
  CHECK(functionalsAttachToHubsOnly(work, c));
  // Every module must be CONNECTED (snapped to the compatible hub), not floated as a root.
  int spokes = 0, snappedSpokes = 0, ventureHubs = 0;
  for (const auto& m : work.modules()) {
    if (m.defId == "prod_gen_hullparts") {
      ++spokes;
      if (!m.links.empty()) ++snappedSpokes;
    }
    if (m.defId == "struct_gen_venturercross_01_macro") ++ventureHubs;
  }
  CHECK(spokes == 3);
  CHECK(snappedSpokes == 3);  // all attached, none floating
  CHECK(ventureHubs == 0);    // the incompatible faction hub is never synthesized
}

namespace {
// A 6-port hub (snap) with ports 200 out on each axis.
ModuleDef hub6(const std::string& id) {
  ModuleDef d;
  d.id = id;
  d.category = Category::Connector;
  d.aabb = AABB{{-50, -50, -50}, {50, 50, 50}};
  for (const auto& [pid, pos] : std::vector<std::pair<std::string, Vec3>>{{"px", {200, 0, 0}},
                                                                          {"nx", {-200, 0, 0}},
                                                                          {"py", {0, 200, 0}},
                                                                          {"ny", {0, -200, 0}},
                                                                          {"pz", {0, 0, 200}},
                                                                          {"nz", {0, 0, -200}}}) {
    ConnectionPoint cp;
    cp.id = pid;
    cp.localPosition = pos;
    cp.localRotation = zTo(pos * (1.0 / length(pos)));  // normal points outward, like real hubs
    cp.type = "snap";
    d.connectionPoints.push_back(cp);
  }
  return d;
}
// A long straight 2-port beam (snap): ports symmetric about the origin along Z, normals outward
// (+Z / −Z) so chaining it extends the spine straight in the mate direction.
ModuleDef beamDef(const std::string& id, double len) {
  ModuleDef d;
  d.id = id;
  d.category = Category::Connector;
  d.aabb = AABB{{-20, -20, -len / 2}, {20, 20, len / 2}};
  for (const auto& [pid, z] :
       std::vector<std::pair<std::string, double>>{{"a", len / 2}, {"b", -len / 2}}) {
    ConnectionPoint cp;
    cp.id = pid;
    cp.localPosition = {0, 0, z};
    cp.localRotation = zTo(Vec3{0, 0, z > 0 ? 1.0 : -1.0});
    cp.type = "snap";
    d.connectionPoints.push_back(cp);
  }
  return d;
}
// A bulky 1-port module: big enough that hubs chained adjacently (400 apart) would collide, so
// the seed MUST space them with beams. Half-extent < the 200 hub-port offset so two modules on a
// hub's adjacent perpendicular ports clear each other (avoids a degenerate exact-corner touch).
ModuleDef bigSpoke(const std::string& id) {
  ModuleDef d;
  d.id = id;
  d.category = Category::Production;
  d.aabb = AABB{{-90, -90, -90}, {90, 90, 90}};
  ConnectionPoint cp;
  cp.id = "s";
  cp.localPosition = {0, 0, 0};
  cp.type = "snap";
  d.connectionPoints.push_back(cp);
  return d;
}
// True if the station is a single connected component (BFS over links from the first module).
bool oneConnectedComponent(const Station& s) {
  if (s.empty()) return true;
  std::vector<InstanceId> stack{s.modules().front().instanceId};
  std::vector<InstanceId> seen{stack.front()};
  while (!stack.empty()) {
    const InstanceId id = stack.back();
    stack.pop_back();
    const PlacedModule* m = s.find(id);
    if (m == nullptr) continue;
    for (const Link& l : m->links)
      if (std::find(seen.begin(), seen.end(), l.otherInstanceId) == seen.end()) {
        seen.push_back(l.otherInstanceId);
        stack.push_back(l.otherInstanceId);
      }
  }
  return seen.size() == s.size();
}
// A 4-port FUNCTIONAL module — an organic "core" that should host several neighbours directly.
ModuleDef coreDef(const std::string& id, Category cat = Category::Storage) {
  ModuleDef d;
  d.id = id;
  d.category = cat;
  d.aabb = kUnit;
  for (const auto& [pid, pos] : std::vector<std::pair<std::string, Vec3>>{
           {"p0", {2, 0, 0}}, {"p1", {-2, 0, 0}}, {"p2", {0, 0, 2}}, {"p3", {0, 0, -2}}}) {
    ConnectionPoint cp;
    cp.id = pid;
    cp.localPosition = pos;
    cp.type = "snap";
    d.connectionPoints.push_back(cp);
  }
  return d;
}
// At least one functional<->functional link exists (organic chains modules directly; the inverse
// of functionalsAttachToHubsOnly).
bool someFunctionalChain(const Station& s, const ModuleCatalog& c) {
  for (const auto& m : s.modules()) {
    const ModuleDef* dm = c.find(m.defId);
    if (dm == nullptr || dm->category == Category::Connector) continue;
    for (const Link& l : m.links) {
      const PlacedModule* o = s.find(l.otherInstanceId);
      if (o == nullptr) continue;
      const ModuleDef* od = c.find(o->defId);
      if (od != nullptr && od->category != Category::Connector) return true;
    }
  }
  return false;
}

// A hub with ports nearly flush at its faces (no pylon reach), like real cross hubs. Fat modules
// mated on it press right against each other, closing a collision shell around every free port.
ModuleDef snugHub(const std::string& id) {
  ModuleDef d;
  d.id = id;
  d.category = Category::Connector;
  d.aabb = AABB{{-50, -50, -50}, {50, 50, 50}};
  for (const auto& [pid, pos] : std::vector<std::pair<std::string, Vec3>>{{"px", {60, 0, 0}},
                                                                          {"nx", {-60, 0, 0}},
                                                                          {"py", {0, 60, 0}},
                                                                          {"ny", {0, -60, 0}},
                                                                          {"pz", {0, 0, 60}},
                                                                          {"nz", {0, 0, -60}}}) {
    ConnectionPoint cp;
    cp.id = pid;
    cp.localPosition = pos;
    cp.localRotation = zTo(pos * (1.0 / length(pos)));
    cp.type = "snap";
    d.connectionPoints.push_back(cp);
  }
  return d;
}
// A 1-port module whose body dwarfs the snug hub: modules on perpendicular hub ports overlap, so
// only opposing ports seat, and a hub injected beside two seated fats is itself walled in — the
// seed must chain MORE structure outward to escape (the real-catalog failure in miniature).
ModuleDef fatLeaf(const std::string& id) {
  ModuleDef d;
  d.id = id;
  d.category = Category::Production;
  d.aabb = AABB{{-200, -200, -200}, {200, 200, 200}};
  ConnectionPoint cp;
  cp.id = "s";
  cp.localPosition = {0, 0, 200};  // flush on the +Z face, normal outward (identity = +Z)
  cp.type = "snap";
  d.connectionPoints.push_back(cp);
  return d;
}

// Longest path through FUNCTIONAL-only modules (hubs break the chain). DFS from each functional
// module over functional<->functional links; returns the max node count on any such path.
int longestFunctionalChain(const Station& s, const ModuleCatalog& c) {
  const auto isFunc = [&](InstanceId id) {
    const PlacedModule* m = s.find(id);
    if (m == nullptr) return false;
    const ModuleDef* d = c.find(m->defId);
    return d != nullptr && d->category != Category::Connector;
  };
  int best = 0;
  for (const auto& start : s.modules()) {
    if (!isFunc(start.instanceId)) continue;
    std::vector<std::pair<InstanceId, int>> stack{{start.instanceId, 1}};
    std::vector<InstanceId> seen{start.instanceId};
    while (!stack.empty()) {
      const auto [id, depth] = stack.back();
      stack.pop_back();
      best = std::max(best, depth);
      const PlacedModule* m = s.find(id);
      if (m == nullptr) continue;
      for (const Link& l : m->links)
        if (isFunc(l.otherInstanceId) &&
            std::find(seen.begin(), seen.end(), l.otherInstanceId) == seen.end()) {
          seen.push_back(l.otherInstanceId);
          stack.push_back({l.otherInstanceId, depth + 1});
        }
    }
  }
  return best;
}
}  // namespace

TEST_CASE("compactSeed builds ONE connected, beam-spaced skeleton for big modules") {
  ModuleCatalog c;
  c.add(hub6("struct_arg_cross_01_macro"));
  c.add(beamDef("struct_arg_base_01_macro", 1000.0));  // a long straight beam to space hubs
  c.add(bigSpoke("prod_arg_big"));
  Station work;  // greenfield
  const std::vector<std::string> interior(8, "prod_arg_big");

  const int connectorsAdded = compactSeed(work, interior, c, AutoLayoutOptions{});

  CHECK(connectorsAdded >= 2);                       // multiple hubs + beams synthesized
  CHECK(functionalsAttachToHubsOnly(work, c));       // never functional-to-functional
  CHECK(oneConnectedComponent(work));                // the whole thing is ONE structure
  CHECK(layoutLegal(work, c, AutoLayoutOptions{}));  // no overlap, in plot, corridors clear
  int spokes = 0, snapped = 0, beams = 0;
  for (const auto& m : work.modules()) {
    if (m.defId == "prod_arg_big") {
      ++spokes;
      if (!m.links.empty()) ++snapped;
    }
    if (m.defId == "struct_arg_base_01_macro") ++beams;
  }
  CHECK(spokes == 8);
  CHECK(snapped == 8);  // every module connected, none floating
  CHECK(beams >= 1);    // hubs were spaced with straight beams, not chained adjacently
}

TEST_CASE("compactSeed places all interior modules legally and synthesizes a hub") {
  ModuleCatalog c;
  c.add(hubDef("struct_arg_cross_01_macro"));  // faction "arg", a hub
  c.add(spokeDef("prod_arg_a"));
  Station work;  // greenfield
  const std::vector<std::string> interior{"prod_arg_a", "prod_arg_a", "prod_arg_a"};

  const int connectorsAdded = compactSeed(work, interior, c, AutoLayoutOptions{});

  CHECK(connectorsAdded >= 1);  // at least one hub synthesized
  CHECK(work.size() == 3u + static_cast<std::size_t>(connectorsAdded));  // 3 spokes + hubs
  CHECK(layoutLegal(work, c, AutoLayoutOptions{}));  // no overlap, in plot, corridors clear
  // Every spoke is connected (the seed snapped them, not floated them).
  int spokes = 0;
  for (const auto& m : work.modules())
    if (m.defId == "prod_arg_a") {
      ++spokes;
      CHECK_FALSE(m.links.empty());
    }
  CHECK(spokes == 3);
}

TEST_CASE("compactSeed is deterministic") {
  ModuleCatalog c;
  c.add(hubDef("struct_arg_cross_01_macro"));
  c.add(spokeDef("prod_arg_a"));
  const std::vector<std::string> interior{"prod_arg_a", "prod_arg_a"};

  Station a;
  Station b;
  compactSeed(a, interior, c, AutoLayoutOptions{});
  compactSeed(b, interior, c, AutoLayoutOptions{});
  REQUIRE(a.size() == b.size());
  for (std::size_t i = 0; i < a.modules().size(); ++i) {
    CHECK(a.modules()[i].defId == b.modules()[i].defId);
    CHECK(a.modules()[i].worldTransform.position.x ==
          doctest::Approx(b.modules()[i].worldTransform.position.x));
  }
}

TEST_CASE("compactSeed is additive: a pre-existing module is left untouched") {
  ModuleCatalog c;
  c.add(hubDef("struct_arg_cross_01_macro"));
  c.add(spokeDef("prod_arg_a"));
  Station work;
  PlacedModule pre;
  pre.defId = "struct_arg_cross_01_macro";
  pre.worldTransform.position = {0, 0, 0};
  const InstanceId preId = work.add(pre);  // an existing hub with free ports

  compactSeed(work, {"prod_arg_a"}, c, AutoLayoutOptions{});

  REQUIRE(work.find(preId) != nullptr);
  CHECK(work.find(preId)->worldTransform.position.x == doctest::Approx(0.0));  // not moved
  CHECK(work.size() >= 2u);  // the added spoke (snapped onto the existing hub)
}

TEST_CASE("fishboneSeed places every module of a single type, legal and connected on hubs") {
  ModuleCatalog c;
  c.add(hub6("struct_arg_cross_01_macro"));
  c.add(beamDef("struct_arg_base_01_macro", 1000.0));  // straight beam to space hubs
  c.add(bigSpoke("prod_arg_big"));
  Station work;  // greenfield
  const std::vector<std::string> interior(10, "prod_arg_big");

  const int connectorsAdded = fishboneSeed(work, interior, c, AutoLayoutOptions{});

  CHECK(connectorsAdded >= 1);
  int spokes = 0, snapped = 0;
  for (const auto& m : work.modules())
    if (m.defId == "prod_arg_big") {
      ++spokes;
      if (!m.links.empty()) ++snapped;
    }
  CHECK(spokes == 10);                               // all placed
  CHECK(snapped == 10);                              // every module connected, none floating
  CHECK(functionalsAttachToHubsOnly(work, c));       // modules sit on connector ports only
  CHECK(oneConnectedComponent(work));                // one structure
  CHECK(layoutLegal(work, c, AutoLayoutOptions{}));  // no overlap, in plot, corridors clear
}

TEST_CASE("fishboneSeed builds a high backbone with modules hanging below it") {
  ModuleCatalog c;
  c.add(hub6("struct_arg_cross_01_macro"));
  c.add(beamDef("struct_arg_base_01_macro", 1000.0));
  c.add(bigSpoke("prod_arg_big"));
  Station work;
  const std::vector<std::string> interior(10, "prod_arg_big");

  AutoLayoutOptions opts;  // default plotHalfExtent 10000 -> skeleton plane at 0.35*10000 = 3500
  fishboneSeed(work, interior, c, opts);

  double maxConnectorY = -1e18;
  double minFunctionalY = 1e18;
  for (const auto& m : work.modules()) {
    const ModuleDef* d = c.find(m.defId);
    if (d == nullptr) continue;
    const double y = m.worldTransform.position.y;
    if (d->category == Category::Connector)
      maxConnectorY = std::max(maxConnectorY, y);
    else
      minFunctionalY = std::min(minFunctionalY, y);
  }
  // The skeleton plane is biased high toward +Y, not at the origin like the greedy seed.
  CHECK(maxConnectorY == doctest::Approx(3500.0).epsilon(0.05));
  // Production modules grow DOWNWARD off the ribs, so the lowest sits well below the backbone.
  CHECK(minFunctionalY < 3300.0);
}

TEST_CASE("fishboneSeed groups distinct module types into separate branches") {
  ModuleCatalog c;
  c.add(hub6("struct_arg_cross_01_macro"));
  c.add(beamDef("struct_arg_base_01_macro", 1000.0));
  c.add(bigSpoke("prod_arg_a"));
  c.add(bigSpoke("prod_arg_b"));
  Station work;
  std::vector<std::string> interior(6, "prod_arg_a");
  for (int i = 0; i < 6; ++i) interior.emplace_back("prod_arg_b");

  AutoLayoutOptions opts;
  opts.mixRibModules = false;
  fishboneSeed(work, interior, c, opts);

  int aCount = 0, bCount = 0;
  double aSumX = 0, bSumX = 0;
  for (const auto& m : work.modules()) {
    if (m.defId == "prod_arg_a") {
      ++aCount;
      aSumX += m.worldTransform.position.x;
    } else if (m.defId == "prod_arg_b") {
      ++bCount;
      bSumX += m.worldTransform.position.x;
    }
  }
  CHECK(aCount == 6);
  CHECK(bCount == 6);
  CHECK(layoutLegal(work, c, opts));
  CHECK(oneConnectedComponent(work));
  // Each type hangs off its own backbone hub, so the two types occupy distinct X bands.
  CHECK(std::abs(aSumX / aCount - bSumX / bCount) > 200.0);
}

TEST_CASE("fishboneSeed mixes distinct module types when mixRibModules is true") {
  ModuleCatalog c;
  c.add(hub6("struct_arg_cross_01_macro"));
  c.add(beamDef("struct_arg_base_01_macro", 1000.0));
  c.add(bigSpoke("prod_arg_a"));
  c.add(bigSpoke("prod_arg_b"));
  Station work;
  std::vector<std::string> interior(6, "prod_arg_a");
  for (int i = 0; i < 6; ++i) interior.emplace_back("prod_arg_b");

  AutoLayoutOptions opts;
  opts.mixRibModules = true;
  fishboneSeed(work, interior, c, opts);

  int aCount = 0, bCount = 0;
  double aSumX = 0, bSumX = 0;
  for (const auto& m : work.modules()) {
    if (m.defId == "prod_arg_a") {
      ++aCount;
      aSumX += m.worldTransform.position.x;
    } else if (m.defId == "prod_arg_b") {
      ++bCount;
      bSumX += m.worldTransform.position.x;
    }
  }
  CHECK(aCount == 6);
  CHECK(bCount == 6);
  CHECK(layoutLegal(work, c, opts));
  CHECK(oneConnectedComponent(work));
  // They are mixed in the same ribs/columns off the same backbone hub, so their average X
  // coordinate is close or identical.
  CHECK(std::abs(aSumX / aCount - bSumX / bCount) < 50.0);
}

TEST_CASE("fishboneSeed is additive: a pre-existing module is left untouched") {
  ModuleCatalog c;
  c.add(hub6("struct_arg_cross_01_macro"));
  c.add(beamDef("struct_arg_base_01_macro", 1000.0));
  c.add(bigSpoke("prod_arg_a"));
  Station work;
  PlacedModule pre;
  pre.defId = "struct_arg_cross_01_macro";
  pre.worldTransform.position = {0, 0, 0};
  const InstanceId preId = work.add(pre);  // an existing hub with free ports

  fishboneSeed(work, std::vector<std::string>(4, "prod_arg_a"), c, AutoLayoutOptions{});

  REQUIRE(work.find(preId) != nullptr);
  CHECK(work.find(preId)->worldTransform.position.x == doctest::Approx(0.0));  // not moved
  CHECK(work.find(preId)->worldTransform.position.y == doctest::Approx(0.0));
  int placed = 0;
  for (const auto& m : work.modules())
    if (m.defId == "prod_arg_a") ++placed;
  CHECK(placed == 4);
  CHECK(layoutLegal(work, c, AutoLayoutOptions{}));
}

TEST_CASE("fishboneSeed degrades to floating when no hub can mate the cart") {
  ModuleCatalog c;
  c.add(spokeDef("prod_arg_a"));  // a module, but no connector hub in the catalog at all
  Station work;
  const std::vector<std::string> interior(5, "prod_arg_a");

  fishboneSeed(work, interior, c, AutoLayoutOptions{});

  int placed = 0;
  for (const auto& m : work.modules())
    if (m.defId == "prod_arg_a") ++placed;
  CHECK(placed == 5);                                // every module still placed (floating)
  CHECK(layoutLegal(work, c, AutoLayoutOptions{}));  // floats are spaced, in-plot, legal
}

TEST_CASE("fishboneSeed is deterministic (byte-identical transforms)") {
  ModuleCatalog c;
  c.add(hub6("struct_arg_cross_01_macro"));
  c.add(beamDef("struct_arg_base_01_macro", 1000.0));
  c.add(bigSpoke("prod_arg_a"));
  c.add(bigSpoke("prod_arg_b"));
  std::vector<std::string> interior(7, "prod_arg_a");
  for (int i = 0; i < 5; ++i) interior.emplace_back("prod_arg_b");

  Station a, b;
  fishboneSeed(a, interior, c, AutoLayoutOptions{});
  fishboneSeed(b, interior, c, AutoLayoutOptions{});

  REQUIRE(a.size() == b.size());
  for (std::size_t i = 0; i < a.modules().size(); ++i) {
    CHECK(a.modules()[i].defId == b.modules()[i].defId);
    CHECK(a.modules()[i].worldTransform.position.x == b.modules()[i].worldTransform.position.x);
    CHECK(a.modules()[i].worldTransform.position.y == b.modules()[i].worldTransform.position.y);
    CHECK(a.modules()[i].worldTransform.position.z == b.modules()[i].worldTransform.position.z);
  }
}

namespace {
// Assert the fishbone's hard invariants for one cart: legal (in-plot, no non-adjacent overlap,
// corridors clear), modules-on-connectors, deterministic (byte-identical), and every interior
// module placed. `interior` is the un-ordered cart of functional def ids.
void checkFishboneInvariants(const ModuleCatalog& c, const std::vector<std::string>& interior) {
  Station a, b;
  const int ca = fishboneSeed(a, interior, c, AutoLayoutOptions{});
  const int cb = fishboneSeed(b, interior, c, AutoLayoutOptions{});

  CHECK(layoutLegal(a, c, AutoLayoutOptions{}));
  CHECK(functionalsAttachToHubsOnly(a, c));

  REQUIRE(a.size() == b.size());
  CHECK(ca == cb);
  for (std::size_t i = 0; i < a.modules().size(); ++i) {
    CHECK(a.modules()[i].defId == b.modules()[i].defId);
    CHECK(a.modules()[i].worldTransform.position.x == b.modules()[i].worldTransform.position.x);
    CHECK(a.modules()[i].worldTransform.position.y == b.modules()[i].worldTransform.position.y);
    CHECK(a.modules()[i].worldTransform.position.z == b.modules()[i].worldTransform.position.z);
  }

  std::unordered_map<std::string, int> want, got;
  for (const std::string& id : interior) ++want[id];
  for (const auto& m : a.modules()) {
    const ModuleDef* d = c.find(m.defId);
    if (d != nullptr && d->category != Category::Connector) ++got[m.defId];
  }
  for (const auto& [id, n] : want) CHECK(got[id] == n);  // every interior module placed
}
}  // namespace

TEST_CASE("fishboneSeed holds its invariants across a sweep of carts") {
  ModuleCatalog c;
  c.add(hub6("struct_arg_cross_01_macro"));
  c.add(beamDef("struct_arg_base_01_macro", 1000.0));
  c.add(bigSpoke("prod_arg_big"));                          // bulky production
  c.add(spokeDef("prod_arg_small", Category::Production));  // tiny, 1-port
  c.add(twoPortDef("stor_arg_a", Category::Storage));       // storage, 2-port
  c.add(spokeDef("hab_arg_a", Category::Habitat));          // habitat

  SUBCASE("single module") { checkFishboneInvariants(c, {"prod_arg_big"}); }
  SUBCASE("one type, many (multi-hub, multi-sub-spine)") {
    checkFishboneInvariants(c, std::vector<std::string>(30, "prod_arg_big"));
  }
  SUBCASE("mixed categories -> separate branches") {
    std::vector<std::string> cart;
    for (int i = 0; i < 8; ++i) cart.emplace_back("prod_arg_big");
    for (int i = 0; i < 6; ++i) cart.emplace_back("prod_arg_small");
    for (int i = 0; i < 5; ++i) cart.emplace_back("stor_arg_a");
    for (int i = 0; i < 4; ++i) cart.emplace_back("hab_arg_a");
    checkFishboneInvariants(c, cart);
  }
  SUBCASE("tiny mixed") { checkFishboneInvariants(c, {"hab_arg_a", "stor_arg_a"}); }
}

TEST_CASE("fishboneSeed: venture-trap hub is rejected, modules still placed legally") {
  // The only faction hub has "snap venture" ports that mate nothing plain "snap"; a non-faction
  // hub does mate. The seed must host the cart on the compatible hub, never the venture trap.
  ModuleCatalog c;
  ModuleDef venture = hub6("struct_gen_venturercross_01_macro");
  for (auto& cp : venture.connectionPoints) cp.type = "snap venture";
  c.add(venture);
  c.add(hub6("struct_arg_cross_01_macro"));  // plain "snap"
  c.add(beamDef("struct_arg_base_01_macro", 1000.0));
  c.add(spokeDef("prod_gen_hullparts"));  // faction "gen", plain "snap"

  Station work;
  const std::vector<std::string> interior(12, "prod_gen_hullparts");
  fishboneSeed(work, interior, c, AutoLayoutOptions{});

  int ventureHubs = 0, placed = 0;
  for (const auto& m : work.modules()) {
    if (m.defId == "struct_gen_venturercross_01_macro") ++ventureHubs;
    if (m.defId == "prod_gen_hullparts") ++placed;
  }
  CHECK(placed == 12);
  CHECK(ventureHubs == 0);
  CHECK(layoutLegal(work, c, AutoLayoutOptions{}));
  CHECK(functionalsAttachToHubsOnly(work, c));
}

TEST_CASE("fishboneSeed packs small modules tightly (4 per hub) and spaces locally") {
  ModuleCatalog c;
  c.add(hub6("struct_arg_cross_01_macro"));
  c.add(beamDef("struct_arg_base_01_macro", 1000.0));
  c.add(spokeDef("prod_arg_small", Category::Production));

  Station work;
  const std::vector<std::string> cart(4, "prod_arg_small");

  const int connectorsAdded = fishboneSeed(work, cart, c, AutoLayoutOptions{});

  // We want to place 4 modules of a small type.
  // With dynamic packing, they should all fit on a single hub of the vertical line!
  // The first hub is backbone, the second is the rib hub, the third is the sub-spine hub.
  // So connectorsAdded should be exactly 3 (1 backbone hub + 1 rib hub + 1 sub-spine hub), and no
  // beams!
  CHECK(connectorsAdded == 3);
  CHECK(layoutLegal(work, c, AutoLayoutOptions{}));
  CHECK(oneConnectedComponent(work));
}

TEST_CASE("fishboneSeed grows ribs in both +Z and -Z directions when many modules are present") {
  ModuleCatalog c;
  c.add(hub6("struct_arg_cross_01_macro"));
  c.add(beamDef("struct_arg_base_01_macro", 1000.0));
  c.add(bigSpoke("prod_arg_big"));

  Station work;
  // Use 40 modules, which will force multiple lines in both directions
  std::vector<std::string> cart(40, "prod_arg_big");

  AutoLayoutOptions opts;
  opts.mixRibModules = true;
  fishboneSeed(work, cart, c, opts);

  CHECK(layoutLegal(work, c, opts));
  CHECK(oneConnectedComponent(work));

  // Count modules on +Z side (z > 50) and -Z side (z < -50)
  int posZCount = 0;
  int negZCount = 0;
  int spineCount = 0;

  for (const auto& m : work.modules()) {
    if (m.defId == "prod_arg_big") {
      const double z = m.worldTransform.position.z;
      if (z > 50.0) {
        ++posZCount;
      } else if (z < -50.0) {
        ++negZCount;
      } else {
        ++spineCount;
      }
    }
  }

  // With bidirectional growth, we should have production modules on both sides
  CHECK(posZCount > 0);
  CHECK(negZCount > 0);
  // No production modules should be dumped straight on the spine (Z near 0)
  CHECK(spineCount == 0);
}

TEST_CASE("organicSeed produces a legal, connected layout (plumbing)") {
  ModuleCatalog c;
  c.add(hubDef("struct_arg_cross_01_macro"));  // a 4-port connector for synthesis
  c.add(spokeDef("prod_arg_a"));               // 1-port functional leaf
  Station work;                                // greenfield
  const std::vector<std::string> interior{"prod_arg_a", "prod_arg_a", "prod_arg_a"};

  const int connectorsAdded = organicSeed(work, interior, c, AutoLayoutOptions{});

  CHECK(connectorsAdded >= 1);                       // at least one hub synthesized
  CHECK(oneConnectedComponent(work));                // ONE structure
  CHECK(layoutLegal(work, c, AutoLayoutOptions{}));  // in-plot, no illegal overlap, corridors clear
}

TEST_CASE("organicSeed chains modules onto functional cores (no hub needed)") {
  ModuleCatalog c;
  c.add(coreDef("stor_arg_core"));     // 4-port functional core
  c.add(twoPortDef("prod_arg_link"));  // 2-port functional
  Station work;
  std::vector<std::string> interior{"stor_arg_core"};
  for (int i = 0; i < 4; ++i) interior.push_back("prod_arg_link");

  const int connectorsAdded = organicSeed(work, interior, c, AutoLayoutOptions{});

  CHECK(connectorsAdded == 0);  // no structural hub synthesized
  CHECK(oneConnectedComponent(work));
  CHECK(layoutLegal(work, c, AutoLayoutOptions{}));
  CHECK(someFunctionalChain(work, c));  // modules ARE the structure
}

TEST_CASE("organicSeed is deterministic (same cart -> identical station)") {
  ModuleCatalog c;
  c.add(coreDef("stor_arg_core"));
  c.add(twoPortDef("prod_arg_link"));
  const std::vector<std::string> interior{"stor_arg_core", "prod_arg_link", "prod_arg_link",
                                          "prod_arg_link"};
  Station a, b;
  organicSeed(a, interior, c, AutoLayoutOptions{});
  organicSeed(b, interior, c, AutoLayoutOptions{});

  REQUIRE(a.size() == b.size());
  for (std::size_t i = 0; i < a.modules().size(); ++i) {
    CHECK(a.modules()[i].defId == b.modules()[i].defId);
    CHECK(a.modules()[i].worldTransform.position.x ==
          doctest::Approx(b.modules()[i].worldTransform.position.x));
    CHECK(a.modules()[i].worldTransform.position.z ==
          doctest::Approx(b.modules()[i].worldTransform.position.z));
  }
}

TEST_CASE("organicSeed grows additively from an existing free port") {
  ModuleCatalog c;
  c.add(hubDef("struct_arg_cross_01_macro"));
  c.add(twoPortDef("prod_arg_link"));
  Station work;
  PlacedModule pre;  // a pre-existing hub with free ports
  pre.defId = "struct_arg_cross_01_macro";
  work.add(pre);
  const std::vector<std::string> interior{"prod_arg_link", "prod_arg_link"};

  organicSeed(work, interior, c, AutoLayoutOptions{});

  CHECK(oneConnectedComponent(work));  // attached to the existing hub, not floated
  CHECK(layoutLegal(work, c, AutoLayoutOptions{}));
}

TEST_CASE("autoLayout dispatches to organicSeed when mode is Organic") {
  ModuleCatalog c;
  c.add(hubDef("struct_arg_cross_01_macro"));
  c.add(spokeDef("prod_arg_a"));
  AutoLayoutOptions opts;
  opts.mode = LayoutMode::Organic;
  const QuantityList cart{{"prod_arg_a", 3}};

  const AutoLayoutResult r = autoLayout(Station{}, cart, c, opts);

  CHECK(oneConnectedComponent(r.station));
  CHECK(layoutLegal(r.station, c, opts));
}

TEST_CASE("organicSeed connects an all-leaves cart by synthesizing hubs (nothing floats)") {
  ModuleCatalog c;
  c.add(hubDef("struct_arg_cross_01_macro"));  // 4-port connector
  c.add(spokeDef("prod_arg_hull"));            // 1-port leaf
  Station work;
  const std::vector<std::string> interior(20, "prod_arg_hull");

  const int connectorsAdded = organicSeed(work, interior, c, AutoLayoutOptions{});

  CHECK(connectorsAdded >= 1);  // hubs synthesized to host the leaves
  CHECK(oneConnectedComponent(work));
  CHECK(layoutLegal(work, c, AutoLayoutOptions{}));
  int leaves = 0, floating = 0;
  for (const auto& m : work.modules())
    if (m.defId == "prod_arg_hull") {
      ++leaves;
      if (m.links.empty()) ++floating;
    }
  CHECK(leaves == 20);
  CHECK(floating == 0);  // every leaf attached to a hub
}

TEST_CASE("organicSeed splits long chains with hubs (anti-snake)") {
  ModuleCatalog c;
  c.add(hubDef("struct_arg_cross_01_macro"));
  c.add(twoPortDef("prod_arg_link"));  // 2-port functional — chains end to end
  Station work;
  const std::vector<std::string> interior(12, "prod_arg_link");

  organicSeed(work, interior, c, AutoLayoutOptions{});

  CHECK(oneConnectedComponent(work));
  CHECK(layoutLegal(work, c, AutoLayoutOptions{}));
  CHECK(longestFunctionalChain(work, c) <= 4);  // kMaxChain — no runaway noodle
}

TEST_CASE("organicSeed spends fewer connectors than fishbone on a mixed-core cart") {
  ModuleCatalog c;
  c.add(hub6("struct_arg_cross_01_macro"));            // shared 6-port connector
  c.add(beamDef("struct_arg_base_01_macro", 1000.0));  // beam for fishbone spacing
  c.add(coreDef("stor_arg_core"));                     // 4-port functional cores
  c.add(twoPortDef("prod_arg_link"));                  // 2-port functional links
  std::vector<std::string> interior;
  for (int i = 0; i < 8; ++i) interior.push_back("stor_arg_core");
  for (int i = 0; i < 16; ++i) interior.push_back("prod_arg_link");

  Station of, ff;
  AutoLayoutOptions opts;
  const int organicConns = organicSeed(of, interior, c, opts);
  const int fishboneConns = fishboneSeed(ff, interior, c, opts);

  CHECK(oneConnectedComponent(of));
  CHECK(layoutLegal(of, c, opts));
  CHECK(organicConns < fishboneConns);  // the headline efficiency win
}

TEST_CASE("organicSeed escapes a big-module collision shell (nothing floats or drops)") {
  ModuleCatalog c;
  c.add(snugHub("struct_arg_cross_01_macro"));
  c.add(fatLeaf("prod_arg_fat"));
  Station work;
  const std::vector<std::string> interior(3, "prod_arg_fat");

  organicSeed(work, interior, c, AutoLayoutOptions{});

  // The third fat module has no direct seat and ONE hub injection still lands inside the shell —
  // the seed must keep growing structure outward until a seat clears (the real-catalog trap).
  int placed = 0;
  for (const auto& m : work.modules())
    if (m.defId == "prod_arg_fat") {
      ++placed;
      CHECK(!m.links.empty());
    }
  CHECK(placed == 3);
  CHECK(oneConnectedComponent(work));
  CHECK(layoutLegal(work, c, AutoLayoutOptions{}));
}

TEST_CASE("organicSeed survives seam-burying modules (both ports on one face, like meat/wheat)") {
  // Real production slabs (meat, wheat) carry BOTH ports on the same face. Chaining two directly
  // mates the second flipped-under, and the pair's leftover ports end up buried in the seam
  // between the two bodies — a station with ZERO seatable ports, where no hub, beam, or module
  // can ever attach again. The seed must inject structure while ports are still alive.
  ModuleDef seam;  // real prod_arg_meat geometry: the mate lands the pair dead-centre stacked.
  // The slightly asymmetric box matters — it tips the centroid-nearest port choice to the +x
  // port, whose first-compatible mate is the dead-centre burial (the symmetric box zigzags).
  seam.id = "prod_arg_seam";
  seam.category = Category::Production;
  seam.aabb = AABB{{-757, -202, -356}, {758, 178, 357}};
  for (const auto& [pid, x] :
       std::vector<std::pair<std::string, double>>{{"a", -600}, {"b", 600}}) {
    ConnectionPoint cp;
    cp.id = pid;
    cp.localPosition = {x, -200, -200};
    cp.localRotation = zTo(Vec3{0, -1, 0});  // both ports on the bottom face, normals down
    cp.type = "snap";
    seam.connectionPoints.push_back(cp);
  }
  ModuleCatalog c;
  c.add(snugHub("struct_arg_cross_01_macro"));
  c.add(seam);
  Station work;
  const std::vector<std::string> interior(6, "prod_arg_seam");

  organicSeed(work, interior, c, AutoLayoutOptions{});

  int placed = 0;
  for (const auto& m : work.modules())
    if (m.defId == "prod_arg_seam") {
      ++placed;
      CHECK(!m.links.empty());
    }
  CHECK(placed == 6);
  CHECK(oneConnectedComponent(work));
  CHECK(layoutLegal(work, c, AutoLayoutOptions{}));
}

TEST_CASE("organicSeed keeps growing past shell closure (12 fat modules stay connected)") {
  // The real-catalog trap at scale (known-issues 4.3), in real proportions: a 400m cross
  // chained hub-to-hub gives a 400m pitch, but a spacefuel-class body is ~730-780m wide, so
  // every placement shadows neighbouring ports. By the 8th module the cluster is a closed
  // ball where NOTHING — module, hub, or beam — can seat (every candidate narrow-collides),
  // and the rest float disconnected. Escape structure must be grown while a beam-width
  // tunnel out of the shell still exists, not after the last seat is gone.
  ModuleCatalog c;
  ModuleDef cross;  // real struct_arg_cross_01 geometry: 400m box, one port per face
  cross.id = "struct_arg_cross_01_macro";
  cross.category = Category::Connector;
  cross.aabb = AABB{{-200, -200, -200}, {200, 200, 200}};
  for (const auto& [pid, pos] : std::vector<std::pair<std::string, Vec3>>{{"pz", {0, 0, 200}},
                                                                          {"nz", {0, 0, -200}},
                                                                          {"nx", {-200, 0, 0}},
                                                                          {"px", {200, 0, 0}},
                                                                          {"ny", {0, -200, 0}},
                                                                          {"py", {0, 200, 0}}}) {
    ConnectionPoint cp;
    cp.id = pid;
    cp.localPosition = pos;
    cp.localRotation = zTo(pos * (1.0 / length(pos)));
    cp.type = "snap";
    cross.connectionPoints.push_back(cp);
  }
  c.add(cross);
  ModuleDef beam;  // real struct_arg_base_03 geometry: 1600m straight thin span
  beam.id = "struct_arg_base_03_macro";
  beam.category = Category::Connector;
  beam.aabb = AABB{{-139, -75, -800}, {139, 75, 800}};
  for (const auto& [pid, z] :
       std::vector<std::pair<std::string, double>>{{"a", 800.0}, {"b", -800.0}}) {
    ConnectionPoint cp;
    cp.id = pid;
    cp.localPosition = {0, 0, z};
    cp.localRotation = zTo(Vec3{0, 0, z > 0 ? 1.0 : -1.0});
    cp.type = "snap";
    beam.connectionPoints.push_back(cp);
  }
  c.add(beam);
  ModuleDef fat;  // real prod_arg_spacefuel geometry: ~730x780x710 body, one bottom port
  fat.id = "prod_arg_fat";
  fat.category = Category::Production;
  fat.aabb = AABB{{-353, -400, -356}, {374, 380, 354}};
  ConnectionPoint fp;
  fp.id = "s";
  fp.localPosition = {0, -400, 0};
  fp.localRotation = zTo(Vec3{0, -1, 0});
  fp.type = "snap";
  fat.connectionPoints.push_back(fp);
  c.add(fat);
  Station work;
  const std::vector<std::string> interior(12, "prod_arg_fat");

  organicSeed(work, interior, c, AutoLayoutOptions{});

  int placed = 0;
  for (const auto& m : work.modules())
    if (m.defId == "prod_arg_fat") {
      ++placed;
      CHECK(!m.links.empty());
    }
  CHECK(placed == 12);
  CHECK(oneConnectedComponent(work));
  CHECK(layoutLegal(work, c, AutoLayoutOptions{}));
}

TEST_CASE("organicSeed overflow floats stay inside the plot (nothing silently dropped)") {
  // The overflow fallback marched floats along +X only, and a float past the plot edge was
  // dropped SILENTLY (the 'skipped' count of known-issues 4.3). Unplaceable leftovers must
  // instead take any legal in-plot pose — the plot has room in every direction.
  ModuleDef brick;  // no ports: can never snap, always overflows
  brick.id = "prod_arg_brick";
  brick.category = Category::Production;
  brick.aabb = AABB{{-200, -100, -100}, {200, 100, 100}};
  ModuleCatalog c;
  c.add(brick);
  Station work;
  AutoLayoutOptions opts;
  opts.plotHalfExtent = 1000.0;  // a +X-only march fits ~1 more brick; the plot fits dozens
  const std::vector<std::string> interior(8, "prod_arg_brick");

  organicSeed(work, interior, c, opts);

  int placed = 0;
  for (const auto& m : work.modules())
    if (m.defId == "prod_arg_brick") ++placed;
  CHECK(placed == 8);
  CHECK(layoutLegal(work, c, opts));
}

TEST_CASE("organicSeed grows a blob, not a worm (bounded aspect ratio)") {
  ModuleCatalog c;
  c.add(snugHub("struct_arg_cross_01_macro"));
  c.add(fatLeaf("prod_arg_fat"));
  Station work;
  const std::vector<std::string> interior(8, "prod_arg_fat");

  organicSeed(work, interior, c, AutoLayoutOptions{});

  CHECK(oneConnectedComponent(work));
  CHECK(layoutLegal(work, c, AutoLayoutOptions{}));
  // A tip-only growth policy chains each injected hub off the previous one, so the station
  // extends along a single axis into a >4:1 staircase; organic must spread around the seed.
  const Vec3 s = bounds(work, c).max - bounds(work, c).min;
  const double longest = std::max({s.x, s.y, s.z});
  const double shortest = std::min({s.x, s.y, s.z});
  CHECK(longest <= 3.0 * shortest);
}
