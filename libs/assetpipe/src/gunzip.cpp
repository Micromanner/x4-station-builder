#include "x4sb/assetpipe/gunzip.hpp"

#include <sinfl.h>

#include <cstdint>

namespace x4sb {

std::optional<std::vector<char>> gunzip(const std::string& gzipped) {
  if (gzipped.size() < 18) return std::nullopt;
  const auto* ptr = reinterpret_cast<const std::uint8_t*>(gzipped.data());

  if (ptr[0] != 0x1f || ptr[1] != 0x8b || ptr[2] != 0x08) return std::nullopt;

  std::uint8_t flags = ptr[3];
  std::size_t offset = 10;

  if (flags & 0x04) {  // FEXTRA
    if (offset + 2 > gzipped.size()) return std::nullopt;
    std::uint16_t xlen = ptr[offset] | (ptr[offset + 1] << 8);
    offset += 2 + xlen;
  }
  if (flags & 0x08) {  // FNAME
    while (offset < gzipped.size() && ptr[offset] != 0) offset++;
    offset++;
  }
  if (flags & 0x10) {  // FCOMMENT
    while (offset < gzipped.size() && ptr[offset] != 0) offset++;
    offset++;
  }
  if (flags & 0x02) {  // FHCRC
    offset += 2;
  }

  if (offset + 8 > gzipped.size()) return std::nullopt;

  std::size_t footerOff = gzipped.size() - 8;
  std::uint32_t isize = ptr[footerOff + 4] | (ptr[footerOff + 5] << 8) |
                        (ptr[footerOff + 6] << 16) | (ptr[footerOff + 7] << 24);

  std::vector<char> out(isize);
  int res = sinflate(out.data(), static_cast<int>(isize), gzipped.data() + offset,
                     static_cast<int>(footerOff - offset));

  if (res < 0 || static_cast<std::uint32_t>(res) != isize) return std::nullopt;
  return out;
}

}  // namespace x4sb
