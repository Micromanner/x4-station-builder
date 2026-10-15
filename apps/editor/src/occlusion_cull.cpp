#include "occlusion_cull.hpp"

#include "external/glad.h"
#include "raymath.h"
#include "rlgl.h"

#include <algorithm>
#include <bit>

namespace x4sb::editor {
namespace {

constexpr int kDepthUnit = 28;
constexpr int kHizUnit = 27;
constexpr unsigned int kBucketBinding = 11;
constexpr unsigned int kCountBinding = 12;
constexpr unsigned int kLastBinding = 13;
constexpr unsigned int kNextBinding = 14;
constexpr unsigned int kCountIndexBinding = 15;
constexpr unsigned int kCommandBinding = 7;
constexpr GLuint kGroup = 64;  // the cull's and the patch's local_size_x
constexpr GLuint kTile = 8;    // the pyramid passes' local_size

// Level L texel p covers screen pixels [p, p + 1) * 2^(L+1), clamped at the edge: half
// resolution, sizes rounded up. Level 0 reads each pixel once (a footprint that doesn't
// divide the screen read the 4x depth ~2.5 times over: 0.2 ms).
// One thread per pixel, reduced 2x2 through shared memory: a thread looping over its
// texel's 16 samples was fetch-latency bound (0.25 ms).
constexpr const char* kHizFirstCs = R"(#version 430
layout(local_size_x = 16, local_size_y = 16) in;
layout(binding = 28) uniform sampler2DMS depth;
layout(r32f, binding = 0) uniform restrict writeonly image2D dst;
uniform ivec2 srcSize;
uniform ivec2 dstSize;
uniform int samples;
shared float far[16][16];
void main() {
  ivec2 pixel = min(ivec2(gl_GlobalInvocationID.xy), srcSize - 1);  // edge: repeat the last
  float d = 0.0;
  for (int s = 0; s < samples; ++s) d = max(d, texelFetch(depth, pixel, s).r);
  uvec2 l = gl_LocalInvocationID.xy;
  far[l.y][l.x] = d;
  barrier();
  if (any(greaterThanEqual(l, uvec2(8)))) return;
  ivec2 p = ivec2(gl_WorkGroupID.xy * 8u + l);
  if (any(greaterThanEqual(p, dstSize))) return;
  uvec2 c = l * 2u;
  imageStore(dst, p, vec4(max(max(far[c.y][c.x], far[c.y][c.x + 1u]),
                              max(far[c.y + 1u][c.x], far[c.y + 1u][c.x + 1u]))));
}
)";

constexpr const char* kHizDownCs = R"(#version 430
layout(local_size_x = 8, local_size_y = 8) in;
layout(r32f, binding = 0) uniform restrict readonly image2D src;
layout(r32f, binding = 1) uniform restrict writeonly image2D dst;
uniform ivec2 srcSize;
uniform ivec2 dstSize;
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(p, dstSize))) return;
  ivec2 a = min(p * 2, srcSize - 1);
  ivec2 b = min(p * 2 + 1, srcSize - 1);
  float far = max(max(imageLoad(src, a).r, imageLoad(src, ivec2(b.x, a.y)).r),
                  max(imageLoad(src, ivec2(a.x, b.y)).r, imageLoad(src, b).r));
  imageStore(dst, p, vec4(far));
}
)";

