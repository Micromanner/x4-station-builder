#include "texture_mips.hpp"

#include "external/glad.h"

namespace x4sb::editor {

void ensureUncompressedMips(::Texture2D& texture) {
  if (texture.id == 0) return;
  // Format 0 marks BC4/5/7 from loadDdsFromMemory (no raylib enum), which are compressed.
  if (texture.format == 0 || texture.format >= PIXELFORMAT_COMPRESSED_DXT1_RGB) return;
  if (texture.mipmaps <= 1) {
    // loadDdsFromMemory clamps MAX_LEVEL to the shipped chain; glGenerateMipmap
    // honors that clamp and would build nothing while raylib still counts the levels.
    GLint previousTexture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    glBindTexture(GL_TEXTURE_2D, texture.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    GenTextureMipmaps(&texture);
  }
  // Trilinear needs the updated mipmaps count in the struct (SetTextureFilter
  // silently degrades to bilinear when it reads mipmaps == 1).
  if (texture.mipmaps > 1) SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);
}

}  // namespace x4sb::editor
