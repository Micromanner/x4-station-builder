#include "x4sb/assetpipe/metadata_cache.hpp"
#include "x4sb/autolayout/autolayout.hpp"
#include "x4sb/autolayout/report.hpp"
#include "x4sb/data/catalog.hpp"
#include "x4sb/document/station.hpp"
#include "x4sb/planio/plan.hpp"
#include "x4sb/platform/platform.hpp"

#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace x4sb;

// The benchmark cart (user-specified): a realistic mid-size station that stresses connector
// synthesis — 50 single-port hull-part modules plus peripherals (piers, dock, defense,
// energy cells). The interior (hull parts) all need hub ports, so a good layout must
// synthesize and pack many hubs without chaining or splitting into disconnected lanes.
QuantityList benchmarkCart() {
  return QuantityList{
      {"pier_arg_harbor_01_macro", 2},         // two Argon 3-Dock T piers
      {"defence_arg_disc_01_macro", 2},        // two Argon disc defense platforms
      {"dockarea_arg_m_station_01_macro", 1},  // one 1M6S (S/M) dock area
      {"prod_gen_energycells_macro", 2},       // two energy-cell production modules
      {"prod_gen_hullparts_macro", 50},        // fifty hull-part production modules
  };
}

// Parse "id:count,id:count" into a QuantityList. Bad tokens are skipped with a warning.
QuantityList parseCart(const std::string& spec) {
  QuantityList cart;
  std::size_t i = 0;
  while (i < spec.size()) {
    const std::size_t comma = spec.find(',', i);
    const std::string tok =
        spec.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
    const std::size_t colon = tok.find(':');
    if (colon != std::string::npos) {
      const std::string id = tok.substr(0, colon);
      try {
        const int n = std::stoi(tok.substr(colon + 1));
        if (!id.empty() && n > 0) cart[id] += n;
      } catch (const std::exception&) {
        std::cerr << "warning: bad cart token '" << tok << "'\n";
      }
    }
    if (comma == std::string::npos) break;
    i = comma + 1;
  }
  return cart;
}

void printUsage(const char* exe) {
  std::cout << "x4sb-layoutlab — auto-layout topology diagnostic\n\n"
            << "Usage:\n  " << exe
            << " [--x4 <X4 install>] [--cart id:n,id:n | --benchmark | --import-plan "
               "<plan.xml> [--export-plan <out.xml>]]"
            << " [--no-modules] [--iters N] [--seed N] [--mode fishbone|organic]\n\n"
            << "Defaults: --x4 = the install saved by the editor, --benchmark cart.\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  std::string gameDirectory;
  std::string cartSpec;
  std::string importPlanPath;
  std::string exportPlanPath;
  bool perModule = true;
  AutoLayoutOptions opts;

  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--x4" && i + 1 < args.size()) {
      gameDirectory = args[++i];
    } else if (args[i] == "--cart" && i + 1 < args.size()) {
      cartSpec = args[++i];
    } else if (args[i] == "--benchmark") {
      cartSpec.clear();
    } else if (args[i] == "--import-plan" && i + 1 < args.size()) {
      importPlanPath = args[++i];
    } else if (args[i] == "--export-plan" && i + 1 < args.size()) {
      exportPlanPath = args[++i];
    } else if (args[i] == "--no-modules") {
      perModule = false;
    } else if (args[i] == "--iters" && i + 1 < args.size()) {
      opts.maxIters = std::stoi(args[++i]);
    } else if (args[i] == "--seed" && i + 1 < args.size()) {
      opts.seed = static_cast<std::uint32_t>(std::stoul(args[++i]));
    } else if (args[i] == "--mode" && i + 1 < args.size()) {
      const std::string m = args[++i];
      opts.mode = (m == "organic") ? LayoutMode::Organic : LayoutMode::Fishbone;
    } else if (args[i] == "-h" || args[i] == "--help") {
      printUsage(argv[0]);
      return 0;
    }
  }

  if (gameDirectory.empty())
    gameDirectory = platform::loadGameDirectory(platform::settingsFilePath());
  const auto cacheDirectory = platform::appCacheDir();
  const auto install =
      gameDirectory.empty()
          ? std::nullopt
          : loadInstallMetadata(gameDirectory, cacheDirectory.empty()
                                                   ? cacheDirectory
                                                   : cacheDirectory / "catalog.json");
  if (!install) {
    std::cerr << "error: could not load X4 game data (pass --x4 <install>)\n";
    return 1;
  }
  const ModuleCatalog* catalog = &install->metadata.catalog;

  QuantityList cart;
  if (!importPlanPath.empty()) {
    std::ifstream f(importPlanPath);
    if (!f.is_open()) {
      std::cerr << "error: cannot open plan file " << importPlanPath << "\n";
      return 1;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::optional<Station> planStation = importPlanXml(ss.str());
    if (!planStation) {
      std::cerr << "error: failed to parse plan xml " << importPlanPath << "\n";
      return 1;
    }
    for (const auto& m : planStation->modules()) {
      const ModuleDef* d = catalog->find(m.defId);
      if (d != nullptr && d->category != Category::Connector) {
        cart[m.defId]++;
      }
    }
  } else {
    cart = cartSpec.empty() ? benchmarkCart() : parseCart(cartSpec);
  }

  std::cout << "catalog: " << gameDirectory << " (" << catalog->size() << " modules)\n";
  std::cout << "cart (" << cart.size() << " distinct):\n";
  for (const auto& [id, count] : cart) {
    const ModuleDef* d = catalog->find(id);
    std::cout << "  " << count << "x  " << id << "  "
              << (d != nullptr ? "[" + std::string(displayName(*d)) + "]" : "[UNKNOWN]") << "\n";
  }
  std::cout << "\n";

  const Station empty;  // greenfield
  const AutoLayoutResult r = autoLayout(empty, cart, *catalog, opts);
  const char* modeLabel = opts.mode == LayoutMode::Organic ? "ORGANIC" : "FISHBONE";
  std::cout << layoutReport(r, *catalog, modeLabel, perModule) << "\n";

  if (!r.report.skippedDefs.empty()) {
    std::cout << "SKIPPED DEFINITIONS:\n";
    for (const auto& s : r.report.skippedDefs) {
      std::cout << "  " << s << "\n";
    }
    std::cout << "\n";
  }

  if (!exportPlanPath.empty()) {
    if (!writePlanFile(r.station, *catalog, exportPlanPath)) {
      std::cerr << "error: failed to write plan to " << exportPlanPath << "\n";
      return 1;
    }
    std::cout << "Exported layout plan to " << exportPlanPath << "\n";
  }

  return 0;
}
