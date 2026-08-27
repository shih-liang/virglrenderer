#include "vrend_angle_blob_cache.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CACHE_ENV "NATIVEPIPE_ANGLE_CACHE_DIR"
#define MAX_KEY_SIZE 64
#define MAX_BLOB_SIZE (16 * 1024 * 1024)
#define MAX_CACHE_SIZE (64 * 1024 * 1024)

static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static int cache_dir = -1;

static bool has_extension(const char *extensions, const char *name)
{
   size_t length = strlen(name);
   const char *match = extensions;

   while (match && (match = strstr(match, name))) {
      if ((match == extensions || match[-1] == ' ') &&
          (match[length] == '\0' || match[length] == ' '))
         return true;
      match += length;
   }
   return false;
}

static bool key_name(const void *key, EGLsizeiANDROID key_size,
                     char name[MAX_KEY_SIZE * 2 + 1])
{
   static const char hex[] = "0123456789abcdef";
   const uint8_t *bytes = key;

   if (!key || key_size <= 0 || key_size > MAX_KEY_SIZE)
      return false;
   for (EGLsizeiANDROID i = 0; i < key_size; i++) {
      name[i * 2] = hex[bytes[i] >> 4];
      name[i * 2 + 1] = hex[bytes[i] & 0xf];
   }
   name[key_size * 2] = '\0';
   return true;
}

static bool write_all(int fd, const uint8_t *data, size_t size)
{
   while (size) {
      ssize_t written = write(fd, data, size);
      if (written < 0) {
         if (errno == EINTR)
            continue;
         return false;
      }
      data += written;
      size -= (size_t)written;
   }
   return true;
}

static bool read_all(int fd, uint8_t *data, size_t size)
{
   while (size) {
      ssize_t count = read(fd, data, size);
      if (count < 0) {
         if (errno == EINTR)
            continue;
         return false;
      }
      if (count == 0)
         return false;
      data += count;
      size -= (size_t)count;
   }
   return true;
}

static void EGLAPIENTRY cache_set(const void *key, EGLsizeiANDROID key_size,
                                  const void *value, EGLsizeiANDROID value_size)
{
   char name[MAX_KEY_SIZE * 2 + 1];
   char temporary[sizeof(name) + 48];

   if (!value || value_size <= 0 || value_size > MAX_BLOB_SIZE ||
       !key_name(key, key_size, name))
      return;

   pthread_mutex_lock(&cache_lock);
   if (cache_dir >= 0) {
      snprintf(temporary, sizeof(temporary), ".%s.%ld", name, (long)getpid());
      int fd = openat(cache_dir, temporary,
                      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                      0600);
      if (fd >= 0) {
         bool complete = write_all(fd, value, (size_t)value_size);
         close(fd);
         if (complete)
            renameat(cache_dir, temporary, cache_dir, name);
         else
            unlinkat(cache_dir, temporary, 0);
      }
   }
   pthread_mutex_unlock(&cache_lock);
}

static EGLsizeiANDROID EGLAPIENTRY cache_get(const void *key,
                                             EGLsizeiANDROID key_size,
                                             void *value,
                                             EGLsizeiANDROID value_size)
{
   char name[MAX_KEY_SIZE * 2 + 1];
   EGLsizeiANDROID result = 0;

   if (!key_name(key, key_size, name))
      return 0;

   pthread_mutex_lock(&cache_lock);
   if (cache_dir >= 0) {
      int fd = openat(cache_dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
      if (fd >= 0) {
         struct stat statbuf;
         if (fstat(fd, &statbuf) == 0 && S_ISREG(statbuf.st_mode) &&
             statbuf.st_size > 0 && statbuf.st_size <= MAX_BLOB_SIZE) {
            result = (EGLsizeiANDROID)statbuf.st_size;
            if (value && value_size >= result &&
                !read_all(fd, value, (size_t)result))
               result = 0;
         }
         close(fd);
      }
   }
   pthread_mutex_unlock(&cache_lock);
   return result;
}

static void trim_cache(void)
{
   DIR *directory = fdopendir(dup(cache_dir));
   struct dirent *entry;
   off_t total = 0;

   if (!directory)
      return;
   while ((entry = readdir(directory))) {
      struct stat statbuf;
      if (entry->d_name[0] == '.' ||
          fstatat(cache_dir, entry->d_name, &statbuf, AT_SYMLINK_NOFOLLOW) != 0 ||
          !S_ISREG(statbuf.st_mode))
         continue;
      total += statbuf.st_size;
   }
   rewinddir(directory);
   while (total > MAX_CACHE_SIZE && (entry = readdir(directory))) {
      struct stat statbuf;
      if (entry->d_name[0] == '.' ||
          fstatat(cache_dir, entry->d_name, &statbuf, AT_SYMLINK_NOFOLLOW) != 0 ||
          !S_ISREG(statbuf.st_mode))
         continue;
      if (unlinkat(cache_dir, entry->d_name, 0) == 0)
         total -= statbuf.st_size;
   }
   closedir(directory);
}

bool vrend_angle_blob_cache_enable(EGLDisplay display, const char *extensions)
{
   const char *path = getenv(CACHE_ENV);
   PFNEGLSETBLOBCACHEFUNCSANDROIDPROC set_cache;

   if (!path || !path[0] ||
       !has_extension(extensions, "EGL_ANDROID_blob_cache"))
      return false;

   pthread_mutex_lock(&cache_lock);
   if (cache_dir < 0) {
      cache_dir = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (cache_dir >= 0)
         trim_cache();
   }
   pthread_mutex_unlock(&cache_lock);
   if (cache_dir < 0)
      return false;

   set_cache = (PFNEGLSETBLOBCACHEFUNCSANDROIDPROC)
      eglGetProcAddress("eglSetBlobCacheFuncsANDROID");
   if (!set_cache)
      return false;
   /* Do not report an unrelated error left by earlier EGL initialization. */
   (void)eglGetError();
   set_cache(display, cache_set, cache_get);
   return eglGetError() == EGL_SUCCESS;
}
