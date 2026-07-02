#include "x4sb/autolayout/seed.hpp"

#include "x4sb/autolayout/connectors.hpp"
#include "x4sb/autolayout/metrics.hpp"
#include "x4sb/data/math.hpp"
#include "x4sb/snap/snap.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace x4sb {
namespace {

double aabbVolume(const AABB& b) {
  const Vec3 d = b.max - b.min;
  return std::abs(d.x * d.y * d.z);
}

bool insidePlot(const AABB& box, double h) {
  return box.min.x >= -h && box.max.x <= h && box.min.y >= -h && box.max.y <= h &&
         box.min.z >= -h && box.max.z <= h;
}

bool collidesAabb(const ModuleDef& def, const Transform& xf, InstanceId targetId,
                  const Station& work, const ModuleCatalog& catalog) {
  const AABB myBox = worldAabb(def.aabb, xf);
  for (const auto& o : work.modules()) {
    if (o.instanceId == targetId) continue;
    const ModuleDef* od = catalog.find(o.defId);
    if (od != nullptr && overlaps(myBox, worldAabb(od->aabb, o.worldTransform))) {
      return true;
    }
  }
  return false;
}

// Minimum body-to-body gap the fishbone leaves between adjacent modules / hubs. Small on purpose:
// the layout packs tight (the user's "it can be tighter"), beam-length quantisation usually adds
// the visible breathing room, and the conservative-AABB legality check only needs a strictly
// positive gap (real modules mate at 90° multiples, so their world-AABBs are exact, not inflated).
const double kSpineClearance = 10.0;

// The distinct connector types the cart's modules expose, so hub selection can require a
// hub that actually mates them (bestHubFor). Empty result -> {""} so an untagged cart still
// matches any hub.
std::vector<std::string> cartPortTypes(const std::vector<std::string>& ids,
                                       const ModuleCatalog& catalog) {
  std::vector<std::string> types;
  for (const std::string& id : ids) {
    const ModuleDef* d = catalog.find(id);
    if (d == nullptr) continue;
    for (const auto& cp : d->connectionPoints) types.push_back(cp.type);
  }
  std::sort(types.begin(), types.end());
  types.erase(std::unique(types.begin(), types.end()), types.end());
  if (types.empty()) types.emplace_back("");
  return types;
}

// The minimum hub spacing along `axis` that keeps neighbouring hubs' modules clear. Rather than
// guess from the module's largest dimension (which over-spaces — modules hang off PERPENDICULAR
// ports, not along the spine), measure it: mate each module onto each perpendicular port of a
// lone hub and take the largest resulting body extent projected on the axis, plus a margin.
// Floored at the hub's own footprint so hubs never overlap when no module could be measured.
double requiredPitch(const ModuleCatalog& catalog, const ModuleDef& hub, Vec3 axis,
                     const std::vector<std::string>& moduleIds, double margin) {
  Station tmp;
  PlacedModule h;
  h.defId = hub.id;
  const InstanceId hid = tmp.add(h);  // a lone hub at the origin (identity transform)

  std::vector<std::string> uniq = moduleIds;
  std::sort(uniq.begin(), uniq.end());
  uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());

  const auto axisExtent = [&](const AABB& b) {
    const Vec3 s = b.max - b.min;
    return std::abs(s.x * axis.x + s.y * axis.y + s.z * axis.z);
  };

  double maxExt = 0.0;
  for (const std::string& id : uniq) {
    const ModuleDef* def = catalog.find(id);
    if (def == nullptr) continue;
    for (const auto& cp : hub.connectionPoints) {
      const double pl = length(cp.localPosition);
      if (pl < 1e-6 || std::abs(dot(cp.localPosition, axis)) > 0.5 * pl) continue;  // perpendicular
      for (const auto& np : def->connectionPoints) {
        if (!connectorsCompatible(cp, np)) continue;
        const Transform xf = computeSnapTransform(tmp, catalog, hid, cp.id, *def, np.id);
        maxExt = std::max(maxExt, axisExtent(worldAabb(def->aabb, xf)));
        break;  // first compatible connector is representative of this port
      }
    }
  }
  return std::max(maxExt, axisExtent(hub.aabb)) + margin;
}

struct Cand {
  InstanceId targetId = 0;
  std::string targetPointId;
  std::string newPointId;
  bool hubTarget = false;  // prefer attaching to Connector-category (hub) ports
};

// Snap `def` onto a free compatible connector of `work` (hub ports preferred), accepting
// the first legal mate (in-plot + body-collision-free + clearance-free, ignoring the snap
// partner's body for the joint). Adds it (links both sides) and returns true on success.
// Deterministic: candidates sorted by an explicit key. `hubOnly` restricts targets to
// Connector-category (hub) modules — the lever that makes functional modules sit on hub
// ports (one hub holds many) instead of chaining functional-to-functional (spec §8.1/§9.1).
bool trySnap(Station& work, const ModuleCatalog& catalog, const ModuleDef& def,
             const AutoLayoutOptions& opts, bool hubOnly = false) {
  std::vector<Cand> cands;
  for (const auto& t : work.modules()) {
    const ModuleDef* td = catalog.find(t.defId);
    if (td == nullptr) continue;
    const bool hub = td->category == Category::Connector;
    if (hubOnly && !hub) continue;  // functional modules attach to hub ports only
    for (const auto& tp : td->connectionPoints) {
      if (connectorIsLinked(t, tp.id)) continue;
      for (const auto& np : def.connectionPoints) {
        if (!connectorsCompatible(tp, np)) continue;
        cands.push_back(Cand{t.instanceId, tp.id, np.id, hub});
      }
    }
  }
  std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
    if (a.hubTarget != b.hubTarget) return a.hubTarget;  // hub ports first
    if (a.targetId != b.targetId) return a.targetId < b.targetId;
    if (a.targetPointId != b.targetPointId) return a.targetPointId < b.targetPointId;
    return a.newPointId < b.newPointId;
  });

  for (const Cand& cand : cands) {
    const Transform xf = computeSnapTransform(work, catalog, cand.targetId, cand.targetPointId, def,
                                              cand.newPointId);
    if (!insidePlot(worldAabb(def.aabb, xf), opts.plotHalfExtent)) continue;
    if (collidesWithStation(def, xf, cand.targetId, work, catalog)) continue;
    if (collidesClearance(def, xf, 0, 0, work, catalog)) continue;
    if (collidesAabb(def, xf, cand.targetId, work, catalog)) continue;
    PlacedModule m;
    m.defId = def.id;
    m.worldTransform = xf;
    m.links.push_back(Link{cand.newPointId, cand.targetId, cand.targetPointId});
    const InstanceId id = work.add(m);
    if (PlacedModule* t = work.find(cand.targetId))
      t->links.push_back(Link{cand.targetPointId, id, cand.newPointId});
    return true;
  }
  return false;
}

void addRoot(Station& work, const ModuleDef& def, const Transform& xf) {
  PlacedModule m;
  m.defId = def.id;
  m.worldTransform = xf;
  work.add(m);
}

// Snap `def` onto a SPECIFIC free connector (targetId, targetPointId); first legal np wins.
// Returns the new instance id, or 0 on failure. Links both sides. (Spine uses this to chain.)
InstanceId snapOnto(Station& work, const ModuleCatalog& catalog, const ModuleDef& def,
                    InstanceId targetId, const std::string& targetPointId,
                    const AutoLayoutOptions& opts) {
  const PlacedModule* t = work.find(targetId);
  if (t == nullptr) return 0;
  const ModuleDef* td = catalog.find(t->defId);
  if (td == nullptr) return 0;
  const ConnectionPoint* tp = nullptr;
  for (const auto& cp : td->connectionPoints)
    if (cp.id == targetPointId) tp = &cp;
  if (tp == nullptr) return 0;
  for (const auto& np : def.connectionPoints) {
    if (!connectorsCompatible(*tp, np)) continue;
    const Transform xf = computeSnapTransform(work, catalog, targetId, targetPointId, def, np.id);
    if (!insidePlot(worldAabb(def.aabb, xf), opts.plotHalfExtent)) continue;
    if (collidesWithStation(def, xf, targetId, work, catalog)) continue;
    if (collidesClearance(def, xf, 0, 0, work, catalog)) continue;
    if (collidesAabb(def, xf, targetId, work, catalog)) continue;
    PlacedModule m;
    m.defId = def.id;
    m.worldTransform = xf;
    m.links.push_back(Link{np.id, targetId, targetPointId});
    const InstanceId id = work.add(m);
    if (PlacedModule* tt = work.find(targetId)) tt->links.push_back(Link{targetPointId, id, np.id});
    return id;
  }
  return 0;
}

