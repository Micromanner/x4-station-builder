#include "x4sb/archive/archive.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <utility>

namespace x4sb {
namespace fs = std::filesystem;

namespace {

// Split a .cat line into (path, size). The path may itself contain spaces, so we
// peel the three trailing fields (size, mtime, md5) off the right.
bool parseLine(const std::string& line, std::string& path, std::uint64_t& size) {
  const auto md5 = line.find_last_of(' ');
  if (md5 == std::string::npos) return false;
  const auto mtime = line.find_last_of(' ', md5 - 1);
  if (mtime == std::string::npos) return false;
  const auto sz = line.find_last_of(' ', mtime - 1);
  if (sz == std::string::npos) return false;

  const std::string sizeStr = line.substr(sz + 1, mtime - sz - 1);
  try {
    size = std::stoull(sizeStr);
  } catch (const std::exception&) {
    return false;
  }
  path = line.substr(0, sz);
  return !path.empty();
}

std::string swapExtension(const std::string& catPath, const char* ext) {
  fs::path p(catPath);
  p.replace_extension(ext);
  return p.string();
}

bool isSignatureCatalog(const fs::path& p) {
  const std::string stem = p.stem().string();
  return stem.size() >= 4 && stem.compare(stem.size() - 4, 4, "_sig") == 0;
}

std::string readSlice(const std::string& datPath, std::uint64_t offset, std::uint64_t size) {
  std::ifstream in(datPath, std::ios::binary);
  if (!in) return {};
  in.seekg(static_cast<std::streamoff>(offset));
  std::string buf(size, '\0');
  in.read(buf.data(), static_cast<std::streamsize>(size));
  if (static_cast<std::uint64_t>(in.gcount()) != size) return {};
  return buf;
}

}  // namespace

CatIndex CatIndex::parse(const std::string& catText) {
  CatIndex idx;
  std::uint64_t offset = 0;
  std::size_t pos = 0;
  while (pos < catText.size()) {
    std::size_t eol = catText.find('\n', pos);
    if (eol == std::string::npos) eol = catText.size();
    std::string line = catText.substr(pos, eol - pos);
    pos = eol + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;

    std::string path;
    std::uint64_t size = 0;
    if (!parseLine(line, path, size)) continue;
    idx.entries_.push_back({path, size, offset});
    offset += size;
  }
  idx.total_ = offset;
  return idx;
}

const CatEntry* CatIndex::find(const std::string& path) const {
  for (const auto& e : entries_)
    if (e.path == path) return &e;
  return nullptr;
}

bool Archive::addCatalog(const std::string& catPath, const std::string& logicalPrefix) {
  if (isSignatureCatalog(fs::path(catPath))) return false;

  std::ifstream in(catPath, std::ios::binary);
  if (!in) return false;
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  const std::string datPath = swapExtension(catPath, ".dat");
  const CatIndex idx = CatIndex::parse(text);
  for (const auto& e : idx.entries())
    index_[logicalPrefix + e.path] = Source{datPath, e.offset, e.size};  // later cats overwrite
  return true;
}

int Archive::addInstall(const std::string& x4Dir) {
  std::error_code ec;
  if (!fs::is_directory(x4Dir, ec)) return 0;

  // Base catalogs: numbered *.cat at the install root, ascending, sans _sig.
  std::vector<std::string> base;
  for (const auto& de : fs::directory_iterator(x4Dir, ec)) {
    const fs::path& p = de.path();
    if (p.extension() == ".cat" && !isSignatureCatalog(p)) base.push_back(p.string());
  }
  std::sort(base.begin(), base.end());

  // Extension (DLC) catalogs, grouped per extension dir (alphabetical), each
  // dir's cats ascending. Loaded after the base game so DLCs override. Their
  // entries are addressed game-wide under "extensions/<dlc>/", so that prefix is
  // applied to every path (DLC component XML geometry refs use the same prefix).
  std::vector<std::pair<std::string, std::string>> exts;  // (catPath, logicalPrefix)
  const fs::path extRoot = fs::path(x4Dir) / "extensions";
  if (fs::is_directory(extRoot, ec)) {
    std::vector<fs::path> dirs;
    for (const auto& de : fs::directory_iterator(extRoot, ec))
      if (de.is_directory(ec)) dirs.push_back(de.path());
    std::sort(dirs.begin(), dirs.end());
    for (const auto& d : dirs) {
      const std::string prefix = "extensions/" + d.filename().string() + "/";
      std::vector<std::string> here;
      for (const auto& de : fs::directory_iterator(d, ec)) {
        const fs::path& p = de.path();
        if (p.extension() == ".cat" && !isSignatureCatalog(p)) here.push_back(p.string());
      }
      std::sort(here.begin(), here.end());
      for (const auto& c : here) exts.emplace_back(c, prefix);
    }
  }

  int added = 0;
  for (const auto& c : base) added += addCatalog(c) ? 1 : 0;
  for (const auto& [cat, prefix] : exts) added += addCatalog(cat, prefix) ? 1 : 0;
  return added;
}

std::optional<std::string> Archive::extract(const std::string& path) const {
  const auto it = index_.find(path);
  if (it == index_.end()) return std::nullopt;
  std::string bytes = readSlice(it->second.datPath, it->second.offset, it->second.size);
  if (bytes.size() != it->second.size) return std::nullopt;
  return bytes;
}

}  // namespace x4sb
