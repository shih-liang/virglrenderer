/* Exercise video through the public VirGL command/resource API and static ANGLE. */
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <EGL/eglext_angle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include "virglrenderer.h"
#include "virgl_hw.h"
#include "virgl_protocol.h"
#include "virgl_video_hw.h"
#include "pipe/p_state.h"
#include "pipe/p_video_enums.h"
#include "pipe/p_video_state.h"

static EGLDisplay display;
static EGLConfig config;
static unsigned context_id = 1;
static unsigned checks, failures;
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
    const EGLint attrs[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE};
    display = eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, NULL, attrs);
    CHECK(eglInitialize(display, NULL, NULL));
    const EGLint configs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLint count = 0;
    CHECK(eglChooseConfig(display, configs, &config, 1, &count) && count);
    struct virgl_renderer_callbacks cb = {.version = 4, .write_fence = fence,
        .create_gl_context = create_context, .destroy_gl_context = destroy_context,
        .make_current = make_current, .get_egl_display = get_display};
    CHECK(virgl_renderer_init(&cb, VIRGL_RENDERER_USE_GLES | VIRGL_RENDERER_USE_SURFACELESS |
        VIRGL_RENDERER_NATIVE_SHARE_TEXTURE | VIRGL_RENDERER_USE_VIDEO, &cb) == 0);
    CHECK(virgl_renderer_context_create(1, 5, "video") == 0);
    if (failures) return 1;
    union virgl_caps caps = {0}; uint32_t version, size;
    virgl_renderer_get_cap_set(2, &version, &size);
    virgl_renderer_fill_caps(2, version, &caps);
    CHECK(caps.v2.num_video_caps > 0); /* No DRM fd callback on macOS. */
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
    desc.base.profile = PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH;
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
    COMMAND(CREATE_VIDEO_CODEC, 10, PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH,
            PIPE_VIDEO_ENTRYPOINT_ENCODE, PIPE_VIDEO_CHROMA_FORMAT_420, 51, 128, 128, 4);
    COMMAND(CREATE_VIDEO_BUFFER, 50, PIPE_FORMAT_NV12, 128, 128, 1, 2);
    COMMAND(BEGIN_FRAME, 10, 50);
    COMMAND(ENCODE_BITSTREAM, 10, 50, 3, 4, 5);
    COMMAND(END_FRAME, 10, 50);
    CHECK(feedback.stat == VIRGL_VIDEO_ENCODE_STAT_SUCCESS && feedback.coded_size > 4);
    resource(6, VIRGL_FORMAT_R8_UNORM, sizeof(bitstream), 1, VIRGL_BIND_CUSTOM, &bs_iov);
    memset(&desc, 0, sizeof(desc)); desc.base.profile = PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH;
    COMMAND(CREATE_VIDEO_CODEC, 11, PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH,
            PIPE_VIDEO_ENTRYPOINT_BITSTREAM, PIPE_VIDEO_CHROMA_FORMAT_420, 51, 128, 128, 4);
    COMMAND(BEGIN_FRAME, 11, 50);
    COMMAND(DECODE_BITSTREAM, 11, 50, 4, 6, feedback.coded_size);
    COMMAND(END_FRAME, 11, 50);
    memset(y, 0, sizeof(y));
    CHECK(virgl_renderer_transfer_read_iov(1, 1, 0, 128, sizeof(y), &y_box, 0, &y_iov, 1) == 0);
    CHECK(abs((int)y[64 * 128 + 64] - 76) <= 3);

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
    for (unsigned i = 1; i <= 7; i++) virgl_renderer_resource_unref(i);
    virgl_renderer_cleanup(&cb);
    eglMakeCurrent(display, NULL, NULL, NULL); eglTerminate(display);
    printf("video commands: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
