/* Exercise video through the public VirGL command/resource API and static ANGLE. */
#include "vrend/vrend_gl.h"
#include <EGL/eglext_angle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#include <Metal/Metal.h>
#include <stdatomic.h>
#include "virglrenderer.h"
#include "virgl_hw.h"
#include "virgl_protocol.h"
#include "virgl_video_hw.h"
#include "pipe/p_state.h"
#include "pipe/p_video_enums.h"
#include "pipe/p_video_state.h"

/* Wire values from system Mesa 26.2, independent of VGL's copied headers. */
enum { MESA_H264_HIGH = 11, MESA_CHROMA_420 = 2 };
_Static_assert(PIPE_VIDEO_PROFILE_HEVC_MAIN == 15, "Mesa HEVC Main wire ABI");
_Static_assert(PIPE_VIDEO_PROFILE_HEVC_MAIN_10 == 16, "Mesa HEVC Main10 wire ABI");
_Static_assert(PIPE_VIDEO_PROFILE_VP9_PROFILE0 == 24, "Mesa VP9 wire ABI");
_Static_assert(PIPE_VIDEO_PROFILE_AV1_MAIN == 26, "Mesa AV1 wire ABI");

static EGLDisplay display;
static EGLConfig config;
static unsigned context_id = 1;
static unsigned checks, failures;
static atomic_uint_fast64_t retired_fence;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); failures++; \
} } while (0)
static void *create_context(void *cookie, int scanout, struct virgl_renderer_gl_ctx_param *p)
{
    (void)cookie; (void)scanout;
    const EGLint attrs[] = {EGL_CONTEXT_MAJOR_VERSION, p->major_ver,
        EGL_CONTEXT_MINOR_VERSION, p->minor_ver, EGL_NONE};
    return eglCreateContext(display, config, p->shared ? eglGetCurrentContext() : EGL_NO_CONTEXT, attrs);
}
static void destroy_context(void *cookie, void *context)
{ (void)cookie; eglDestroyContext(display, context); }
static int make_current(void *cookie, int scanout, void *context)
{ (void)cookie; (void)scanout; return eglMakeCurrent(display, NULL, NULL, context) ? 0 : -1; }
static void *get_display(void *cookie) { (void)cookie; return display; }
static void fence(void *cookie, uint32_t id) { (void)cookie; (void)id; }
static void context_fence(void *cookie, uint32_t ctx, uint32_t ring, uint64_t id)
{ (void)cookie; (void)ctx; (void)ring; retired_fence = id; }
static void resource(unsigned id, unsigned format, unsigned width, unsigned height,
                     unsigned bind, struct iovec *iov)
{
    struct virgl_renderer_resource_create_args args = {.handle = id,
        .target = height > 1 ? PIPE_TEXTURE_2D : PIPE_BUFFER, .format = format,
        .width = width, .height = height, .depth = 1, .array_size = 1, .bind = bind};
    CHECK(virgl_renderer_resource_create(&args, NULL, 0) == 0);
    if (iov) {
        /* Follow virtio's separate CREATE_RESOURCE / ATTACH_BACKING sequence.
         * Attaching a custom buffer initializes guest memory from host storage. */
        void *contents = malloc(iov->iov_len);
        if (!contents) abort();
        memcpy(contents, iov->iov_base, iov->iov_len);
        CHECK(virgl_renderer_resource_attach_iov(id, iov, 1) == 0);
        memcpy(iov->iov_base, contents, iov->iov_len);
        free(contents);
    }
    virgl_renderer_ctx_attach_resource(context_id, id);
}
static int submit(const uint32_t *words, size_t count)
{ return virgl_renderer_submit_cmd((void *)words, context_id, count); }
#define COMMAND(name, ...) do { uint32_t payload[] = {__VA_ARGS__}; \
    uint32_t command[1 + sizeof(payload) / 4]; \
    command[0] = VIRGL_CMD0(VIRGL_CCMD_##name, 0, sizeof(payload) / 4); \
    memcpy(command + 1, payload, sizeof(payload)); CHECK(submit(command, sizeof(command) / 4) == 0); \
} while (0)

