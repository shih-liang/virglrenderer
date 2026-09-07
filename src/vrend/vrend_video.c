/**************************************************************************
 *
 * Copyright (C) 2022 Kylin Software Co., Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

/**
 * @file
 * The video implementation of the vrend renderer.
 *
 * It is based on the general virgl video submodule and handles data transfer
 * and synchronization between host and guest.
 *
 * The relationship between vaSurface and video buffer objects:
 *
 *           GUEST (Mesa)           |       HOST (Virglrenderer)
 *                                  |
 *         +------------+           |          +------------+
 *         | vaSurface  |           |          | vaSurface  | <------+
 *         +------------+           |          +------------+        |
 *               |                  |                                |
 *  +---------------------------+   |   +-------------------------+  |
 *  |    virgl_video_buffer     |   |   |    vrend_video_buffer   |  |
 *  | +-----------------------+ |   |   |  +-------------------+  |  |
 *  | |    vl_video_buffer    | |   |   |  | vrend_resource(s) |  |  |
 *  | | +-------------------+ | |<--+-->|  +-------------------+  |  |
 *  | | | virgl_resource(s) | | |   |   |  +--------------------+ |  |
 *  | | +-------------------+ | |   |   |  | virgl_video_buffer |-+--+
 *  | +-----------------------+ |   |   |  +--------------------+ |
 *  +---------------------------+   |   +-------------------------+
 *
 * The relationship between vaContext and video codec objects:
 *
 *           GUEST (Mesa)         |         HOST (Virglrenderer)
 *                                |
 *         +------------+         |           +------------+
 *         | vaContext  |         |           | vaContext  | <-------+
 *         +------------+         |           +------------+         |
 *               |                |                                  |
 *  +------------------------+    |    +--------------------------+  |
 *  |    virgl_video_codec   | <--+--> |    vrend_video_codec     |  |
 *  +------------------------+    |    |  +--------------------+  |  |
 *                                |    |  | virgl_video_codec  | -+--+
 *                                |    |  +--------------------+  |
 *                                |    +--------------------------+
 *
 * @author Feng Jiang <jiangfeng@kylinos.cn>
 */


#include "virgl_video.h"
#include "virgl_video_hw.h"

#include "vrend_debug.h"
#include "vrend_winsys.h"
#include "vrend_renderer.h"
#include "vrend_video.h"
#ifdef __APPLE__
#include "virgl_resource.h"
#include "virgl_video_metal.h"
#include <CoreFoundation/CoreFoundation.h>
#include <EGL/eglext_angle.h>
#endif

#define VREND_VIDEO_FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
     ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

struct vrend_context;

struct vrend_video_context {
    struct vrend_context *ctx;
    struct list_head codecs;
    struct list_head buffers;
#ifdef __APPLE__
    struct virgl_video_metal *metal;
#endif
};

struct vrend_video_codec {
    struct virgl_video_codec *codec;
    uint32_t handle;
    uint32_t entrypoint;
    uint32_t feed_handle;    /* Look up again at completion: resources may detach. */
    uint32_t dest_handle;
    struct vrend_video_context *ctx;
    struct list_head head;
};

struct vrend_video_plane {
    uint32_t res_handle;
#ifdef __APPLE__
    void *native_texture;
#else
    GLuint texture;         /* texture for temporary use */
    EGLImageKHR egl_image;  /* egl image for temporary use */
#endif
};

struct vrend_video_buffer {
    struct virgl_video_buffer *buffer;

    uint32_t handle;
    uint32_t format;
    uint32_t width, height;
    struct vrend_video_context *ctx;
    struct list_head head;

    uint32_t num_planes;
    struct vrend_video_plane planes[3];
};

static struct vrend_video_codec *vrend_video_codec(
        struct virgl_video_codec *codec)
{
    return virgl_video_codec_opaque_data(codec);
}

static struct vrend_video_buffer *vrend_video_buffer(
        struct virgl_video_buffer *buffer)
{
    return virgl_video_buffer_opaque_data(buffer);
}

static struct vrend_video_codec *get_video_codec(
                                        struct vrend_video_context *ctx,
                                        uint32_t cdc_handle)
{
    list_for_each_entry(struct vrend_video_codec, cdc, &ctx->codecs, head) {
        if (cdc->handle == cdc_handle)
            return cdc;
    }

    return NULL;
}

static struct vrend_video_buffer *get_video_buffer(
                                        struct vrend_video_context *ctx,
                                        uint32_t buf_handle)
{
    list_for_each_entry(struct vrend_video_buffer, buf, &ctx->buffers, head) {
        if (buf->handle == buf_handle)
            return buf;
    }

    return NULL;
}