// Like snapOnto, but among the legal mates picks the new-module port that leaves it MOST
// axis-aligned (largest |rotation.w| ⇒ smallest turn from identity). snapOnto takes the first
// compatible port, which spins a symmetric hub to an arbitrary orientation — then modules combed
// off it splay in inconsistent world directions. Keeping every skeleton hub identity-oriented is
// what makes the comb a regular grid. Returns the new instance id, or 0.
InstanceId snapHubOnto(Station& work, const ModuleCatalog& catalog, const ModuleDef& def,
                       InstanceId targetId, const std::string& targetPointId,
                       const AutoLayoutOptions& opts) {
  const PlacedModule* t = work.find(targetId);
  if (t == nullptr) return 0;
  const ModuleDef* td = catalog.find(t->defId);
  if (td == nullptr) return 0;
  const ConnectionPoint* tp = nullptr;
  for (const auto& cp : td->connectionPoints)
    if (cp.id == targetPointId) tp = &cp;
  if (tp == nullptr) return 0;

  std::string bestNp;
  Transform bestXf;
  double bestUpright = -1.0;  // |rotation.w|: 1 = no rotation, 0 = 180°
  for (const auto& np : def.connectionPoints) {
    if (!connectorsCompatible(*tp, np)) continue;
    const Transform xf = computeSnapTransform(work, catalog, targetId, targetPointId, def, np.id);
    if (!insidePlot(worldAabb(def.aabb, xf), opts.plotHalfExtent)) continue;
    if (collidesWithStation(def, xf, targetId, work, catalog)) continue;
    if (collidesClearance(def, xf, 0, 0, work, catalog)) continue;
    if (collidesAabb(def, xf, targetId, work, catalog)) continue;
    const double upright = std::abs(xf.rotation.w);
    if (upright > bestUpright) {
      bestUpright = upright;
      bestNp = np.id;
      bestXf = xf;
    }
  }
  if (bestUpright < 0.0) return 0;
  PlacedModule m;
  m.defId = def.id;
  m.worldTransform = bestXf;
  m.links.push_back(Link{bestNp, targetId, targetPointId});
  const InstanceId id = work.add(m);
  if (PlacedModule* tt = work.find(targetId)) tt->links.push_back(Link{targetPointId, id, bestNp});
  return id;
}

// The free port on `hubId` whose world position is most aligned with `worldDir`, or "".
// Vector+sort (not running-max) to stay -Wfloat-equal-clean and deterministic.
std::string freePortToward(const Station& work, const ModuleCatalog& catalog, InstanceId hubId,
                           Vec3 worldDir) {
  const PlacedModule* h = work.find(hubId);
  if (h == nullptr) return "";
  const ModuleDef* hd = catalog.find(h->defId);
  if (hd == nullptr) return "";
  std::vector<std::pair<double, std::string>> ports;  // (alignment dot, port id)
  for (const auto& cp : hd->connectionPoints) {
    if (connectorIsLinked(*h, cp.id)) continue;
    const Vec3 world = apply(h->worldTransform, cp.localPosition) - h->worldTransform.position;
    ports.push_back({dot(world, worldDir), cp.id});
  }
  if (ports.empty()) return "";
  std::sort(ports.begin(), ports.end(),
            [](const std::pair<double, std::string>& a, const std::pair<double, std::string>& b) {
              if (a.first > b.first) return true;  // most-aligned first (no float == compare)
              if (a.first < b.first) return false;
              return a.second < b.second;  // deterministic tiebreak
            });
  return ports.front().second;
}

// Free (unlinked) port count on a placed hub.
std::size_t freePortCount(const Station& work, const ModuleCatalog& catalog, InstanceId hubId) {
  const PlacedModule* h = work.find(hubId);
  if (h == nullptr) return 0;
  const ModuleDef* hd = catalog.find(h->defId);
  if (hd == nullptr) return 0;
  std::size_t n = 0;
  for (const auto& cp : hd->connectionPoints)
    if (!connectorIsLinked(*h, cp.id)) ++n;
  return n;
}

// Hang `def` off the first free port of `hubId` (skipping `exceptPort`) that yields a legal
// placement (collision/clearance/plot-checked by snapOnto). Returns true on success. Functional
// modules attach here so they always sit on a hub, never chain functional-to-functional.
// `exceptPort` reserves the backbone tip's forward port so a module never steals the next link.
bool attachToHub(Station& work, const ModuleCatalog& catalog, const ModuleDef& def,
                 InstanceId hubId, const std::string& exceptPort, const AutoLayoutOptions& opts) {
  const PlacedModule* h = work.find(hubId);
  if (h == nullptr) return false;
  const ModuleDef* hd = catalog.find(h->defId);
  if (hd == nullptr) return false;
  for (const auto& tp : hd->connectionPoints) {
    if (tp.id == exceptPort || connectorIsLinked(*h, tp.id)) continue;
    if (snapOnto(work, catalog, def, hubId, tp.id, opts) != 0) return true;
  }
  return false;
}

// Attach `def` to the first hub (ascending id) with a free port that admits it, reserving the
// spine tip's forward port (`tipId`/`axis`) so the backbone can always grow. Packs earlier hubs'
// spare ports before the tip, so the whole spine fills before it lengthens. Returns true on hit.
bool attachToAnyHub(Station& work, const ModuleCatalog& catalog, const ModuleDef& def,
                    InstanceId tipId, Vec3 axis, const AutoLayoutOptions& opts) {
  std::vector<InstanceId> hubs;
  for (const auto& m : work.modules()) {
    const ModuleDef* d = catalog.find(m.defId);
    if (d != nullptr && d->category == Category::Connector) hubs.push_back(m.instanceId);
  }
  std::sort(hubs.begin(), hubs.end());
  for (const InstanceId hubId : hubs) {
    const std::string reserved = hubId == tipId ? freePortToward(work, catalog, tipId, axis) : "";
    if (attachToHub(work, catalog, def, hubId, reserved, opts)) return true;
  }
  return false;
}

// Remove `id` AND the reciprocal links to it from its neighbours (Station::remove leaves those
// dangling). Used to roll back the beams of a bay whose end hub could not be placed, so failed
// growth attempts leave no dead-end stubs.
void removeWithUnlink(Station& work, InstanceId id) {
  const PlacedModule* m = work.find(id);
  if (m == nullptr) return;
  std::vector<InstanceId> neighbours;
  for (const Link& l : m->links) neighbours.push_back(l.otherInstanceId);
  for (const InstanceId nb : neighbours) {
    PlacedModule* p = work.find(nb);
    if (p == nullptr) continue;
    auto& pl = p->links;
    pl.erase(std::remove_if(pl.begin(), pl.end(),
                            [&](const Link& l) { return l.otherInstanceId == id; }),
             pl.end());
  }
  work.remove(id);
}

// Lowest-id existing Connector module that still has a free port, or 0. Lets an additive run
// extend the backbone from structure already present instead of synthesizing a redundant hub.
InstanceId firstHubWithFreePort(const Station& work, const ModuleCatalog& catalog) {
  InstanceId best = 0;
  for (const auto& m : work.modules()) {
    const ModuleDef* d = catalog.find(m.defId);
    if (d == nullptr || d->category != Category::Connector) continue;
    if (freePortCount(work, catalog, m.instanceId) == 0) continue;
    if (best == 0 || m.instanceId < best) best = m.instanceId;
  }
  return best;
}

// ---- Organic seed helpers (spec §9.1 organic-clustering) ----------------------------------

double maxBodyExtent(const ModuleDef& d) {
  const Vec3 s = d.aabb.max - d.aabb.min;
  return std::max({s.x, s.y, s.z});
}

bool hasAnyFreePort(const Station& work, const ModuleCatalog& catalog) {
  for (const auto& m : work.modules())
    if (freePortCount(work, catalog, m.instanceId) > 0) return true;
  return false;
}

