/* SPDX-License-Identifier: MIT */
#ifndef VIRGL_VIDEO_METAL_H
#define VIRGL_VIDEO_METAL_H

#include <stdbool.h>
#include <stdint.h>

/* Native video storage/transfer boundary. No EGL, GL or Vulkan dependency.
 * Textures are borrowed from ordinary shared resources, never replaced.
 * Supply Y/UV for NV12/P010, or Y/U/V for 8-bit planar 4:2:0. */
struct virgl_video_metal;
struct virgl_video_metal_fence;
struct virgl_video_metal *virgl_video_metal_create(void *texture);
void virgl_video_metal_destroy(struct virgl_video_metal *video);
bool virgl_video_metal_failed(struct virgl_video_metal *video);
/* Reserve completion before asynchronous decode. The reservation owns the
 * exact target textures and prior GPU dependency, not renderer/context pointers.
 * complete(NULL) reports decode failure and completes the reservation. */
struct virgl_video_metal_fence *virgl_video_metal_prepare(
   struct virgl_video_metal *video, void *const *textures, unsigned num_planes,
   unsigned width, unsigned height, bool p010, bool to_resources,
   void *wait_event, uint64_t wait_value);
bool virgl_video_metal_complete(struct virgl_video_metal_fence *fence, void *pixel_buffer);
struct virgl_video_metal_fence *virgl_video_metal_fence_ref(struct virgl_video_metal_fence *fence);
struct virgl_video_metal_fence *virgl_video_metal_copy(
   struct virgl_video_metal *video, void *pixel_buffer,
   void *const *textures, unsigned num_planes,
   unsigned width, unsigned height, bool p010,
   bool to_resources, void *wait_event, uint64_t wait_value);
/* Result: 0 pending, 1 complete, -1 failed. No callback uses the submitting
 * context: destroying it while a copy is running is safe. */
int virgl_video_metal_fence_wait(struct virgl_video_metal_fence *fence, bool block);
void virgl_video_metal_fence_unref(struct virgl_video_metal_fence *fence);

#endif
