/* SPDX-License-Identifier: MIT */
#include "virgl_video_metal.h"
#include <CoreVideo/CoreVideo.h>
#include <Metal/Metal.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <pthread.h>

struct virgl_video_metal {
   atomic_uint refs;
   atomic_bool failed;
   pthread_mutex_t lock;
   id<MTLCommandQueue> queue;
   CVMetalTextureCacheRef cache;
   id<MTLComputePipelineState> split_uv, join_uv;
};

struct virgl_video_metal_fence {
   atomic_uint refs;
   atomic_int status;
   atomic_bool started;
   struct virgl_video_metal *video;
   id<MTLCommandBuffer> commands;
   id<MTLSharedEvent> before;
   uint64_t before_value;
   dispatch_group_t completion;
   void *textures[3];
   unsigned num_planes, width, height;
   bool p010, to_resources;
};

struct virgl_video_metal *virgl_video_metal_create(void *texture)
{
   if (!texture) return NULL;
   struct virgl_video_metal *video = calloc(1, sizeof(*video));
   if (!video) return NULL;
   atomic_init(&video->refs, 1);
   atomic_init(&video->failed, false);
   if (pthread_mutex_init(&video->lock, NULL)) { free(video); return NULL; }
   id<MTLDevice> device = [(id<MTLTexture>)texture device];
   video->queue = [device newCommandQueue];
   if (!video->queue ||
       CVMetalTextureCacheCreate(NULL, NULL, device, NULL, &video->cache)) {
      virgl_video_metal_destroy(video);
      return NULL;
   }
   video->queue.label = @"VGL video plane copies";
   return video;
}

void virgl_video_metal_destroy(struct virgl_video_metal *video)
{
   if (!video || atomic_fetch_sub(&video->refs, 1) != 1) return;
   if (video->cache) CFRelease(video->cache);
   [video->queue release];
   [video->split_uv release];
   [video->join_uv release];
   pthread_mutex_destroy(&video->lock);
   free(video);
}

bool virgl_video_metal_failed(struct virgl_video_metal *video)
{
   return video && atomic_load(&video->failed);
}

/* I420/YV12 and NV12 contain the same samples, only UV packing differs.
 * Compile once per video context; no intermediate texture or color conversion. */
static bool create_planar_pipelines(struct virgl_video_metal *video)
{
   if (video->split_uv && video->join_uv) return true;
   NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
      "kernel void split_uv(texture2d<float, access::read> uv [[texture(0)]],"
      " texture2d<float, access::write> u [[texture(1)]],"
      " texture2d<float, access::write> v [[texture(2)]],"
      " constant uint2 &size [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {"
      " if (any(p >= size)) return; float2 c = uv.read(p).rg;"
      " u.write(float4(c.x, 0, 0, 1), p); v.write(float4(c.y, 0, 0, 1), p); }\n"
      "kernel void join_uv(texture2d<float, access::write> uv [[texture(0)]],"
      " texture2d<float, access::read> u [[texture(1)]],"
      " texture2d<float, access::read> v [[texture(2)]],"
      " constant uint2 &size [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {"
      " if (any(p >= size)) return;"
      " uv.write(float4(u.read(p).r, v.read(p).r, 0, 1), p); }\n";
   id<MTLLibrary> library = [video->queue.device newLibraryWithSource:source options:nil error:NULL];
   if (!library) return false;
   id<MTLFunction> split = [library newFunctionWithName:@"split_uv"];
   id<MTLFunction> join = [library newFunctionWithName:@"join_uv"];
   [video->split_uv release];
   [video->join_uv release];
   video->split_uv = split ? [video->queue.device newComputePipelineStateWithFunction:split error:NULL] : nil;
   video->join_uv = join ? [video->queue.device newComputePipelineStateWithFunction:join error:NULL] : nil;
   [split release]; [join release]; [library release];
   return video->split_uv && video->join_uv;
}

