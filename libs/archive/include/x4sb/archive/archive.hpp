#pragma once
// In-house X4 .cat/.dat archive reader (spec §4A / §10.1). Render-free: pure
// std, no external tool (users never run Egosoft's XRCatTool). A .cat is a
// plaintext index of "<path> <size> <mtime> <md5>" lines; the paired .dat is the
// raw file bytes concatenated in index order, so an entry's byte offset is the
// cumulative sum of the sizes before it.
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace x4sb {

// One file entry from a .cat index, with its resolved byte offset into the .dat.
struct CatEntry {
  std::string path;  // logical path, '/'-separated, lower-cased by X4
  std::uint64_t size{0};
  std::uint64_t offset{0};  // byte offset into the paired .dat
};

// A parsed .cat index. Offsets are filled in during parse().
class CatIndex {
 public:
  // Parse .cat index text. Malformed lines (missing the trailing
  // "<size> <mtime> <md5>" fields) are skipped rather than throwing.
  static CatIndex parse(const std::string& catText);

  [[nodiscard]] const std::vector<CatEntry>& entries() const { return entries_; }
  [[nodiscard]] const CatEntry* find(const std::string& path) const;
  [[nodiscard]] std::uint64_t totalSize() const { return total_; }  // expected .dat size

 private:
  std::vector<CatEntry> entries_;
  std::uint64_t total_{0};
};

// An X4 asset archive: one or more .cat/.dat pairs resolved in load order.
// Catalogs added later override earlier ones for the same logical path, which is
// how base-game patches (01..NN) and DLC extensions layer on top of each other.
// `_sig` signature catalogs are ignored.
class Archive {
 public:
  // Register a single .cat file; its .dat is the same path with the extension
  // swapped. Returns false if the .cat cannot be read. `_sig` cats are skipped.
  // `logicalPrefix` is prepended to every indexed path — used for DLC catalogs,
  // whose entries are stored relative to the extension folder but are addressed
  // game-wide as "extensions/<dlc>/<path>" (matching component geometry refs).
  bool addCatalog(const std::string& catPath, const std::string& logicalPrefix = "");

  // Register a whole X4 install: base catalogs (numbered *.cat, ascending) then
  // each extensions/<ext>/*.cat (DLC load order). Returns the count added.
  int addInstall(const std::string& x4Dir);

  // Extract a logical path's bytes (latest registered catalog wins). Returns
  // nullopt if the path is unknown or the .dat read fails.
  std::optional<std::string> extract(const std::string& path) const;

  bool contains(const std::string& path) const { return index_.count(path) != 0; }
  std::size_t fileCount() const { return index_.size(); }

  // Returns the distinct logical-path prefixes indexed so far, in first-seen
  // order. `""` for the base game; `"extensions/<dlc>/"` for each DLC.
  [[nodiscard]] const std::vector<std::string>& sources() const { return sources_; }

 private:
  struct Source {
    std::string datPath;
    std::uint64_t offset{0};
    std::uint64_t size{0};
  };
  std::unordered_map<std::string, Source> index_;  // logical path -> .dat slice
  std::vector<std::string> sources_;               // distinct prefixes, load order
};

}  // namespace x4sb
