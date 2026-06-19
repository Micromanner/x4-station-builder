#include "x4sb/assetpipe/localization.hpp"

#include <pugixml.hpp>

#include <cctype>
#include <charconv>
#include <cstddef>

namespace x4sb {
namespace {

constexpr int kMaxDepth = 8;                         // breaks reference cycles
constexpr std::uint64_t kPageStride = 1'000'000ULL;  // text ids are < 1e6 in X4

std::uint64_t makeKey(long page, long id) {
  return static_cast<std::uint64_t>(page) * kPageStride + static_cast<std::uint64_t>(id);
}

// A text id must stay below the page stride, else makeKey would alias it into
// the next page's keyspace. The single guard for the page*stride+id key scheme.
bool idInRange(long id) { return static_cast<std::uint64_t>(id) < kPageStride; }

// Parse a single non-negative integer that fills the whole view (no spaces/sign).
bool parseInt(std::string_view s, long& out) {
  // std::from_chars takes raw pointers; that's the entire API surface (C++17).
  const char* end = s.data() + s.size();  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto [ptr, ec] =
      std::from_chars(s.data(), end, out);  // NOLINT(bugprone-suspicious-stringview-data-usage)
  return ec == std::errc{} && ptr == end && out >= 0;
}

// Parse "{page,id}" into two non-negative, in-range ints. false on any other shape.
bool parseRef(std::string_view ref, long& page, long& id) {
  if (ref.size() < 2 || ref.front() != '{' || ref.back() != '}') return false;
  const std::string_view inner = ref.substr(1, ref.size() - 2);
  const std::size_t comma = inner.find(',');
  if (comma == std::string_view::npos) return false;
  if (!parseInt(inner.substr(0, comma), page) || !parseInt(inner.substr(comma + 1), id))
    return false;
  return idInRange(id);
}

// Collapse internal whitespace runs to one space and trim the ends.
std::string normalizeWhitespace(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool pendingSpace = false;
  for (const char c : s) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      pendingSpace = true;
      continue;
    }
    if (pendingSpace && !out.empty()) out.push_back(' ');
    pendingSpace = false;
    out.push_back(c);
  }
  return out;
}

}  // namespace

void TextDatabase::merge(std::string_view l044Xml) {
  pugi::xml_document doc;
  if (!doc.load_buffer(l044Xml.data(), l044Xml.size())) return;
  // Tolerate a missing <language> wrapper (fixtures / partial files).
  const pugi::xml_node lang = doc.child("language");
  const pugi::xml_node root = lang ? lang : doc.root();
  for (const pugi::xml_node page : root.children("page")) {
    long pageId = 0;
    if (!parseInt(page.attribute("id").value(), pageId)) continue;
    for (const pugi::xml_node t : page.children("t")) {
      long textId = 0;
      if (!parseInt(t.attribute("id").value(), textId)) continue;
      if (!idInRange(textId)) continue;  // out-of-range id would alias another page
      entries_[makeKey(pageId, textId)] = t.text().get();
    }
  }
}

std::optional<std::string> TextDatabase::resolveRef(std::string_view ref) const {
  long page = 0;
  long id = 0;
  if (!parseRef(ref, page, id)) return std::nullopt;
  return resolve(page, id, 0);
}

std::optional<std::string> TextDatabase::resolve(long page, long id, int depth) const {
  if (depth > kMaxDepth) return std::nullopt;
  const auto it = entries_.find(makeKey(page, id));
  if (it == entries_.end()) return std::nullopt;
  const std::string& raw = it->second;

  std::string out;
  out.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size();) {
    const char c = raw[i];
    if (c == '\\' && i + 1 < raw.size() && (raw[i + 1] == '(' || raw[i + 1] == ')')) {
      out.push_back(raw[i + 1]);  // escaped paren is literal text, kept
      i += 2;
      continue;
    }
    if (c == '(') {  // unescaped translator comment: skip to the next unescaped ')'
      ++i;
      while (i < raw.size()) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
          i += 2;
          continue;
        }
        const bool close = raw[i] == ')';
        ++i;
        if (close) break;
      }
      continue;
    }
    if (c == '{') {  // nested reference -> resolve recursively (unresolved -> "")
      const std::size_t end = raw.find('}', i);
      if (end == std::string::npos) {
        out.push_back(c);  // stray brace: treat as literal
        ++i;
        continue;
      }
      const std::string_view inner = std::string_view(raw).substr(i, end - i + 1);
      long p = 0;
      long t = 0;
      if (parseRef(inner, p, t)) {
        if (const std::optional<std::string> sub = resolve(p, t, depth + 1)) out += *sub;
      }
      i = end + 1;
      continue;
    }
    out.push_back(c);
    ++i;
  }

  std::string result = normalizeWhitespace(out);
  if (result.empty()) return std::nullopt;
  return result;
}

}  // namespace x4sb
