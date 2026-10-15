#include "x4sb/assetpipe/dds.hpp"

#include <doctest/doctest.h>

#include <array>
#include <string>

namespace {

void put(std::string& bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned int i = 0; i < 4; ++i)
    bytes[offset + i] = static_cast<char>((value >> (8 * i)) & 255U);
}

std::string makeDds(std::string_view cc, std::uint32_t width, std::uint32_t height,
                    std::uint32_t mips, std::size_t payload) {
  std::string bytes(128 + payload, '\0');
  bytes.replace(0, 4, "DDS ");
  put(bytes, 4, 124);
  put(bytes, 12, height);
  put(bytes, 16, width);
  put(bytes, 28, mips);
  put(bytes, 76, 32);
  put(bytes, 80, 4);
  bytes.replace(84, 4, cc);
  return bytes;
}

}  // namespace

TEST_CASE("DDS native layout retains every rectangular BC mip including block tails") {
  const auto bytes = makeDds("DXT1", 8, 2, 4, 16 + 8 + 8 + 8);
  const auto layout = x4sb::parseDds(bytes);
  REQUIRE(layout);
  CHECK(layout->format == x4sb::DdsFormat::Bc1);
  REQUIRE(layout->mips.size() == 4);
  const std::array<std::size_t, 4> offsets{128, 144, 152, 160};
  for (std::size_t i = 0; i < offsets.size(); ++i) {
    CHECK(layout->mips[i].offset == offsets[i]);
    CHECK(layout->mips[i].width == (8U >> i));
    CHECK(layout->mips[i].height == (i == 0 ? 2 : 1));
  }
  for (std::size_t size = 0; size < bytes.size(); ++size)
    CHECK_FALSE(x4sb::parseDds(std::string_view(bytes).substr(0, size)));
}

TEST_CASE("DDS accepts unsigned legacy block formats without transcoding") {
  const std::array<std::pair<std::string_view, x4sb::DdsFormat>, 7> formats{
      {{"DXT1", x4sb::DdsFormat::Bc1},
       {"DXT3", x4sb::DdsFormat::Bc2},
       {"DXT5", x4sb::DdsFormat::Bc3},
       {"ATI1", x4sb::DdsFormat::Bc4},
       {"BC4U", x4sb::DdsFormat::Bc4},
       {"ATI2", x4sb::DdsFormat::Bc5},
       {"BC5U", x4sb::DdsFormat::Bc5}}};
  for (const auto& [cc, format] : formats) {
    const auto layout = x4sb::parseDds(makeDds(cc, 3, 5, 0, 32));
    REQUIRE(layout);
    CHECK(layout->format == format);
    CHECK(layout->mips.size() == 1);
    CHECK(layout->mips.front().size ==
          (format == x4sb::DdsFormat::Bc1 || format == x4sb::DdsFormat::Bc4 ? 16 : 32));
  }
}

TEST_CASE("DDS rejects unsupported shapes formats and corrupt headers") {
  const auto valid = makeDds("ATI2", 4, 4, 1, 16);
  const std::array<std::pair<std::size_t, std::uint32_t>, 9> invalid{{{4, 123},
                                                                      {76, 31},
                                                                      {16, 0},
                                                                      {12, 0},
                                                                      {16, 16385},
                                                                      {28, 4},
                                                                      {24, 2},
                                                                      {112, 0x200},
                                                                      {112, 0x200000}}};
  for (const auto& [offset, value] : invalid) {
    auto bytes = valid;
    put(bytes, offset, value);
    CHECK_FALSE(x4sb::parseDds(bytes));
  }
  CHECK_FALSE(x4sb::parseDds(makeDds("BC5S", 4, 4, 1, 16)));
  CHECK_FALSE(x4sb::parseDds(makeDds("junk", 4, 4, 1, 16)));
}