// True if SOME free port admits a legal placement of `def` (same checks as snapOnto, nothing
// placed). The strand guard's probe: "could a module like this still seat here?"
bool anySeatAdmits(const Station& work, const ModuleCatalog& catalog, const ModuleDef& def,
                   const AutoLayoutOptions& opts) {
  for (const auto& t : work.modules()) {
    const ModuleDef* td = catalog.find(t.defId);
    if (td == nullptr) continue;
    for (const auto& tp : td->connectionPoints) {
      if (connectorIsLinked(t, tp.id)) continue;
      for (const auto& np : def.connectionPoints) {
        if (!connectorsCompatible(tp, np)) continue;
        const Transform xf = computeSnapTransform(work, catalog, t.instanceId, tp.id, def, np.id);
        if (!insidePlot(worldAabb(def.aabb, xf), opts.plotHalfExtent)) continue;
        if (collidesWithStation(def, xf, t.instanceId, work, catalog)) continue;
        if (collidesClearance(def, xf, 0, 0, work, catalog)) continue;
        if (collidesAabb(def, xf, t.instanceId, work, catalog)) continue;
        return true;
      }
    }
  }
  return false;
}

// Attach `def` to the open port NEAREST the station centroid, then lowest host id, then port id —
// a total order, so the choice is deterministic. Filling the middle first grows a blob: collision
// rejects full inner seats and the search falls outward naturally. (The previous "most free ports
// first" order did the opposite of its intent — a freshly injected hub always has the most free
// ports, so every module hopped to the newest hub and the station chained into a worm at the
// tip.) Allows FUNCTIONAL hosts (direct chaining), unlike fishbone's hub-only attach. Legality
// (plot/collision/clearance) is enforced by snapOnto. Returns the chosen host id, or 0.
InstanceId organicSnap(Station& work, const ModuleCatalog& catalog, const ModuleDef& def,
                       const AutoLayoutOptions& opts, const ModuleDef* keepSeatFor = nullptr) {
  struct OCand {
    InstanceId host = 0;
    std::string hp;
    double dist = 0.0;
  };
  const Vec3 mid = centroid(work, catalog);
  std::vector<OCand> cands;
  for (const auto& t : work.modules()) {
    const ModuleDef* td = catalog.find(t.defId);
    if (td == nullptr) continue;
    for (const auto& tp : td->connectionPoints) {
      if (connectorIsLinked(t, tp.id)) continue;
      bool compatible = false;
      for (const auto& np : def.connectionPoints)
        if (connectorsCompatible(tp, np)) {
          compatible = true;
          break;
        }
      if (compatible)
        cands.push_back(
            OCand{t.instanceId, tp.id, length(apply(t.worldTransform, tp.localPosition) - mid)});
    }
  }
  std::sort(cands.begin(), cands.end(), [](const OCand& a, const OCand& b) {
    if (a.dist < b.dist) return true;  // centroid-nearest seat first
    if (a.dist > b.dist) return false;
    if (a.host != b.host) return a.host < b.host;
    return a.hp < b.hp;
  });
  for (const OCand& cand : cands) {
    const InstanceId placed = snapOnto(work, catalog, def, cand.host, cand.hp, opts);
    if (placed == 0) continue;
    if (keepSeatFor != nullptr && !anySeatAdmits(work, catalog, *keepSeatFor, opts)) {
      // Strand-aware seat choice: the guard saw that placements are starting to seal the
      // station, so reject any seat whose occupation leaves no opening for the probe body —
      // the module diverts outward instead of consuming the last module-sized gap. The caller
      // falls back to an unguarded snap when every seat seals (best effort beats floating).
      removeWithUnlink(work, placed);
      continue;
    }
    return cand.host;
  }
  return 0;
}

// Longest run of directly-chained low-port modules before organic forces a hub split, so chains
// bloom into clusters instead of long noodles (spec §9.1, anti-snake checker).
const int kMaxChain = 4;

// Structural injections organicSeed may chain to free a seat for ONE module before floating it.
// Real big-bodied modules wall in every free port (the collision shell), and a single injection
// usually lands inside the shell too — escape needs a short ladder of structure grown outward.
// Bounded so a truly unseatable module cannot grow structure forever.
const int kMaxEscalation = 8;

// True if the body of placed module `id` lies SUBSTANTIALLY inside its host's body. snapHubOnto's
// collision check exempts the host (so the port joint may touch), but a point-symmetric hub can
// mate 180°-flipped and land its WHOLE body back on the host — a degenerate, port-wasting
// "injection" that fills no new space. Any-overlap is the wrong test for it: real modules' AABBs
// overhang their ports by a few metres, so every legal mate nicks the host's box a little. Only a
// large shared volume marks the flip-back.
bool bodyOverlapsHost(const Station& work, const ModuleCatalog& catalog, InstanceId id,
                      InstanceId hostId) {
  const PlacedModule* m = work.find(id);
  const PlacedModule* h = work.find(hostId);
  if (m == nullptr || h == nullptr) return false;
  const ModuleDef* md = catalog.find(m->defId);
  const ModuleDef* hd = catalog.find(h->defId);
  if (md == nullptr || hd == nullptr) return false;
  const AABB a = worldAabb(md->aabb, m->worldTransform);
  const AABB b = worldAabb(hd->aabb, h->worldTransform);
  const double ix = std::min(a.max.x, b.max.x) - std::max(a.min.x, b.min.x);
  const double iy = std::min(a.max.y, b.max.y) - std::max(a.min.y, b.min.y);
  const double iz = std::min(a.max.z, b.max.z) - std::max(a.min.z, b.min.z);
  if (ix <= 0.0 || iy <= 0.0 || iz <= 0.0) return false;
  const double vol = aabbVolume(a);
  return vol > 0.0 && ix * iy * iz > 0.5 * vol;
}

// Synthesize `hub` (preferred) or `span` onto the best open seat, injecting fresh ports. Seats
// are walked ports-of-`preferHost` first — so an escape ladder EXTENDS the structure it just
// added instead of re-scanning the middle (depth) — then centroid-nearest, so fresh ladders
// sprout from the cluster's core and the station radiates instead of worming (breadth). Kept
// upright by snapHubOnto so combed modules stay axis-aligned. Returns the injected instance id
// (0 = nothing seats anywhere) and bumps `connectorsAdded` on success.
//
// `bodyExtent` is the widest body this structure must eventually host. When that body exceeds
// the direct hub-to-hub pitch, a directly-chained hub puts every port of BOTH hubs under the
// bodies mated on them — placements keep succeeding while the station seals itself into a
// closed collision shell where nothing (module, hub, or beam) can ever seat again, and once
// closed no ladder can thread out (known-issues 4.3, the 12x-spacefuel trap). So on a
// CONNECTOR host whose pitch is too tight, the rung is span+hub — the beam buys the pitch the
// bodies need, paid only when the cart's geometry demands it. The span alone is never the rung
// (a 2-port beam adds no fan-out; chained beams would noodle forever).
InstanceId injectStructure(Station& work, const ModuleCatalog& catalog, const ModuleDef* hub,
                           const ModuleDef* span, const AutoLayoutOptions& opts,
                           int& connectorsAdded, InstanceId preferHost, double bodyExtent = 0.0) {
  struct SCand {
    InstanceId host = 0;
    std::string hp;
    double dist = 0.0;
    bool preferred = false;
    bool hubHost = false;
    double portOffset = 0.0;
  };
  const Vec3 mid = centroid(work, catalog);
  std::vector<SCand> cands;
  for (const auto& t : work.modules()) {
    const ModuleDef* td = catalog.find(t.defId);
    if (td == nullptr) continue;
    for (const auto& tp : td->connectionPoints) {
      if (connectorIsLinked(t, tp.id)) continue;
      bool compatible = false;
      for (const ModuleDef* d : {hub, span}) {
        if (d == nullptr || compatible) continue;
        for (const auto& np : d->connectionPoints)
          if (connectorsCompatible(tp, np)) {
            compatible = true;
            break;
          }
      }
      if (compatible)
        cands.push_back(SCand{t.instanceId, tp.id,
                              length(apply(t.worldTransform, tp.localPosition) - mid),
                              t.instanceId == preferHost, td->category == Category::Connector,
                              length(tp.localPosition)});
    }
  }
  std::sort(cands.begin(), cands.end(), [](const SCand& a, const SCand& b) {
    if (a.preferred != b.preferred) return a.preferred;
    if (a.preferred) {  // ladder tip: extend OUTWARD, or the ladder curls back and orbits the core
      if (a.dist > b.dist) return true;
      if (a.dist < b.dist) return false;
    } else {  // fresh structure: sprout from the middle
      if (a.dist < b.dist) return true;
      if (a.dist > b.dist) return false;
    }
    if (a.host != b.host) return a.host < b.host;
    return a.hp < b.hp;
  });
  double hubReach = 0.0;  // hub centre -> port distance; direct chain pitch = host + hub reach
  if (hub != nullptr)
    for (const auto& np : hub->connectionPoints)
      hubReach = std::max(hubReach, length(np.localPosition));
  for (const SCand& cand : cands) {
    const bool tooTight = hub != nullptr && cand.hubHost && bodyExtent > cand.portOffset + hubReach;
    if (tooTight && span != nullptr) {  // beam-spaced rung: span first, hub on its far port
      const InstanceId sp = snapHubOnto(work, catalog, *span, cand.host, cand.hp, opts);
      if (sp != 0) {
        const PlacedModule* spm = work.find(sp);
        InstanceId nh = 0;
        for (const auto& np : span->connectionPoints) {
          if (spm == nullptr || connectorIsLinked(*spm, np.id)) continue;
          nh = snapHubOnto(work, catalog, *hub, sp, np.id, opts);
          if (nh != 0) break;
        }
        if (nh != 0 && !bodyOverlapsHost(work, catalog, nh, sp)) {
          connectorsAdded += 2;
          return nh;
        }
        if (nh != 0) removeWithUnlink(work, nh);
        removeWithUnlink(work, sp);  // half a rung helps nobody — roll back the beam
      }
      continue;  // do NOT fall back to a direct hub here: that is what seals the shell
    }
    for (const ModuleDef* d : {hub, span}) {
      if (d == nullptr) continue;
      const InstanceId nh = snapHubOnto(work, catalog, *d, cand.host, cand.hp, opts);
      if (nh == 0) continue;
      if (bodyOverlapsHost(work, catalog, nh, cand.host)) {
        removeWithUnlink(work, nh);  // degenerate self-coincident mate — undo and try the next
        continue;
      }
      ++connectorsAdded;
      return nh;
    }
  }
  return 0;
}

