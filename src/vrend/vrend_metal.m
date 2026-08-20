/*
 * Copyright 2025 Turing Software, LLC
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "virglrenderer.h"
#include "vrend_metal.h"
#include "pipe/p_state.h"
#include "util/u_math.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <Metal/Metal.h>

typedef void *(*retain_heap_buffer_fn)(void *);
typedef void *(*retain_heap_texture_fn)(void *);
static retain_heap_buffer_fn retain_heap_buffer;
static retain_heap_texture_fn retain_heap_texture;
static pthread_once_t retain_heap_buffer_once = PTHREAD_ONCE_INIT;

static void lookup_heap_buffer_accessor(void)
{
   /* MoltenVK is already loaded by LightHouse as Frameworks/libMoltenVK.dylib.
    * dlopen("libMoltenVK.dylib") does not search that directory, so scanout
    * would fall back to a second overlapping heap buffer and present black.
    */
   retain_heap_buffer = (retain_heap_buffer_fn)dlsym(
      RTLD_DEFAULT, "np_mvk_retain_heap_backing_buffer");
   retain_heap_texture = (retain_heap_texture_fn)dlsym(
      RTLD_DEFAULT, "np_mvk_retain_heap_render_texture");
   if (getenv("NATIVEPIPE_GPU_TRACE"))
      fprintf(stderr, "[metal] heap accessors buffer=%p texture=%p\n",
              (void *)retain_heap_buffer, (void *)retain_heap_texture);
}

struct metal_format_conversion {
   uint32_t virgl_format;
   MTLPixelFormat metal_format;
};

static bool virgl_format_to_metal_format(uint32_t format, MTLPixelFormat *metal_format)
{
   static const struct metal_format_conversion conversions[] = {
      { VIRGL_FORMAT_R8G8B8A8_UNORM, MTLPixelFormatRGBA8Unorm },
      { VIRGL_FORMAT_R8G8B8A8_SRGB, MTLPixelFormatRGBA8Unorm_sRGB },
      { VIRGL_FORMAT_B8G8R8X8_UNORM, MTLPixelFormatBGRA8Unorm },
      { VIRGL_FORMAT_B8G8R8A8_UNORM, MTLPixelFormatBGRA8Unorm },
      { VIRGL_FORMAT_B8G8R8A8_SRGB, MTLPixelFormatBGRA8Unorm_sRGB },
      { VIRGL_FORMAT_R16G16B16A16_FLOAT, MTLPixelFormatRGBA16Float },
      { VIRGL_FORMAT_R32G32B32A32_FLOAT, MTLPixelFormatRGBA32Float },
      { VIRGL_FORMAT_R10G10B10A2_UNORM, MTLPixelFormatRGB10A2Unorm },
      { VIRGL_FORMAT_R8_UNORM, MTLPixelFormatR8Unorm },
      { VIRGL_FORMAT_R16_UNORM, MTLPixelFormatR16Unorm },
      { VIRGL_FORMAT_R8G8_UNORM, MTLPixelFormatRG8Unorm },
      { VIRGL_FORMAT_R16G16_UNORM, MTLPixelFormatRG16Unorm },
   };

   for (uint32_t i = 0; i < ARRAY_SIZE(conversions); i++) {
      if (conversions[i].virgl_format == format) {
         *metal_format = conversions[i].metal_format;
         return true;
      }
   }

   return false;
}

static MTLTextureUsage virgl_bind_to_metal_usage_flags(uint32_t flags)
{
   MTLTextureUsage ret = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;

   if (flags & PIPE_BIND_RENDER_TARGET)
      ret |= MTLTextureUsageRenderTarget;
   if (flags & PIPE_BIND_DEPTH_STENCIL)
      ret |= MTLTextureUsageRenderTarget;

   return ret;
}

static MTLResourceOptions virgl_usage_to_metal_resource_options(uint32_t usage)
{
   switch (usage) {
   case PIPE_USAGE_DEFAULT:
   case PIPE_USAGE_STAGING:
   default:
      return MTLResourceStorageModeShared | MTLResourceCPUCacheModeDefaultCache;
   case PIPE_USAGE_IMMUTABLE:
      return MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined | MTLHazardTrackingModeUntracked;
   case PIPE_USAGE_DYNAMIC:
   case PIPE_USAGE_STREAM:
      return MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined;
   }
}

static MTLTextureDescriptor *new_descriptor(const struct vrend_metal_texture_description *desc)
{
   MTLPixelFormat pixel_format;

   if (!virgl_format_to_metal_format(desc->format, &pixel_format)) {
      return NULL;
   }

   MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
   descriptor.textureType = MTLTextureType2D;
   descriptor.pixelFormat = pixel_format;
   descriptor.width = desc->width;
   descriptor.height = desc->height;
   descriptor.resourceOptions = virgl_usage_to_metal_resource_options(desc->usage);
   descriptor.usage = virgl_bind_to_metal_usage_flags(desc->bind);
   if (desc->usage == PIPE_USAGE_IMMUTABLE) {
      descriptor.usage &= ~MTLTextureUsageShaderWrite;
   }

   return descriptor;
}

