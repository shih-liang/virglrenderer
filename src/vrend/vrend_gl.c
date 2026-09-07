/* SPDX-License-Identifier: MIT */
#include "vrend_gl.h"
#include <string.h>

int vrend_gl_version(void)
{
   GLint major = 0, minor = 0;
   glGetIntegerv(GL_MAJOR_VERSION, &major);
   glGetIntegerv(GL_MINOR_VERSION, &minor);
   return major * 10 + minor;
}

bool vrend_is_desktop_gl(void)
{
   const char *version = (const char *)glGetString(GL_VERSION);
   return version && strncmp(version, "OpenGL ES", 9) != 0;
}

bool vrend_has_extension(const char *extensions, const char *extension)
{
   if (!extensions || !extension || !*extension || strchr(extension, ' '))
      return false;
   size_t length = strlen(extension);
   for (const char *p = extensions; (p = strstr(p, extension)); p += length)
      if ((p == extensions || p[-1] == ' ') && (!p[length] || p[length] == ' '))
         return true;
   return false;
}

bool vrend_has_gl_extension(const char *extension)
{
   GLint count = 0;
   if (!extension || !*extension) return false;
   glGetIntegerv(GL_NUM_EXTENSIONS, &count);
   for (GLint i = 0; i < count; i++) {
      const char *name = (const char *)glGetStringi(GL_EXTENSIONS, i);
      if (name && !strcmp(name, extension)) return true;
   }
   return false;
}
