/* Standalone parser/lifetime and hardware round-trip tests. No guest required. */
#include "vrend/virgl_video_videotoolbox.c"
#include <stdio.h>

static unsigned checks, failures, frame_value, decoded_frames;
static bool upload_failure, download_failure;
static struct byte_buffer encoded;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); failures++; \
} } while (0)

static int upload(struct virgl_video_codec *codec, const struct virgl_video_dma_buf *mapping)
{
    (void)codec;
    if (upload_failure) return -1;
    for (unsigned plane = 0; plane < 2; plane++) {
        bool p010 = mapping->drm_format == DRM_FORMAT_P010;
        unsigned value = plane ? 128 : frame_value;
        for (unsigned row = 0; row < mapping->planes[plane].height; row++) {
            uint8_t *bytes = mapping->planes[plane].data;
            bytes += row * mapping->planes[plane].pitch;
            unsigned components = mapping->planes[plane].width * (plane ? 2 : 1);
            if (p010)
                for (unsigned x = 0; x < components; x++) ((uint16_t *)bytes)[x] = value << 8;
            else memset(bytes, value, components);
        }
    }
    return 0;
}
static int decoded(struct virgl_video_codec *codec, const struct virgl_video_dma_buf *mapping)
{
    (void)codec;
    decoded_frames++;
    unsigned value = mapping->drm_format == DRM_FORMAT_P010 ?
        (*(const uint16_t *)mapping->planes[0].data >> 8) :
        *(const uint8_t *)mapping->planes[0].data;
    CHECK(abs((int)value - (int)frame_value) <= 3);
    return download_failure ? -1 : 0;
}
static int coded(struct virgl_video_codec *codec,
                 const struct virgl_video_dma_buf *source,
                 const struct virgl_video_dma_buf *reference, unsigned count,
                 const void *const *data, const unsigned *sizes)
{
    (void)codec; (void)source; (void)reference;
    bb_clear(&encoded);
    for (unsigned i = 0; i < count; i++)
        if (!bb_append(&encoded, data[i], sizes[i])) return -1;
    return 0;
}

static void round_trip(enum pipe_video_profile profile)
{
    struct virgl_video_create_codec_args args = {
        .profile = profile, .entrypoint = PIPE_VIDEO_ENTRYPOINT_ENCODE,
        .chroma_format = PIPE_VIDEO_CHROMA_FORMAT_420, .level = 51,
        .width = profile == PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH ? 130 : 128,
        .height = profile == PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH ? 126 : 128,
        .max_references = 4,
    };
    struct virgl_video_codec *encoder = virgl_video_create_codec(&args);
    CHECK(encoder);
    if (!encoder) return;
    args.entrypoint = PIPE_VIDEO_ENTRYPOINT_BITSTREAM;
    struct virgl_video_codec *decoder = virgl_video_create_codec(&args);
    struct virgl_video_create_buffer_args bargs = {
        .format = surface_format_for_profile(profile), .width = args.width, .height = args.height,
    };
    struct virgl_video_buffer *source = virgl_video_create_buffer(&bargs);
    struct virgl_video_buffer *target = virgl_video_create_buffer(&bargs);
    CHECK(decoder && source && target);
    if (!decoder || !source || !target) goto out;
    CHECK(!source->pixel_buffer && !target->pixel_buffer);
    CHECK(virgl_video_begin_frame(decoder, target) == 0);
    unsigned before = decoded_frames;
    CHECK(virgl_video_end_frame(decoder, target) != 0);
    CHECK(before == decoded_frames);
    upload_failure = true;
    CHECK(virgl_video_begin_frame(encoder, source) != 0);
    upload_failure = false;
    CMTime previous_pts = kCMTimeInvalid;
    VTDecompressionSessionRef first_session = NULL;
    for (unsigned frame = 0; frame < 10; frame++) {
        frame_value = 64 + frame * 8;
        union virgl_picture_desc picture = {0}, decode_picture = {0};
        picture.base.profile = decode_picture.base.profile = profile;
        unsigned fps = frame < 4 ? 30 : 60;
        if (h264_profile_supported(profile)) {
            picture.h264_enc.picture_type = frame ? PIPE_H2645_ENC_PICTURE_TYPE_P : PIPE_H2645_ENC_PICTURE_TYPE_IDR;
            picture.h264_enc.rate_ctrl[0].target_bitrate = 2000000;
            picture.h264_enc.rate_ctrl[0].frame_rate_num = fps;
            picture.h264_enc.rate_ctrl[0].frame_rate_den = 1;
        } else {
            picture.h265_enc.picture_type = frame ? PIPE_H2645_ENC_PICTURE_TYPE_P : PIPE_H2645_ENC_PICTURE_TYPE_IDR;
            picture.h265_enc.rc.target_bitrate = 2000000;
            picture.h265_enc.rc.frame_rate_num = fps;
            picture.h265_enc.rc.frame_rate_den = 1;
        }
        CHECK(virgl_video_begin_frame(encoder, source) == 0);
        CHECK(virgl_video_encode_bitstream(encoder, source, &picture) == 0);
        CHECK(virgl_video_end_frame(encoder, source) == 0);
        CHECK(encoded.size > 4 && read_u32be(encoded.data) == 1);
        if (CMTIME_IS_VALID(previous_pts)) CHECK(CMTimeCompare(encoder->next_timestamp, previous_pts) > 0);
        previous_pts = encoder->next_timestamp;
        CHECK(virgl_video_begin_frame(decoder, target) == 0);
        const void *data = encoded.data;
        unsigned size = encoded.size;
        /* Slice-data commands may split even inside a NAL prefix. */
        unsigned split = frame == 3 ? 2 : size / 2;
        CHECK(virgl_video_decode_bitstream(decoder, target, &decode_picture, 1, &data, &split) == 0);
        const void *tail = (const uint8_t *)data + split;
        unsigned tail_size = size - split;
        CHECK(virgl_video_decode_bitstream(decoder, target, &decode_picture, 1, &tail, &tail_size) == 0);
        download_failure = frame == 9;
        CHECK((virgl_video_end_frame(decoder, target) == 0) == !download_failure);
        download_failure = false;
        if (!frame) first_session = decoder->decoder;
        CHECK(first_session == decoder->decoder);
    }
    CHECK(virgl_video_begin_frame(decoder, target) == 0);
    const void *bad = ""; unsigned size = 0;
    union virgl_picture_desc picture = {0}; picture.base.profile = profile;
    before = decoded_frames;
    CHECK(virgl_video_decode_bitstream(decoder, target, &picture, 1, &bad, &size) != 0);
    CHECK(virgl_video_end_frame(decoder, target) != 0);
    CHECK(decoded_frames == before);
out:
    virgl_video_destroy_buffer(target);
    virgl_video_destroy_buffer(source);
    virgl_video_destroy_codec(decoder);
    virgl_video_destroy_codec(encoder);
    bb_clear(&encoded);
    printf("hardware round trip profile=%u complete\n", profile);
}