// ---- Fishbone seed helpers (spec §9.1 production-line rework) -------------------------------

double requiredRowHeight(const ModuleCatalog& catalog, const ModuleDef& hub,
                         const std::vector<std::string>& moduleIds) {
  double maxExt = 0.0;

  std::vector<std::string> uniq = moduleIds;
  std::sort(uniq.begin(), uniq.end());
  uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());

  for (const std::string& id : uniq) {
    const ModuleDef* def = catalog.find(id);
    if (def == nullptr) continue;
    for (const auto& cp : hub.connectionPoints) {
      if (std::abs(cp.localPosition.y) > 10.0) continue; // skip vertical ports
      Station tmp;
      PlacedModule h;
      h.defId = hub.id;
      const InstanceId hid = tmp.add(h);

      AutoLayoutOptions opts;
      opts.plotHalfExtent = 1e9;
      opts.floatingMargin = 0.0;

      const InstanceId mid = snapOnto(tmp, catalog, *def, hid, cp.id, opts);
      if (mid != 0) {
        const PlacedModule* pm = tmp.find(mid);
        const AABB wa = worldAabb(def->aabb, pm->worldTransform);
        maxExt = std::max(maxExt, std::abs(wa.max.y - wa.min.y));
      }
    }
  }
  return maxExt;
}

// Measures the maximum horizontal reach of a module snapped perpendicular to the hub.
double moduleMaxReach(const ModuleCatalog& catalog, const ModuleDef& hub, Vec3 axis,
                      const std::string& type) {
  const ModuleDef* def = catalog.find(type);
  if (def == nullptr) return 0.0;
  Station tmp;
  PlacedModule h;
  h.defId = hub.id;
  const InstanceId hid = tmp.add(h);
  const Vec3 hubCenter = (hub.aabb.min + hub.aabb.max) * 0.5;
  double maxReach = 0.0;
  for (const auto& cp : hub.connectionPoints) {
    const double pl = length(cp.localPosition);
    if (pl < 1e-6 || std::abs(dot(cp.localPosition, axis)) < 0.5 * pl) continue;  // axis-aligned
    for (const auto& np : def->connectionPoints) {
      if (!connectorsCompatible(cp, np)) continue;
      const AABB wa =
          worldAabb(def->aabb, computeSnapTransform(tmp, catalog, hid, cp.id, *def, np.id));
      maxReach = std::max(maxReach, std::max(std::abs(dot(wa.max - hubCenter, axis)),
                                             std::abs(dot(wa.min - hubCenter, axis))));
      break;  // first compatible connector is representative
    }
  }
  if (maxReach <= 0.0)
    maxReach = std::abs(dot(hub.aabb.max - hubCenter, axis));  // bare hub fallback
  return maxReach;
}

// Lists horizontal ports on a hub, ordered with ±X first, then ±Z, for clean symmetric packing.
std::vector<std::string> orderedHorizontalPorts(const Station& work, const ModuleCatalog& catalog,
                                                InstanceId hubId) {
  const PlacedModule* h = work.find(hubId);
  if (h == nullptr) return {};
  const ModuleDef* hd = catalog.find(h->defId);
  if (hd == nullptr) return {};

  std::vector<std::string> xPorts;
  std::vector<std::string> zPorts;

  for (const auto& cp : hd->connectionPoints) {
    const Vec3 worldDir = apply(h->worldTransform, cp.localPosition) - h->worldTransform.position;
    const double wl = length(worldDir);
    // Horizontal: perpendicular to vertical -Y spine growth
    if (wl > 1e-6 && std::abs(dot(worldDir, Vec3{0, -1, 0})) < 0.5 * wl) {
      if (std::abs(dot(worldDir, Vec3{1, 0, 0})) > 0.5 * wl) {
        xPorts.push_back(cp.id);
      } else {
        zPorts.push_back(cp.id);
      }
    }
  }
  std::sort(xPorts.begin(), xPorts.end());
  std::sort(zPorts.begin(), zPorts.end());

  std::vector<std::string> result = xPorts;
  result.insert(result.end(), zPorts.begin(), zPorts.end());
  return result;
}


// Beams needed to bridge one bay of length `pitch` with `beam`; 0 if no beam (chain hubs direct).
std::size_t beamsForPitch(const ModuleDef* beam, double pitch) {
  if (beam == nullptr) return 0;
  const double span = spanLength(*beam);
  if (span <= 0.0) return 0;
  return static_cast<std::size_t>(std::max(1.0, std::ceil(pitch / span)));
}

// The hub's port offset along `axis` (how far an axis-aligned port sits from the hub centre).
double hubPortOffset(const ModuleDef& hub, Vec3 axis) {
  double off = 0.0;
  for (const auto& cp : hub.connectionPoints) {
    const double pl = length(cp.localPosition);
    if (pl > 1e-6 && std::abs(dot(cp.localPosition, axis)) > 0.5 * pl)  // axis-aligned port
      off = std::max(off, std::abs(dot(cp.localPosition, axis)));
  }
  return off;
}

// The beam + count that bridges a spine bay to a target HUB-CENTRE spacing. Chaining two hubs with
// N beams spaces their centres by 2·portOffset + N·span, so the beam only has to span the gap that
// the joints leave (`target − 2·portOffset`) — comparing the raw beam span to `target` (ignoring
// the offsets) over-shoots a whole beam size, which is what made the seed reach for the long `…03`
// beam where a `…02`/`…01` already clears the module. Returns no beam (chain hubs directly) when
// the bare joint already meets the target.
struct SpineSpec {
  const ModuleDef* beam = nullptr;
  std::size_t beams = 0;
};
SpineSpec chooseSpine(const ModuleCatalog& catalog, const std::string& faction,
                      const std::vector<std::string>& needTypes, const ModuleDef& hub, Vec3 axis,
                      double target) {
  const double base = 2.0 * hubPortOffset(hub, axis);
  if (base >= target) return {nullptr, 0};  // hubs already clear the target when chained directly
  const double bridge = target - base;
  const ModuleDef* beam = bestSpanFor(catalog, faction, needTypes, bridge);
  return {beam, beamsForPitch(beam, bridge)};
}

