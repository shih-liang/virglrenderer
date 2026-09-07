/* SPDX-License-Identifier: MIT
 * Test-only blocking observation of the backend's asynchronous decode API.
 * Include after virgl_video_videotoolbox.c. Production has no such wrapper. */
#include <dispatch/dispatch.h>

struct test_decode {
   struct virgl_video_codec *codec;
   struct virgl_video_buffer *target;
   int (*inspect)(struct virgl_video_codec *, const struct virgl_video_dma_buf *);
   dispatch_semaphore_t done;
   int result;
};

static void test_decode_completed(void *data, void *pixels)
{
   struct test_decode *test = data;
   if (pixels) {
      CVPixelBufferRetain(pixels);
      if (test->target->pixel_buffer) CVPixelBufferRelease(test->target->pixel_buffer);
      test->target->pixel_buffer = pixels;
      test->result = !test->inspect || call_with_planes(test->codec, test->target,
         VIRGL_VIDEO_DMABUF_READ_ONLY, test->inspect) ? 0 : -1;
   }
   dispatch_semaphore_signal(test->done);
}

static int test_decode_wait(struct virgl_video_codec *codec, struct virgl_video_buffer *target,
                            struct byte_buffer *sample,
                            int (*inspect)(struct virgl_video_codec *, const struct virgl_video_dma_buf *))
{
   struct test_decode test = {codec, target, inspect, dispatch_semaphore_create(0), -1};
   int result = sample
      ? submit_decode_sample(codec, target, sample, test_decode_completed, &test)
      : virgl_video_end_frame_async(codec, target, test_decode_completed, &test);
   if (!result) {
      if (dispatch_semaphore_wait(test.done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)))
         abort(); /* Do not let stack callback data outlive a failed test. */
      result = test.result;
   }
   dispatch_release(test.done);
   return result;
}