#ifdef __APPLE__
/* GL is an importer of the shared resource. Bridge only its execution
 * dependency; the codec and the actual plane transfer never call GL/EGL. */
static struct virgl_video_metal_fence *prepare_video_planes(
    struct vrend_video_buffer *buf, bool download)
{
    bool p010 = buf->format == PIPE_FORMAT_P010;
    struct vrend_video_context *ctx = buf->ctx;
    void *textures[3] = {buf->planes[0].native_texture, buf->planes[1].native_texture,
                         buf->planes[2].native_texture};
    if (buf->format == PIPE_FORMAT_YV12) {
        textures[1] = buf->planes[2].native_texture;
        textures[2] = buf->planes[1].native_texture;
    }
    if (!ctx->metal) ctx->metal = virgl_video_metal_create(textures[0]);
    if (!ctx->metal)
        return NULL;

    EGLDisplay display = eglGetCurrentDisplay();
    PFNEGLCOPYMETALSHAREDEVENTANGLEPROC copy_event =
        (PFNEGLCOPYMETALSHAREDEVENTANGLEPROC)eglGetProcAddress("eglCopyMetalSharedEventANGLE");
    if (!copy_event) return NULL;
    const EGLAttrib before_attrs[] = {
        EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_LO_ANGLE, 1,
        EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_HI_ANGLE, 0, EGL_NONE};
    EGLSync before = eglCreateSync(display, EGL_SYNC_METAL_SHARED_EVENT_ANGLE, before_attrs);
    if (before == EGL_NO_SYNC) return NULL;
    void *event = copy_event(display, before);
    glFlush();
    eglDestroySync(display, before);
    if (!event) return NULL;
    struct virgl_video_metal_fence *copy = virgl_video_metal_prepare(
        ctx->metal, textures, buf->num_planes, buf->width, buf->height, p010, download, event, 1);
    CFRelease(event);
    return copy;
}

bool vrend_video_failed(struct vrend_video_context *ctx)
{
    return ctx && virgl_video_metal_failed(ctx->metal);
}

static void decoded_async(void *data, void *pixels)
{
    /* Runs on VT's callback thread. No GL, context or mutable resource lookup. */
    struct virgl_video_metal_fence *fence = data;
    virgl_video_metal_complete(fence, pixels);
    virgl_video_metal_fence_unref(fence);
}

static int sync_video_planes(struct vrend_video_buffer *buf,
                             const struct virgl_video_dma_buf *frame,
                             bool download)
{
    /* Encoding is a CPU API consumer of the input CVPixelBuffer. Decode uses
     * the asynchronous callback below and never enters this waiting path. */
    uint32_t required = download ? VIRGL_VIDEO_DMABUF_READ_ONLY : VIRGL_VIDEO_DMABUF_WRITE_ONLY;
    if (!frame || !frame->native_frame || !(frame->flags & required)) return -1;
    struct virgl_video_metal_fence *copy = prepare_video_planes(buf, download);
    if (!copy) return -1;
    virgl_video_metal_complete(copy, frame->native_frame);
    int result = virgl_video_metal_fence_wait(copy, true);
    virgl_video_metal_fence_unref(copy);
    return result == 1 ? 0 : -1;
}

#else
/* Video transfers share the guest's GL context. Preserve real GL state so the
 * renderer's cached bindings remain valid, including PBOs and pixel offsets. */
