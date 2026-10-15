#include "dds_texture.hpp"
#include "external/glad.h"
#include "raylib.h"
#include "texture_mips.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

void put(std::string& bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned int i = 0; i < 4; ++i)
    bytes[offset + i] = static_cast<char>((value >> (8 * i)) & 255U);
}

bool verify(std::string_view cc, std::size_t blockSize, GLint expectedFormat) {
  // 8x2 -> 4x1 -> 2x1 -> 1x1 catches raylib's historical rectangular-tail bug.
  std::string bytes(128 + 5 * blockSize, '\0');
  bytes.replace(0, 4, "DDS ");
  bytes.replace(84, 4, cc);
  put(bytes, 4, 124);
  put(bytes, 12, 2);
  put(bytes, 16, 8);
  put(bytes, 28, 4);
  put(bytes, 76, 32);
  put(bytes, 80, 4);
  if (cc == "ATI2") {
    for (std::size_t offset = 128; offset < bytes.size(); offset += 16) {
      bytes[offset] = static_cast<char>(128);
      bytes[offset + 8] = static_cast<char>(64);
    }
  }
  GLuint previous = 0;
  glGenTextures(1, &previous);
  glBindTexture(GL_TEXTURE_2D, previous);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 17);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, 2);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, 3);
  const auto texture = x4sb::editor::loadDdsFromMemory(bytes);
  GLint binding = 0;
  GLint alignment = 0;
  GLint rowLength = 0;
  GLint skipRows = 0;
  GLint skipPixels = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
  glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
  glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
  glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPixels);
  bool ok = texture.id != 0 && texture.mipmaps == 4 && binding == static_cast<GLint>(previous) &&
            alignment == 8 && rowLength == 17 && skipRows == 2 && skipPixels == 3;
  if (texture.id != 0) {
    glBindTexture(GL_TEXTURE_2D, texture.id);
    for (GLint level = 0; level < 4; ++level) {
      GLint width = 0;
      GLint height = 0;
      GLint format = 0;
      glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &width);
      glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &height);
      glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_INTERNAL_FORMAT, &format);
      ok =
          ok && width == (8 >> level) && height == (level == 0 ? 2 : 1) && format == expectedFormat;
    }
    if (cc == "ATI2") {
      std::array<unsigned char, 4> pixel{};
      glGetTexImage(GL_TEXTURE_2D, 3, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
      ok = ok && pixel[0] == 128 && pixel[1] == 64 && pixel[2] == 0 && pixel[3] == 255;
    }
    UnloadTexture(texture);
  }
  glDeleteTextures(1, &previous);
  ok = ok && glGetError() == GL_NO_ERROR;
  if (!ok) std::fprintf(stderr, "DDS GPU validation failed for %.4s\n", cc.data());
  return ok;
}

// A single-mip L8 file must sample as grey, and the generated chain must really
// exist: loadDdsFromMemory clamps MAX_LEVEL to the shipped mips.
bool verifyLuminanceMips() {
  std::string bytes = std::string(128, char{}) + std::string(64, static_cast<char>(200));
  bytes.replace(0, 4, "DDS ");
  put(bytes, 4, 124);
  put(bytes, 12, 8);
  put(bytes, 16, 8);
  put(bytes, 28, 1);
  put(bytes, 76, 32);
  put(bytes, 80, 0x20000U);
  put(bytes, 88, 8);
  put(bytes, 92, 0xFFU);
  auto texture = x4sb::editor::loadDdsFromMemory(bytes);
  bool ok = texture.id != 0 && texture.mipmaps == 1;
  if (texture.id != 0) {
    x4sb::editor::ensureUncompressedMips(texture);
    glBindTexture(GL_TEXTURE_2D, texture.id);
    GLint swizzleG = 0;
    GLint swizzleB = 0;
    GLint tailWidth = 0;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, &swizzleG);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, &swizzleB);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 3, GL_TEXTURE_WIDTH, &tailWidth);
    ok = ok && swizzleG == GL_RED && swizzleB == GL_RED && texture.mipmaps == 4 && tailWidth == 1;
    UnloadTexture(texture);
  }
  ok = ok && glGetError() == GL_NO_ERROR;
  if (!ok) std::fprintf(stderr, "DDS GPU validation failed for L8 mips\n");
  return ok;
}

}  // namespace

bool verifyXmfUpload();

int main() {
  SetConfigFlags(FLAG_WINDOW_HIDDEN);
  InitWindow(64, 64, "DDS upload validation");
  if (!IsWindowReady()) return 1;
  bool ok = verify("DXT1", 8, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT);
  ok = verify("DXT5", 16, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT) && ok;
  ok = verify("ATI2", 16, GL_COMPRESSED_RG_RGTC2) && ok;
  ok = verifyLuminanceMips() && ok;
  ok = verifyXmfUpload() && ok;
  CloseWindow();
  return ok ? 0 : 1;
}