bool virgl_metal_create_texture(MTLDevice_id device,
                                const struct vrend_metal_texture_description *desc,
                                MTLTexture_id *tex)
{
   id<MTLDevice> mtl_device = (id<MTLDevice>)device;
   MTLTextureDescriptor *descriptor = new_descriptor(desc);
   if (descriptor) {
      *tex = [mtl_device newTextureWithDescriptor:descriptor];
      [descriptor release];
      return true;
   }

   return false;
}

bool virgl_metal_create_texture_from_heap(MTLHeap_id heap,
                                          const struct vrend_metal_texture_description *desc,
                                          MTLTexture_id *tex)
{
   id<MTLHeap> mtl_heap = (id<MTLHeap>)heap;
   id<MTLDevice> mtl_device = mtl_heap.device;
   MTLTextureDescriptor *descriptor = new_descriptor(desc);
   *tex = nil;
   if (descriptor) {
      NSUInteger deviceAlignment, bytesPerRow;
      pthread_once(&retain_heap_buffer_once, lookup_heap_buffer_accessor);
      id<MTLTexture> render_texture = retain_heap_texture
         ? (id<MTLTexture>)retain_heap_texture((void *)mtl_heap)
         : nil;
      if (render_texture) {
         if (render_texture.width == desc->width &&
             render_texture.height == desc->height) {
            if (getenv("NATIVEPIPE_GPU_TRACE"))
               fprintf(stderr,
                       "[metal] exact heap=%p render texture=%p %lux%lu row=%u format=%lu\n",
                       (void *)mtl_heap, (void *)render_texture,
                       (unsigned long)render_texture.width,
                       (unsigned long)render_texture.height, desc->stride,
                       (unsigned long)render_texture.pixelFormat);
            *tex = render_texture;
            [descriptor release];
            return true;
         }
         if (getenv("NATIVEPIPE_GPU_TRACE"))
            fprintf(stderr,
                    "[metal] rejected render texture %p %lux%lu, expected %ux%u\n",
                    (void *)render_texture, (unsigned long)render_texture.width,
                    (unsigned long)render_texture.height, desc->width, desc->height);
         [render_texture release];
      }
      /* swap B/R for existing texture */
      if (desc->format == VIRGL_FORMAT_B8G8R8X8_UNORM || desc->format == VIRGL_FORMAT_B8G8R8A8_UNORM) {
         descriptor.pixelFormat = MTLPixelFormatRGBA8Unorm;
      }
      /* Regardless of what we want, we have to respect the heap's options */
      descriptor.resourceOptions = mtl_heap.resourceOptions;
      deviceAlignment = [mtl_device minimumLinearTextureAlignmentForPixelFormat:descriptor.pixelFormat];
      bytesPerRow = align(desc->stride, deviceAlignment);
      /* MoltenVK backs a linear VkImage with an MTLBuffer allocated from this
       * placement heap. Creating a second overlapping buffer here makes the
       * original allocation aliasable/undefined when the guest reuses its
       * swapchain image. Ask the bundled MoltenVK for the exact buffer and
       * create only a texture view of it.
       *
       * Keep the old heap path as a compatibility fallback for renderers that
       * do not expose the NativePipe accessor.
       */
      id<MTLBuffer> mtl_buffer = retain_heap_buffer
         ? (id<MTLBuffer>)retain_heap_buffer((void *)mtl_heap)
         : nil;
      const bool borrowed_backing = mtl_buffer != nil;
      if (!mtl_buffer) {
         mtl_buffer = [mtl_heap newBufferWithLength:bytesPerRow * desc->height
                                            options:mtl_heap.resourceOptions
                                             offset:desc->offset];
      }
      if (mtl_buffer) {
         *tex = [mtl_buffer newTextureWithDescriptor:descriptor
                                              offset:borrowed_backing ? desc->offset : 0
                                         bytesPerRow:bytesPerRow];
         [mtl_buffer release];
      }
      [descriptor release];
      return !!*tex;
   }

   return false;
}

uint64_t virgl_metal_heap_size(MTLHeap_id heap)
{
   id<MTLHeap> mtl_heap = (id<MTLHeap>)heap;

   return mtl_heap ? (uint64_t)mtl_heap.size : 0;
}

void virgl_metal_release_texture(MTLTexture_id tex)
{
   id<MTLTexture> mtl_texture = (id<MTLTexture>)tex;

   [mtl_texture release];
}
