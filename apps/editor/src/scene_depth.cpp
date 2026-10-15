#include "scene_depth.hpp"

#include "external/glad.h"

namespace x4sb::editor {

SceneDepthCopy::~SceneDepthCopy() { release(); }

void SceneDepthCopy::release() {
  if (fbo_ != 0) glDeleteFramebuffers(1, &fbo_);
  if (texture_ != 0) glDeleteTextures(1, &texture_);
  fbo_ = 0;
  texture_ = 0;
}

bool SceneDepthCopy::capture() {
  GLint drawFbo = 0;
  GLint readFbo = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
  if (drawFbo == 0) return false;
  std::array<GLint, 4> vp{};
  glGetIntegerv(GL_VIEWPORT, vp.data());
  const std::array<int, 2> dims{vp[2], vp[3]};
  if (dims[0] <= 0 || dims[1] <= 0) return false;

  // A depth blit needs matching formats: mirror the scene's depth attachment.
  glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
  GLint bits = 0;
  GLint type = 0;
  glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                        GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &bits);
  glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                        GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &type);
  int format = GL_DEPTH_COMPONENT24;
  if (bits == 16) format = GL_DEPTH_COMPONENT16;
  if (bits == 32) format = type == GL_FLOAT ? GL_DEPTH_COMPONENT32F : GL_DEPTH_COMPONENT32;

  if (dims != dims_ || format != internalFormat_ || texture_ == 0) {
    release();
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, format, dims[0], dims[1], 0, GL_DEPTH_COMPONENT, GL_FLOAT,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, texture_, 0);
    dims_ = dims;
    internalFormat_ = format;
  }

  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_);
  glBlitFramebuffer(0, 0, dims[0], dims[1], 0, 0, dims[0], dims[1], GL_DEPTH_BUFFER_BIT,
                    GL_NEAREST);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
  glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFbo));
  size_ = {static_cast<float>(dims[0]), static_cast<float>(dims[1])};
  return true;
}

}  // namespace x4sb::editor
