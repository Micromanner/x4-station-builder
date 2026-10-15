#include "visibility_buffer.hpp"

#include "external/glad.h"
#include "raymath.h"
#include "rlgl.h"

#include <algorithm>

namespace x4sb::editor {
namespace {

// Distinct materials one tile may bin; past it, the overflowing groups go unshaded.
constexpr std::uint32_t kRecordsPerTile = 8;
constexpr std::uint32_t kNoSurface = 0xFFFFFFFFU;
constexpr int kShadedUnit = 30;

// Copies the resolved samples into the scene target, per sample; the sky's samples
// stay. A raster pass, so the scene colour never becomes a storage image (on AMD that
// costs its colour compression: +0.5 ms on the sky layers drawn into it).
constexpr const char* kCompositeVs = R"(#version 430
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
constexpr const char* kCompositeFs = R"(#version 430
layout(binding = 29) uniform usampler2DMS visIds;
layout(binding = 30) uniform sampler2DMS shaded;
out vec4 color;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  if (texelFetch(visIds, p, gl_SampleID).x == 0xFFFFFFFFu) discard;
  color = texelFetch(shaded, p, gl_SampleID);
}
)";

constexpr const char* kMarkCs = R"(#version 430
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 29) uniform usampler2DMS visIds;
struct DrawCommand { uint count; uint instanceCount; uint firstIndex; int baseVertex; uint baseInstance; };
layout(std430, binding = 7) readonly buffer Commands { DrawCommand commands[]; };
layout(std430, binding = 11) buffer Marks { uint marks[]; };
uniform ivec2 screenSize;
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(p, screenSize))) return;
  for (int s = 0; s < 4; ++s) {
    uint x = texelFetch(visIds, p, s).x;
    if (x == 0xFFFFFFFFu) continue;
    marks[commands[x >> 16].baseInstance + (x & 0xFFFFu)] = 1u;
  }
}
)";

// rlgl logs a failed compile or link; 0 = failed.
unsigned int loadCompute(const std::string& source) {
  const unsigned int shader = rlCompileShader(source.c_str(), RL_COMPUTE_SHADER);
  if (shader == 0) return 0;
  const unsigned int program = rlLoadComputeShaderProgram(shader);
  glDeleteShader(shader);
  return program;
}

}  // namespace

VisibilityBuffer::VisibilityBuffer(const std::string& classifySource,
                                   const std::string& resolveSource)
    : classify_(loadCompute(classifySource)),
      resolve_(loadCompute(resolveSource)),
      composite_(LoadShaderFromMemory(kCompositeVs, kCompositeFs)) {
  GLint maxBlock = 0;
  glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &maxBlock);
  maxStorageBytes_ = static_cast<std::size_t>(maxBlock);
  if (!ok()) return;
  mvpLoc_ = glGetUniformLocation(resolve_, "mvp");
  screenLoc_ = glGetUniformLocation(resolve_, "screenSize");
  samplePosLoc_ = glGetUniformLocation(resolve_, "samplePos");
  classifyScreenLoc_ = glGetUniformLocation(classify_, "screenSize");
  glGenBuffers(1, &tiles_);
  glGenVertexArrays(1, &emptyVao_);
}

VisibilityBuffer::~VisibilityBuffer() {
  release();
  if (tiles_ != 0) glDeleteBuffers(1, &tiles_);
  if (emptyVao_ != 0) glDeleteVertexArrays(1, &emptyVao_);
  for (const unsigned int program : {classify_, resolve_, mark_})
    if (program != 0) rlUnloadShaderProgram(program);
  UnloadShader(composite_);  // skips raylib's default shader, which a failed compile returns
}

void VisibilityBuffer::release() {
  if (fbo_ != 0) glDeleteFramebuffers(1, &fbo_);
  for (const unsigned int texture : {ids_, shaded_})
    if (texture != 0) glDeleteTextures(1, &texture);
  fbo_ = ids_ = shaded_ = depth_ = 0;
  size_ = {};
}