static int sync_video_planes(struct vrend_video_buffer *buf,
                             const struct virgl_video_dma_buf *dmabuf,
                             bool download)
{
    static const GLenum stores[] = {
        GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_PIXELS, GL_PACK_SKIP_ROWS,
        GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_PIXELS,
        GL_UNPACK_SKIP_ROWS,
    };
    GLint previous[ARRAY_SIZE(stores)], texture, framebuffer, pack, unpack;
    GLuint transfer_fbo = 0;
    int result = -1;
    uint32_t required = download ? VIRGL_VIDEO_DMABUF_READ_ONLY
                                : VIRGL_VIDEO_DMABUF_WRITE_ONLY;
    if (!dmabuf || !(dmabuf->flags & required) ||
        !dmabuf->num_planes || dmabuf->num_planes != buf->num_planes)
        return -1;

    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
    for (unsigned i = 0; i < ARRAY_SIZE(stores); i++) {
        glGetIntegerv(stores[i], &previous[i]);
        glPixelStorei(stores[i], i == 0 || i == 4 ? 1 : 0);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    for (unsigned i = 0; i < buf->num_planes; i++) {
        struct vrend_video_plane *plane = &buf->planes[i];
        const struct virgl_video_dma_buf_plane *mapping = &dmabuf->planes[i];
        struct vrend_resource *res =
            vrend_renderer_ctx_res_lookup(buf->ctx->ctx, plane->res_handle);
        if (!res || res->target != GL_TEXTURE_2D)
            goto out;

        if (mapping->data) {
            bool p010 = dmabuf->drm_format == VREND_VIDEO_FOURCC('P', '0', '1', '0');
            enum pipe_format expected = p010 ?
                (i ? PIPE_FORMAT_R16G16_UNORM : PIPE_FORMAT_R16_UNORM) :
                (i ? PIPE_FORMAT_R8G8_UNORM : PIPE_FORMAT_R8_UNORM);
            unsigned bpp = (i ? 2 : 1) * (p010 ? 2 : 1);
            unsigned width = (dmabuf->width + (i ? 1 : 0)) >> (i ? 1 : 0);
            unsigned height = (dmabuf->height + (i ? 1 : 0)) >> (i ? 1 : 0);
            if ((!p010 && dmabuf->drm_format != VREND_VIDEO_FOURCC('N', 'V', '1', '2')) ||
                dmabuf->num_planes != 2 || res->base.format != expected ||
                mapping->width < width || mapping->height < height ||
                res->base.width0 < width || res->base.height0 < height ||
                mapping->pitch % bpp || mapping->pitch / bpp < width ||
                (uint64_t)mapping->pitch * height > mapping->size)
                goto out;
            if (download) {
                glBindTexture(GL_TEXTURE_2D, res->gl_id);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, mapping->pitch / bpp);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                                i ? GL_RG : GL_RED,
                                p010 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE,
                                mapping->data);
            } else {
                if (!transfer_fbo) glGenFramebuffers(1, &transfer_fbo);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, transfer_fbo);
                glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D, res->gl_id, 0);
                if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                    goto out;
                glPixelStorei(GL_PACK_ROW_LENGTH, mapping->pitch / bpp);
                glReadPixels(0, 0, width, height, i ? GL_RG : GL_RED,
                             p010 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, mapping->data);
            }
        } else {
            if (plane->egl_image == EGL_NO_IMAGE_KHR) {
                EGLint attributes[] = {
                    EGL_LINUX_DRM_FOURCC_EXT, mapping->drm_format,
                    EGL_WIDTH, res->base.width0,
                    EGL_HEIGHT, res->base.height0,
                    EGL_DMA_BUF_PLANE0_FD_EXT, mapping->fd,
                    EGL_DMA_BUF_PLANE0_OFFSET_EXT, mapping->offset,
                    EGL_DMA_BUF_PLANE0_PITCH_EXT, mapping->pitch, EGL_NONE,
                };
                plane->egl_image = eglCreateImageKHR(eglGetCurrentDisplay(),
                    EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attributes);
            }
            if (plane->egl_image == EGL_NO_IMAGE_KHR)
                goto out;
            glBindTexture(GL_TEXTURE_2D, plane->texture);
            glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, plane->egl_image);
            if (!transfer_fbo) glGenFramebuffers(1, &transfer_fbo);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, transfer_fbo);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                GL_TEXTURE_2D, download ? plane->texture : res->gl_id, 0);
            if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                goto out;
            glBindTexture(GL_TEXTURE_2D, download ? res->gl_id : plane->texture);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0,
                                res->base.width0, res->base.height0);
        }
        if (glGetError() != GL_NO_ERROR)
            goto out;
    }
    result = 0;
out:
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glDeleteFramebuffers(1, &transfer_fbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pack);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack);
    for (unsigned i = 0; i < ARRAY_SIZE(stores); i++)
        glPixelStorei(stores[i], previous[i]);
    return result;
}
#endif

static int vrend_video_decode_completed(struct virgl_video_codec *codec,
                                        const struct virgl_video_dma_buf *dmabuf)
{
    (void)codec;
    return sync_video_planes(vrend_video_buffer(dmabuf->buf), dmabuf, true);
}

static int vrend_video_encode_upload_picture(struct virgl_video_codec *codec,
                                              const struct virgl_video_dma_buf *dmabuf)
{
    (void)codec;
    return sync_video_planes(vrend_video_buffer(dmabuf->buf), dmabuf, false);
}

