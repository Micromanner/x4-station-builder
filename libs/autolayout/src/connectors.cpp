#include "x4sb/autolayout/connectors.hpp"
#include <iostream>

#include "x4sb/snap/snap.hpp"  // connectorsCompatible — the single source of truth for mating

#include <algorithm>
#include <string>
#include <vector>

namespace x4sb {
namespace {

// A connector candidate, captured so selection can sort deterministically rather
// than depend on the catalog's unordered iteration.
struct Cand {
  const ModuleDef* def = nullptr;
  std::size_t ports = 0;
  bool factionMatch = false;
};

// True if `hub` has at least one port that can mate a module port of some type in
// `needTypes`, using the real connectorsCompatible rule (untagged-or-equal). A null
// `needTypes` means "no compatibility constraint" (bestHub's faction-only behavior).
bool canHost(const ModuleDef& hub, const std::vector<std::string>* needTypes) {
  if (needTypes == nullptr) return true;
  for (const auto& cp : hub.connectionPoints)
    for (const std::string& nt : *needTypes) {
      ConnectionPoint probe;
      probe.type = nt;
      if (connectorsCompatible(cp, probe)) return true;
    }
  return false;
}

const ModuleDef* pick(const ModuleCatalog& catalog, const std::string& faction,
                      std::size_t minPorts, bool spanOnly,
                      const std::vector<std::string>* needTypes, bool preferMorePorts) {
  std::vector<Cand> cands;
  for (const auto& [id, def] : catalog.all()) {
    if (def.category != Category::Connector) continue;
    if (id.find("bor_base_05") != std::string::npos) continue; // blacklist wavy base_05
    if (id.find("bor_cross_02") != std::string::npos) continue; // blacklist horizontal-only cross
    const std::size_t ports = def.connectionPoints.size();
    if (spanOnly) {
      // minPorts applies only to hubs; spans are fixed at exactly 2 ports.
      if (ports != 2) continue;
    } else if (ports < minPorts) {
      continue;
    }
    if (!canHost(def, needTypes)) continue;  // reject hubs the cart can never mate
    cands.push_back(Cand{&def, ports, factionOf(id) == faction});
  }
  if (cands.empty()) return nullptr;
  std::sort(cands.begin(), cands.end(), [preferMorePorts](const Cand& a, const Cand& b) {
    if (a.factionMatch != b.factionMatch) return a.factionMatch;  // faction first
    // A seed hub hosts many modules AND must keep a port free to chain to the next hub, so the
    // backbone seed wants MORE ports; bestHub's original "fewest ports (least waste)" suits a
    // one-off connector pick.
    if (a.ports != b.ports) return preferMorePorts ? a.ports > b.ports : a.ports < b.ports;
    return a.def->id < b.def->id;  // stable tiebreak
  });
  return cands.front().def;
}

}  // namespace

std::string factionOf(const std::string& id) {
  const std::size_t first = id.find('_');
  if (first == std::string::npos) return "";
  const std::size_t second = id.find('_', first + 1);
  return id.substr(first + 1, (second == std::string::npos ? id.size() : second) - (first + 1));
}

std::size_t portCount(const ModuleDef& def) { return def.connectionPoints.size(); }

const ModuleDef* bestHub(const ModuleCatalog& catalog, const std::string& faction,
                         std::size_t minPorts) {
  return pick(catalog, faction, minPorts, /*spanOnly=*/false, /*needTypes=*/nullptr,
              /*preferMorePorts=*/false);
}

const ModuleDef* bestHubFor(const ModuleCatalog& catalog, const std::string& faction,
                            std::size_t minPorts, const std::vector<std::string>& needTypes) {
  return pick(catalog, faction, minPorts, /*spanOnly=*/false, &needTypes,
              /*preferMorePorts=*/true);
}

const ModuleDef* bestSpan(const ModuleCatalog& catalog, const std::string& faction) {
  return pick(catalog, faction, /*minPorts=*/0, /*spanOnly=*/true, /*needTypes=*/nullptr,
              /*preferMorePorts=*/false);
}

double spanLength(const ModuleDef& def) {
  if (def.connectionPoints.size() != 2) return 0.0;
  return length(def.connectionPoints[1].localPosition - def.connectionPoints[0].localPosition);
}

namespace {
// A straight beam has its two ports symmetric about the module origin (so chaining it extends
// the backbone in a straight line). An arc/bend connector fails this and would make the spine
// wander, so the skeleton seed must not use it as a span.
bool isStraightSpan(const ModuleDef& def) {
  if (def.connectionPoints.size() != 2) return false;
  const double span = spanLength(def);
  if (span <= 0.0) return false;
  const Vec3 sum = def.connectionPoints[0].localPosition + def.connectionPoints[1].localPosition;
  return length(sum) < 0.1 * span;  // midpoint ~at origin => collinear ends
}
}  // namespace

const ModuleDef* bestSpanFor(const ModuleCatalog& catalog, const std::string& faction,
                             const std::vector<std::string>& needTypes, double minLength) {
  struct SpanCand {
    const ModuleDef* def = nullptr;
    double len = 0.0;
    bool factionMatch = false;
  };
  std::vector<SpanCand> cands;
  for (const auto& [id, def] : catalog.all()) {
    if (def.category != Category::Connector || def.connectionPoints.size() != 2) continue;
    if (id.find("bor_base_05") != std::string::npos) continue; // blacklist wavy base_05
    if (!isStraightSpan(def)) continue;  // arcs/bends would make the spine wander
    if (!canHost(def, &needTypes)) continue;
    cands.push_back(SpanCand{&def, spanLength(def), factionOf(id) == faction});
  }
  if (cands.empty()) return nullptr;

  // Prefer the shortest span that still reaches minLength (least wasted material). When none
  // reaches it, take the longest available — a best-effort gap, still legal and deterministic.
  std::vector<SpanCand> reaching;
  for (const SpanCand& c : cands)
    if (c.len >= minLength) reaching.push_back(c);
  if (!reaching.empty()) {
    std::sort(reaching.begin(), reaching.end(), [](const SpanCand& a, const SpanCand& b) {
      if (a.factionMatch != b.factionMatch) return a.factionMatch;
      if (a.len < b.len) return true;
      if (a.len > b.len) return false;
      return a.def->id < b.def->id;
    });
    return reaching.front().def;
  }
  std::sort(cands.begin(), cands.end(), [](const SpanCand& a, const SpanCand& b) {
    if (a.factionMatch != b.factionMatch) return a.factionMatch;
    if (a.len > b.len) return true;
    if (a.len < b.len) return false;
    return a.def->id < b.def->id;
  });
  return cands.front().def;
}

}  // namespace x4sb