// One work group per bucket. Survivors are compacted with a prefix sum per 64-entry
// chunk, not atomics, so an instance keeps its place in the list: draw order (and the
// blend order of overlapping alpha-blended copies) stays stable frame to frame.
constexpr const char* kCullCs = R"(#version 430
layout(local_size_x = 64) in;
struct Bucket {
  vec3 boundsMin;
  uint first;
  vec3 boundsMax;
  uint count;
};
layout(std430, binding = 0) readonly buffer Instances { mat4 instances[]; };
layout(std430, binding = 1) buffer Visible { uint visible[]; };
layout(std430, binding = 11) readonly buffer Buckets { Bucket buckets[]; };
layout(std430, binding = 12) buffer Counts { uint counts[]; };
layout(std430, binding = 13) readonly buffer Last { uint lastVisible[]; };
layout(std430, binding = 14) buffer Next { uint nextVisible[]; };
layout(binding = 27) uniform sampler2D hiz;
uniform mat4 mvp;
uniform uint entries;     // one region's length
uniform uint bucketCount;
uniform int phase;        // 0 = A, 1 = B
uniform bool useHiz;
uniform int hizLevels;
uniform ivec2 screenSize;  // the depth the pyramid was built from
shared uint scan[64];

// x: inside the frustum, y: not behind the pyramid's depth.
bvec2 test(mat4 model, vec3 lo, vec3 hi) {
  mat4 m = mvp * model;
  // Corners all outside one clip plane: off screen. Bit k of `o` = beyond plane k.
  uint allOut = 63u;
  bool behind = false;
  vec3 ndcLo = vec3(1.0);
  vec3 ndcHi = vec3(-1.0);
  for (int i = 0; i < 8; ++i) {
    vec3 corner = vec3((i & 1) != 0 ? hi.x : lo.x, (i & 2) != 0 ? hi.y : lo.y,
                       (i & 4) != 0 ? hi.z : lo.z);
    vec4 c = m * vec4(corner, 1.0);
    uint o = (c.x < -c.w ? 1u : 0u) | (c.x > c.w ? 2u : 0u) | (c.y < -c.w ? 4u : 0u) |
             (c.y > c.w ? 8u : 0u) | (c.z < -c.w ? 16u : 0u) | (c.z > c.w ? 32u : 0u);
    allOut &= o;
    if (c.w <= 1e-6 || c.z < -c.w) {
      behind = true;
    } else {
      vec3 n = c.xyz / c.w;
      ndcLo = min(ndcLo, n);
      ndcHi = max(ndcHi, n);
    }
  }
  if (allOut != 0u) return bvec2(false, false);
  if (!useHiz || behind) return bvec2(true, true);
  vec2 pxLo = clamp(ndcLo.xy * 0.5 + 0.5, 0.0, 1.0) * vec2(screenSize);
  vec2 pxHi = clamp(ndcHi.xy * 0.5 + 0.5, 0.0, 1.0) * vec2(screenSize);
  // The level whose texels (2^(level+1) pixels) the box spans at most five of per axis.
  vec2 extent = pxHi - pxLo;
  int level = clamp(int(ceil(log2(max(max(extent.x, extent.y), 1.0)))) - 3, 0, hizLevels - 1);
  float texel = exp2(float(level + 1));
  ivec2 size = (screenSize + int(texel) - 1) / int(texel);  // in use; the storage is larger
  ivec2 a = clamp(ivec2(pxLo / texel), ivec2(0), size - 1);
  ivec2 b = clamp(ivec2(pxHi / texel), ivec2(0), size - 1);
  float far = 0.0;
  for (int y = a.y; y <= b.y; ++y)
    for (int x = a.x; x <= b.x; ++x) far = max(far, texelFetch(hiz, ivec2(x, y), level).r);
  // The epsilon covers the 24-bit depth's rounding and float error at km-scale
  // translations, so a part flush with a phase-A surface is never culled where LEQUAL
  // would show it.
  return bvec2(true, ndcLo.z * 0.5 + 0.5 <= far + 2e-6);
}