static int vrend_video_encode_completed(
                                struct virgl_video_codec *codec,
                                const struct virgl_video_dma_buf *src_buf,
                                const struct virgl_video_dma_buf *ref_buf,
                                unsigned num_coded_bufs,
                                const void * const *coded_bufs,
                                const unsigned *coded_sizes)
{
    struct vrend_video_codec *cdc = vrend_video_codec(codec);
    struct vrend_resource *dest =
        vrend_renderer_ctx_res_lookup(cdc->ctx->ctx, cdc->dest_handle);
    struct vrend_resource *feed =
        vrend_renderer_ctx_res_lookup(cdc->ctx->ctx, cdc->feed_handle);
    struct virgl_video_encode_feedback feedback = {
        .stat = VIRGL_VIDEO_ENCODE_STAT_FAILURE,
    };
    GLint previous;
    size_t total = 0;
    (void)src_buf;
    (void)ref_buf;
    cdc->dest_handle = cdc->feed_handle = 0;
    if (!feed || vrend_get_iovec_size(feed->iov, feed->num_iovs) < sizeof(feedback))
        return -1;
    if (!dest || !has_bit(dest->storage_bits, VREND_STORAGE_GL_BUFFER) ||
        !num_coded_bufs || !coded_bufs || !coded_sizes)
        goto out;
    /* Never truncate a bitstream and report success. Validate before writing. */
    for (unsigned i = 0; i < num_coded_bufs; i++) {
        if (!coded_bufs[i] || coded_sizes[i] > dest->base.width0 - total)
            goto out;
        total += coded_sizes[i];
    }
    if (!total || vrend_get_iovec_size(dest->iov, dest->num_iovs) < total)
        goto out;
    glGetIntegerv(GL_COPY_WRITE_BUFFER_BINDING, &previous);
    glBindBuffer(GL_COPY_WRITE_BUFFER, dest->gl_id);
    for (unsigned i = 0, offset = 0; i < num_coded_bufs; i++) {
        glBufferSubData(GL_COPY_WRITE_BUFFER, offset, coded_sizes[i], coded_bufs[i]);
        vrend_write_to_iovec(dest->iov, dest->num_iovs, offset, coded_bufs[i], coded_sizes[i]);
        offset += coded_sizes[i];
    }
    if (glGetError() == GL_NO_ERROR) {
        feedback.stat = VIRGL_VIDEO_ENCODE_STAT_SUCCESS;
        feedback.coded_size = total;
    }
    glBindBuffer(GL_COPY_WRITE_BUFFER, previous);
out:
    vrend_write_to_iovec(feed->iov, feed->num_iovs, 0,
                         (const char *)&feedback, sizeof(feedback));
    return feedback.stat == VIRGL_VIDEO_ENCODE_STAT_SUCCESS ? 0 : -1;
}

static struct virgl_video_callbacks video_callbacks = {
    .decode_completed           = vrend_video_decode_completed,
    .encode_upload_picture      = vrend_video_encode_upload_picture,
    .encode_completed           = vrend_video_encode_completed,
};

int vrend_video_init(int drm_fd)
{
#ifndef __APPLE__
    if (drm_fd < 0)
        return -1;
#endif

    return virgl_video_init(drm_fd, &video_callbacks, 0);
}

void vrend_video_fini(void)
{
    virgl_video_destroy();
}

int vrend_video_fill_caps(union virgl_caps *caps)
{
    return virgl_video_fill_caps(caps);
}

int vrend_video_create_codec(struct vrend_video_context *ctx,
                             uint32_t handle,
                             uint32_t profile,
                             uint32_t entrypoint,
                             uint32_t chroma_format,
                             uint32_t level,
                             uint32_t width,
                             uint32_t height,
                             uint32_t max_ref,
                             uint32_t flags)
{
    struct vrend_video_codec *cdc = get_video_codec(ctx, handle);
    struct virgl_video_create_codec_args args;

    if (cdc)
        return 0;

    if (profile <= PIPE_VIDEO_PROFILE_UNKNOWN ||
        profile >= PIPE_VIDEO_PROFILE_MAX)
        return -1;

    if (entrypoint <= PIPE_VIDEO_ENTRYPOINT_UNKNOWN ||
        entrypoint > PIPE_VIDEO_ENTRYPOINT_ENCODE)
        return -1;

    if (chroma_format == PIPE_VIDEO_CHROMA_FORMAT_NONE ||
        chroma_format > PIPE_VIDEO_CHROMA_FORMAT_444)
        return -1;

    if (!width || !height)
        return -1;

    cdc = (struct vrend_video_codec *)calloc(1, sizeof(*cdc));
    if (!cdc)
        return -1;

    args.profile = profile;
    args.entrypoint = entrypoint;
    args.chroma_format = chroma_format;
    args.level = level;
    args.width = width;
    args.height = height;
    args.max_references = max_ref;
    args.flags = flags;
    args.opaque = cdc;
    cdc->codec = virgl_video_create_codec(&args);
    if (!cdc->codec) {
        free(cdc);
        return -1;
    }

    cdc->handle = handle;
    cdc->entrypoint = entrypoint;
    cdc->ctx = ctx;
    list_add(&cdc->head, &ctx->codecs);

    return 0;
}

