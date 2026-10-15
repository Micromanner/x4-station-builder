#include "x4sb/archive/archive.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace x4sb {
namespace fs = std::filesystem;

// One immutable handle per .dat, shared by its entries and Archive copies.
// All reads supply their own offset; no shared seek cursor or archive-wide lock.
class Archive::File {
 public:
  explicit File(const std::string& path) {
#ifdef _WIN32
    handle_ = CreateFileW(fs::path(path).c_str(), GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
#else
    handle_ = open(path.c_str(), O_RDONLY | O_CLOEXEC);
#endif
  }
  ~File() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
    if (handle_ >= 0) close(handle_);
#endif
  }
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  File(File&&) = delete;
  File& operator=(File&&) = delete;

  [[nodiscard]] std::optional<std::string> read(std::uint64_t offset, std::uint64_t size) const {
    std::uint64_t fileSize = 0;
#ifdef _WIN32
    LARGE_INTEGER length{};
    if (handle_ == INVALID_HANDLE_VALUE || !GetFileSizeEx(handle_, &length) || length.QuadPart < 0)
      return std::nullopt;
    fileSize = static_cast<std::uint64_t>(length.QuadPart);
#else
    struct stat info{};
    if (handle_ < 0 || fstat(handle_, &info) != 0 || info.st_size < 0) return std::nullopt;
    fileSize = static_cast<std::uint64_t>(info.st_size);
#endif
    if (offset > fileSize || size > fileSize - offset || size > std::string{}.max_size())
      return std::nullopt;
    std::string bytes(static_cast<std::size_t>(size), '\0');
    std::size_t done = 0;
#ifdef _WIN32
    struct Event {
      HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      ~Event() {
        if (handle != nullptr) CloseHandle(handle);
      }
      Event() = default;
      Event(const Event&) = delete;
      Event& operator=(const Event&) = delete;
      Event(Event&&) = delete;
      Event& operator=(Event&&) = delete;
    };
    const Event event;
    if (event.handle == nullptr) return std::nullopt;
#endif
    while (done < bytes.size()) {
      const auto chunk = std::min(bytes.size() - done, std::size_t{1} << 30);
#ifdef _WIN32
      OVERLAPPED request{};
      const auto position = offset + done;
      // OVERLAPPED exposes its documented file offset through a Windows union.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
      request.Offset = static_cast<DWORD>(position & 0xFFFFFFFFU);
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
      request.OffsetHigh = static_cast<DWORD>(position >> 32);
      request.hEvent = event.handle;
      DWORD received = 0;
      if (!ReadFile(handle_, bytes.data() + done, static_cast<DWORD>(chunk), &received, &request)) {
        if (GetLastError() != ERROR_IO_PENDING ||
            !GetOverlappedResult(handle_, &request, &received, TRUE))
          return std::nullopt;
      }
      if (received == 0) return std::nullopt;
      done += received;
#else
      const auto received =
          pread(handle_, bytes.data() + done, chunk, static_cast<off_t>(offset + done));
      if (received < 0 && errno == EINTR) continue;
      if (received <= 0) return std::nullopt;
      done += static_cast<std::size_t>(received);
#endif
    }
    return bytes;
  }

 private:
#ifdef _WIN32
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int handle_ = -1;
#endif
};

namespace {

// Split a .cat line into (path, size). The path may itself contain spaces, so we
// peel the three trailing fields (size, mtime, md5) off the right.
bool parseLine(const std::string& line, std::string& path, std::uint64_t& size) {
  const auto md5 = line.find_last_of(' ');
  if (md5 == std::string::npos || md5 == 0) return false;
  const auto mtime = line.find_last_of(' ', md5 - 1);
  if (mtime == std::string::npos || mtime == 0) return false;
  const auto sz = line.find_last_of(' ', mtime - 1);
  if (sz == std::string::npos) return false;

  const std::string sizeStr = line.substr(sz + 1, mtime - sz - 1);
  const auto parsed = std::from_chars(sizeStr.data(), sizeStr.data() + sizeStr.size(), size);
  if (parsed.ec != std::errc{} || parsed.ptr != sizeStr.data() + sizeStr.size()) return false;
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

// X4's VFS is case-insensitive: the .cat ships paths lower-cased, but a query
// derived from a component XML may be camelCase (e.g. "anim_cableHolder"). Normalize
// both sides to lower-case (ASCII; X4 paths are ASCII) so they match.
std::string toLowerAscii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return s;
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
    if (size > std::numeric_limits<std::uint64_t>::max() - offset) return {};
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
  // Read the whole catalog via rdbuf() rather than an istreambuf_iterator pair:
  // GCC 13 under -O3 mis-fires -Werror=null-dereference on the iterator idiom
  // (a known false positive); streaming the buffer reads identically and clean.
  std::ostringstream contents;
  contents << in.rdbuf();
  const std::string text = contents.str();

  const std::string datPath = swapExtension(catPath, ".dat");
  const auto file = std::make_shared<File>(datPath);
  const CatIndex idx = CatIndex::parse(text);
  catalogPaths_.push_back(fs::absolute(catPath).lexically_normal().string());
  for (const auto& e : idx.entries())
    // Lower-cased key so extract()'s case-insensitive lookup matches (later cats overwrite).
    index_[toLowerAscii(logicalPrefix + e.path)] = Source{file, e.offset, e.size};

  // Record this prefix once (dedup: linear scan; there are only a handful).
  if (std::find(sources_.begin(), sources_.end(), logicalPrefix) == sources_.end())
    sources_.push_back(logicalPrefix);
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
  const auto it = index_.find(toLowerAscii(path));
  if (it == index_.end()) return std::nullopt;
  return it->second.file->read(it->second.offset, it->second.size);
}

bool Archive::contains(const std::string& path) const {
  return index_.count(toLowerAscii(path)) != 0;
}

}  // namespace x4sb