// Extend a spine by one bay in world direction `dir`: chain `beams` straight beams off `fromId`,
// then a hub at the far end. Rolls back every beam if any piece (or the end hub) won't place, so
// a dead-end attempt (plot edge, collision) never strands a stub. Returns the new hub id, or 0.
InstanceId growSpine(Station& work, const ModuleCatalog& catalog, InstanceId fromId, Vec3 dir,
                     const ModuleDef* beam, std::size_t beams, const ModuleDef& hub,
                     const AutoLayoutOptions& opts, int& connectorsAdded) {
  std::vector<InstanceId> laid;
  InstanceId prev = fromId;
  for (std::size_t k = 0; k < beams; ++k) {
    const std::string p = freePortToward(work, catalog, prev, dir);
    const InstanceId b = p.empty() ? 0 : snapOnto(work, catalog, *beam, prev, p, opts);
    if (b == 0) {
      for (auto it = laid.rbegin(); it != laid.rend(); ++it) removeWithUnlink(work, *it);
      return 0;
    }
    laid.push_back(b);
    prev = b;
  }
  const std::string hp = freePortToward(work, catalog, prev, dir);
  const InstanceId nh = hp.empty() ? 0 : snapHubOnto(work, catalog, hub, prev, hp, opts);
  if (nh == 0) {
    for (auto it = laid.rbegin(); it != laid.rend(); ++it) removeWithUnlink(work, *it);
    return 0;
  }
  connectorsAdded += static_cast<int>(laid.size()) + 1;
  return nh;
}

// A "prod line": the vertical connector sub-spine that hangs off ONE rib hub and actually carries
// the modules. Drops a −Y sub-spine off `ribHub` and combs up to `want` copies of modules in `defs`
// (starting from `defsIndex`) off the sub-spine hubs' horizontal ports. Returns the count placed.
std::size_t buildProdLine(Station& work, const ModuleCatalog& catalog, InstanceId ribHub,
                          const std::vector<std::string>& defs, std::size_t& defsIndex,
                          std::size_t want, const ModuleDef& hub, const ModuleDef* vBeam,
                          std::size_t vBeams, const AutoLayoutOptions& opts, int& connectorsAdded) {
  const Vec3 down{0, -1, 0};
  std::size_t placed = 0;
  InstanceId prev = ribHub;  // grow a hub BELOW the rib first, so the rib bar itself stays clear
  while (placed < want && defsIndex < defs.size()) {
    const InstanceId vHub =
        growSpine(work, catalog, prev, down, vBeam, vBeams, hub, opts, connectorsAdded);
    if (vHub == 0) break;  // hit the plot floor

    // Comb up to (want - placed) modules onto vHub
    const std::vector<std::string> portIds = orderedHorizontalPorts(work, catalog, vHub);
    std::size_t got = 0;
    for (const std::string& pid : portIds) {
      if (placed + got >= want || defsIndex >= defs.size()) break;
      const PlacedModule* h = work.find(vHub);
      if (h == nullptr || connectorIsLinked(*h, pid)) continue;
      const ModuleDef* currentDef = catalog.find(defs[defsIndex]);
      if (currentDef != nullptr && snapOnto(work, catalog, *currentDef, vHub, pid, opts) != 0) {
        ++got;
        ++defsIndex;
      }
    }

    placed += got;
    if (got == 0) break;  // this hub took nothing (collision) — stop rather than spin
    prev = vHub;
  }
  return placed;
}


}  // namespace

// Scan a def-sized grid across the y=0 plot plane, nearest-the-station first, and take the
// first collision- and clearance-free pose (contract in seed.hpp).
bool floatInsidePlot(Station& work, const ModuleCatalog& catalog, const ModuleDef& def,
                     const AutoLayoutOptions& opts) {
  const double h = opts.plotHalfExtent;
  const double xLo = -h - def.aabb.min.x;
  const double xHi = h - def.aabb.max.x;
  const double zLo = -h - def.aabb.min.z;
  const double zHi = h - def.aabb.max.z;
  if (xLo > xHi || zLo > zHi) return false;  // wider than the plot itself
  // Quarter-body steps: a def-sized grid offers a huge module (a 6.7km pier in a 20km plot)
  // only a handful of poses, and its dock corridors reject them all; overlap of CANDIDATES is
  // fine — the collision check decides.
  const double px = ((def.aabb.max.x - def.aabb.min.x) + opts.floatingMargin) * 0.25;
  const double pz = ((def.aabb.max.z - def.aabb.min.z) + opts.floatingMargin) * 0.25;
  const Vec3 mid = work.empty() ? Vec3{} : centroid(work, catalog);
  struct Cell {
    Vec3 pos;
    double dist = 0.0;
  };
  // Three coarse y-layers: dock modules with radial corridor stars (the Boron trade-station
  // pier fans SIX 100km corridors every 60°) shadow the whole y=0 plane around a built-out
  // station — the only legal poses lift the corridor plane clear of the station.
  const std::array<double, 3> yLayers{0.0, h * 0.5, -h * 0.5};
  std::vector<Cell> cells;
  const int nx = static_cast<int>((xHi - xLo) / px);
  const int nz = static_cast<int>((zHi - zLo) / pz);
  for (int ix = 0; ix <= nx; ++ix)
    for (int iz = 0; iz <= nz; ++iz)
      for (const double y : yLayers) {
        const Vec3 p{xLo + ix * px, y, zLo + iz * pz};
        cells.push_back(Cell{p, length(p - mid)});
      }
  std::sort(cells.begin(), cells.end(), [](const Cell& a, const Cell& b) {
    if (a.dist < b.dist) return true;
    if (a.dist > b.dist) return false;
    if (a.pos.x < b.pos.x) return true;
    if (a.pos.x > b.pos.x) return false;
    return a.pos.z < b.pos.z;
  });
  // Yaw variants matter for dock modules: their flight corridors reach ~100km, so from most
  // positions only the rotation that points the corridors AWAY from the station is legal.
  const std::array<Quat, 4> yaws{Quat{},                                            // identity
                                 Quat{std::sqrt(0.5), 0.0, std::sqrt(0.5), 0.0},    // +90° about Y
                                 Quat{0.0, 0.0, 1.0, 0.0},                          // 180°
                                 Quat{std::sqrt(0.5), 0.0, -std::sqrt(0.5), 0.0}};  // -90°
  for (const Cell& cell : cells) {
    for (const Quat& yaw : yaws) {
      const Transform xf{cell.pos, yaw};
      if (!insidePlot(worldAabb(def.aabb, xf), opts.plotHalfExtent)) continue;
      if (collidesWithStation(def, xf, 0, work, catalog)) continue;
      if (collidesClearance(def, xf, 0, 0, work, catalog)) continue;
      addRoot(work, def, xf);
      return true;
    }
  }
  return false;
}

