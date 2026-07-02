#include "x4sb/autolayout/anneal.hpp"

#include "x4sb/autolayout/autolayout.hpp"  // AutoLayoutOptions (full definition)

#include <cmath>
#include <optional>
#include <random>
#include <utility>

namespace x4sb {

Station anneal(Station seed, const ModuleCatalog& catalog, const Objective& objective,
               const std::vector<Move>& moves, const AutoLayoutOptions& opts) {
  Station current = std::move(seed);
  Station best = current;
  if (moves.empty() || opts.maxIters <= 0 || !objective) return best;

  double currentCost = objective(current, catalog);
  double bestCost = currentCost;

  std::mt19937 rng(opts.seed);
  std::uniform_int_distribution<std::size_t> pickMove(0, moves.size() - 1);
  std::uniform_real_distribution<double> unit(0.0, 1.0);

  // Geometric cooling startTemp -> endTemp over maxIters (T_i = start * (end/start)^(i/N)).
  const double ratio = opts.endTemp / opts.startTemp;
  const double iters = static_cast<double>(opts.maxIters);

  for (int i = 0; i < opts.maxIters; ++i) {
    const double temp = opts.startTemp * std::pow(ratio, static_cast<double>(i) / iters);
    const Move& move = moves[pickMove(rng)];
    if (!move) continue;
    std::optional<Station> candidate = move(current, catalog, rng);
    if (!candidate || !layoutLegal(*candidate, catalog, opts)) continue;

    const double candidateCost = objective(*candidate, catalog);
    const double delta = candidateCost - currentCost;
    // Accept all improvements; accept worsenings with probability exp(-delta/temp).
    if (delta < 0.0 || unit(rng) < std::exp(-delta / temp)) {
      current = std::move(*candidate);
      currentCost = candidateCost;
      if (currentCost < bestCost) {
        best = current;
        bestCost = currentCost;
      }
    }
  }
  return best;
}

}  // namespace x4sb
