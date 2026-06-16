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

// Collapse runs of '/' into a single '/'. Some X4 source paths use doubled
// separators (a "\\" that becomes "//" after the '\\'->'/' pass); this normalizes
// them to match the single-slash archive entries. Takes by value so callers can
// move into it.
[[nodiscard]] inline std::string collapseSlashes(std::string s) {
  s.erase(std::unique(s.begin(), s.end(), [](char a, char b) { return a == '/' && b == '/'; }),
          s.end());
  return s;
}

}  // namespace detail
}  // namespace x4sb