void main() {
  uint bucket = gl_WorkGroupID.x;
  Bucket b = buckets[bucket];
  uint lane = gl_LocalInvocationID.x;
  // Survivors so far: A in the low half, B | visible << 16 in phase B.
  uint runLo = 0u;
  uint runHi = 0u;
  for (uint base = 0u; base < b.count; base += 64u) {
    uint i = base + lane;
    uint instance = 0u;
    uint flags = 0u;  // bit 0: region A or B, bit 16: region visible
    if (i < b.count) {
      instance = visible[b.first + i];
      bool last = (lastVisible[instance >> 5] & (1u << (instance & 31u))) != 0u;
      bvec2 t = test(instances[instance], b.boundsMin, b.boundsMax);
      if (phase == 0) {
        flags = last && t.x ? 1u : 0u;
      } else {
        bool seen = t.x && t.y;
        flags = (seen && !last ? 1u : 0u) | (seen ? 0x10000u : 0u);
        if (seen) atomicOr(nextVisible[instance >> 5], 1u << (instance & 31u));
      }
    }
    scan[lane] = flags;
    barrier();
    for (uint offset = 1u; offset < 64u; offset <<= 1u) {
      uint add = lane >= offset ? scan[lane - offset] : 0u;
      barrier();
      scan[lane] += add;
      barrier();
    }
    uint before = scan[lane] - flags;
    uint total = scan[63];
    if ((flags & 1u) != 0u)
      visible[(phase == 0 ? 1u : 2u) * entries + b.first + runLo + (before & 0xFFFFu)] = instance;
    if ((flags & 0x10000u) != 0u)
      visible[3u * entries + b.first + runHi + (before >> 16)] = instance;
    runLo += total & 0xFFFFu;
    runHi += total >> 16;
    barrier();
  }
  if (lane == 0u) {
    counts[uint(phase) * bucketCount + bucket] = runLo;
    if (phase == 1) counts[2u * bucketCount + bucket] = runHi;
  }
}
)";

constexpr const char* kPatchCs = R"(#version 430
layout(local_size_x = 64) in;
struct DrawCommand {
  uint count;
  uint instanceCount;
  uint firstIndex;
  int baseVertex;
  uint baseInstance;
};
layout(std430, binding = 7) buffer Commands { DrawCommand commands[]; };
layout(std430, binding = 12) readonly buffer Counts { uint counts[]; };
layout(std430, binding = 15) readonly buffer CountIndex { uint countIndex[]; };
uniform uint first;
uniform uint commandCount;
void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i >= commandCount) return;
  commands[first + i].instanceCount = counts[countIndex[i]];
}
)";

// rlgl logs a failed compile or link; 0 = failed.
unsigned int loadCompute(const char* source) {
  const unsigned int shader = rlCompileShader(source, RL_COMPUTE_SHADER);
  if (shader == 0) return 0;
  const unsigned int program = rlLoadComputeShaderProgram(shader);
  glDeleteShader(shader);
  return program;
}

void resize(unsigned int& buffer, std::size_t bytes) {
  if (buffer == 0) glGenBuffers(1, &buffer);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
  glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(std::max<std::size_t>(bytes, 16)),
               nullptr, GL_DYNAMIC_COPY);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

GLuint groups(std::size_t n, GLuint size) { return static_cast<GLuint>((n + size - 1) / size); }

}  // namespace

OcclusionCull::OcclusionCull()
    : cull_(loadCompute(kCullCs)),
      patch_(loadCompute(kPatchCs)),
      hizFirst_(loadCompute(kHizFirstCs)),
      hizDown_(loadCompute(kHizDownCs)) {
  GLint maxGroups = 0;
  glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0, &maxGroups);
  maxBuckets_ = static_cast<std::size_t>(maxGroups);
}

OcclusionCull::~OcclusionCull() {
  for (const unsigned int buffer : {buckets_, counts_, history_[0], history_[1], countIndex_})
    if (buffer != 0) glDeleteBuffers(1, &buffer);
  if (hiz_ != 0) glDeleteTextures(1, &hiz_);
  for (const unsigned int program : {cull_, patch_, hizFirst_, hizDown_})
    if (program != 0) rlUnloadShaderProgram(program);
}

