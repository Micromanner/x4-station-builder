#pragma once
// The station parts' shared vertex and index buffers (design 2026-10-14 §4.3): one
// VAO for every part, so a draw is just an index range + base vertex. Grow-only:
// MeshCache never evicts a part. Needs a live GL context for its whole lifetime.
#include "xmf_model.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace x4sb::editor {

class GeometryArena {
 public:
  // Where append() put a part: its index ranges and base vertices are relative to these.
  struct Placement {
    std::uint32_t firstVertex{0};
    std::uint32_t firstIndex{0};
  };

  GeometryArena();
  ~GeometryArena();
  GeometryArena(const GeometryArena&) = delete;
  GeometryArena& operator=(const GeometryArena&) = delete;
  GeometryArena(GeometryArena&&) = delete;
  GeometryArena& operator=(GeometryArena&&) = delete;

  // nullopt when the arena would pass the u32 vertex/index range.
  [[nodiscard]] std::optional<Placement> append(std::span<const ArenaVertex> vertices,
                                                std::span<const std::uint32_t> indices);
  // Binds the VAO (with its element buffer); the caller unbinds when done.
  void bind() const;
  // Binds the used vertex and index bytes as shader storage (the visibility resolve).
  void bindStorage(unsigned int vertexBinding, unsigned int indexBinding) const;
  [[nodiscard]] std::size_t vertexBytes() const { return vertices_.used; }
  [[nodiscard]] std::size_t indexBytes() const { return indices_.used; }

 private:
  struct Buffer {
    unsigned int id{0};
    std::size_t capacity{0};  // bytes
    std::size_t used{0};
  };
  // Doubles `buffer` until `extra` more bytes fit, carrying the old contents over.
  static void reserve(Buffer& buffer, std::size_t extra);
  static void write(Buffer& buffer, const void* data, std::size_t bytes);

  unsigned int vao_{0};
  Buffer vertices_;
  Buffer indices_;
};

}  // namespace x4sb::editor
