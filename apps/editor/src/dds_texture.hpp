#pragma once

#include "raylib.h"

#include <string_view>

namespace x4sb::editor {

// Requires the render thread and a live OpenGL context. The caller owns the
// returned texture (UnloadTexture); failure returns id 0. Authored mips are
// uploaded unchanged, including sub-block and non-square tail levels. DDS
// arrays are rejected: they need loadDdsArrayFromMemory.
[[nodiscard]] ::Texture2D loadDdsFromMemory(std::string_view bytes, bool srgb = false);

// A GL_TEXTURE_2D_ARRAY, which raylib's Texture2D cannot describe.
struct DdsArrayTexture {
  unsigned int id{};  // 0 = failed
  int layers{};
};

// Same contract and sampler state as loadDdsFromMemory; the caller owns the texture
// (rlUnloadTexture). A single-layer DDS loads as a one-layer array.
[[nodiscard]] DdsArrayTexture loadDdsArrayFromMemory(std::string_view bytes, bool srgb = false);

}  // namespace x4sb::editor
