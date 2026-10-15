#pragma once
// FNV-1a over the inputs a cached render product depends on (the sun shadow
// cascades): an unchanged key means reuse.
#include <cstddef>
#include <cstdint>

namespace x4sb::editor {

class ContentKey {
 public:
  void bytes(const void* data, std::size_t n) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i) h_ = (h_ ^ p[i]) * 1099511628211ULL;
  }
  template <typename T>
  void value(const T& v) {
    bytes(&v, sizeof(v));
  }
  [[nodiscard]] std::uint64_t result() const { return h_; }

 private:
  std::uint64_t h_{14695981039346656037ULL};
};

}  // namespace x4sb::editor
