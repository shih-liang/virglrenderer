/* Video transfer regression: real ANGLE GL state, fixture resource lookup. */
#include "vrend/vrend_video.c"
#include <EGL/eglext_angle.h>
#include <stdio.h>

static struct vrend_resource resources[8];
struct vrend_resource *vrend_renderer_ctx_res_lookup(struct vrend_context *ctx, int id)
{
    (void)ctx;
    return id > 0 && id < 8 ? &resources[id] : NULL;
}
static unsigned failures, checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s (GL=%x)\n", __LINE__, #expr, glGetError()); \
    failures++; } } while (0)

int main(void)
{
    const EGLint attrs[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE,
        EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE};
    EGLDisplay display = eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, NULL, attrs);
    CHECK(eglInitialize(display, NULL, NULL));
    const EGLint config_attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLConfig config;
    EGLint count;
    CHECK(eglChooseConfig(display, config_attrs, &config, 1, &count) && count);
    const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    CHECK(context != EGL_NO_CONTEXT);
    CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context));
    if (failures) return 1;

    struct vrend_video_context vctx = {0};
    struct vrend_video_buffer buffer = {.ctx = &vctx, .num_planes = 2};
    GLuint sentinel_texture, sentinel_fbo, sentinel_pbo;
    glGenTextures(1, &sentinel_texture);
    glGenFramebuffers(1, &sentinel_fbo);
    glGenBuffers(1, &sentinel_pbo);
    for (unsigned p010 = 0; p010 < 2; p010++) {
        uint8_t pixels[2][256], restored[2][256];
        memset(pixels, 0, sizeof(pixels));
        struct virgl_video_dma_buf mapped = {.width = 8, .height = 4,
            .num_planes = 2, .flags = VIRGL_VIDEO_DMABUF_READ_WRITE,
            .drm_format = p010 ? VREND_VIDEO_FOURCC('P','0','1','0')
                              : VREND_VIDEO_FOURCC('N','V','1','2')};
        for (unsigned i = 0; i < 2; i++) {
            struct vrend_resource *res = &resources[i + 1];
            res->target = GL_TEXTURE_2D;
            res->base.width0 = 8 >> i;
            res->base.height0 = 4 >> i;
            res->base.format = p010 ? (i ? PIPE_FORMAT_R16G16_UNORM : PIPE_FORMAT_R16_UNORM)
                                   : (i ? PIPE_FORMAT_R8G8_UNORM : PIPE_FORMAT_R8_UNORM);
            glGenTextures(1, &res->gl_id);
            glBindTexture(GL_TEXTURE_2D, res->gl_id);
            glTexStorage2D(GL_TEXTURE_2D, 1,
                p010 ? (i ? GL_RG16_EXT : GL_R16_EXT) : (i ? GL_RG8 : GL_R8),
                res->base.width0, res->base.height0);
            CHECK(glGetError() == GL_NO_ERROR);
            buffer.planes[i].res_handle = i + 1;
            mapped.planes[i].data = pixels[i];
            mapped.planes[i].width = res->base.width0;
            mapped.planes[i].height = res->base.height0;
            mapped.planes[i].pitch = 32;
            mapped.planes[i].size = sizeof(pixels[i]);
            for (unsigned row = 0; row < res->base.height0; row++)
                for (unsigned x = 0; x < 8; x++) {
                    if (p010) ((uint16_t *)(pixels[i] + row * 32))[x] = (100 + x + row) << 6;
                    else pixels[i][row * 32 + x] = 60 + x + row;
                }
        }
        glBindTexture(GL_TEXTURE_2D, sentinel_texture);
        glBindFramebuffer(GL_FRAMEBUFFER, sentinel_fbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, sentinel_pbo);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, sentinel_pbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 8);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
        glPixelStorei(GL_PACK_ROW_LENGTH, 27);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 29);
        glPixelStorei(GL_PACK_SKIP_ROWS, 2);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 3);
        CHECK(sync_video_planes(&buffer, &mapped, true) == 0);
        memset(restored, 0, sizeof(restored));
        for (unsigned i = 0; i < 2; i++) mapped.planes[i].data = restored[i];
        CHECK(sync_video_planes(&buffer, &mapped, false) == 0);
        for (unsigned i = 0; i < 2; i++)
            for (unsigned row = 0; row < mapped.planes[i].height; row++)
                CHECK(!memcmp(pixels[i] + row * 32, restored[i] + row * 32, p010 ? 16 : 8));
        const GLenum names[] = {GL_TEXTURE_BINDING_2D, GL_READ_FRAMEBUFFER_BINDING,
            GL_DRAW_FRAMEBUFFER_BINDING, GL_PIXEL_PACK_BUFFER_BINDING,
            GL_PIXEL_UNPACK_BUFFER_BINDING, GL_PACK_ALIGNMENT, GL_UNPACK_ALIGNMENT,
            GL_PACK_ROW_LENGTH, GL_UNPACK_ROW_LENGTH, GL_PACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS};
        const GLint expected[] = {sentinel_texture, sentinel_fbo, sentinel_fbo,
            sentinel_pbo, sentinel_pbo, 8, 8, 27, 29, 2, 3};
        for (unsigned i = 0; i < ARRAY_SIZE(names); i++) {
            GLint value; glGetIntegerv(names[i], &value); CHECK(value == expected[i]);
        }
        mapped.planes[0].size = 1;
        CHECK(sync_video_planes(&buffer, &mapped, true) != 0);
        CHECK(glGetError() == GL_NO_ERROR);
        for (unsigned i = 0; i < 2; i++) {
            glDeleteTextures(1, &resources[i + 1].gl_id);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }
    glDeleteTextures(1, &sentinel_texture);
    glDeleteFramebuffers(1, &sentinel_fbo);
    glDeleteBuffers(1, &sentinel_pbo);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglTerminate(display);
    printf("video planes: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