int compactSeed(Station& work, const std::vector<std::string>& interiorOrdered,
                const ModuleCatalog& catalog, const AutoLayoutOptions& opts) {
  // Largest AABB first (anchor big modules while there is room), then def id.
  std::vector<std::string> order;
  for (const std::string& id : interiorOrdered)
    if (catalog.find(id) != nullptr) order.push_back(id);
  std::sort(order.begin(), order.end(), [&](const std::string& a, const std::string& b) {
    const double va = aabbVolume(catalog.find(a)->aabb);
    const double vb = aabbVolume(catalog.find(b)->aabb);
    if (va > vb) return true;
    if (va < vb) return false;
    return a < b;
  });

  const std::string faction = opts.faction.empty()
                                  ? (order.empty() ? std::string{} : factionOf(order.front()))
                                  : opts.faction;
  const std::vector<std::string> needTypes = cartPortTypes(order, catalog);
  const ModuleDef* hub = bestHubFor(catalog, faction, /*minPorts=*/3, needTypes);

  // Coarse outward float line — the last resort, and the whole degrade path when no compatible
  // hub exists. Starts past existing structure so additive floats don't overlap it.
  double floatCursorX = work.empty() ? 0.0 : bounds(work, catalog).max.x;
  const auto floatModule = [&](const ModuleDef& def) {
    floatCursorX += (def.aabb.max.x - def.aabb.min.x) + opts.floatingMargin;
    Transform xf;
    xf.position = Vec3{floatCursorX, 0.0, 0.0};
    if (insidePlot(worldAabb(def.aabb, xf), opts.plotHalfExtent)) addRoot(work, def, xf);
  };

  if (hub == nullptr) {  // no hub the cart can mate -> float everything (legal, if ugly)
    for (const std::string& id : order) floatModule(*catalog.find(id));
    return 0;
  }

  // Footprint-driven spacing (spec §9.1): adjacent hubs must sit far enough apart that their
  // modules never overlap whatever orientation a module mates in, so the gap is the cart's
  // LARGEST module dimension plus a margin. The backbone is therefore a spine of hubs joined by
  // straight beams of that length, with modules hung off the perpendicular ports — collision-
  // free by construction, not by luck. Beam length comes from the catalog, so big modules get
  // long beams and small ones get short beams automatically.
  const Vec3 axis{1, 0, 0};
  const double pitch = requiredPitch(catalog, *hub, axis, order, opts.floatingMargin);
  const ModuleDef* beam = bestSpanFor(catalog, faction, cartPortTypes({hub->id}, catalog), pitch);
  const double beamSpan = beam != nullptr ? spanLength(*beam) : 0.0;
  // How many beams to bridge one bay; 0 when no beam exists (degrade: chain hubs directly).
  const std::size_t beamsPerBay =
      beam != nullptr && beamSpan > 0.0
          ? static_cast<std::size_t>(std::max(1.0, std::ceil(pitch / beamSpan)))
          : 0;

  int connectorsAdded = 0;
  InstanceId curTip = firstHubWithFreePort(work, catalog);  // reuse existing structure if any
  if (curTip == 0) {
    if (work.empty()) {
      addRoot(work, *hub, Transform{});  // greenfield head at the origin
      curTip = work.modules().back().instanceId;
      ++connectorsAdded;
    } else if (trySnap(work, catalog, *hub, opts)) {  // snap the head onto existing structure
      curTip = work.modules().back().instanceId;
      ++connectorsAdded;
    } else {  // cannot attach a hub to what is there -> degrade to floating
      for (const std::string& id : order) floatModule(*catalog.find(id));
      return connectorsAdded;
    }
  }

  // Extend the spine by one bay: chain `beamsPerBay` straight beams off the tip, then a hub at
  // the far end. Tries +X first, then the perpendicular axes (and -X last), so when growth along
  // the backbone hits the plot edge the spine TURNS and keeps filling instead of stranding the
  // module. With no compatible beam, chains the hub directly (tight, but connected). Returns the
  // new tip, or 0 if no direction admits a full bay (caller then floats the module).
  const std::array<Vec3, 6> dirs{
      {axis, Vec3{0, 0, 1}, Vec3{0, 0, -1}, Vec3{0, 1, 0}, Vec3{0, -1, 0}, Vec3{-1, 0, 0}}};
  const auto extendBay = [&]() -> InstanceId {
    for (const Vec3 dir : dirs) {
      std::vector<InstanceId> laid;  // beams placed this attempt; rolled back if the hub fails
      InstanceId prev = curTip;
      bool laidBeams = true;
      for (std::size_t k = 0; k < beamsPerBay; ++k) {
        const std::string p = freePortToward(work, catalog, prev, dir);
        if (p.empty()) {
          laidBeams = false;
          break;
        }
        const InstanceId b = snapOnto(work, catalog, *beam, prev, p, opts);
        if (b == 0) {
          laidBeams = false;
          break;
        }
        laid.push_back(b);
        prev = b;
      }
      InstanceId nh = 0;
      if (laidBeams) {
        const std::string hp = freePortToward(work, catalog, prev, dir);
        if (!hp.empty()) nh = snapOnto(work, catalog, *hub, prev, hp, opts);
      }
      if (nh != 0) {
        connectorsAdded += static_cast<int>(laid.size()) + 1;
        return nh;
      }
      // This direction is a dead end (edge of plot, collision): undo its beams and try the next,
      // so a failed turn never leaves a stub beam behind.
      for (auto it = laid.rbegin(); it != laid.rend(); ++it) removeWithUnlink(work, *it);
    }
    return 0;
  };

  for (const std::string& id : order) {
    const ModuleDef* def = catalog.find(id);  // non-null (filtered above)
    if (attachToAnyHub(work, catalog, *def, curTip, axis, opts)) continue;
    // The whole spine is full (or this module collides on every free port) -> grow a bay, retry.
    const InstanceId nt = extendBay();
    if (nt != 0) {
      curTip = nt;
      if (attachToAnyHub(work, catalog, *def, curTip, axis, opts)) continue;
    }
    floatModule(*def);  // could not place even on a fresh hub — last resort
  }
  return connectorsAdded;
}


std::size_t measureHubCapacity(const ModuleCatalog& catalog, const ModuleDef& hub, const std::string& moduleType, const AutoLayoutOptions& opts) {
  const ModuleDef* def = catalog.find(moduleType);
  if (def == nullptr) return 1;

  Station tmp;
  PlacedModule h;
  h.defId = hub.id;
  const InstanceId hid = tmp.add(h);

  std::size_t capacity = 0;
  AutoLayoutOptions dummyOpts = opts;
  dummyOpts.plotHalfExtent = 1e9; // Ignore plot bounds for theoretical measurement

  std::vector<std::string> horiz = orderedHorizontalPorts(tmp, catalog, hid);
  for (const std::string& pid : horiz) {
    if (snapOnto(tmp, catalog, *def, hid, pid, dummyOpts) != 0) {
      capacity++;
    }
  }
  return std::max<std::size_t>(1, capacity);
}

