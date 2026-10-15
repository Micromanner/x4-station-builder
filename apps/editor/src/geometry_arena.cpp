#include "geometry_arena.hpp"

#include "external/glad.h"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace x4sb::editor {
namespace {

// First allocation per buffer; it doubles from there (the 584-module plan settles
// near 0.7 GB of vertices).
constexpr std::size_t kInitialBytes = std::size_t{64} << 20U;

// The locations the station shaders pin: raylib's defaults (rlgl.h) for the streams
// raylib knows, free slots for DATA and UV2/3.
constexpr GLuint kPosition = 0;
constexpr GLuint kTexCoord = 1;
constexpr GLuint kNormal = 2;
constexpr GLuint kColor = 3;
constexpr GLuint kTexCoord2 = 5;
constexpr GLuint kData = 7;
constexpr GLuint kUv23 = 8;

void format(GLuint location, GLint size, GLenum type, bool normalized, std::size_t offset) {
  glEnableVertexAttribArray(location);
  glVertexAttribFormat(location, size, type, normalized ? GL_TRUE : GL_FALSE,
                       static_cast<GLuint>(offset));
  glVertexAttribBinding(location, 0);
}

}  // namespace

GeometryArena::GeometryArena() {
  glGenVertexArrays(1, &vao_);
  glBindVertexArray(vao_);
  format(kPosition, 3, GL_FLOAT, false, offsetof(ArenaVertex, position));
  format(kNormal, 4, GL_INT_2_10_10_10_REV, true, offsetof(ArenaVertex, normal));
  format(kTexCoord, 2, GL_FLOAT, false, offsetof(ArenaVertex, uv0));
  format(kTexCoord2, 2, GL_FLOAT, false, offsetof(ArenaVertex, uv1));
  format(kUv23, 4, GL_FLOAT, false, offsetof(ArenaVertex, uv23));
  format(kColor, 4, GL_UNSIGNED_BYTE, true, offsetof(ArenaVertex, color));
  // Unnormalized: the shaders read DATA as 0-255 (multimat's array layer must round-trip).
  format(kData, 4, GL_UNSIGNED_BYTE, false, offsetof(ArenaVertex, data));
  glBindVertexArray(0);
}

GeometryArena::~GeometryArena() {
  for (const Buffer* buffer : {&vertices_, &indices_})
    if (buffer->id != 0) glDeleteBuffers(1, &buffer->id);
  if (vao_ != 0) glDeleteVertexArrays(1, &vao_);
}

std::optional<GeometryArena::Placement> GeometryArena::append(
    std::span<const ArenaVertex> vertices, std::span<const std::uint32_t> indices) {
  constexpr std::size_t kMax = std::numeric_limits<std::uint32_t>::max();
  const std::size_t firstVertex = vertices_.used / sizeof(ArenaVertex);
  const std::size_t firstIndex = indices_.used / sizeof(std::uint32_t);
  if (vertices.size() > kMax - firstVertex || indices.size() > kMax - firstIndex)
    return std::nullopt;
  const unsigned int oldVertices = vertices_.id;
  const unsigned int oldIndices = indices_.id;
  reserve(vertices_, vertices.size_bytes());
  reserve(indices_, indices.size_bytes());
  if (vertices_.id != oldVertices || indices_.id != oldIndices) {
    glBindVertexArray(vao_);
    glBindVertexBuffer(0, vertices_.id, 0, sizeof(ArenaVertex));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices_.id);  // VAO state
    glBindVertexArray(0);
  }
  write(vertices_, vertices.data(), vertices.size_bytes());
  write(indices_, indices.data(), indices.size_bytes());
  return Placement{static_cast<std::uint32_t>(firstVertex), static_cast<std::uint32_t>(firstIndex)};
}

void GeometryArena::bind() const { glBindVertexArray(vao_); }

void GeometryArena::bindStorage(unsigned int vertexBinding, unsigned int indexBinding) const {
  glBindBufferRange(GL_SHADER_STORAGE_BUFFER, vertexBinding, vertices_.id, 0,
                    static_cast<GLsizeiptr>(vertices_.used));
  glBindBufferRange(GL_SHADER_STORAGE_BUFFER, indexBinding, indices_.id, 0,
                    static_cast<GLsizeiptr>(indices_.used));
}

void GeometryArena::reserve(Buffer& buffer, std::size_t extra) {
  if (buffer.used + extra <= buffer.capacity) return;
  std::size_t capacity = std::max(buffer.capacity, kInitialBytes);
  while (capacity < buffer.used + extra) capacity *= 2;
  unsigned int grown = 0;
  glGenBuffers(1, &grown);
  // The copy targets leave GL_ARRAY_BUFFER and the VAO's element binding to raylib and bind().
  glBindBuffer(GL_COPY_WRITE_BUFFER, grown);
  glBufferData(GL_COPY_WRITE_BUFFER, static_cast<GLsizeiptr>(capacity), nullptr, GL_STATIC_DRAW);
  if (buffer.id != 0) {
    glBindBuffer(GL_COPY_READ_BUFFER, buffer.id);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0,
                        static_cast<GLsizeiptr>(buffer.used));
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    glDeleteBuffers(1, &buffer.id);
  }
  glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
  buffer.id = grown;
  buffer.capacity = capacity;
}

void GeometryArena::write(Buffer& buffer, const void* data, std::size_t bytes) {
  glBindBuffer(GL_COPY_WRITE_BUFFER, buffer.id);
  glBufferSubData(GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(buffer.used),
                  static_cast<GLsizeiptr>(bytes), data);
  glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
  buffer.used += bytes;
}

}  // namespace x4sb::editor
