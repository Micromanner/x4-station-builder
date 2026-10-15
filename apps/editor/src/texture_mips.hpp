#pragma once

#include "raylib.h"

namespace x4sb::editor {

// A single-mip uncompressed texture sampled far below its size re-picks one texel
// per frame and sparkles (the star atlas twinkled). Generate the chain on the GPU
// and filter trilinearly. Compressed textures are left alone: they ship authored
// mips, and glGenerateMipmap is undefined for them.
void ensureUncompressedMips(::Texture2D& texture);

}  // namespace x4sb::editor