void OcclusionCull::setBuckets(const std::vector<Bucket>& buckets, std::size_t instances,
                               bool resetHistory) {
  bucketCount_ = buckets.size();
  if (buckets_ == 0) glGenBuffers(1, &buckets_);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, buckets_);
  glBufferData(GL_SHADER_STORAGE_BUFFER,
               static_cast<GLsizeiptr>(std::max<std::size_t>(buckets.size(), 1) * sizeof(Bucket)),
               nullptr, GL_STREAM_DRAW);
  glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                  static_cast<GLsizeiptr>(buckets.size() * sizeof(Bucket)), buckets.data());
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  resize(counts_, 3 * bucketCount_ * sizeof(std::uint32_t));

  // Instance ids index the history bits: new ids (an edit) or more of them = no history.
  const std::size_t words = (instances + 31) / 32;
  if (resetHistory || words > historyWords_) {
    historyWords_ = std::max(words, historyWords_);
    for (unsigned int& bits : history_) {
      resize(bits, historyWords_ * sizeof(std::uint32_t));
      glBindBuffer(GL_SHADER_STORAGE_BUFFER, bits);
      glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT,
                        nullptr);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  }
}

void OcclusionCull::cullPhaseA(const ::Matrix& mvp, unsigned int visible, std::uint32_t entries) {
  cull(0, mvp, visible, entries, false);
}

void OcclusionCull::cullPhaseB(const ::Matrix& mvp, unsigned int visible, std::uint32_t entries,
                               bool hiz) {
  unsigned int next = history_[static_cast<std::size_t>(1 - last_)];
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, next);
  glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  cull(1, mvp, visible, entries, hiz);
  last_ = 1 - last_;
}

void OcclusionCull::cull(int phase, const ::Matrix& mvp, unsigned int visible,
                         std::uint32_t entries, bool hiz) {
  if (bucketCount_ == 0) return;
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, visible);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBucketBinding, buckets_);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kCountBinding, counts_);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kLastBinding,
                   history_[static_cast<std::size_t>(last_)]);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kNextBinding,
                   history_[static_cast<std::size_t>(1 - last_)]);
  glActiveTexture(GL_TEXTURE0 + kHizUnit);
  glBindTexture(GL_TEXTURE_2D, hiz ? hiz_ : 0);
  glUseProgram(cull_);
  glUniformMatrix4fv(glGetUniformLocation(cull_, "mvp"), 1, GL_FALSE, MatrixToFloatV(mvp).v);
  glUniform1ui(glGetUniformLocation(cull_, "entries"), entries);
  glUniform1ui(glGetUniformLocation(cull_, "bucketCount"), static_cast<GLuint>(bucketCount_));
  glUniform1i(glGetUniformLocation(cull_, "phase"), phase);
  glUniform1i(glGetUniformLocation(cull_, "useHiz"), hiz ? 1 : 0);
  glUniform1i(glGetUniformLocation(cull_, "hizLevels"), hizLevels_);
  glUniform2i(glGetUniformLocation(cull_, "screenSize"), hizScreen_[0], hizScreen_[1]);
  glDispatchCompute(static_cast<GLuint>(bucketCount_), 1, 1);
  // The sub-draws read the compacted lists; the patch reads the counts.
  glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE0);
  glUseProgram(0);
}

