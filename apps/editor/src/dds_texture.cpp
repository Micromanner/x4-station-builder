#include "dds_texture.hpp"

#include "external/glad.h"
#include "x4sb/assetpipe/dds.hpp"

#include <algorithm>
#include <cstring>
#include <optional>

namespace x4sb::editor {
namespace {

// EXT_texture_sRGB tokens omitted from raylib's generated GL 3.3 header.
constexpr GLenum kSrgbBc1 = 0x8C4D;
constexpr GLenum kSrgbBc2 = 0x8C4E;
constexpr GLenum kSrgbBc3 = 0x8C4F;

bool hasExtension(const char* name) {
  GLint count = 0;
  glGetIntegerv(GL_NUM_EXTENSIONS, &count);
  for (GLint i = 0; i < count; ++i) {
    const auto* extension = glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
    if (extension != nullptr && std::strcmp(reinterpret_cast<const char*>(extension), name) == 0)
      return true;
  }
  return false;
}

struct GlFormat {
  GLenum internal{};
  GLenum source{GL_RGBA};
  int rayFormat{PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
  bool compressed{true};
};

std::optional<GlFormat> glFormat(const DdsLayout& layout, bool srgb) {
  GlFormat f;
  switch (layout.format) {
    case DdsFormat::Bc1:
      f.internal = srgb ? kSrgbBc1 : GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
      f.rayFormat = PIXELFORMAT_COMPRESSED_DXT1_RGBA;
      break;
    case DdsFormat::Bc2:
      f.internal = srgb ? kSrgbBc2 : GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
      f.rayFormat = PIXELFORMAT_COMPRESSED_DXT3_RGBA;
      break;
    case DdsFormat::Bc3:
      f.internal = srgb ? kSrgbBc3 : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
      f.rayFormat = PIXELFORMAT_COMPRESSED_DXT5_RGBA;
      break;
    // Raylib has no BC4/5/7 enum. format 0 deliberately prevents callers from
    // treating these as a different compressed encoding for readback/re-upload.
    case DdsFormat::Bc4:
      f.internal = GL_COMPRESSED_RED_RGTC1;
      f.rayFormat = 0;
      break;
    case DdsFormat::Bc5:
      f.internal = GL_COMPRESSED_RG_RGTC2;
      f.rayFormat = 0;
      break;
    case DdsFormat::Bc7:
      if (!hasExtension("GL_ARB_texture_compression_bptc")) return std::nullopt;
      f.internal = srgb ? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM : GL_COMPRESSED_RGBA_BPTC_UNORM;
      f.rayFormat = 0;
      break;
    case DdsFormat::Bgra8:
      f.source = GL_BGRA;
      f.compressed = false;
      break;
    case DdsFormat::Rgba8:
      f.compressed = false;
      break;
    case DdsFormat::R8:
      f.source = GL_RED;
      f.compressed = false;
      f.rayFormat = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE;
      break;
  }
  if ((layout.format == DdsFormat::Bc1 || layout.format == DdsFormat::Bc2 ||
       layout.format == DdsFormat::Bc3) &&
      !GLAD_GL_EXT_texture_compression_s3tc)
    return std::nullopt;
  if (!f.compressed) f.internal = f.source == GL_RED ? GL_R8 : (srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8);
  return f;
}

// 0 on failure. Array levels are allocated whole, then filled per slice: the DDS stores
// slices slice-major, so one mip's layers are not contiguous in `bytes`.
GLuint upload(const DdsLayout& layout, std::string_view bytes, const GlFormat& f, GLenum target) {
  const bool array = target == GL_TEXTURE_2D_ARRAY;
  const auto layers = static_cast<GLsizei>(layout.layers);
  GLint previousTexture = 0;
  GLint alignment = 0;
  GLint rowLength = 0;
  GLint skipRows = 0;
  GLint skipPixels = 0;
  GLint unpackBuffer = 0;
  glGetIntegerv(array ? GL_TEXTURE_BINDING_2D_ARRAY : GL_TEXTURE_BINDING_2D, &previousTexture);
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
  glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
  glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
  glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPixels);
  glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
  GLuint id = 0;
  glGenTextures(1, &id);
  glBindTexture(target, id);
  bool valid = id != 0;
  for (std::size_t i = 0; i < layout.mips.size() && valid; ++i) {
    const auto& mip = layout.mips[i];
    const auto level = static_cast<GLint>(i);
    const auto width = static_cast<GLsizei>(mip.width);
    const auto height = static_cast<GLsizei>(mip.height);
    const auto size = static_cast<GLsizei>(mip.size);
    if (!array) {
      if (f.compressed)
        glCompressedTexImage2D(target, level, f.internal, width, height, 0, size,
                               bytes.data() + mip.offset);
      else
        glTexImage2D(target, level, static_cast<GLint>(f.internal), width, height, 0, f.source,
                     GL_UNSIGNED_BYTE, bytes.data() + mip.offset);
    } else {
      // parseDds bounded every slice of every mip inside `bytes`, so size x layers fits too.
      if (f.compressed)
        glCompressedTexImage3D(target, level, f.internal, width, height, layers, 0,
                               static_cast<GLsizei>(mip.size * layout.layers), nullptr);
      else
        glTexImage3D(target, level, static_cast<GLint>(f.internal), width, height, layers, 0,
                     f.source, GL_UNSIGNED_BYTE, nullptr);
      for (GLsizei s = 0; s < layers; ++s) {
        const char* slice =
            bytes.data() + mip.offset + static_cast<std::size_t>(s) * layout.sliceStride;
        if (f.compressed)
          glCompressedTexSubImage3D(target, level, 0, 0, s, width, height, 1, f.internal, size,
                                    slice);
        else
          glTexSubImage3D(target, level, 0, 0, s, width, height, 1, f.source, GL_UNSIGNED_BYTE,
                          slice);
      }
    }
    GLint uploadedWidth = 0;
    GLint uploadedLayers = 1;
    glGetTexLevelParameteriv(target, level, GL_TEXTURE_WIDTH, &uploadedWidth);
    if (array) glGetTexLevelParameteriv(target, level, GL_TEXTURE_DEPTH, &uploadedLayers);
    valid = uploadedWidth == width && uploadedLayers == layers;
  }
  glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
  glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(layout.mips.size() - 1));
  glTexParameteri(target, GL_TEXTURE_MIN_FILTER,
                  layout.mips.size() > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
  glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_REPEAT);
  if (layout.opaque) glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, GL_ONE);
  // R8 only comes from legacy L8 luminance: sample it as grey, not red.
  if (layout.format == DdsFormat::R8) {
    glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, GL_RED);
    glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, GL_RED);
  }
  if (GLAD_GL_EXT_texture_filter_anisotropic || GLAD_GL_ARB_texture_filter_anisotropic) {
    GLfloat maximum = 1.0F;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maximum);
    glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, std::min(16.0F, maximum));
  }
  glBindTexture(target, static_cast<GLuint>(previousTexture));
  glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, skipRows);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(unpackBuffer));
  if (!valid) {
    if (id != 0) glDeleteTextures(1, &id);
    return 0;
  }
  return id;
}

}  // namespace

::Texture2D loadDdsFromMemory(std::string_view bytes, bool srgb) {
  const auto layout = parseDds(bytes);
  if (!layout || layout->layers != 1) return {};
  const auto format = glFormat(*layout, srgb);
  if (!format) return {};
  const GLuint id = upload(*layout, bytes, *format, GL_TEXTURE_2D);
  if (id == 0) return {};
  return {id, static_cast<int>(layout->mips.front().width),
          static_cast<int>(layout->mips.front().height), static_cast<int>(layout->mips.size()),
          format->rayFormat};
}

DdsArrayTexture loadDdsArrayFromMemory(std::string_view bytes, bool srgb) {
  const auto layout = parseDds(bytes);
  if (!layout) return {};
  const auto format = glFormat(*layout, srgb);
  if (!format) return {};
  const GLuint id = upload(*layout, bytes, *format, GL_TEXTURE_2D_ARRAY);
  if (id == 0) return {};
  return {id, static_cast<int>(layout->layers)};
}

}  // namespace x4sb::editor