int fishboneSeed(Station& work, const std::vector<std::string>& interiorOrdered,
                 const ModuleCatalog& catalog, const AutoLayoutOptions& opts) {
  std::vector<std::string> valid;
  for (const std::string& id : interiorOrdered)
    if (catalog.find(id) != nullptr) valid.push_back(id);
  if (valid.empty()) return 0;

  // Incremental building support: if not empty, fallback to compactSeed for now
  // Removed fallback, implementing Anchor and Grow

  // Group modules
  std::unordered_map<std::string, std::vector<std::string>> keyToModules;
  std::vector<std::string> types;
  std::unordered_map<std::string, std::string> keyToRepr;

  std::unordered_map<std::string, std::size_t> rawCounts;
  for (const std::string& id : valid) rawCounts[id]++;
  const bool megaCompaction = rawCounts.size() > 8;

  for (const std::string& id : valid) {
    const ModuleDef* d = catalog.find(id);
    std::string key = id;
    if ((opts.mixRibModules || megaCompaction) && d != nullptr) {
      if (d->category == Category::Production) key = "category_production";
      else if (d->category == Category::Storage) key = "category_storage";
      else if (d->category == Category::Habitat) key = "category_habitat";
      else if (d->category == Category::Other) key = "category_other";
    }
    if (keyToModules.find(key) == keyToModules.end()) {
      types.push_back(key);
      keyToRepr[key] = id;
    }
    keyToModules[key].push_back(id);
  }

  std::sort(types.begin(), types.end(), [&](const std::string& a, const std::string& b) {
    const double va = aabbVolume(catalog.find(keyToRepr[a])->aabb);
    const double vb = aabbVolume(catalog.find(keyToRepr[b])->aabb);
    return va > vb;
  });

  const std::string faction = opts.faction.empty() ? factionOf(keyToRepr[types.front()]) : opts.faction;
  const std::vector<std::string> needTypes = cartPortTypes(valid, catalog);
  const ModuleDef* hub = bestHubFor(catalog, faction, 3, needTypes);
  if (hub == nullptr) return compactSeed(work, interiorOrdered, catalog, opts);

  const std::vector<std::string> hubTypes = cartPortTypes({hub->id}, catalog);

  struct PlannedColumn {
    std::vector<std::string> modulesToPlace;
  };

  struct PlannedRib {
    std::vector<PlannedColumn> posZColumns;
    std::vector<PlannedColumn> negZColumns;
    double maxReachX = 0.0;
    double maxReachZ = 0.0;
    double maxReachY = 0.0;
  };

  std::vector<PlannedRib> plannedRibs;
  const std::size_t maxVerticalHubs = 4; // Max 4 hubs deep per column (-Y)

  // PLAN THE GRID
  for (const std::string& t : types) {
    const std::string& reprId = keyToRepr[t];
    const std::size_t capacityPerHub = measureHubCapacity(catalog, *hub, reprId, opts);
    const std::size_t capacityPerColumn = maxVerticalHubs * capacityPerHub;

    std::vector<std::string>& defs = keyToModules[t];
    std::size_t remaining = defs.size();
    std::size_t defsIndex = 0;

    double reachX = moduleMaxReach(catalog, *hub, Vec3{1, 0, 0}, reprId);
    double reachZ = moduleMaxReach(catalog, *hub, Vec3{0, 0, 1}, reprId);
    double reachY = requiredRowHeight(catalog, *hub, {reprId});

    while (remaining > 0) {
      PlannedRib rib;
      rib.maxReachX = reachX;
      rib.maxReachZ = reachZ;
      rib.maxReachY = reachY;

      const std::size_t maxColsPerRibHalf = 1; 

      for (std::size_t c = 0; c < maxColsPerRibHalf && remaining > 0; ++c) {
        std::size_t take = std::min(capacityPerColumn, remaining);
        PlannedColumn col;
        for (std::size_t i = 0; i < take; ++i) col.modulesToPlace.push_back(defs[defsIndex++]);
        rib.posZColumns.push_back(col);
        remaining -= take;
      }

      std::size_t numHPorts = 0;
      for (const auto& cp : hub->connectionPoints) {
        if (std::abs(cp.localPosition.y) < 10.0) numHPorts++;
      }
      const bool canSymmetric = numHPorts >= 4;

      if (canSymmetric) {
        for (std::size_t c = 0; c < maxColsPerRibHalf && remaining > 0; ++c) {
          std::size_t take = std::min(capacityPerColumn, remaining);
          PlannedColumn col;
          for (std::size_t i = 0; i < take; ++i) col.modulesToPlace.push_back(defs[defsIndex++]);
          rib.negZColumns.push_back(col);
          remaining -= take;
        }
        plannedRibs.push_back(rib);
      } else {
        plannedRibs.push_back(rib);
        if (remaining > 0) {
          PlannedRib rib2;
          rib2.maxReachX = reachX;
          rib2.maxReachZ = reachZ;
          rib2.maxReachY = reachY;
          for (std::size_t c = 0; c < maxColsPerRibHalf && remaining > 0; ++c) {
            std::size_t take = std::min(capacityPerColumn, remaining);
            PlannedColumn col;
            for (std::size_t i = 0; i < take; ++i) col.modulesToPlace.push_back(defs[defsIndex++]);
            rib2.negZColumns.push_back(col);
            remaining -= take;
          }
          plannedRibs.push_back(rib2);
        }
      }
    }
  }

  int connectorsAdded = 0;
  const double skeletonY = opts.skeletonVerticalBias * opts.plotHalfExtent;

  std::vector<double> xOffsets(plannedRibs.size(), 0.0);
  double totalLength = 0.0;
  for (std::size_t i = 1; i < plannedRibs.size(); ++i) {
    double r1_pos = plannedRibs[i - 1].posZColumns.empty() ? 0.0 : plannedRibs[i - 1].maxReachX;
    double r2_pos = plannedRibs[i].posZColumns.empty() ? 0.0 : plannedRibs[i].maxReachX;
    double r1_neg = plannedRibs[i - 1].negZColumns.empty() ? 0.0 : plannedRibs[i - 1].maxReachX;
    double r2_neg = plannedRibs[i].negZColumns.empty() ? 0.0 : plannedRibs[i].maxReachX;

    const double hubR = hubPortOffset(*hub, Vec3{1, 0, 0});
    
    auto calcReach = [hubR](bool has1, double r1, bool has2, double r2) {
      if (has1 && has2) return (r1 + r2) * 0.5;
      if (has1) return r1 * 0.5 + hubR;
      if (has2) return r2 * 0.5 + hubR;
      return hubR * 2.0;
    };

    const double reachPos = calcReach(!plannedRibs[i - 1].posZColumns.empty(), r1_pos,
                                      !plannedRibs[i].posZColumns.empty(), r2_pos);
    const double reachNeg = calcReach(!plannedRibs[i - 1].negZColumns.empty(), r1_neg,
                                      !plannedRibs[i].negZColumns.empty(), r2_neg);

    double effectiveReach = std::max(reachPos, reachNeg);
    const double target = effectiveReach + kSpineClearance;
    const SpineSpec xS = chooseSpine(catalog, faction, hubTypes, *hub, Vec3{1, 0, 0}, target);
    double spanL = xS.beam != nullptr ? spanLength(*xS.beam) : 0.0;
    const double xSpacing = 2.0 * hubPortOffset(*hub, Vec3{1, 0, 0}) + static_cast<double>(xS.beams) * spanL;
    xOffsets[i] = xOffsets[i - 1] + xSpacing;
    totalLength = xOffsets[i];
  }

  InstanceId spineTip = firstHubWithFreePort(work, catalog);
  if (spineTip == 0) {
    if (work.empty()) {
      const double x0 = plannedRibs.empty() ? 0.0 : std::max(-totalLength * 0.5, -opts.plotHalfExtent + hubPortOffset(*hub, Vec3{1, 0, 0}) + 50.0);
      addRoot(work, *hub, Transform{Vec3{x0, skeletonY, 0}, Quat{}});
      spineTip = work.modules().back().instanceId;
      ++connectorsAdded;
    } else if (trySnap(work, catalog, *hub, opts, true)) { // snap to any hub port
      spineTip = work.modules().back().instanceId;
      ++connectorsAdded;
    } else if (trySnap(work, catalog, *hub, opts, false)) { // snap to any port at all
      spineTip = work.modules().back().instanceId;
      ++connectorsAdded;
    } else { // Degrade to floating
      const double x0 = plannedRibs.empty() ? 0.0 : std::max(-totalLength * 0.5, -opts.plotHalfExtent + hubPortOffset(*hub, Vec3{1, 0, 0}) + 50.0);
      addRoot(work, *hub, Transform{Vec3{x0, skeletonY, 0}, Quat{}});
      spineTip = work.modules().back().instanceId;
      ++connectorsAdded;
    }
  }

  std::vector<std::string> leftover;

  for (std::size_t i = 0; i < plannedRibs.size(); ++i) {
    const auto& rib = plannedRibs[i];
    InstanceId spineHub = spineTip;
    
    if (i > 0) {
      const double target = plannedRibs[i - 1].maxReachX + rib.maxReachX + kSpineClearance;
      const SpineSpec xS = chooseSpine(catalog, faction, hubTypes, *hub, Vec3{1, 0, 0}, target);
      spineHub = growSpine(work, catalog, spineTip, Vec3{1, 0, 0}, xS.beam, xS.beams, *hub, opts, connectorsAdded);
      if (spineHub == 0) {
        for (const auto& c : rib.posZColumns) for (const auto& id : c.modulesToPlace) leftover.push_back(id);
        for (const auto& c : rib.negZColumns) for (const auto& id : c.modulesToPlace) leftover.push_back(id);
        continue;
      }
      spineTip = spineHub;
    }

    const auto buildRibHalf = [&](const std::vector<PlannedColumn>& columns, Vec3 dir) {
      InstanceId prevRibHub = spineHub;
      for (std::size_t c = 0; c < columns.size(); ++c) {
        const auto& col = columns[c];
        InstanceId currentRibHub = prevRibHub;
        
        double zTarget = rib.maxReachZ + kSpineClearance; 
        const SpineSpec zS = chooseSpine(catalog, faction, hubTypes, *hub, dir, zTarget);
        currentRibHub = growSpine(work, catalog, prevRibHub, dir, zS.beam, zS.beams, *hub, opts, connectorsAdded);

        if (currentRibHub == 0) {
          for (const auto& id : col.modulesToPlace) leftover.push_back(id);
          continue;
        }

        double vTarget = rib.maxReachY + kSpineClearance; 
        const SpineSpec vS = chooseSpine(catalog, faction, hubTypes, *hub, Vec3{0, -1, 0}, vTarget);
        std::size_t defsIndex = 0;
        buildProdLine(work, catalog, currentRibHub, col.modulesToPlace, defsIndex, col.modulesToPlace.size(), *hub, vS.beam, vS.beams, opts, connectorsAdded);
        
        for (std::size_t k = defsIndex; k < col.modulesToPlace.size(); ++k) {
          leftover.push_back(col.modulesToPlace[k]);
        }
        prevRibHub = currentRibHub;
      }
    };

    buildRibHalf(rib.posZColumns, Vec3{0, 0, 1});
    buildRibHalf(rib.negZColumns, Vec3{0, 0, -1});
  }

  double floatCursorX = work.empty() ? 0.0 : bounds(work, catalog).max.x;
  for (const std::string& id : leftover) {
    const ModuleDef* def = catalog.find(id);
    if (trySnap(work, catalog, *def, opts, true)) continue;
    floatCursorX += (def->aabb.max.x - def->aabb.min.x) + opts.floatingMargin;
    const Transform xf{Vec3{floatCursorX, 0, 0}, Quat{}};
    if (insidePlot(worldAabb(def->aabb, xf), opts.plotHalfExtent)) addRoot(work, *def, xf);
  }

  return connectorsAdded;
}