static void parser_tests(void)
{
    uint8_t bytes[8];
    struct bit_writer writer = {.data = bytes, .capacity = sizeof(bytes)};
    bw_ue(&writer, UINT32_MAX); CHECK(bw_finish(&writer) == 0);
    writer = (struct bit_writer){.data = bytes, .capacity = sizeof(bytes)};
    bw_se(&writer, INT32_MIN); CHECK(bw_finish(&writer) == 0);
    writer = (struct bit_writer){.data = bytes, .capacity = sizeof(bytes)};
    bw_bits(&writer, 0, 33); CHECK(bw_finish(&writer) == 0);
    struct byte_buffer data = {0};
    CHECK(bb_append(&data, NULL, 0)); CHECK(!bb_append(&data, NULL, 1));
    CHECK(!bb_reserve(&data, MAX_CODED_BYTES + 1));
    const uint8_t short_h264[] = {0x65}, short_hevc[] = {0x26, 1};
    CHECK(slice_pps_id(short_h264, sizeof(short_h264)) == UINT32_MAX);
    CHECK(hevc_slice_pps_id(short_hevc, sizeof(short_hevc)) == UINT32_MAX);
    struct virgl_video_codec codec = {.profile = PIPE_VIDEO_PROFILE_HEVC_MAIN,
        .width = 128, .height = 128, .max_references = 4};
    struct virgl_h265_picture_desc picture = {0};
    picture.pps.sps.chroma_format_idc = 1;
    picture.pps.sps.num_short_term_ref_pic_sets = 1;
    struct byte_buffer vps = {0}, sps = {0}, pps = {0};
    CHECK(!synthesize_hevc_parameter_sets(&codec, &picture, 0, &vps, &sps, &pps));
    CHECK(!vps.size && !sps.size && !pps.size);
    uint8_t avcc[264]; memset(avcc, 0xaa, sizeof(avcc));
    avcc[0] = avcc[1] = 0; avcc[2] = 1; avcc[3] = 4; /* Length 260, not Annex B. */
    avcc[4] = 0x65; avcc[5] = 0xb8; avcc[263] = 0;
    const void *input = avcc; unsigned input_size = sizeof(avcc); uint32_t pps_id;
    CHECK(build_h26x_sample(false, 1, &input, &input_size, &data, &vps, &sps, &pps, &pps_id));
    CHECK(data.size == sizeof(avcc) && !memcmp(data.data, avcc, sizeof(avcc)) && pps_id == 0);
    bb_clear(&data);
    const uint8_t raw[] = {0x65, 0xb8, 0, 0};
    input = raw; input_size = sizeof(raw);
    CHECK(build_h26x_sample(false, 1, &input, &input_size, &data, &vps, &sps, &pps, &pps_id));
    CHECK(data.size == 8 && !memcmp(data.data + 4, raw, sizeof(raw)));
    bb_clear(&data);
    struct virgl_video_create_buffer_args args = {.format = PIPE_FORMAT_NV12, .width = UINT32_MAX, .height = 16};
    CHECK(!virgl_video_create_buffer(&args));
}

int main(void)
{
    struct virgl_video_callbacks callbacks = {
        .decode_completed = decoded, .encode_upload_picture = upload,
        .encode_completed = coded,
    };
    CHECK(virgl_video_init(-1, &callbacks, 0) == 0);
    union virgl_caps caps = {0};
    CHECK(virgl_video_fill_caps(&caps) == 0);
    CHECK(caps.v2.num_video_caps > 0);
    for (unsigned i = 0; i < caps.v2.num_video_caps; i++) {
        printf("capability profile=%u entrypoint=%u\n",
            caps.v2.video_caps[i].profile, caps.v2.video_caps[i].entrypoint);
        CHECK(caps.v2.video_caps[i].entrypoint != PIPE_VIDEO_ENTRYPOINT_BITSTREAM ||
              (!hevc_profile_supported(caps.v2.video_caps[i].profile) &&
               caps.v2.video_caps[i].profile != PIPE_VIDEO_PROFILE_AV1_MAIN));
    }
    parser_tests();
    round_trip(PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE);
    round_trip(PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN);
    round_trip(PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH);
    round_trip(PIPE_VIDEO_PROFILE_HEVC_MAIN);
    round_trip(PIPE_VIDEO_PROFILE_HEVC_MAIN_10);
    virgl_video_destroy();
    printf("VideoToolbox: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
