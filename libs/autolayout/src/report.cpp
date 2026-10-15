#include "x4sb/autolayout/report.hpp"

#include "x4sb/autolayout/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <queue>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace x4sb {
namespace {

const char* categoryTag(Category c) {
  switch (c) {
    case Category::Production:
      return "PROD";
    case Category::Storage:
      return "STOR";
    case Category::Habitat:
      return "HAB ";
    case Category::Dock:
      return "DOCK";
    case Category::Defense:
      return "DEF ";
    case Category::Connector:
      return "CONN";
    case Category::Other:
      return "OTHR";
  }
  return "????";
}

bool isConnector(const ModuleCatalog& catalog, const std::string& defId) {
  const ModuleDef* d = catalog.find(defId);
  return d != nullptr && d->category == Category::Connector;
}

// Strip the catalog's "_macro" suffix so the table reads at a glance.
std::string shortDef(const std::string& id) {
  const std::string suffix = "_macro";
  if (id.size() > suffix.size() &&
      id.compare(id.size() - suffix.size(), suffix.size(), suffix) == 0)
    return id.substr(0, id.size() - suffix.size());
  return id;
}

std::string num(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.0f", v);
  return buf;
}

std::string vec(const Vec3& v) { return "(" + num(v.x) + "," + num(v.y) + "," + num(v.z) + ")"; }

// Disjoint-set over instanceId-indexed nodes (ids are dense small integers in practice,
// but we index by position so arbitrary ids work).
struct DisjointSet {
  std::vector<std::size_t> parent;
  explicit DisjointSet(std::size_t n) : parent(n) {
    for (std::size_t i = 0; i < n; ++i) parent[i] = i;
  }
  std::size_t find(std::size_t x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  }
  void unite(std::size_t a, std::size_t b) { parent[find(a)] = find(b); }
};

}  // namespace