static void destroy_video_codec(struct vrend_video_codec *cdc)
{
    if (cdc) {
        list_del(&cdc->head);
        virgl_video_destroy_codec(cdc->codec);
        free(cdc);
    }
}

void vrend_video_destroy_codec(struct vrend_video_context *ctx,
                               uint32_t handle)
{
    struct vrend_video_codec *cdc = get_video_codec(ctx, handle);

    destroy_video_codec(cdc);
}

int vrend_video_create_buffer(struct vrend_video_context *ctx,
                              uint32_t handle,
                              uint32_t format,
                              uint32_t width,
                              uint32_t height,
                              uint32_t *res_handles,
                              unsigned int num_res)
{
    unsigned i;
    struct vrend_video_plane *plane;
    struct vrend_video_buffer *buf = get_video_buffer(ctx, handle);
    struct virgl_video_create_buffer_args args;

    if (buf)
        return 0;

    if (format <= PIPE_FORMAT_NONE || format >= PIPE_FORMAT_COUNT){
        virgl_error("Invalid vrend video buffer format: %d\n", format);
        return -1;
    }

    if (!width || !height || !res_handles || !num_res)
        return -1;

    buf = (struct vrend_video_buffer *)calloc(1, sizeof(*buf));
    if (!buf)
        return -1;

    args.format = format;
    args.width = width;
    args.height = height;
    args.interlaced = 0;
    args.opaque = buf;
    buf->buffer = virgl_video_create_buffer(&args);
    if (!buf->buffer) {
        free(buf);
        return -1;
    }

#ifndef __APPLE__
    for (i = 0; i < ARRAY_SIZE(buf->planes); i++)
        buf->planes[i].egl_image = EGL_NO_IMAGE_KHR;
#endif

    for (i = 0, buf->num_planes = 0;
         i < num_res && buf->num_planes < ARRAY_SIZE(buf->planes); i++) {

        if (!res_handles[i])
            continue;

        plane = &buf->planes[buf->num_planes++];
        plane->res_handle = res_handles[i];
#ifdef __APPLE__
        struct virgl_resource *resource = virgl_resource_lookup(res_handles[i]);
        struct vrend_resource *view = vrend_renderer_ctx_res_lookup(ctx->ctx, res_handles[i]);
        unsigned p = buf->num_planes - 1;
        bool p010 = format == PIPE_FORMAT_P010;
        bool planar = format == PIPE_FORMAT_IYUV || format == PIPE_FORMAT_YV12;
        unsigned expected = p010 ? (p ? PIPE_FORMAT_R16G16_UNORM : PIPE_FORMAT_R16_UNORM)
                                 : (p && !planar ? PIPE_FORMAT_R8G8_UNORM : PIPE_FORMAT_R8_UNORM);
        unsigned shift = p != 0;
        if ((!p010 && !planar && format != PIPE_FORMAT_NV12) ||
            p >= (planar ? 3u : 2u) || !view || !resource ||
            !resource->native_metal_texture || view->base.format != expected ||
            view->base.width0 < (((uint64_t)width + shift) >> shift) ||
            view->base.height0 < (((uint64_t)height + shift) >> shift))
            goto fail_planes;
        plane->native_texture = (void *)CFRetain(resource->native_metal_texture);
#else
        glGenTextures(1, &plane->texture);
#endif
    }
#ifdef __APPLE__
    if (buf->num_planes != ((format == PIPE_FORMAT_IYUV || format == PIPE_FORMAT_YV12) ? 3u : 2u))
        goto fail_planes;
#endif

    buf->handle = handle;
    buf->format = format;
    buf->width = width;
    buf->height = height;
    buf->ctx = ctx;
    list_add(&buf->head, &ctx->buffers);

    return 0;
#ifdef __APPLE__
fail_planes:
    virgl_error("video buffer %u: incompatible native planes for format %u (%ux%u, %u planes)\n",
                handle, format, width, height, buf->num_planes);
    for (i = 0; i < buf->num_planes; i++)
        if (buf->planes[i].native_texture) CFRelease(buf->planes[i].native_texture);
    virgl_video_destroy_buffer(buf->buffer);
    free(buf);
    return -1;
#endif
}

static void destroy_video_buffer(struct vrend_video_buffer *buf)
{
    unsigned i;
    struct vrend_video_plane *plane;

    if (!buf)
        return;

    list_del(&buf->head);

    for (i = 0; i < buf->num_planes; i++) {
        plane = &buf->planes[i];

#ifdef __APPLE__
        if (plane->native_texture) CFRelease(plane->native_texture);
#else
        glDeleteTextures(1, &plane->texture);
        if (plane->egl_image != EGL_NO_IMAGE_KHR)
            eglDestroyImageKHR(eglGetCurrentDisplay(), plane->egl_image);
#endif
    }

    virgl_video_destroy_buffer(buf->buffer);

    free(buf);
}

