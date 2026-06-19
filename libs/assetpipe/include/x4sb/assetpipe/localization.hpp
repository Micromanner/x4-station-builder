#pragma once
// Parse and resolve X4 localization text (t/0001-l044.xml) so a module name ref
// like "{20104,51401}" becomes "Argon Cross Connection Structure" (issue 3.4).
// X4 text entries carry strip-at-runtime "(...)" translator comments, escaped
// "\(" parens, and nested "{page,id}" references; resolveRef reproduces the
// in-game string faithfully. Untrusted input: bounds-checked + depth-guarded.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace x4sb {

class TextDatabase {
 public:
  // Overlay one X4 <language> file's entries (later merges win, matching DLC
  // overlays). Malformed XML is a safe no-op.
  void merge(std::string_view l044Xml);

  // Resolve "{page,id}" to its faithful display string; nullopt if the ref is
  // malformed, the entry is missing, or it resolves to empty.
  [[nodiscard]] std::optional<std::string> resolveRef(std::string_view ref) const;

  [[nodiscard]] std::size_t size() const { return entries_.size(); }

 private:
  [[nodiscard]] std::optional<std::string> resolve(long page, long id, int depth) const;

  std::unordered_map<std::uint64_t, std::string> entries_;
};

}  // namespace x4sb
