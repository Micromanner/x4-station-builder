#pragma once
// Two-phase GPU occlusion culling of the main view's part instances (spec
// docs/2026-10-15-hiz-occlusion-culling-design.md). Phase A keeps the entries whose
// instance was visible last frame; after they are drawn, phase B tests every entry
// against a max-depth pyramid of that depth. Each phase compacts its survivors into a
// region of the visible buffer, in bucket order, and patches the sub-draws'
// instanceCount from the per-bucket counts.
// Needs a live GL context: construct after InitWindow, destroy before CloseWindow.
#include "raylib.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace x4sb::editor {

class OcclusionCull {
 public:
  // One bucket's slice of the CPU visible list and its part's local box (std430).
  struct Bucket {
    std::array<float, 3> boundsMin{};
    std::uint32_t first{0};
    std::array<float, 3> boundsMax{};
    std::uint32_t count{0};
  };
  static_assert(sizeof(Bucket) == 32);

  // The visible buffer's regions, each `entries` long: the CPU list, then the
  // survivors of phase A, phase B (minus A) and phase B (all).
  enum Region : std::uint32_t { kCpu, kPhaseA, kPhaseB, kVisible };
  static constexpr std::size_t kRegions = 4;

  OcclusionCull();
  ~OcclusionCull();
  OcclusionCull(const OcclusionCull&) = delete;
  OcclusionCull& operator=(const OcclusionCull&) = delete;
  OcclusionCull(OcclusionCull&&) = delete;
  OcclusionCull& operator=(OcclusionCull&&) = delete;

  [[nodiscard]] bool ok() const {
    return cull_ != 0 && patch_ != 0 && hizFirst_ != 0 && hizDown_ != 0;
  }
  // The cull runs one work group per bucket: a batch with more can't be culled.
  [[nodiscard]] std::size_t maxBuckets() const { return maxBuckets_; }
  // This frame's buckets; `instances` bounds the instance ids (the history bits).
  // `resetHistory` after the instance ids moved (a station edit).
  void setBuckets(const std::vector<Bucket>& buckets, std::size_t instances, bool resetHistory);
  // Index into the per-bucket counts for `region`'s survivors of `bucket`.
  [[nodiscard]] std::uint32_t countIndex(Region region, std::uint32_t bucket) const {
    return static_cast<std::uint32_t>((region - 1) * bucketCount_) + bucket;
  }
  // Phase A: compacts last frame's visible entries inside the frustum into kPhaseA.
  // The caller has bound the instance (0) and visible (1) buffers.
  void cullPhaseA(const ::Matrix& mvp, unsigned int visible, std::uint32_t entries);
  // The max-depth pyramid of the bound draw framebuffer's multisample depth; false (no
  // pyramid) for any other depth.
  [[nodiscard]] bool buildHiz();
  // Phase B: fills kPhaseB and kVisible and next frame's history, against the pyramid
  // when `hiz` (buildHiz succeeded), else by frustum only.
  void cullPhaseB(const ::Matrix& mvp, unsigned int visible, std::uint32_t entries, bool hiz);
  // Writes instanceCount of commands [first, first + countIndex.size()) of `commands`
  // from the counts; uploads countIndex itself.
  void patchCommands(unsigned int commands, std::size_t first,
                     const std::vector<std::uint32_t>& countIndex);

 private:
  void cull(int phase, const ::Matrix& mvp, unsigned int visible, std::uint32_t entries, bool hiz);

  unsigned int cull_{0};
  unsigned int patch_{0};
  unsigned int hizFirst_{0};
  unsigned int hizDown_{0};
  unsigned int buckets_{0};
  unsigned int counts_{0};
  std::array<unsigned int, 2> history_{};  // [last] read by both phases, [!last] written by B
  int last_{0};
  std::size_t historyWords_{0};
  unsigned int countIndex_{0};
  std::size_t bucketCount_{0};
  std::size_t maxBuckets_{0};
  unsigned int hiz_{0};
  std::array<int, 2> hizScreen_{};  // the depth size the pyramid was built for
  int hizLevels_{0};
};

}  // namespace x4sb::editor
