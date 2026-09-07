/* SPDX-License-Identifier: MIT
 * Direct EGL/GL entry points. No loader or per-context dispatch table. */
#ifndef VREND_GL_H
#define VREND_GL_H

#include <stdbool.h>
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#ifndef EGL_EGLEXT_PROTOTYPES
#define EGL_EGLEXT_PROTOTYPES 1
#endif
#ifdef ENABLE_ANGLE
#include <angle_gl.h>
/* Khronos desktop tokens describe VirGL formats even when GLES emulates them. */
#include <GL/glcorearb.h>
#include <GL/glext.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif
#include <EGL/egl.h>
#include <EGL/eglext.h>
#ifdef ENABLE_ANGLE
#include <EGL/eglext_angle.h>

/* Use extension entry points even when ANGLE also exports the newer core
 * spelling: core GLES 3.2 calls reject a GLES 3.0 context. Capability checks
 * remain at each VGL call site. These are compile-time aliases, not a loader. */
#define glBindBufferARB glBindBuffer
#define glGenBuffersARB glGenBuffers
#define glBindFragDataLocationIndexed glBindFragDataLocationIndexedEXT
#define glBlendEquationSeparateiARB glBlendEquationSeparateiOES
#define glBlendFuncSeparateiARB glBlendFuncSeparateiOES
#define glBufferStorage glBufferStorageEXT
#define glClearTexSubImage glClearTexSubImageEXT
#define glClipControl glClipControlEXT
#define glColorMaskIndexedEXT glColorMaskiOES
#define glEnableIndexedEXT glEnableiOES
#define glDisableIndexedEXT glDisableiOES
#define glCopyImageSubData glCopyImageSubDataEXT
#define glDebugMessageCallback glDebugMessageCallbackKHR
#define glDebugMessageInsert glDebugMessageInsertKHR
#define glDrawArraysInstancedARB glDrawArraysInstanced
#define glDrawElementsInstancedARB glDrawElementsInstanced
#define glDrawArraysInstancedBaseInstance glDrawArraysInstancedBaseInstanceEXT
#define glDrawElementsInstancedBaseInstance glDrawElementsInstancedBaseInstanceEXT
#define glDrawElementsInstancedBaseVertexBaseInstance glDrawElementsInstancedBaseVertexBaseInstanceEXT
#define glDrawElementsBaseVertex glDrawElementsBaseVertexEXT
#define glDrawRangeElementsBaseVertex glDrawRangeElementsBaseVertexEXT
#define glDrawElementsInstancedBaseVertex glDrawElementsInstancedBaseVertexEXT
#define glFramebufferTexture glFramebufferTextureOES
#define glGetCompressedTexImage glGetCompressedTexImageANGLE
#define glGetTexImage glGetTexImageANGLE
#define glGetQueryObjecti64v glGetQueryObjecti64vEXT
#define glGetQueryObjectiv glGetQueryObjectivEXT
#define glGetQueryObjectui64v glGetQueryObjectui64vEXT
#define glMultiDrawArraysIndirect glMultiDrawArraysIndirectEXT
#define glMultiDrawElementsIndirect glMultiDrawElementsIndirectEXT
#define glMinSampleShading glMinSampleShadingOES
#define glPatchParameteri glPatchParameteriEXT
#define glQueryCounter glQueryCounterEXT
#define glReadnPixels glReadnPixelsKHR
#define glReadnPixelsARB glReadnPixelsKHR
#define glSamplerParameterIuiv glSamplerParameterIuivEXT
#define glTexParameterIuiv glTexParameterIuivEXT
#define glTexBuffer glTexBufferEXT
#define glTexBufferRange glTexBufferRangeEXT
#define glTexStorage3DMultisample glTexStorage3DMultisampleOES
#define glVertexAttribDivisorARB glVertexAttribDivisor

/* Desktop format names used by the shared VirGL format table. */
#define GL_ALPHA8 GL_ALPHA8_EXT
#define GL_ALPHA16 GL_ALPHA16_EXT
#ifndef GL_ALPHA_INTEGER
#define GL_ALPHA_INTEGER GL_ALPHA_INTEGER_EXT
#endif
#endif

int vrend_gl_version(void);
bool vrend_is_desktop_gl(void);
bool vrend_has_gl_extension(const char *extension);
bool vrend_has_extension(const char *extensions, const char *extension);

#endif