std::string layoutReport(const AutoLayoutResult& result, const ModuleCatalog& catalog,
                         const std::string& label, bool perModule) {
  const Station& s = result.station;
  const auto& mods = s.modules();
  const std::size_t n = mods.size();

  // instanceId -> dense index, plus the ordered id list (ascending, as the editor replays).
  std::unordered_map<InstanceId, std::size_t> idx;
  idx.reserve(n);
  for (std::size_t i = 0; i < n; ++i) idx[mods[i].instanceId] = i;

  std::ostringstream o;
  const AutoLayoutReport& rep = result.report;

  o << "================ " << label << " ================\n";
  o << "requested=" << rep.requested << " placed=" << (rep.snapped + rep.floating)
    << " (snapped=" << rep.snapped << " floating=" << rep.floating << ") skipped=" << rep.skipped
    << "  connectorsAdded=" << rep.connectorsAdded << "\n";

  std::size_t connCount = 0, funcCount = 0;
  for (const auto& m : mods) (isConnector(catalog, m.defId) ? connCount : funcCount)++;
  o << "modules=" << n << " (connector=" << connCount << " functional=" << funcCount << ")\n";

  if (n == 0) {
    o << "(empty station)\n";
    return o.str();
  }

  // --- Components (union-find over links) + roots (no lower-id parent). ---
  DisjointSet ds(n);
  std::size_t ffLinks = 0;  // functional<->functional undirected edges
  std::vector<std::size_t> degree(n, 0);
  for (std::size_t i = 0; i < n; ++i) {
    degree[i] = mods[i].links.size();
    const bool iConn = isConnector(catalog, mods[i].defId);
    for (const Link& l : mods[i].links) {
      const auto it = idx.find(l.otherInstanceId);
      if (it == idx.end()) continue;  // dangling link (shouldn't happen); skip
      ds.unite(i, it->second);
      // Count each undirected edge once (lower index counts it) and only func<->func.
      if (it->second > i && !iConn && !isConnector(catalog, mods[it->second].defId)) ++ffLinks;
    }
  }

  // Component sizes + the lowest-id member as the component's representative root.
  std::unordered_map<std::size_t, std::size_t> compSize;
  std::unordered_map<std::size_t, InstanceId> compRoot;  // root = lowest instanceId in component
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t r = ds.find(i);
    ++compSize[r];
    const auto it = compRoot.find(r);
    if (it == compRoot.end() || mods[i].instanceId < it->second) compRoot[r] = mods[i].instanceId;
  }
  // Roots as the editor sees them: modules with no link to a lower instanceId (each becomes a
  // floating PlaceModuleCommand). Mismatch vs component count signals cycles.
  std::size_t emitRoots = 0;
  for (std::size_t i = 0; i < n; ++i) {
    bool hasLowerParent = false;
    for (const Link& l : mods[i].links)
      if (l.otherInstanceId < mods[i].instanceId) {
        hasLowerParent = true;
        break;
      }
    if (!hasLowerParent) ++emitRoots;
  }

  std::vector<std::pair<std::size_t, InstanceId>> comps;  // (size, rootInstanceId)
  comps.reserve(compSize.size());
  for (const auto& [rep2, sz] : compSize) comps.push_back({sz, compRoot[rep2]});
  std::sort(comps.begin(), comps.end(), [](const auto& a, const auto& b) {
    if (a.first != b.first) return a.first > b.first;  // largest first
    return a.second < b.second;
  });

  o << "\nTOPOLOGY\n";
  o << "  components=" << comps.size() << "  (a valid X4 plan is 1)   emitRoots=" << emitRoots
    << "\n";
  for (std::size_t i = 0; i < comps.size(); ++i) {
    const ModuleDef* rd = catalog.find(s.find(comps[i].second)->defId);
    o << "    #" << i << " size=" << comps[i].first << " root=inst" << comps[i].second << "["
      << shortDef(rd != nullptr ? rd->id : "?") << "]"
      << (comps[i].first == 1 ? "  <ISOLATED>" : "") << "\n";
  }

  o << "  functional-functional links=" << ffLinks
    << "  (should be ~0; >0 means modules chain without a hub)\n";

  // Degree histogram.
  std::map<std::size_t, std::size_t> degHist;
  for (const std::size_t d : degree) ++degHist[d];
  o << "  degree:";
  for (const auto& [deg, cnt] : degHist) o << " d" << deg << "=" << cnt;
  o << "\n";

  // --- Longest functional chain: diameter of the func<->func subgraph (double BFS). ---
  std::vector<std::vector<std::size_t>> ffAdj(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (isConnector(catalog, mods[i].defId)) continue;
    for (const Link& l : mods[i].links) {
      const auto it = idx.find(l.otherInstanceId);
      if (it == idx.end()) continue;
      if (!isConnector(catalog, mods[it->second].defId)) ffAdj[i].push_back(it->second);
    }
  }
  const auto bfsFarthest = [&](std::size_t src) {
    std::vector<int> dist(n, -1);
    dist[src] = 0;
    std::queue<std::size_t> q;
    q.push(src);
    std::size_t best = src;
    while (!q.empty()) {
      const std::size_t u = q.front();
      q.pop();
      for (const std::size_t v : ffAdj[u])
        if (dist[v] < 0) {
          dist[v] = dist[u] + 1;
          if (dist[v] > dist[best]) best = v;
          q.push(v);
        }
    }
    return std::pair<std::size_t, int>{best, dist[best]};
  };
  int longestChainEdges = 0;
  std::vector<char> seen(n, 0);
  for (std::size_t i = 0; i < n; ++i) {
    if (seen[i] || isConnector(catalog, mods[i].defId)) continue;
    const auto a = bfsFarthest(i);
    const auto b = bfsFarthest(a.first);
    longestChainEdges = std::max(longestChainEdges, b.second);
    // Mark this functional component seen.
    std::queue<std::size_t> q;
    q.push(i);
    seen[i] = 1;
    while (!q.empty()) {
      const std::size_t u = q.front();
      q.pop();
      for (const std::size_t v : ffAdj[u])
        if (!seen[v]) {
          seen[v] = 1;
          q.push(v);
        }
    }
  }
  o << "  longest functional chain=" << (longestChainEdges + 1) << " modules"
    << (longestChainEdges >= 2 ? "  <chained, not hubbed>" : "") << "\n";

  // --- Hubs: per-connector port utilization + functional fan-out. ---
  // Hubs are >=3-port connectors (junctions modules hang off). 2-port connectors are straight
  // beams/spacers in the spine — counted separately so they don't dilute the hub fan-out.
  o << "\nHUBS (>=3-port connectors)\n";
  std::size_t hubFanSum = 0, hubFanMin = 0, hubFanMax = 0, hubs = 0, spacers = 0;
  bool first = true;
  for (const auto& m : mods) {
    const ModuleDef* d = catalog.find(m.defId);
    if (d == nullptr || d->category != Category::Connector) continue;
    const std::size_t ports = d->connectionPoints.size();
    if (ports < 3) {
      ++spacers;  // a straight beam / spacer, not a junction
      continue;
    }
    const std::size_t used = m.links.size();
    std::size_t funcChildren = 0;
    for (const Link& l : m.links) {
      const auto it = idx.find(l.otherInstanceId);
      if (it != idx.end() && !isConnector(catalog, mods[it->second].defId)) ++funcChildren;
    }
    ++hubs;
    hubFanSum += funcChildren;
    if (first) {
      hubFanMin = hubFanMax = funcChildren;
      first = false;
    } else {
      hubFanMin = std::min(hubFanMin, funcChildren);
      hubFanMax = std::max(hubFanMax, funcChildren);
    }
    o << "  inst" << m.instanceId << "  " << shortDef(d->id) << "  " << used << "/" << ports
      << " ports  funcChildren=" << funcChildren << "\n";
  }
  o << "  spacer beams (2-port)=" << spacers << "\n";
  if (hubs == 0) {
    o << "  (no hubs)\n";
  } else {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f",
                  static_cast<double>(hubFanSum) / static_cast<double>(hubs));
    o << "  fan-out (functional modules per hub): min=" << hubFanMin << " max=" << hubFanMax
      << " mean=" << buf << "\n";
  }

  // --- Geometry + legality. ---
  const AABB b = bounds(s, catalog);
  o << "\nGEOMETRY\n";
  o << "  bounds " << vec(b.min) << " .. " << vec(b.max) << "  size=" << vec(b.max - b.min) << "\n";
  o << "  radiusOfGyration=" << num(std::sqrt(radiusOfGyrationSq(s, catalog))) << "\n";
  // Non-adjacent body overlaps (directly-linked neighbours touch at the joint by design, so
  // they are excluded). 0 = a legal layout; >0 means modules interpenetrate.
  std::size_t overlapPairs = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const ModuleDef* di = catalog.find(mods[i].defId);
    if (di == nullptr) continue;
    const AABB ai = worldAabb(di->aabb, mods[i].worldTransform);
    for (std::size_t j = i + 1; j < n; ++j) {
      const bool linked =
          std::any_of(mods[i].links.begin(), mods[i].links.end(),
                      [&](const Link& l) { return l.otherInstanceId == mods[j].instanceId; });
      if (linked) continue;
      const ModuleDef* dj = catalog.find(mods[j].defId);
      if (dj == nullptr) continue;
      if (overlaps(ai, worldAabb(dj->aabb, mods[j].worldTransform))) {
        ++overlapPairs;
        const AABB aj = worldAabb(dj->aabb, mods[j].worldTransform);
        std::printf(
            "[DEBUG Overlap] inst%d (%s) AABB min=(%f,%f,%f) max=(%f,%f,%f) overlaps inst%d (%s) "
            "AABB min=(%f,%f,%f) max=(%f,%f,%f)\n",
            static_cast<int>(mods[i].instanceId), mods[i].defId.c_str(), ai.min.x, ai.min.y,
            ai.min.z, ai.max.x, ai.max.y, ai.max.z, static_cast<int>(mods[j].instanceId),
            mods[j].defId.c_str(), aj.min.x, aj.min.y, aj.min.z, aj.max.x, aj.max.y, aj.max.z);
      }
    }
  }
  o << "  non-adjacent overlaps=" << overlapPairs
    << (overlapPairs == 0 ? "  (legal)" : "  <ILLEGAL>") << "\n";

  if (perModule) {
    o << "\nMODULES (inst | cat | deg | parent | def | pos)\n";
    std::vector<const PlacedModule*> ordered;
    ordered.reserve(n);
    for (const auto& m : mods) ordered.push_back(&m);
    std::sort(ordered.begin(), ordered.end(), [](const PlacedModule* a, const PlacedModule* c) {
      return a->instanceId < c->instanceId;
    });
    for (const PlacedModule* m : ordered) {
      const ModuleDef* d = catalog.find(m->defId);
      InstanceId parent = 0;
      for (const Link& l : m->links)
        if (l.otherInstanceId < m->instanceId && (parent == 0 || l.otherInstanceId < parent))
          parent = l.otherInstanceId;
      char line[256];
      std::snprintf(line, sizeof(line), "  %4llu | %s | %2zu | %-8s | %-34s | %s\n",
                    static_cast<unsigned long long>(m->instanceId),
                    d != nullptr ? categoryTag(d->category) : "????", m->links.size(),
                    parent != 0 ? ("p" + std::to_string(parent)).c_str() : "root",
                    shortDef(m->defId).c_str(), vec(m->worldTransform.position).c_str());
      o << line;
    }
  }
  return o.str();
}

}  // namespace x4sb