TEST_CASE("DDS DX10 validates dimensions and preserves sRGB metadata") {
  auto bytes = makeDds("DX10", 4, 4, 1, 20 + 16);
  put(bytes, 128, 83);
  put(bytes, 132, 3);
  put(bytes, 140, 1);
  auto layout = x4sb::parseDds(bytes);
  REQUIRE(layout);
  CHECK(layout->format == x4sb::DdsFormat::Bc5);
  CHECK(layout->mips.front().offset == 148);
  put(bytes, 128, 99);
  layout = x4sb::parseDds(bytes);
  REQUIRE(layout);
  CHECK(layout->format == x4sb::DdsFormat::Bc7);
  CHECK(layout->srgb);
  const std::array<std::pair<std::size_t, std::uint32_t>, 5> invalid{
      {{128, 84}, {132, 4}, {136, 4}, {140, 0}, {140, 2049}}};
  for (const auto& [offset, value] : invalid) {
    auto bad = bytes;
    put(bad, offset, value);
    CHECK_FALSE(x4sb::parseDds(bad));
  }
}

TEST_CASE("DDS uncompressed layout honors channel masks and checks row pitch") {
  auto bytes = makeDds("    ", 2, 1, 2, 12);
  put(bytes, 80, 0x41);
  put(bytes, 88, 32);
  put(bytes, 92, 0xFF0000);
  put(bytes, 96, 0xFF00);
  put(bytes, 100, 0xFF);
  put(bytes, 104, 0xFF000000);
  auto layout = x4sb::parseDds(bytes);
  REQUIRE(layout);
  CHECK(layout->format == x4sb::DdsFormat::Bgra8);
  CHECK_FALSE(layout->opaque);
  REQUIRE(layout->mips.size() == 2);
  CHECK(layout->mips[1].offset == 136);
  put(bytes, 92, 0xFF);
  put(bytes, 100, 0xFF0000);
  put(bytes, 104, 0);
  layout = x4sb::parseDds(bytes);
  REQUIRE(layout);
  CHECK(layout->format == x4sb::DdsFormat::Rgba8);
  CHECK(layout->opaque);
  put(bytes, 8, 8);
  put(bytes, 20, 12);
  CHECK_FALSE(x4sb::parseDds(bytes));
  put(bytes, 20, 8);
  REQUIRE(x4sb::parseDds(bytes));
  bytes.pop_back();
  CHECK_FALSE(x4sb::parseDds(bytes));
}

TEST_CASE("DDS DX10 arrays lay out every slice's mip chain slice-major") {
  // 8x4 BC1 with 3 mips: 2*1*8 + 1*1*8 + 1*1*8 = 32 bytes per slice.
  auto bytes = makeDds("DX10", 8, 4, 3, 20 + 3 * 32);
  put(bytes, 128, 71);
  put(bytes, 132, 3);
  put(bytes, 140, 3);
  const auto layout = x4sb::parseDds(bytes);
  REQUIRE(layout);
  CHECK(layout->layers == 3);
  CHECK(layout->sliceStride == 32);
  REQUIRE(layout->mips.size() == 3);
  const std::array<std::size_t, 3> offsets{148, 164, 172};
  for (std::size_t i = 0; i < offsets.size(); ++i) CHECK(layout->mips[i].offset == offsets[i]);
  for (std::size_t size = 0; size < bytes.size(); ++size)
    CHECK_FALSE(x4sb::parseDds(std::string_view(bytes).substr(0, size)));

  auto hostile = bytes;
  put(hostile, 140, 2048);
  CHECK_FALSE(x4sb::parseDds(hostile));
  put(hostile, 140, 3);
  put(hostile, 136, 4);
  CHECK_FALSE(x4sb::parseDds(hostile));
}

TEST_CASE("DDS non-array surfaces report one layer") {
  const auto layout = x4sb::parseDds(makeDds("DXT1", 4, 4, 1, 8));
  REQUIRE(layout);
  CHECK(layout->layers == 1);
  CHECK(layout->sliceStride == 8);
}