void vrend_video_destroy_buffer(struct vrend_video_context *ctx,
                                uint32_t handle)
{
    struct vrend_video_buffer *buf = get_video_buffer(ctx, handle);

    destroy_video_buffer(buf);
}

struct vrend_video_context *vrend_video_create_context(struct vrend_context *ctx)
{
    struct vrend_video_context *vctx;

    vctx = (struct vrend_video_context *)calloc(1, sizeof(*vctx));
    if (vctx) {
        vctx->ctx = ctx;
        list_inithead(&vctx->codecs);
        list_inithead(&vctx->buffers);
    }

    return vctx;
}

void vrend_video_destroy_context(struct vrend_video_context *ctx)
{
   if (!ctx)
      return;
   list_for_each_entry_safe(struct vrend_video_codec, vcdc, &ctx->codecs, head)
      destroy_video_codec(vcdc);

   list_for_each_entry_safe(struct vrend_video_buffer, vbuf, &ctx->buffers, head)
      destroy_video_buffer(vbuf);

#ifdef __APPLE__
   virgl_video_metal_destroy(ctx->metal);
#endif

   free(ctx);
}

int vrend_video_begin_frame(struct vrend_video_context *ctx,
                            uint32_t cdc_handle,
                            uint32_t tgt_handle)
{
    struct vrend_video_codec *cdc = get_video_codec(ctx, cdc_handle);
    struct vrend_video_buffer *tgt = get_video_buffer(ctx, tgt_handle);

    if (!cdc || !tgt)
        return -1;

    return virgl_video_begin_frame(cdc->codec, tgt->buffer);
}

static void modify_h264_picture_desc(struct vrend_video_codec *cdc,
                                     struct vrend_video_buffer *tgt,
                                     struct virgl_h264_picture_desc *desc)
{
    unsigned i;
    struct vrend_video_buffer *vbuf;

    (void)tgt;

    for (i = 0; i < ARRAY_SIZE(desc->buffer_id); i++) {
        vbuf = get_video_buffer(cdc->ctx, desc->buffer_id[i]);
        desc->buffer_id[i] = virgl_video_buffer_id(vbuf ? vbuf->buffer : NULL);
    }
}

static void modify_h265_picture_desc(struct vrend_video_codec *cdc,
                                     struct vrend_video_buffer *tgt,
                                     struct virgl_h265_picture_desc *desc)
{
    unsigned i;
    struct vrend_video_buffer *vbuf;

    (void)tgt;

    for (i = 0; i < ARRAY_SIZE(desc->ref); i++) {
        vbuf = get_video_buffer(cdc->ctx, desc->ref[i]);
        desc->ref[i] = virgl_video_buffer_id(vbuf ? vbuf->buffer : NULL);
    }
}

static void modify_mpeg12_picture_desc(struct vrend_video_codec *cdc,
                                       struct vrend_video_buffer *tgt,
                                       struct virgl_mpeg12_picture_desc *desc)
{
    unsigned i;
    struct vrend_video_buffer *vbuf;

    (void)tgt;

    for (i = 0; i < ARRAY_SIZE(desc->ref); i++) {
        vbuf = get_video_buffer(cdc->ctx, desc->ref[i]);
        desc->ref[i] = virgl_video_buffer_id(vbuf ? vbuf->buffer : NULL);
    }
}


static void modify_mjpeg_picture_desc(struct vrend_video_codec *cdc,
                                      struct vrend_video_buffer *tgt,
                                      struct virgl_mjpeg_picture_desc *desc)
{
    (void)cdc;
    (void)tgt;
    (void)desc;
}

static void modify_vc1_picture_desc(struct vrend_video_codec *cdc,
                                    struct vrend_video_buffer *tgt,
                                    struct virgl_vc1_picture_desc *desc)
{
    unsigned i;
    struct vrend_video_buffer *vbuf;

    (void)tgt;

    for (i = 0; i < ARRAY_SIZE(desc->ref); i++) {
        vbuf = get_video_buffer(cdc->ctx, desc->ref[i]);
        desc->ref[i] = virgl_video_buffer_id(vbuf ? vbuf->buffer : NULL);
    }
}

static void modify_vp9_picture_desc(struct vrend_video_codec *cdc,
                                     struct vrend_video_buffer *tgt,
                                     struct virgl_vp9_picture_desc *desc)
{
    unsigned i;
    struct vrend_video_buffer *vbuf;

    (void)tgt;

    for (i = 0; i < ARRAY_SIZE(desc->ref); i++) {
        vbuf = get_video_buffer(cdc->ctx, desc->ref[i]);
        desc->ref[i] = virgl_video_buffer_id(vbuf ? vbuf->buffer : NULL);
    }
}