bool VisibilityBuffer::begin() {
  if (!ok()) return false;
  GLint scene = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &scene);
  GLint samples = 0;
  glGetIntegerv(GL_SAMPLES, &samples);
  GLint type = GL_NONE;
  if (scene != 0)
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
  if (scene == 0 || samples != kSamples || type != GL_TEXTURE) return false;
  GLint depth = 0;
  glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &depth);
  std::array<int, 2> size{};
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, static_cast<GLuint>(depth));
  glGetTexLevelParameteriv(GL_TEXTURE_2D_MULTISAMPLE, 0, GL_TEXTURE_WIDTH, &size[0]);
  glGetTexLevelParameteriv(GL_TEXTURE_2D_MULTISAMPLE, 0, GL_TEXTURE_HEIGHT, &size[1]);
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
  scene_ = static_cast<unsigned int>(scene);

  // FramePipeline recreates its target on resize: follow it.
  if (static_cast<unsigned int>(depth) != depth_ || size != size_) {
    release();
    const auto target = [&](unsigned int& texture, GLenum format) {
      glGenTextures(1, &texture);
      glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, texture);
      glTexStorage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, kSamples, format, size[0], size[1],
                                GL_TRUE);
    };
    target(ids_, GL_RG32UI);
    target(shaded_, GL_RGBA16F);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
    // The ids and the scene's own depth, which the geometry pass fills for SSAO and the
    // forward passes after it.
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE,
                           ids_, 0);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D_MULTISAMPLE,
                           static_cast<GLuint>(depth), 0);
    const bool complete = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scene_);
    if (!complete) {
      TraceLog(LOG_ERROR, "VisibilityBuffer: id framebuffer incomplete");
      release();
      return false;
    }
    depth_ = static_cast<unsigned int>(depth);
    size_ = size;
    for (int i = 0; i < kSamples; ++i)
      glGetMultisamplefv(GL_SAMPLE_POSITION, static_cast<GLuint>(i),
                         &samplePos_[static_cast<std::size_t>(i) * 2]);

    GLint maxGroups = 0;
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0, &maxGroups);
    const auto tiles = static_cast<std::uint32_t>(((size[0] + kTile - 1) / kTile) *
                                                  ((size[1] + kTile - 1) / kTile));
    tileCapacity_ = std::min(tiles * kRecordsPerTile, static_cast<std::uint32_t>(maxGroups));
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, tiles_);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(16 + std::size_t{tileCapacity_} * 8), nullptr,
                 GL_DYNAMIC_COPY);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  }

  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_);
  const std::array<GLuint, 4> clear{kNoSurface, kNoSurface, 0, 0};
  glClearBufferuiv(GL_COLOR, 0, clear.data());
  return true;
}

void VisibilityBuffer::end() { glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scene_); }

void VisibilityBuffer::markOwners(unsigned int marks) {
  if (mark_ == 0) mark_ = loadCompute(kMarkCs);
  if (mark_ == 0) return;
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 11, marks);
  glActiveTexture(GL_TEXTURE0 + kIdUnit);
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, ids_);
  glUseProgram(mark_);
  glUniform2i(glGetUniformLocation(mark_, "screenSize"), size_[0], size_[1]);
  glDispatchCompute(static_cast<GLuint>((size_[0] + 7) / 8),
                    static_cast<GLuint>((size_[1] + 7) / 8), 1);
  glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
  glUseProgram(0);
}

void VisibilityBuffer::resolve(const ::Matrix& mvp) {
  const std::array<std::uint32_t, 4> header{0, 1, 1, tileCapacity_};  // dispatch x y z, capacity
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, tiles_);
  glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(header), header.data());
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kTileBinding, tiles_);
  glActiveTexture(GL_TEXTURE0 + kIdUnit);
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, ids_);

  glUseProgram(classify_);
  glUniform2i(classifyScreenLoc_, size_[0], size_[1]);
  glDispatchCompute(static_cast<GLuint>((size_[0] + kTile - 1) / kTile),
                    static_cast<GLuint>((size_[1] + kTile - 1) / kTile), 1);
  glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);

  glUseProgram(resolve_);
  glUniformMatrix4fv(mvpLoc_, 1, GL_FALSE, MatrixToFloatV(mvp).v);
  glUniform2i(screenLoc_, size_[0], size_[1]);
  glUniform2fv(samplePosLoc_, kSamples, samplePos_.data());
  glBindImageTexture(0, shaded_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
  glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, tiles_);
  glDispatchComputeIndirect(0);
  glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, 0);
  // Fetch: the composite reads the shaded samples. Buffer update: next frame's header
  // write follows this frame's atomics.
  glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
  glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

  // Composite into the bound scene target: every covered sample, no depth or blend.
  glActiveTexture(GL_TEXTURE0 + kShadedUnit);
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, shaded_);
  const GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
  const GLboolean blend = glIsEnabled(GL_BLEND);
  const GLboolean cull = glIsEnabled(GL_CULL_FACE);
  GLboolean depthMask = GL_TRUE;
  glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDepthMask(GL_FALSE);
  glUseProgram(composite_.id);
  glBindVertexArray(emptyVao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindVertexArray(0);
  if (depthTest == GL_TRUE) glEnable(GL_DEPTH_TEST);
  if (blend == GL_TRUE) glEnable(GL_BLEND);
  if (cull == GL_TRUE) glEnable(GL_CULL_FACE);
  glDepthMask(depthMask);

  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
  glActiveTexture(GL_TEXTURE0 + kIdUnit);
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
  glActiveTexture(GL_TEXTURE0);
  glUseProgram(0);
}

}  // namespace x4sb::editor
