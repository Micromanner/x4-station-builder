#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace x4sb {

enum class DdsFormat : std::uint8_t { Bc1, Bc2, Bc3, Bc4, Bc5, Bc7, Rgba8, Bgra8, R8 };

struct DdsMip {
  std::uint32_t width{};
  std::uint32_t height{};
  std::size_t offset{};
  std::size_t size{};
};

// Offsets refer to the caller's buffer; no pixel data is copied or borrowed. `mips` describes
// slice 0; DX10 array slices are stored slice-major, so slice s of mip m starts at
// mips[m].offset + s * sliceStride.
struct DdsLayout {
  DdsFormat format{};
  bool srgb{};
  bool opaque{};
  std::uint32_t layers{1};
  std::size_t sliceStride{};
  std::vector<DdsMip> mips;
};

// Validates a 2D surface or 2D array and every declared mip of every slice before any GPU
// allocation. Cubes, volumes, signed BC formats and non-tight row pitches are rejected.
[[nodiscard]] std::optional<DdsLayout> parseDds(std::string_view bytes);

}  // namespace x4sb
