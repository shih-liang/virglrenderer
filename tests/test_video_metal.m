/* SPDX-License-Identifier: MIT
 * Native plane-copy/lifetime test. Deliberately not linked to ANGLE/Vulkan. */
#include "virgl_video_metal.h"
#include <CoreVideo/CoreVideo.h>
#include <Metal/Metal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
   fprintf(stderr, "FAIL %u: %s\n", __LINE__, #c); } } while (0)

static CVPixelBufferRef pixels(bool p010, unsigned width, unsigned height)
{
   NSDictionary *attrs = @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{},
                           (id)kCVPixelBufferMetalCompatibilityKey: @YES};
   CVPixelBufferRef result = NULL;
   CHECK(CVPixelBufferCreate(NULL, width, height,
      p010 ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
           : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
      (CFDictionaryRef)attrs, &result) == kCVReturnSuccess);
   return result;
}

static void test_format(id<MTLDevice> device, bool p010, bool planar)
{
   unsigned width = 130, height = 126;
   CVPixelBufferRef src = pixels(p010, width, height), dst = pixels(p010, width, height);
   if (!src || !dst) return;
   unsigned num_planes = planar ? 3 : 2;
   void *textures[3] = {NULL, NULL, NULL};
   for (unsigned p = 0; p < num_planes; p++) {
      MTLPixelFormat format = p010 ? (p ? MTLPixelFormatRG16Unorm : MTLPixelFormatR16Unorm)
                                  : (p && !planar ? MTLPixelFormatRG8Unorm : MTLPixelFormatR8Unorm);
      MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
         width:(width >> (p != 0)) height:(height >> (p != 0)) mipmapped:NO];
      desc.storageMode = MTLStorageModePrivate;
      desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
      textures[p] = [device newTextureWithDescriptor:desc];
      CHECK(textures[p]);
   }
   struct virgl_video_metal *video = virgl_video_metal_create(textures[0]);
   CHECK(video);
   if (!video) goto out;
   for (unsigned frame = 0; frame < 8; frame++) {
      CHECK(CVPixelBufferLockBaseAddress(src, 0) == 0);
      for (unsigned p = 0; p < 2; p++) {
         uint8_t *data = CVPixelBufferGetBaseAddressOfPlane(src, p);
         size_t stride = CVPixelBufferGetBytesPerRowOfPlane(src, p);
         unsigned row_bytes = width * (p010 ? 2 : 1);
         for (unsigned y = 0; y < height >> p; y++)
            for (unsigned x = 0; x < row_bytes; x++)
               data[y * stride + x] = (x * 3 + y + frame * 19 + p * 127) & 255;
      }
      CVPixelBufferUnlockBaseAddress(src, 0);
      id<MTLSharedEvent> gate = [device newSharedEvent];
      struct virgl_video_metal_fence *decode = virgl_video_metal_prepare(
         video, textures, num_planes, width, height, p010, true, gate, 1);
      CHECK(decode && virgl_video_metal_fence_wait(decode, false) == 0);
      struct virgl_video_metal_fence *encode = virgl_video_metal_prepare(
         video, textures, num_planes, width, height, p010, false, NULL, 0);
      CHECK(encode);
      /* Deliberately complete B before A. Enqueue reservations must preserve
       * A -> B even when asynchronous decoder callbacks arrive out of order. */
      CHECK(virgl_video_metal_complete(encode, dst));
      CHECK(virgl_video_metal_fence_wait(encode, false) == 0);
      CHECK(virgl_video_metal_complete(decode, src));
      gate.signaledValue = 1;
      CHECK(virgl_video_metal_fence_wait(encode, true) == 1);
      CHECK(virgl_video_metal_fence_wait(decode, true) == 1);
      CHECK(CVPixelBufferLockBaseAddress(src, kCVPixelBufferLock_ReadOnly) == 0);
      CHECK(CVPixelBufferLockBaseAddress(dst, kCVPixelBufferLock_ReadOnly) == 0);
      bool equal = true;
      for (unsigned p = 0; p < 2; p++) {
         const uint8_t *a = CVPixelBufferGetBaseAddressOfPlane(src, p);
         const uint8_t *b = CVPixelBufferGetBaseAddressOfPlane(dst, p);
         for (unsigned y = 0; y < height >> p; y++)
            equal &= !memcmp(a + y * CVPixelBufferGetBytesPerRowOfPlane(src, p),
                             b + y * CVPixelBufferGetBytesPerRowOfPlane(dst, p), width * (p010 ? 2 : 1));
      }
      CHECK(equal);
      CVPixelBufferUnlockBaseAddress(dst, kCVPixelBufferLock_ReadOnly);
      CVPixelBufferUnlockBaseAddress(src, kCVPixelBufferLock_ReadOnly);
      virgl_video_metal_fence_unref(decode);
      virgl_video_metal_fence_unref(encode);
      [gate release];
   }
   CHECK(!virgl_video_metal_copy(video, src, textures, num_planes, width + 1, height, p010, true, NULL, 0));
   CHECK(!virgl_video_metal_copy(video, src, textures, num_planes, width, height, !p010, true, NULL, 0));
   struct virgl_video_metal_fence *rejected = virgl_video_metal_prepare(
      video, textures, num_planes, width, height, p010, true, NULL, 0);
   CHECK(rejected && !virgl_video_metal_complete(rejected, NULL));
   CHECK(virgl_video_metal_fence_wait(rejected, true) == -1);
   CHECK(virgl_video_metal_failed(video));
   virgl_video_metal_fence_unref(rejected);
   id<MTLSharedEvent> gate = [device newSharedEvent];
   struct virgl_video_metal_fence *pending = virgl_video_metal_prepare(
      video, textures, num_planes, width, height, p010, true, gate, 1);
   CHECK(pending);
   virgl_video_metal_destroy(video);
   CHECK(virgl_video_metal_complete(pending, src));
   CFRelease(src); src = NULL;
   gate.signaledValue = 1;
   CHECK(virgl_video_metal_fence_wait(pending, true) == 1);
   virgl_video_metal_fence_unref(pending);
   [gate release];
out:
   for (unsigned p = 0; p < num_planes; p++) [(id)textures[p] release];
   if (src) CFRelease(src);
   CFRelease(dst);
}

int main(void)
{
   alarm(30);
   @autoreleasepool {
      id<MTLDevice> device = MTLCreateSystemDefaultDevice();
      CHECK(device);
      if (device) {
         test_format(device, false, false);
         test_format(device, true, false);
         test_format(device, false, true);
      }
      [device release];
   }
   printf("native video copies: %u checks, %u failures\n", checks, failures);
   return failures != 0;
}
