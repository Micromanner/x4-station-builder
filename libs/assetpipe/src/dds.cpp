#include "x4sb/assetpipe/dds.hpp"

#include <algorithm>

namespace x4sb {
namespace {

// D3D11's array limit; X4's multimat arrays use 33-64.
constexpr std::uint32_t kMaxLayers = 2048;

std::uint32_t u32(std::string_view bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (unsigned int i = 0; i < 4; ++i)
    value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + i])) << (i * 8);
  return value;
}

}  // namespace

std::optional<DdsLayout> parseDds(std::string_view bytes) {
  if (bytes.size() < 128 || bytes.substr(0, 4) != "DDS " || u32(bytes, 4) != 124 ||
      u32(bytes, 76) != 32)
    return std::nullopt;
  auto width = u32(bytes, 16);
  auto height = u32(bytes, 12);
  if (width == 0 || height == 0 || width > 16384 || height > 16384 || u32(bytes, 24) > 1 ||
      (u32(bytes, 112) & 0x20FE00U) != 0)
    return std::nullopt;
  DdsLayout result;
  std::size_t offset = 128;
  std::size_t blockBytes = 0;
  std::size_t pixelBytes = 0;
  const auto flags = u32(bytes, 80);
  if ((flags & 4U) != 0) {
    const auto cc = bytes.substr(84, 4);
    if (cc == "DXT1") {
      result.format = DdsFormat::Bc1;
    } else if (cc == "DXT3") {
      result.format = DdsFormat::Bc2;
    } else if (cc == "DXT5") {
      result.format = DdsFormat::Bc3;
    } else if (cc == "ATI1" || cc == "BC4U") {
      result.format = DdsFormat::Bc4;
    } else if (cc == "ATI2" || cc == "BC5U") {
      result.format = DdsFormat::Bc5;
    } else if (cc == "DX10") {
      if (bytes.size() < 148 || u32(bytes, 132) != 3 || (u32(bytes, 136) & 4U) != 0 ||
          u32(bytes, 140) == 0 || u32(bytes, 140) > kMaxLayers)
        return std::nullopt;
      offset = 148;
      result.layers = u32(bytes, 140);
      const auto format = u32(bytes, 128);
      result.srgb = format == 29 || format == 72 || format == 75 || format == 78 || format == 91 ||
                    format == 99;
      switch (format) {
        case 28:
        case 29:
          result.format = DdsFormat::Rgba8;
          break;
        case 61:
          result.format = DdsFormat::R8;
          break;
        case 71:
        case 72:
          result.format = DdsFormat::Bc1;
          break;
        case 74:
        case 75:
          result.format = DdsFormat::Bc2;
          break;
        case 77:
        case 78:
          result.format = DdsFormat::Bc3;
          break;
        case 80:
          result.format = DdsFormat::Bc4;
          break;
        case 83:
          result.format = DdsFormat::Bc5;
          break;
        case 87:
        case 91:
          result.format = DdsFormat::Bgra8;
          break;
        case 98:
        case 99:
          result.format = DdsFormat::Bc7;
          break;
        default:
          return std::nullopt;
      }
      result.opaque = (u32(bytes, 144) & 7U) == 3;
    } else {
      return std::nullopt;
    }
  } else if ((flags & 0x40U) != 0 && u32(bytes, 88) == 32 && u32(bytes, 96) == 0xFF00U &&
             (u32(bytes, 104) == 0xFF000000U || u32(bytes, 104) == 0)) {
    if (u32(bytes, 92) == 0xFF0000U && u32(bytes, 100) == 0xFFU)
      result.format = DdsFormat::Bgra8;
    else if (u32(bytes, 92) == 0xFFU && u32(bytes, 100) == 0xFF0000U)
      result.format = DdsFormat::Rgba8;
    else
      return std::nullopt;
    result.opaque = (flags & 1U) == 0 || u32(bytes, 104) == 0;
  } else if ((flags & 0x20000U) != 0 && u32(bytes, 88) == 8 && u32(bytes, 92) == 0xFFU &&
             u32(bytes, 96) == 0 && u32(bytes, 100) == 0 && u32(bytes, 104) == 0) {
    result.format = DdsFormat::R8;
  } else {
    return std::nullopt;
  }

  switch (result.format) {
    case DdsFormat::Bc1:
    case DdsFormat::Bc4:
      blockBytes = 8;
      break;
    case DdsFormat::Bc2:
    case DdsFormat::Bc3:
    case DdsFormat::Bc5:
    case DdsFormat::Bc7:
      blockBytes = 16;
      break;
    case DdsFormat::Rgba8:
    case DdsFormat::Bgra8:
      pixelBytes = 4;
      break;
    case DdsFormat::R8:
      pixelBytes = 1;
      break;
  }
  if (pixelBytes != 0 && (u32(bytes, 8) & 8U) != 0 &&
      u32(bytes, 20) != static_cast<std::size_t>(width) * pixelBytes)
    return std::nullopt;

  const auto mipCount = std::max(1U, u32(bytes, 28));
  std::uint32_t maxMips = 1;
  for (auto dimension = std::max(width, height); dimension > 1; dimension /= 2) ++maxMips;
  if (mipCount > maxMips) return std::nullopt;
  // Dimensions cap at 16384 and mips at 15, so one slice cannot overflow size_t; the slice
  // count is checked by division so a hostile arraySize cannot wrap the total.
  const auto dataStart = offset;
  for (std::uint32_t level = 0; level < mipCount; ++level) {
    const std::size_t size =
        blockBytes != 0
            ? static_cast<std::size_t>((width + 3) / 4) * ((height + 3) / 4) * blockBytes
            : static_cast<std::size_t>(width) * height * pixelBytes;
    result.mips.push_back({width, height, offset, size});
    offset += size;
    width = std::max(1U, width / 2);
    height = std::max(1U, height / 2);
  }
  result.sliceStride = offset - dataStart;
  if (result.layers > (bytes.size() - dataStart) / result.sliceStride) return std::nullopt;
  return result;
}

}  // namespace x4sb