struct virgl_video_metal_fence *virgl_video_metal_prepare(
   struct virgl_video_metal *video, void *const *textures, unsigned num_planes,
   unsigned width, unsigned height, bool p010, bool to_resources,
   void *wait_event, uint64_t wait_value)
{
   bool planar = num_planes == 3;
   if (!video || !textures || !width || !height ||
       (num_planes != 2 && !planar) || (planar && p010)) return NULL;
   for (unsigned p = 0; p < num_planes; p++) {
      id<MTLTexture> target = textures[p];
      MTLPixelFormat format = p010 ? (p ? MTLPixelFormatRG16Unorm : MTLPixelFormatR16Unorm)
         : (p && !planar ? MTLPixelFormatRG8Unorm : MTLPixelFormatR8Unorm);
      unsigned shift = p != 0;
      if (!target || target.device != video->queue.device || target.pixelFormat != format ||
          target.width < (((size_t)width + shift) >> shift) ||
          target.height < (((size_t)height + shift) >> shift) ||
          target.textureType != MTLTextureType2D || target.sampleCount != 1 ||
          (planar && p && target.usage &&
           !(target.usage & (to_resources ? MTLTextureUsageShaderWrite : MTLTextureUsageShaderRead))))
         return NULL;
   }
   struct virgl_video_metal_fence *fence = calloc(1, sizeof(*fence));
   if (!fence) return NULL;
   fence->commands = [[video->queue commandBuffer] retain];
   if (!fence->commands) { free(fence); return NULL; }
   atomic_init(&fence->refs, 1);
   atomic_init(&fence->status, 0);
   atomic_init(&fence->started, false);
   atomic_fetch_add(&video->refs, 1);
   fence->video = video;
   fence->before = [(id)wait_event retain];
   fence->before_value = wait_value;
   fence->num_planes = num_planes;
   fence->width = width; fence->height = height;
   fence->p010 = p010; fence->to_resources = to_resources;
   for (unsigned p = 0; p < num_planes; p++) fence->textures[p] = [(id)textures[p] retain];
   fence->completion = dispatch_group_create();
   dispatch_group_enter(fence->completion);
   if (fence->before)
      [fence->commands encodeWaitForEvent:fence->before value:fence->before_value];
   /* Preserve submission order even if asynchronous callbacks arrive out of order. */
   [fence->commands enqueue];
   return fence;
}

static void finish_copy(struct virgl_video_metal_fence *fence, bool success)
{
   if (!success) atomic_store(&fence->video->failed, true);
   atomic_store(&fence->status, success ? 1 : -1);
   dispatch_group_leave(fence->completion);
}

