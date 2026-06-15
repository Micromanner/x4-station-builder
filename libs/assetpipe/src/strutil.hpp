#pragma once
// Internal string helpers shared across the assetpipe parsers. Header-only
// (inline) — these are implementation details, not part of the public API.
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>

namespace x4sb {
namespace detail {

// Lower-case a string (ASCII). Takes by value so callers can move into it.
[[nodiscard]] inline std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return s;
}

// Whole-word match: true if `token` appears as a whitespace-delimited token in
// `tokens` (so "module" matches "a module b" but not "submodule").
[[nodiscard]] inline bool hasToken(const std::string& tokens, const char* token) {
  std::istringstream in(tokens);
  std::string tok;
  while (in >> tok)
    if (tok == token) return true;
  return false;
}

}  // namespace detail
}  // namespace x4sb
