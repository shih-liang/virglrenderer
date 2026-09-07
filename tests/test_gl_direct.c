/* Direct-link extension entry points must work in a GLES 3.0 context.
 * Exporting a GLES 3.2 symbol does not make it legal in a 3.0 context. */
#include "vrend/vrend_gl.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures, features;
#define CHECK(expr) do { checks++; if (!(expr)) { \
   fprintf(stderr, "FAIL %d: %s\n", __LINE__, #expr); failures++; \
} } while (0)

int main(void)
{
   CHECK(vrend_has_extension("GL_A GL_AB GL_B", "GL_A"));
   CHECK(!vrend_has_extension("XGL_A GL_AB", "GL_A"));
   CHECK(!vrend_has_extension("GL_A", ""));
   CHECK(!vrend_has_extension(NULL, "GL_A"));
   const EGLint attributes[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE,
      EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE};
   EGLDisplay display = eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, NULL, attributes);
   CHECK(eglInitialize(display, NULL, NULL));
   CHECK(eglBindAPI(EGL_OPENGL_ES_API));
   EGLConfig config;
   EGLint count;
   const EGLint config_attributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
   CHECK(eglChooseConfig(display, config_attributes, &config, 1, &count) && count);
   const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3,
      EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
   EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
   CHECK(context != EGL_NO_CONTEXT);
   CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context));
   if (failures) return 1;
   CHECK(vrend_gl_version() == 30);
   CHECK(!vrend_is_desktop_gl());

   if (vrend_has_gl_extension("GL_OES_draw_buffers_indexed")) {
      features++;
      glEnableIndexedEXT(GL_BLEND, 0);
      CHECK(glGetError() == GL_NO_ERROR);
      glBlendEquationSeparateiARB(0, GL_FUNC_ADD, GL_FUNC_ADD);
      CHECK(glGetError() == GL_NO_ERROR);
      glBlendFuncSeparateiARB(0, GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
      CHECK(glGetError() == GL_NO_ERROR);
      glColorMaskIndexedEXT(0, GL_TRUE, GL_FALSE, GL_TRUE, GL_TRUE);
      CHECK(glGetError() == GL_NO_ERROR);
      glColorMaskIndexedEXT(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      glDisableIndexedEXT(GL_BLEND, 0);
      CHECK(glGetError() == GL_NO_ERROR);
   }
   if (vrend_has_gl_extension("GL_KHR_debug")) {
      features++;
      glDebugMessageCallback(NULL, NULL);
      glDebugMessageInsert(GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_MARKER,
                          1, GL_DEBUG_SEVERITY_NOTIFICATION, -1, "direct ANGLE");
      CHECK(glGetError() == GL_NO_ERROR);
   }
   if (vrend_has_gl_extension("GL_EXT_copy_image")) {
      features++;
      GLuint textures[2], framebuffer;
      const unsigned char expected[4] = {29, 71, 151, 255};
      unsigned char actual[4] = {0};
      glGenTextures(2, textures);
      for (unsigned i = 0; i < 2; i++) {
         glBindTexture(GL_TEXTURE_2D, textures[i]);
         glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1);
      }
      glBindTexture(GL_TEXTURE_2D, textures[0]);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, expected);
      CHECK(glGetError() == GL_NO_ERROR);
      glCopyImageSubData(textures[0], GL_TEXTURE_2D, 0, 0, 0, 0,
                         textures[1], GL_TEXTURE_2D, 0, 0, 0, 0, 1, 1, 1);
      CHECK(glGetError() == GL_NO_ERROR);
      glGenFramebuffers(1, &framebuffer);
      glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[1], 0);
      CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
      glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, actual);
      CHECK(glGetError() == GL_NO_ERROR);
      CHECK(!memcmp(actual, expected, sizeof(actual)));
      glDeleteFramebuffers(1, &framebuffer);
      glDeleteTextures(2, textures);
   }
   CHECK(glGetError() == GL_NO_ERROR);
   CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
   CHECK(eglDestroyContext(display, context));
   CHECK(eglTerminate(display));
   printf("direct ANGLE: %u extension groups, %u checks, %u failures\n", features, checks, failures);
   return failures ? 1 : 0;
}