bool OcclusionCull::buildHiz() {
  GLint fbo = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
  if (fbo == 0) return false;  // the default framebuffer has no depth attachment to query
  GLint samples = 0;
  glGetIntegerv(GL_SAMPLES, &samples);
  GLint type = GL_NONE;
  glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
  // FramePipeline's scene: a multisample depth texture. Anything else culls by frustum only.
  if (samples <= 1 || type != GL_TEXTURE) return false;
  GLint depth = 0;
  glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &depth);
  std::array<int, 2> src{};
  glActiveTexture(GL_TEXTURE0 + kDepthUnit);
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, static_cast<GLuint>(depth));
  glGetTexLevelParameteriv(GL_TEXTURE_2D_MULTISAMPLE, 0, GL_TEXTURE_WIDTH, &src[0]);
  glGetTexLevelParameteriv(GL_TEXTURE_2D_MULTISAMPLE, 0, GL_TEXTURE_HEIGHT, &src[1]);
  if (src[0] <= 0 || src[1] <= 0) {
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
    glActiveTexture(GL_TEXTURE0);
    return false;
  }

  // The texels in use per level: the half-resolution footprint, rounded up.
  const auto used = [&](int level) {
    const int texel = 2 << level;
    return std::array<int, 2>{(src[0] + texel - 1) / texel, (src[1] + texel - 1) / texel};
  };
  const std::array<int, 2> size = used(0);
  if (src != hizScreen_) {
    if (hiz_ != 0) glDeleteTextures(1, &hiz_);
    hizScreen_ = src;
    // Power-of-two storage: GL halves level sizes rounding down, the footprints round up.
    const std::array<int, 2> storage{
        static_cast<int>(std::bit_ceil(static_cast<unsigned>(size[0]))),
        static_cast<int>(std::bit_ceil(static_cast<unsigned>(size[1])))};
    hizLevels_ = std::bit_width(static_cast<unsigned>(std::max(storage[0], storage[1])));
    glGenTextures(1, &hiz_);
    glBindTexture(GL_TEXTURE_2D, hiz_);
    glTexStorage2D(GL_TEXTURE_2D, hizLevels_, GL_R32F, storage[0], storage[1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  glUseProgram(hizFirst_);
  glUniform2i(glGetUniformLocation(hizFirst_, "srcSize"), src[0], src[1]);
  glUniform2i(glGetUniformLocation(hizFirst_, "dstSize"), size[0], size[1]);
  glUniform1i(glGetUniformLocation(hizFirst_, "samples"), samples);
  glBindImageTexture(0, hiz_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
  glDispatchCompute(groups(static_cast<std::size_t>(size[0]), kTile),
                    groups(static_cast<std::size_t>(size[1]), kTile), 1);
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
  glActiveTexture(GL_TEXTURE0);

  glUseProgram(hizDown_);
  const GLint srcLoc = glGetUniformLocation(hizDown_, "srcSize");
  const GLint dstLoc = glGetUniformLocation(hizDown_, "dstSize");
  std::array<int, 2> from = size;
  for (int level = 1; level < hizLevels_; ++level) {
    const std::array<int, 2> to = used(level);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    glUniform2i(srcLoc, from[0], from[1]);
    glUniform2i(dstLoc, to[0], to[1]);
    glBindImageTexture(0, hiz_, level - 1, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
    glBindImageTexture(1, hiz_, level, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
    glDispatchCompute(groups(static_cast<std::size_t>(to[0]), kTile),
                      groups(static_cast<std::size_t>(to[1]), kTile), 1);
    from = to;
  }
  glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
  glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
  glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
  glUseProgram(0);
  return true;
}

void OcclusionCull::patchCommands(unsigned int commands, std::size_t first,
                                  const std::vector<std::uint32_t>& countIndex) {
  if (countIndex.empty()) return;
  if (countIndex_ == 0) glGenBuffers(1, &countIndex_);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, countIndex_);
  glBufferData(GL_SHADER_STORAGE_BUFFER,
               static_cast<GLsizeiptr>(countIndex.size() * sizeof(std::uint32_t)),
               countIndex.data(), GL_STREAM_DRAW);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kCommandBinding, commands);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kCountBinding, counts_);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kCountIndexBinding, countIndex_);
  glUseProgram(patch_);
  glUniform1ui(glGetUniformLocation(patch_, "first"), static_cast<GLuint>(first));
  glUniform1ui(glGetUniformLocation(patch_, "commandCount"),
               static_cast<GLuint>(countIndex.size()));
  glDispatchCompute(groups(countIndex.size(), kGroup), 1, 1);
  glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
  glUseProgram(0);
}

}  // namespace x4sb::editor