int organicSeed(Station& work, const std::vector<std::string>& interiorOrdered,
                const ModuleCatalog& catalog, const AutoLayoutOptions& opts) {
  std::vector<std::string> order;
  for (const std::string& id : interiorOrdered)
    if (catalog.find(id) != nullptr) order.push_back(id);
  if (order.empty()) return 0;

  // Biggest bodies first (then most ports, then id): a fat single-port module placed late
  // arrives at a crowded interior with no seat its body clears — placed early it claims a
  // clean seat while space is abundant, and the small/chainable modules that follow fit the
  // remaining gaps (known-issues 4.3). Multi-port "cores" still lead within a size class.
  std::sort(order.begin(), order.end(), [&](const std::string& a, const std::string& b) {
    const ModuleDef* da = catalog.find(a);
    const ModuleDef* db = catalog.find(b);
    const double va = aabbVolume(da->aabb);
    const double vb = aabbVolume(db->aabb);
    if (va > vb) return true;
    if (va < vb) return false;
    const std::size_t pa = da->connectionPoints.size();
    const std::size_t pb = db->connectionPoints.size();
    if (pa != pb) return pa > pb;
    return a < b;
  });

  const std::string faction = opts.faction.empty() ? factionOf(order.front()) : opts.faction;
  const std::vector<std::string> needTypes = cartPortTypes(order, catalog);
  const ModuleDef* hub = bestHubFor(catalog, faction, /*minPorts=*/5, needTypes);
  if (hub == nullptr) hub = bestHubFor(catalog, faction, /*minPorts=*/3, needTypes);

  // The rung span: bodies wider than the direct hub-to-hub pitch need beam-spaced structure
  // (injectStructure's tooTight rule). The joint itself already spans two port offsets, so the
  // beam only has to bridge the REST of the largest body — sizing it to the full extent grows
  // rungs so long the station sprawls into the plot edge on big-module carts and later modules
  // run out of in-plot seats.
  double maxExtent = 0.0;
  for (const std::string& id : order)
    maxExtent = std::max(maxExtent, maxBodyExtent(*catalog.find(id)));
  double hubReach = 0.0;
  if (hub != nullptr)
    for (const auto& np : hub->connectionPoints)
      hubReach = std::max(hubReach, length(np.localPosition));
  const ModuleDef* span =
      bestSpanFor(catalog, faction, needTypes, std::max(0.0, maxExtent - 2.0 * hubReach));

  int connectorsAdded = 0;
  std::size_t next = 0;
  std::unordered_map<InstanceId, int> chainDepth;  // functional hops from nearest hub/core

  // Seed the frontier. Additive: reuse an existing free port. Greenfield: plant a root core — but a
  // 1-port leaf can't host anything, so synthesize a hub root instead when the catalog has one.
  if (!hasAnyFreePort(work, catalog)) {
    const ModuleDef* first = catalog.find(order.front());
    if (first->connectionPoints.size() <= 1 && hub != nullptr) {
      addRoot(work, *hub, Transform{});
      ++connectorsAdded;
    } else {
      addRoot(work, *first, Transform{});
      next = 1;  // consumed as the root
    }
  }

  std::vector<std::string> leftover;
  for (; next < order.size(); ++next) {
    const ModuleDef* def = catalog.find(order[next]);  // non-null (filtered)
    const ModuleDef* probe = nullptr;  // set by the guard: largest body still to be placed
    bool strands = false;              // guard verdict: placements are sealing the station
    // Chain depth of a would-be host (hub host => depth 1; functional host => +1), for the
    // anti-snake rule: a low-port module (<=2 ports) must not extend a chain past kMaxChain.
    const auto depthOf = [&](InstanceId host) {
      const PlacedModule* h = work.find(host);
      const ModuleDef* hd = h ? catalog.find(h->defId) : nullptr;
      if (hd != nullptr && hd->category == Category::Connector) return 1;
      const auto it = chainDepth.find(host);
      return (it == chainDepth.end() ? 1 : it->second) + 1;
    };
    if (hub != nullptr) {
      // Dry-run the placement on a scratch copy, and inject structure FIRST when it would either
      // over-extend a chain (anti-snake) or STRAND the station — some slabs carry both ports on
      // one face (meat, wheat), and a direct mate stacks the pair and buries every leftover port
      // in the seam, leaving no seatable port for any later injection. A free-port COUNT cannot
      // guard this: buried ports still count as free, so the count never drops. The probe is
      // "could the largest remaining module still SEAT afterwards" (not just "could a hub still
      // inject") — bodies wider than the direct-chain hub pitch shadow neighbouring ports, and a
      // hub can keep injecting into interior gaps long after every module-sized seat is walled
      // in. Escape structure must grow while a beam-width tunnel out of the shell still exists,
      // so the ladder runs HERE, pre-emptively, not after the shell has closed.
      Station peek = work;
      const InstanceId peekHost = organicSnap(peek, catalog, *def, opts);
      if (peekHost != 0) {
        const bool lowPort = def->connectionPoints.size() <= 2;
        const bool overExtends = lowPort && depthOf(peekHost) > kMaxChain;
        probe = def;  // largest remaining body (incl. this one) by max extent
        for (std::size_t k = next + 1; k < order.size(); ++k) {
          const ModuleDef* d = catalog.find(order[k]);
          if (maxBodyExtent(*d) > maxBodyExtent(*probe)) probe = d;
        }
        const double probeExtent = maxBodyExtent(*probe);
        int scratch = 0;
        strands = injectStructure(peek, catalog, hub, span, opts, scratch, 0, probeExtent) == 0;
        if (!strands) strands = !anySeatAdmits(peek, catalog, *probe, opts);
        if (overExtends || strands) {
          InstanceId rung = 0;
          for (int round = 0; round < kMaxEscalation; ++round) {
            rung =
                injectStructure(work, catalog, hub, span, opts, connectorsAdded, rung, probeExtent);
            if (rung == 0) break;
            Station reProbe = work;
            if (organicSnap(reProbe, catalog, *probe, opts) != 0 &&
                anySeatAdmits(reProbe, catalog, *probe, opts))
              break;  // two probe bodies seat again — the frontier is open, stop growing
          }
        }
      }
    }
    // Under strand pressure, prefer a seat that keeps a probe-sized opening; if every seat
    // would seal the station, place anyway (best effort beats floating).
    InstanceId host =
        strands && probe != nullptr ? organicSnap(work, catalog, *def, opts, probe) : 0;
    if (host == 0) host = organicSnap(work, catalog, *def, opts);
    InstanceId ladder = 0;  // each round extends what the last round added, so escapes gain depth
    for (int round = 0; host == 0 && hub != nullptr && round < kMaxEscalation; ++round) {
      ladder = injectStructure(work, catalog, hub, span, opts, connectorsAdded, ladder,
                               maxBodyExtent(*def));
      if (ladder == 0) break;
      host = organicSnap(work, catalog, *def, opts);
    }
    if (host != 0) {
      chainDepth[work.modules().back().instanceId] = depthOf(host);
      continue;
    }
    leftover.push_back(order[next]);  // genuinely no legal placement — float as last resort
  }

  // Overflow: retry the snap first — an earlier float exposes live ports, so later leftovers
  // cluster onto it instead of scattering — then float at the nearest legal in-plot spot
  // (nothing is silently dropped).
  for (const std::string& id : leftover) {
    const ModuleDef* def = catalog.find(id);
    if (organicSnap(work, catalog, *def, opts) != 0) continue;
    floatInsidePlot(work, catalog, *def, opts);
  }
  return connectorsAdded;
}

InstanceId injectStructureFor(Station& work, const ModuleCatalog& catalog, const ModuleDef& forDef,
                              const AutoLayoutOptions& opts, int& connectorsAdded,
                              InstanceId preferHost) {
  const std::string faction = opts.faction.empty() ? factionOf(forDef.id) : opts.faction;
  const std::vector<std::string> needTypes = cartPortTypes({forDef.id}, catalog);
  const ModuleDef* hub = bestHubFor(catalog, faction, /*minPorts=*/5, needTypes);
  if (hub == nullptr) hub = bestHubFor(catalog, faction, /*minPorts=*/3, needTypes);
  const double extent = maxBodyExtent(forDef);
  double hubReach = 0.0;
  if (hub != nullptr)
    for (const auto& np : hub->connectionPoints)
      hubReach = std::max(hubReach, length(np.localPosition));
  const ModuleDef* span =
      bestSpanFor(catalog, faction, needTypes, std::max(0.0, extent - 2.0 * hubReach));
  if (hub == nullptr && span == nullptr) return 0;
  return injectStructure(work, catalog, hub, span, opts, connectorsAdded, preferHost, extent);
}
}  // namespace x4sb