bool virgl_video_metal_complete(struct virgl_video_metal_fence *fence, void *pixel_buffer)
{
   if (!fence || atomic_exchange(&fence->started, true)) return false;
   CVPixelBufferRef pixels = pixel_buffer;
   bool p010 = fence->p010, planar = fence->num_planes == 3;
   bool to_resources = fence->to_resources;
   unsigned width = fence->width, height = fence->height;
   struct virgl_video_metal *video = fence->video;
   OSType format = p010 ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
                       : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
   CVMetalTextureRef views[2] = {NULL, NULL};
   bool valid = false;
   id<MTLCommandBuffer> commands = fence->commands;
   /* VT callbacks from different codecs may run concurrently. Serialize only
    * cache/pipeline access and command encoding, never the GPU completion. */
   pthread_mutex_lock(&video->lock);
   @autoreleasepool {
      if (!pixels || CVPixelBufferGetPlaneCount(pixels) != 2 ||
          CVPixelBufferGetPixelFormatType(pixels) != format) goto out;
      if (planar && !create_planar_pipelines(video)) goto out;
      NSDictionary *attrs = planar ? @{
         (id)kCVMetalTextureUsage: @(to_resources ? MTLTextureUsageShaderRead : MTLTextureUsageShaderWrite)
      } : nil;
      for (unsigned p = 0; p < 2; p++) {
         MTLPixelFormat plane_format = p010
            ? (p ? MTLPixelFormatRG16Unorm : MTLPixelFormatR16Unorm)
            : (p ? MTLPixelFormatRG8Unorm : MTLPixelFormatR8Unorm);
         if (CVPixelBufferGetWidthOfPlane(pixels, p) < (((size_t)width + p) >> p) ||
             CVPixelBufferGetHeightOfPlane(pixels, p) < (((size_t)height + p) >> p)) goto out;
         if (CVMetalTextureCacheCreateTextureFromImage(NULL, video->cache, pixels,
                (CFDictionaryRef)attrs, plane_format, CVPixelBufferGetWidthOfPlane(pixels, p),
                CVPixelBufferGetHeightOfPlane(pixels, p), p, &views[p])) goto out;
      }
      id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
      if (!blit) goto out;
      for (unsigned p = 0; p < (planar ? 1u : 2u); p++) {
         id<MTLTexture> native = CVMetalTextureGetTexture(views[p]);
         id<MTLTexture> resource = fence->textures[p];
         MTLSize size = MTLSizeMake(((size_t)width + p) >> p, ((size_t)height + p) >> p, 1);
         [blit copyFromTexture:to_resources ? native : resource
                  sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:size toTexture:to_resources ? resource : native
             destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
      }
      [blit endEncoding];
      if (planar) {
         id<MTLComputeCommandEncoder> compute = [commands computeCommandEncoder];
         if (!compute) goto out;
         unsigned size[2] = {(width + 1) / 2, (height + 1) / 2};
         [compute setComputePipelineState:to_resources ? video->split_uv : video->join_uv];
         [compute setTexture:CVMetalTextureGetTexture(views[1]) atIndex:0];
         [compute setTexture:(id<MTLTexture>)fence->textures[1] atIndex:1];
         [compute setTexture:(id<MTLTexture>)fence->textures[2] atIndex:2];
         [compute setBytes:size length:sizeof(size) atIndex:0];
         [compute dispatchThreads:MTLSizeMake(size[0], size[1], 1)
            threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
         [compute endEncoding];
      }
      valid = true;
   }
out:
   pthread_mutex_unlock(&video->lock);
   /* Even failure commits the reserved queue entry, so neither this frame nor
    * later frames leave a permanent hole in the queue. */
   CVMetalTextureRef y = views[0], uv = views[1];
   if (pixels) CFRetain(pixels);
   virgl_video_metal_fence_ref(fence);
   [commands addCompletedHandler:^(id<MTLCommandBuffer> completed) {
      if (completed.status != MTLCommandBufferStatusCompleted)
         NSLog(@"VGL video plane copy failed: %@", completed.error);
      finish_copy(fence, valid && completed.status == MTLCommandBufferStatusCompleted);
      if (y) CFRelease(y);
      if (uv) CFRelease(uv);
      if (pixels) CFRelease(pixels);
      virgl_video_metal_fence_unref(fence);
   }];
   [commands commit];
   return valid;
}

struct virgl_video_metal_fence *virgl_video_metal_copy(
   struct virgl_video_metal *video, void *pixel_buffer, void *const *textures,
   unsigned num_planes, unsigned width, unsigned height, bool p010,
   bool to_resources, void *wait_event, uint64_t wait_value)
{
   struct virgl_video_metal_fence *fence = virgl_video_metal_prepare(
      video, textures, num_planes, width, height, p010, to_resources, wait_event, wait_value);
   if (fence && !virgl_video_metal_complete(fence, pixel_buffer)) {
      virgl_video_metal_fence_unref(fence);
      return NULL;
   }
   return fence;
}

int virgl_video_metal_fence_wait(struct virgl_video_metal_fence *fence, bool block)
{
   if (!fence) return 1;
   if (block) dispatch_group_wait(fence->completion, DISPATCH_TIME_FOREVER);
   return atomic_load(&fence->status);
}

struct virgl_video_metal_fence *virgl_video_metal_fence_ref(struct virgl_video_metal_fence *fence)
{
   if (fence) atomic_fetch_add(&fence->refs, 1);
   return fence;
}

void virgl_video_metal_fence_unref(struct virgl_video_metal_fence *fence)
{
   if (!fence || atomic_fetch_sub(&fence->refs, 1) != 1) return;
   if (!atomic_load(&fence->started)) {
      atomic_store(&fence->refs, 1);
      virgl_video_metal_complete(fence, NULL);
      virgl_video_metal_fence_unref(fence);
      return;
   }
   for (unsigned p = 0; p < fence->num_planes; p++) [(id)fence->textures[p] release];
   [fence->commands release];
   [fence->before release];
   dispatch_release(fence->completion);
   virgl_video_metal_destroy(fence->video);
   free(fence);
}