static void modify_av1_picture_desc(struct vrend_video_codec *cdc,
                                    struct vrend_video_buffer *tgt,
                                    struct virgl_av1_picture_desc *desc)
{
    unsigned i;
    struct vrend_video_buffer *vbuf;

    (void)tgt;

    for (i = 0; i < ARRAY_SIZE(desc->ref); i++) {
        vbuf = get_video_buffer(cdc->ctx, desc->ref[i]);
        desc->ref[i] = virgl_video_buffer_id(vbuf ? vbuf->buffer : NULL);
    }

    vbuf = get_video_buffer(cdc->ctx, desc->film_grain_target);
    desc->film_grain_target = virgl_video_buffer_id(vbuf ? vbuf->buffer : NULL);
}

static void modify_picture_desc(struct vrend_video_codec *cdc,
                                struct vrend_video_buffer *tgt,
                                union virgl_picture_desc *desc)
{
    switch(virgl_video_codec_profile(cdc->codec)) {
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_EXTENDED:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH10:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH422:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH444:
        modify_h264_picture_desc(cdc, tgt, &desc->h264);
        break;
    case PIPE_VIDEO_PROFILE_HEVC_MAIN:
    case PIPE_VIDEO_PROFILE_HEVC_MAIN_10:
    case PIPE_VIDEO_PROFILE_HEVC_MAIN_STILL:
    case PIPE_VIDEO_PROFILE_HEVC_MAIN_12:
    case PIPE_VIDEO_PROFILE_HEVC_MAIN_444:
        modify_h265_picture_desc(cdc, tgt, &desc->h265);
        break;
    case PIPE_VIDEO_PROFILE_MPEG2_MAIN:
    case PIPE_VIDEO_PROFILE_MPEG2_SIMPLE:
        modify_mpeg12_picture_desc(cdc, tgt, &desc->mpeg12);
        break;
    case PIPE_VIDEO_PROFILE_JPEG_BASELINE:
        modify_mjpeg_picture_desc(cdc, tgt, &desc->mjpeg);
        break;
    case PIPE_VIDEO_PROFILE_VC1_SIMPLE:
    case PIPE_VIDEO_PROFILE_VC1_MAIN:
    case PIPE_VIDEO_PROFILE_VC1_ADVANCED:
        modify_vc1_picture_desc(cdc, tgt, &desc->vc1);
        break;
    case PIPE_VIDEO_PROFILE_VP9_PROFILE0:
    case PIPE_VIDEO_PROFILE_VP9_PROFILE2:
        modify_vp9_picture_desc(cdc, tgt, &desc->vp9);
        break;
    case PIPE_VIDEO_PROFILE_AV1_MAIN:
        modify_av1_picture_desc(cdc, tgt, &desc->av1);
        break;
    default:
        break;
    }
}

int vrend_video_decode_bitstream(struct vrend_video_context *ctx,
                                 uint32_t cdc_handle,
                                 uint32_t tgt_handle,
                                 uint32_t desc_handle,
                                 unsigned num_buffers,
                                 const uint32_t *buffer_handles,
                                 const uint32_t *buffer_sizes)
{
    int err = -1;
    unsigned i, num_bs, *bs_sizes = NULL;
    void **bs_buffers = NULL;
    struct vrend_resource *res;
    struct vrend_video_codec  *cdc = get_video_codec(ctx, cdc_handle);
    struct vrend_video_buffer *tgt = get_video_buffer(ctx, tgt_handle);
    union virgl_picture_desc desc;

    if (!cdc || !tgt){
        virgl_error("video codec: %p, video buffer: %p, invalid.\n", (void *)cdc, (void *)tgt);
        return -1;
    }

    bs_buffers = calloc(num_buffers, sizeof(void *));
    if (!bs_buffers) {
        virgl_error("%s: alloc bs_buffers failed\n", __func__);
        return -1;
    }

    bs_sizes = calloc(num_buffers, sizeof(unsigned));
    if (!bs_sizes) {
        virgl_error("%s: alloc bs_sizes failed\n", __func__);
        goto err;
    }

    for (i = 0, num_bs = 0; i < num_buffers; i++) {
        res = vrend_renderer_ctx_res_lookup(ctx->ctx, buffer_handles[i]);
        if (!res || !res->ptr || !buffer_sizes[i] ||
            buffer_sizes[i] > res->base.width0 ||
            vrend_get_iovec_size(res->iov, res->num_iovs) < buffer_sizes[i]) {
            virgl_warn("%s: bs res %d invalid or not found",
                       __func__, buffer_handles[i]);
            goto err;
        }

        vrend_read_from_iovec(res->iov, res->num_iovs, 0,
                              res->ptr, buffer_sizes[i]);
        bs_buffers[num_bs] = res->ptr;
        bs_sizes[num_bs] = buffer_sizes[i];
        num_bs++;
    }

    res = vrend_renderer_ctx_res_lookup(ctx->ctx, desc_handle);
    if (!res) {
        virgl_error("%s: desc res %d not found\n", __func__, desc_handle);
        goto err;
    }
    memset(&desc, 0, sizeof(desc));
    vrend_read_from_iovec(res->iov, res->num_iovs, 0, (char *)(&desc),
                          MIN2(res->base.width0, sizeof(desc)));
    modify_picture_desc(cdc, tgt, &desc);

    err = virgl_video_decode_bitstream(cdc->codec, tgt->buffer, &desc,
                           num_bs, (const void * const *)bs_buffers, bs_sizes);

err:
    free(bs_buffers);
    free(bs_sizes);

    return err;
}

