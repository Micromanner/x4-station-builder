#include "x4sb/autolayout/anneal.hpp"
#include "x4sb/autolayout/autolayout.hpp"
#include "x4sb/autolayout/search.hpp"
#include "x4sb/data/math.hpp"

#include <doctest/doctest.h>

#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace x4sb;

namespace {
const AABB kUnit{{-0.5, -0.5, -0.5}, {0.5, 0.5, 0.5}};

ModuleDef boxDef(const std::string& id) {
  ModuleDef d;
  d.id = id;
  d.aabb = kUnit;
  return d;
}

// Synthetic objective: squared distance of the single module from the origin (optimum
// at the origin). Catalog unused.
double distSqObjective(const Station& s, const ModuleCatalog&) {
  if (s.empty()) return 0.0;
  const Vec3 p = s.modules().front().worldTransform.position;
  return dot(p, p);
}

// Synthetic move: nudge the single module by +/-1 along a random axis. Always legal in
// these tests (one module, well inside the plot), so it exercises the accept path.
[[maybe_unused]] std::optional<Station> nudgeMove(const Station& s, const ModuleCatalog&,
                                                  std::mt19937& rng) {
  if (s.empty()) return std::nullopt;
  const InstanceId id = s.modules().front().instanceId;
  Station out = s;
  PlacedModule* m = out.find(id);
  std::uniform_int_distribution<int> axis(0, 2);
  std::uniform_int_distribution<int> dir(0, 1);
  const double step = (dir(rng) == 0 ? -1.0 : 1.0);
  const int a = axis(rng);
  if (a == 0)
    m->worldTransform.position.x += step;
  else if (a == 1)
    m->worldTransform.position.y += step;
  else
    m->worldTransform.position.z += step;
  return out;
}

Station seedAt(const ModuleCatalog&, Vec3 pos) {
  Station s;
  PlacedModule m;
  m.defId = "b";
  m.worldTransform.position = pos;
  s.add(m);
  return s;
}
}  // namespace

TEST_CASE("anneal reduces a synthetic objective from its seed") {
  ModuleCatalog c;
  c.add(boxDef("b"));
  const Station seed = seedAt(c, {10, 0, 0});  // seed cost = 100
  AutoLayoutOptions opts;
  opts.maxIters = 3000;

  const Station out = anneal(seed, c, distSqObjective, {nudgeMove}, opts);
  CHECK(distSqObjective(out, c) < distSqObjective(seed, c));  // strictly improved
}

TEST_CASE("anneal is deterministic: same seed -> identical result") {
  ModuleCatalog c;
  c.add(boxDef("b"));
  const Station seed = seedAt(c, {10, 0, 0});
  AutoLayoutOptions opts;
  opts.maxIters = 3000;

  const Station a = anneal(seed, c, distSqObjective, {nudgeMove}, opts);
  const Station b = anneal(seed, c, distSqObjective, {nudgeMove}, opts);
  const Vec3 pa = a.modules().front().worldTransform.position;
  const Vec3 pb = b.modules().front().worldTransform.position;
  CHECK(pa.x == doctest::Approx(pb.x));
  CHECK(pa.y == doctest::Approx(pb.y));
  CHECK(pa.z == doctest::Approx(pb.z));
}

TEST_CASE("anneal with a different seed is itself reproducible (and may differ)") {
  ModuleCatalog c;
  c.add(boxDef("b"));
  const Station seed = seedAt(c, {10, 0, 0});
  AutoLayoutOptions o1;
  o1.maxIters = 2000;
  o1.seed = 1u;
  AutoLayoutOptions o2 = o1;
  o2.seed = 2u;

  const Station a1 = anneal(seed, c, distSqObjective, {nudgeMove}, o1);
  const Station a1again = anneal(seed, c, distSqObjective, {nudgeMove}, o1);
  const Station a2 = anneal(seed, c, distSqObjective, {nudgeMove}, o2);
  // seed 1 reproduces exactly...
  CHECK(a1.modules().front().worldTransform.position.x ==
        doctest::Approx(a1again.modules().front().worldTransform.position.x));
  // ...and both are valid optimisations (cost not worse than the seed).
  CHECK(distSqObjective(a2, c) <= distSqObjective(seed, c));
}

TEST_CASE("anneal returns the seed unchanged when maxIters is 0 or there are no moves") {
  ModuleCatalog c;
  c.add(boxDef("b"));
  const Station seed = seedAt(c, {10, 0, 0});

  AutoLayoutOptions zero;
  zero.maxIters = 0;
  CHECK(anneal(seed, c, distSqObjective, {nudgeMove}, zero)
            .modules()
            .front()
            .worldTransform.position.x == doctest::Approx(10.0));

  AutoLayoutOptions some;
  some.maxIters = 100;
  CHECK(anneal(seed, c, distSqObjective, {}, some).modules().front().worldTransform.position.x ==
        doctest::Approx(10.0));
}