int main(void)
{
    alarm(30); /* A broken asynchronous dependency must fail rather than hang CI. */
    @autoreleasepool {
    const EGLint attrs[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE};
    display = eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, NULL, attrs);
    CHECK(eglInitialize(display, NULL, NULL));
    const EGLint configs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLint count = 0;
    CHECK(eglChooseConfig(display, configs, &config, 1, &count) && count);
    struct virgl_renderer_callbacks cb = {.version = 4, .write_fence = fence,
        .write_context_fence = context_fence,
        .create_gl_context = create_context, .destroy_gl_context = destroy_context,
        .make_current = make_current, .get_egl_display = get_display};
    CHECK(virgl_renderer_init(&cb, VIRGL_RENDERER_USE_GLES | VIRGL_RENDERER_USE_SURFACELESS |
        VIRGL_RENDERER_NATIVE_SHARE_TEXTURE | VIRGL_RENDERER_USE_VIDEO |
        (getenv("TEST_THREADED_FENCES") ? VIRGL_RENDERER_THREAD_SYNC | VIRGL_RENDERER_ASYNC_FENCE_CB : 0), &cb) == 0);
    CHECK(virgl_renderer_context_create(1, 5, "video") == 0);
    if (failures) return 1;
    union virgl_caps caps = {0}; uint32_t version, size;
    virgl_renderer_get_cap_set(2, &version, &size);
    virgl_renderer_fill_caps(2, version, &caps);
    CHECK(caps.v2.num_video_caps > 0); /* No DRM fd callback on macOS. */
    unsigned h264_caps = 0;
    for (unsigned i = 0; i < caps.v2.num_video_caps; i++)
        if (caps.v2.video_caps[i].profile == MESA_H264_HIGH &&
            caps.v2.video_caps[i].entrypoint == 1)
            h264_caps++;
    CHECK(h264_caps == 1);
    resource(1, VIRGL_FORMAT_R8_UNORM, 128, 128,
             VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_RENDER_TARGET, NULL);
    resource(2, VIRGL_FORMAT_R8G8_UNORM, 64, 64,
             VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_RENDER_TARGET, NULL);
    uint8_t y[128 * 128], uv[64 * 64 * 2], bitstream[65536] = {0};
    memset(y, 76, sizeof(y)); memset(uv, 128, sizeof(uv));
    struct iovec y_iov = {y, sizeof(y)}, uv_iov = {uv, sizeof(uv)};
    struct virgl_box y_box = {0, 0, 0, 128, 128, 1}, uv_box = {0, 0, 0, 64, 64, 1};
    CHECK(virgl_renderer_transfer_write_iov(1, 1, 0, 128, sizeof(y), &y_box, 0, &y_iov, 1) == 0);
    CHECK(virgl_renderer_transfer_write_iov(2, 1, 0, 128, sizeof(uv), &uv_box, 0, &uv_iov, 1) == 0);
    union virgl_picture_desc desc = {0};
    desc.base.profile = MESA_H264_HIGH;
    desc.h264_enc.picture_type = PIPE_H2645_ENC_PICTURE_TYPE_IDR;
    desc.h264_enc.rate_ctrl[0].target_bitrate = 1000000;
    desc.h264_enc.rate_ctrl[0].frame_rate_num = 30;
    desc.h264_enc.rate_ctrl[0].frame_rate_den = 1;
    struct virgl_video_encode_feedback feedback = {0};
    struct iovec desc_iov = {&desc, sizeof(desc)}, bs_iov = {bitstream, sizeof(bitstream)};
    struct iovec feed_iov = {&feedback, sizeof(feedback)};
    resource(3, VIRGL_FORMAT_R8_UNORM, sizeof(bitstream), 1, 0, &bs_iov);
    resource(4, VIRGL_FORMAT_R8_UNORM, sizeof(desc), 1, VIRGL_BIND_CUSTOM, &desc_iov);
    resource(5, VIRGL_FORMAT_R8_UNORM, sizeof(feedback), 1, VIRGL_BIND_CUSTOM, &feed_iov);
    COMMAND(CREATE_VIDEO_CODEC, 10, MESA_H264_HIGH,
            4, MESA_CHROMA_420, 51, 128, 128, 4);
    COMMAND(CREATE_VIDEO_BUFFER, 50, PIPE_FORMAT_NV12, 128, 128, 1, 2);
    COMMAND(BEGIN_FRAME, 10, 50);
    COMMAND(ENCODE_BITSTREAM, 10, 50, 3, 4, 5);
    COMMAND(END_FRAME, 10, 50);
    CHECK(feedback.stat == VIRGL_VIDEO_ENCODE_STAT_SUCCESS && feedback.coded_size > 4);
    resource(6, VIRGL_FORMAT_R8_UNORM, sizeof(bitstream), 1, VIRGL_BIND_CUSTOM, &bs_iov);
    memset(&desc, 0, sizeof(desc)); desc.base.profile = MESA_H264_HIGH;
    COMMAND(CREATE_VIDEO_CODEC, 11, MESA_H264_HIGH,
            1, MESA_CHROMA_420, 51, 128, 128, 4);
    COMMAND(BEGIN_FRAME, 11, 50);
    COMMAND(DECODE_BITSTREAM, 11, 50, 4, 6, feedback.coded_size);
    COMMAND(END_FRAME, 11, 50);
    memset(y, 0, sizeof(y));
    CHECK(virgl_renderer_transfer_read_iov(1, 1, 0, 128, sizeof(y), &y_box, 0, &y_iov, 1) == 0);
    CHECK(abs((int)y[64 * 128 + 64] - 76) <= 3);

    /* A new decode destination has never been uploaded/read through GL. */
    resource(10, VIRGL_FORMAT_R8_UNORM, 128, 128,
             VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_RENDER_TARGET, NULL);
    resource(11, VIRGL_FORMAT_R8G8_UNORM, 64, 64,
             VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_RENDER_TARGET, NULL);
    COMMAND(CREATE_VIDEO_BUFFER, 54, PIPE_FORMAT_NV12, 128, 128, 10, 11);
    COMMAND(BEGIN_FRAME, 11, 54);
    COMMAND(DECODE_BITSTREAM, 11, 54, 4, 6, feedback.coded_size);
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLSharedEvent> gate = [device newSharedEvent];
    EGLAttrib gate_attrs[] = {
        EGL_SYNC_METAL_SHARED_EVENT_OBJECT_ANGLE, (EGLAttrib)gate,
        EGL_SYNC_CONDITION, EGL_SYNC_METAL_SHARED_EVENT_SIGNALED_ANGLE,
        EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_LO_ANGLE, 1, EGL_NONE};
    EGLSync gate_sync = eglCreateSync(display, EGL_SYNC_METAL_SHARED_EVENT_ANGLE, gate_attrs);
    CHECK(gate_sync != EGL_NO_SYNC && eglWaitSync(display, gate_sync, 0));
    eglDestroySync(display, gate_sync);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC),
        dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{ gate.signaledValue = 1; });
    COMMAND(END_FRAME, 11, 54);
    CHECK(gate.signaledValue == 1); /* END_FRAME must wait for the plane write. */
    /* Only now create the ordinary submission fence. No video fence is
     * attached to it, and no consumer installs a video-specific wait. */
    CHECK(virgl_renderer_context_create_fence(1, 0, 0, 101) == 0);
    /* Native storage remains valid after deleting the video wrapper. */
    COMMAND(DESTROY_VIDEO_BUFFER, 54);
    for (unsigned i = 0; retired_fence != 101 && i < 2000; i++) {
        virgl_renderer_poll(); usleep(1000);
    }
    CHECK(retired_fence == 101);
    [gate release]; [device release];
    memset(y, 0, sizeof(y));
    CHECK(virgl_renderer_transfer_read_iov(10, 1, 0, 128, sizeof(y), &y_box, 0, &y_iov, 1) == 0);
    CHECK(abs((int)y[64 * 128 + 64] - 76) <= 3);

    /* COPY_TRANSFER3D follows END_FRAME without its own video wait. */
    memset(y, 0, sizeof(y));
    CHECK(virgl_renderer_transfer_write_iov(10, 1, 0, 128, sizeof(y), &y_box, 0, &y_iov, 1) == 0);
    CHECK(virgl_renderer_resource_attach_iov(10, &y_iov, 1) == 0);
    uint8_t copied_y[sizeof(y)] = {0};
    struct iovec copied_iov = {copied_y, sizeof(copied_y)};
    resource(12, VIRGL_FORMAT_R8_UNORM, sizeof(copied_y), 1, VIRGL_BIND_CUSTOM, &copied_iov);
    COMMAND(CREATE_VIDEO_BUFFER, 55, PIPE_FORMAT_NV12, 128, 128, 10, 11);
    COMMAND(BEGIN_FRAME, 11, 55);
    COMMAND(DECODE_BITSTREAM, 11, 55, 4, 6, feedback.coded_size);
    id<MTLDevice> copy_device = MTLCreateSystemDefaultDevice();
    id<MTLSharedEvent> copy_gate = [copy_device newSharedEvent];
    gate_attrs[1] = (EGLAttrib)copy_gate;
    gate_sync = eglCreateSync(display, EGL_SYNC_METAL_SHARED_EVENT_ANGLE, gate_attrs);
    CHECK(gate_sync != EGL_NO_SYNC && eglWaitSync(display, gate_sync, 0));
    eglDestroySync(display, gate_sync);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC),
        dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{ copy_gate.signaledValue = 1; });
    COMMAND(END_FRAME, 11, 55);
    CHECK(copy_gate.signaledValue == 1);
    COMMAND(COPY_TRANSFER3D, 10, 0, 0, 128, sizeof(y), 0, 0, 0, 128, 128, 1,
        12, 0, VIRGL_COPY_TRANSFER3D_FLAGS_READ_FROM_HOST);
    CHECK(abs((int)copied_y[64 * 128 + 64] - 76) <= 3);
    COMMAND(DESTROY_VIDEO_BUFFER, 55);
    [copy_gate release]; [copy_device release];

    /* The system VA driver can select I420 or YV12 even when NV12 is preferred.
     * Exercise both plane orders with distinct U/V samples through public commands. */
    resource(8, VIRGL_FORMAT_R8_UNORM, 64, 64,
             VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_RENDER_TARGET, NULL);
    resource(9, VIRGL_FORMAT_R8_UNORM, 64, 64,
             VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_RENDER_TARGET, NULL);
    unsigned planar_formats[] = {PIPE_FORMAT_IYUV, PIPE_FORMAT_YV12};
    uint8_t chroma[2][64 * 64];
    for (unsigned layout = 0; layout < 2; layout++) {
        for (unsigned p = 0; p < 2; p++) {
            memset(chroma[p], (p ^ layout) ? 208 : 48, sizeof(chroma[p]));
            struct iovec iov = {chroma[p], sizeof(chroma[p])};
            CHECK(virgl_renderer_transfer_write_iov(8 + p, 1, 0, 64,
                sizeof(chroma[p]), &uv_box, 0, &iov, 1) == 0);
        }
        memset(&desc, 0, sizeof(desc)); desc.base.profile = PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH;
        desc.h264_enc.picture_type = PIPE_H2645_ENC_PICTURE_TYPE_IDR;
        COMMAND(CREATE_VIDEO_BUFFER, 52 + layout, planar_formats[layout], 128, 128, 1, 8, 9);
        COMMAND(BEGIN_FRAME, 10, 52 + layout);
        COMMAND(ENCODE_BITSTREAM, 10, 52 + layout, 3, 4, 5);
        COMMAND(END_FRAME, 10, 52 + layout);
        CHECK(feedback.stat == VIRGL_VIDEO_ENCODE_STAT_SUCCESS && feedback.coded_size > 4);
        memset(&desc, 0, sizeof(desc)); desc.base.profile = PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH;
        COMMAND(BEGIN_FRAME, 11, 52 + layout);
        COMMAND(DECODE_BITSTREAM, 11, 52 + layout, 4, 6, feedback.coded_size);
        COMMAND(END_FRAME, 11, 52 + layout);
        for (unsigned p = 0; p < 2; p++) {
            memset(chroma[p], 0, sizeof(chroma[p]));
            struct iovec iov = {chroma[p], sizeof(chroma[p])};
            CHECK(virgl_renderer_transfer_read_iov(8 + p, 1, 0, 64,
                sizeof(chroma[p]), &uv_box, 0, &iov, 1) == 0);
            CHECK(abs((int)chroma[p][32 * 64 + 32] - ((p ^ layout) ? 208 : 48)) <= 3);
        }
        COMMAND(DESTROY_VIDEO_BUFFER, 52 + layout);
    }

    /* Invalid guest bitstream length must fail, not overrun host allocation. */
    COMMAND(BEGIN_FRAME, 11, 50);
    uint32_t invalid[] = {VIRGL_CMD0(VIRGL_CCMD_DECODE_BITSTREAM, 0, 5),
        11, 50, 4, 6, sizeof(bitstream) + 1};
    CHECK(submit(invalid, sizeof(invalid) / 4) != 0);
    virgl_renderer_context_destroy(1); /* Pending frame and video resources. */
    CHECK(virgl_renderer_context_create(2, 5, "after") == 0);
    context_id = 2;
    for (unsigned i = 1; i <= 6; i++) virgl_renderer_ctx_attach_resource(2, i);
    uint8_t small[4] = {0xa5, 0xa5, 0xa5, 0xa5};
    struct iovec small_iov = {small, sizeof(small)};
    resource(7, VIRGL_FORMAT_R8_UNORM, sizeof(small), 1, 0, &small_iov);
    memset(&desc, 0, sizeof(desc));
    desc.base.profile = PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH;
    desc.h264_enc.picture_type = PIPE_H2645_ENC_PICTURE_TYPE_IDR;
    COMMAND(CREATE_VIDEO_CODEC, 12, PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH,
            PIPE_VIDEO_ENTRYPOINT_ENCODE, PIPE_VIDEO_CHROMA_FORMAT_420, 51, 128, 128, 4);
    COMMAND(CREATE_VIDEO_BUFFER, 51, PIPE_FORMAT_NV12, 128, 128, 1, 2);
    COMMAND(BEGIN_FRAME, 12, 51);
    COMMAND(ENCODE_BITSTREAM, 12, 51, 7, 4, 5);
    uint32_t end[] = {VIRGL_CMD0(VIRGL_CCMD_END_FRAME, 0, 2), 12, 51};
    CHECK(submit(end, sizeof(end) / 4) != 0);
    CHECK(feedback.stat == VIRGL_VIDEO_ENCODE_STAT_FAILURE && !feedback.coded_size);
    CHECK(small[0] == 0xa5 && small[3] == 0xa5); /* No truncated write. */
    virgl_renderer_context_destroy(2);
    CHECK(virgl_renderer_context_create(3, 5, "alive") == 0);
    virgl_renderer_context_destroy(3);
    for (unsigned i = 1; i <= 12; i++) virgl_renderer_resource_unref(i);
    virgl_renderer_cleanup(&cb);
    eglMakeCurrent(display, NULL, NULL, NULL); eglTerminate(display);
    printf("video commands: %u checks, %u failures\n", checks, failures);
    return failures != 0;
    }
}