int vrend_video_encode_bitstream(struct vrend_video_context *ctx,
                                 uint32_t cdc_handle,
                                 uint32_t src_handle,
                                 uint32_t dest_handle,
                                 uint32_t desc_handle,
                                 uint32_t feed_handle)
{
    union virgl_picture_desc desc;
    struct vrend_resource *dest_res, *desc_res, *feed_res;
    struct vrend_video_codec  *cdc = get_video_codec(ctx, cdc_handle);
    struct vrend_video_buffer *src = get_video_buffer(ctx, src_handle);

    if (!cdc || !src)
        return -1;

    /* Feedback resource */
    feed_res = vrend_renderer_ctx_res_lookup(ctx->ctx, feed_handle);
    if (!feed_res || feed_res->base.width0 < sizeof(struct virgl_video_encode_feedback) ||
        vrend_get_iovec_size(feed_res->iov, feed_res->num_iovs) <
            sizeof(struct virgl_video_encode_feedback)) {
        virgl_error("%s: feedback res %d not found\n", __func__, feed_handle);
        return -1;
    }

    /* Picture descriptor resource */
    desc_res = vrend_renderer_ctx_res_lookup(ctx->ctx, desc_handle);
    if (!desc_res) {
        virgl_error("%s: desc res %d not found\n", __func__, desc_handle);
        return -1;
    }
    memset(&desc, 0, sizeof(desc));
    vrend_read_from_iovec(desc_res->iov, desc_res->num_iovs, 0, (char *)(&desc),
                          MIN2(desc_res->base.width0, sizeof(desc)));

    /* Destination buffer resource. */
    dest_res = vrend_renderer_ctx_res_lookup(ctx->ctx, dest_handle);
    if (!dest_res) {
        virgl_error("%s: dest res %d not found\n", __func__, dest_handle);
        return -1;
    }

    cdc->feed_handle = feed_handle;
    cdc->dest_handle = dest_handle;

    return virgl_video_encode_bitstream(cdc->codec, src->buffer, &desc);
}

int vrend_video_end_frame(struct vrend_video_context *ctx,
                          uint32_t cdc_handle,
                          uint32_t tgt_handle)
{
    struct vrend_video_codec *cdc = get_video_codec(ctx, cdc_handle);
    struct vrend_video_buffer *tgt = get_video_buffer(ctx, tgt_handle);

    if (!cdc || !tgt)
        return -1;

#ifdef __APPLE__
    if (cdc->entrypoint == PIPE_VIDEO_ENTRYPOINT_BITSTREAM) {
        for (unsigned p = 0; p < tgt->num_planes; p++) {
            struct virgl_resource *resource = virgl_resource_lookup(tgt->planes[p].res_handle);
            struct vrend_resource *plane = vrend_renderer_ctx_res_lookup(ctx->ctx, tgt->planes[p].res_handle);
            if (!resource || !plane || resource->native_metal_texture != tgt->planes[p].native_texture)
                return -1;
        }
        struct virgl_video_metal_fence *fence = prepare_video_planes(tgt, true);
        if (!fence) return -1;
        virgl_video_metal_fence_ref(fence); /* Callback owns this reference. */
        int result = virgl_video_end_frame_async(cdc->codec, tgt->buffer,
                                                 decoded_async, fence);
        if (result) decoded_async(fence, NULL);
        /* Match upstream END_FRAME: decoded pixels must reach the fixed
         * planes before dispatch proceeds. Wait for native completion here,
         * not for the VirGL submission fence created after this returns. */
        bool completed = virgl_video_metal_fence_wait(fence, true) > 0;
        virgl_video_metal_fence_unref(fence);
        return result ? result : (completed ? 0 : -1);
    }
#endif
    return virgl_video_end_frame(cdc->codec, tgt->buffer);
}
