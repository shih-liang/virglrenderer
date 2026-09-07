/* SPDX-License-Identifier: MIT
 * Test-only FFmpeg syntax oracle + VideoToolbox decode comparison. The guest
 * descriptor is populated from trace_headers, not from the code under test.
 */
#include "vrend/virgl_video_videotoolbox.c"
#include "video_decode_test.h"
#include <libavformat/avformat.h>
#include <libavcodec/bsf.h>
#include <libavutil/log.h>
#include <stdio.h>
#include <stdarg.h>

static unsigned checks, failures, frames, pixel_differences;
static unsigned completed_frames;
#define CHECK(c) do { checks++; if (!(c)) { \
   fprintf(stderr, "FAIL frame %u line %u: %s\n", frames, __LINE__, #c); failures++; \
} } while (0)

static int completed(struct virgl_video_codec *codec, const struct virgl_video_dma_buf *planes)
{
   (void)codec;
   CHECK(planes && planes->num_planes == 2 && planes->native_frame &&
         !planes->planes[0].data && !planes->planes[1].data);
   completed_frames++;
   return 0;
}

struct trace_state {
   struct virgl_h265_picture_desc picture;
   unsigned type, tid, lsb, pps_id, slices, negative, positive;
   unsigned delta[2][16], used[2][16];
   size_t header_end[128];
   size_t rps_begin[128], rps_end[128], align_begin[128];
   bool in_slice;
};
static struct trace_state *traced;
static char line[4096];
static size_t line_size;

static uint8_t *scaling_matrix(struct virgl_h265_sps *s, unsigned size, unsigned matrix)
{
   if (size == 0) return s->ScalingList4x4[matrix];
   if (size == 1) return s->ScalingList8x8[matrix];
   if (size == 2) return s->ScalingList16x16[matrix];
   return s->ScalingList32x32[matrix / 3];
}

static void default_scaling(struct virgl_h265_sps *s)
{
   /* H.265 default 8x8 matrices (raster order), converted to diagonal order
    * used by the Gallium descriptor. These are normative numeric tables. */
   static const uint8_t defaults[2][64] = {
      {16,16,16,16,17,18,21,24, 16,16,16,16,17,19,22,25,
       16,16,17,18,20,22,25,29, 16,16,18,21,24,27,31,36,
       17,17,20,24,30,35,41,47, 18,19,22,27,35,44,54,65,
       21,22,25,31,41,54,70,88, 24,25,29,36,47,65,88,115},
      {16,16,16,16,17,18,20,24, 16,16,16,17,18,20,24,25,
       16,16,17,18,20,24,25,28, 16,17,18,20,24,25,28,33,
       17,18,20,24,25,28,33,41, 18,20,24,25,28,33,41,54,
       20,24,25,28,33,41,54,71, 24,25,28,33,41,54,71,91},
   };
   memset(s->ScalingList4x4, 16, sizeof(s->ScalingList4x4));
   memset(s->ScalingListDCCoeff16x16, 16, sizeof(s->ScalingListDCCoeff16x16));
   memset(s->ScalingListDCCoeff32x32, 16, sizeof(s->ScalingListDCCoeff32x32));
   for (unsigned size = 1; size < 4; size++) for (unsigned m = 0; m < 6; m += size == 3 ? 3 : 1) {
      uint8_t *dst = scaling_matrix(s, size, m); unsigned n = 0;
      for (unsigned diagonal = 0; diagonal < 15; diagonal++)
         for (unsigned x = diagonal > 7 ? diagonal - 7 : 0; x <= diagonal && x < 8; x++)
            dst[n++] = defaults[m >= 3][(diagonal - x) * 8 + x];
   }
}

static void parse_line(void)
{
   if (!traced) return;
   struct trace_state *t = traced;
   if (strstr(line, "Slice Segment Header")) { t->slices++; t->in_slice = true; }
   if (strstr(line, "Parameter Set")) t->in_slice = false;
   size_t pos; char name[128], bits[256]; long value;
   if (sscanf(line, "%zu %127s %255s = %ld", &pos, name, bits, &value) != 4) return;
   if (t->in_slice) {
      if (t->slices > 128) { failures++; return; }
      unsigned slice = t->slices - 1;
      t->header_end[slice] = pos + strlen(bits);
      if (!strcmp(name, "alignment_bit_equal_to_one")) t->align_begin[slice] = pos;
      if (!strcmp(name, "short_term_ref_pic_set_sps_flag")) t->rps_begin[slice] = pos;
      if (!strcmp(name, "num_negative_pics") || !strcmp(name, "num_positive_pics") ||
          !strncmp(name, "delta_poc_s", 11) || !strncmp(name, "used_by_curr_pic_s", 18))
         t->rps_end[slice] = pos + strlen(bits);
      if (t->slices != 1) return;
      unsigned i;
      if (!strcmp(name, "nal_unit_type")) t->type = value;
      if (!strcmp(name, "nuh_temporal_id_plus1")) t->tid = value;
      if (!strcmp(name, "slice_pic_order_cnt_lsb")) t->lsb = value;
      if (!strcmp(name, "slice_pic_parameter_set_id")) t->pps_id = value;
      if (!strcmp(name, "num_negative_pics")) t->negative = value;
      if (!strcmp(name, "num_positive_pics")) t->positive = value;
      if (sscanf(name, "delta_poc_s0_minus1[%u]", &i) == 1 && i < 16) t->delta[0][i] = value + 1;
      if (sscanf(name, "delta_poc_s1_minus1[%u]", &i) == 1 && i < 16) t->delta[1][i] = value + 1;
      if (sscanf(name, "used_by_curr_pic_s0_flag[%u]", &i) == 1 && i < 16) t->used[0][i] = value;
      if (sscanf(name, "used_by_curr_pic_s1_flag[%u]", &i) == 1 && i < 16) t->used[1][i] = value;
      return;
   }
   struct virgl_h265_sps *s = &t->picture.pps.sps;
   struct virgl_h265_pps *p = &t->picture.pps;
   if (!strcmp(name, "scaling_list_enabled_flag") && value) default_scaling(s);
   unsigned sz, matrix, coef;
   if (sscanf(name, "scaling_list_dc_coef_minus8[%u][%u]", &sz, &matrix) == 2 && sz < 2 && matrix < 6) {
      if (sz == 0) s->ScalingListDCCoeff16x16[matrix] = value + 8;
      else s->ScalingListDCCoeff32x32[matrix / 3] = value + 8;
   }
   if (sscanf(name, "scaling_list_delta_coeff[%u][%u][%u]", &sz, &matrix, &coef) == 3 &&
       sz < 4 && matrix < 6 && coef < (sz ? 64u : 16u)) {
      uint8_t *values = scaling_matrix(s, sz, matrix);
      unsigned previous = coef ? values[coef - 1] : sz < 2 ? 8 :
         sz == 2 ? s->ScalingListDCCoeff16x16[matrix] : s->ScalingListDCCoeff32x32[matrix / 3];
      values[coef] = (previous + value + 256) & 255;
   }
#define S(field) if (!strcmp(name, #field)) s->field = value
#define P(field) if (!strcmp(name, #field)) p->field = value
   S(pic_width_in_luma_samples); S(pic_height_in_luma_samples); S(chroma_format_idc);
   S(separate_colour_plane_flag); S(bit_depth_luma_minus8); S(bit_depth_chroma_minus8);
   S(log2_max_pic_order_cnt_lsb_minus4); S(log2_min_luma_coding_block_size_minus3);
   S(log2_diff_max_min_luma_coding_block_size); S(max_transform_hierarchy_depth_inter);
   S(max_transform_hierarchy_depth_intra); S(scaling_list_enabled_flag); S(amp_enabled_flag);
   S(sample_adaptive_offset_enabled_flag); S(pcm_enabled_flag);
   S(pcm_sample_bit_depth_luma_minus1); S(pcm_sample_bit_depth_chroma_minus1);
   S(log2_min_pcm_luma_coding_block_size_minus3); S(log2_diff_max_min_pcm_luma_coding_block_size);
   S(pcm_loop_filter_disabled_flag); S(num_short_term_ref_pic_sets);
   S(long_term_ref_pics_present_flag); S(num_long_term_ref_pics_sps);
   S(sps_temporal_mvp_enabled_flag); S(strong_intra_smoothing_enabled_flag);
   if (!strcmp(name, "log2_min_luma_transform_block_size_minus2")) s->log2_min_transform_block_size_minus2 = value;
   if (!strcmp(name, "log2_diff_max_min_luma_transform_block_size")) s->log2_diff_max_min_transform_block_size = value;
   if (!strncmp(name, "sps_max_dec_pic_buffering_minus1[", 31)) s->sps_max_dec_pic_buffering_minus1 = value;
   P(dependent_slice_segments_enabled_flag); P(output_flag_present_flag);
   P(num_extra_slice_header_bits); P(sign_data_hiding_enabled_flag); P(cabac_init_present_flag);
   P(num_ref_idx_l0_default_active_minus1); P(num_ref_idx_l1_default_active_minus1);
   P(init_qp_minus26); P(constrained_intra_pred_flag); P(transform_skip_enabled_flag);
   P(cu_qp_delta_enabled_flag); P(diff_cu_qp_delta_depth); P(pps_cb_qp_offset); P(pps_cr_qp_offset);
   P(pps_slice_chroma_qp_offsets_present_flag); P(weighted_pred_flag); P(weighted_bipred_flag);
   P(transquant_bypass_enabled_flag); P(tiles_enabled_flag); P(entropy_coding_sync_enabled_flag);
   P(num_tile_columns_minus1); P(num_tile_rows_minus1); P(uniform_spacing_flag);
   P(loop_filter_across_tiles_enabled_flag); P(pps_loop_filter_across_slices_enabled_flag);
   P(deblocking_filter_control_present_flag); P(deblocking_filter_override_enabled_flag);
   P(pps_deblocking_filter_disabled_flag); P(pps_beta_offset_div2); P(pps_tc_offset_div2);
   P(lists_modification_present_flag); P(log2_parallel_merge_level_minus2);
   P(slice_segment_header_extension_present_flag);
#undef P
#undef S
}

static void trace_log(void *context, int level, const char *format, va_list ap)
{
   (void)context;
   if (level > AV_LOG_TRACE) return;
   char part[2048]; vsnprintf(part, sizeof(part), format, ap);
   if (level <= AV_LOG_ERROR) { fputs(part, stderr); failures++; }
   for (const char *p = part; *p; p++) {
      if (line_size + 1 < sizeof(line)) line[line_size++] = *p;
      if (*p == '\n') { line[line_size] = 0; parse_line(); line_size = 0; }
   }
}

static size_t payload_offset(const uint8_t *nal, size_t size, size_t bits)
{
   if (bits % 8 || bits < 16) return SIZE_MAX;
   size_t bytes = bits / 8 - 2, pos = 2;
   unsigned zeroes = 0;
   while (bytes && pos < size) {
      if (zeroes == 2 && nal[pos] == 3) { zeroes = 0; pos++; continue; }
      zeroes = nal[pos] ? 0 : zeroes + 1; pos++; bytes--;
   }
   return bytes ? SIZE_MAX : pos;
}

static bool refer_to_sps(const uint8_t *nal, size_t size, const struct trace_state *t,
                         unsigned slice, struct byte_buffer *out)
{
   if (!t->rps_begin[slice]) return bb_append(out, nal, size);
   size_t start = t->rps_begin[slice] - 16, end = t->rps_end[slice] - 16;
   size_t alignment = t->align_begin[slice] - 16;
   size_t payload = payload_offset(nal, size, t->header_end[slice]);
   if (end <= start || alignment < end || payload > size) return false;
   uint8_t *rbsp = malloc(payload), *header = malloc(payload), *escaped = malloc(payload * 2);
   if (!rbsp || !header || !escaped) { free(rbsp); free(header); free(escaped); return false; }
   size_t n = 0; unsigned zeros = 0;
   for (size_t i = 2; i < payload; i++) {
      if (zeros == 2 && nal[i] == 3) { zeros = 0; continue; }
      rbsp[n++] = nal[i]; zeros = nal[i] ? 0 : zeros + 1;
   }
   struct bit_writer w = {.data = header, .capacity = payload};
   for (size_t bit = 0; bit < start; bit++) bw_bit(&w, rbsp[bit / 8] >> (7 - bit % 8) & 1);
   bw_bit(&w, 1); /* use the one SPS RPS whose resolved values are in the descriptor */
   for (size_t bit = end; bit < alignment; bit++) bw_bit(&w, rbsp[bit / 8] >> (7 - bit % 8) & 1);
   size_t header_bytes = bw_finish(&w);
   size_t escaped_bytes = hevc_nal_escape((nal[0] >> 1) & 63, header, header_bytes, escaped, payload * 2);
   if (escaped_bytes) escaped[1] = nal[1];
   bool ok = escaped_bytes && bb_append(out, escaped, escaped_bytes) && bb_append(out, nal + payload, size - payload);
   free(rbsp); free(header); free(escaped);
   return ok;
}

static void compare_payloads(const struct byte_buffer *a, const struct byte_buffer *b,
                              const struct trace_state *ta, const struct trace_state *tb)
{
   CHECK(ta->slices == tb->slices);
   size_t ai = 0, bi = 0; unsigned slice = 0;
   while (ai < a->size && bi < b->size) {
      unsigned an = read_u32be(a->data + ai), bn = read_u32be(b->data + bi);
      ai += 4; bi += 4;
      if ((a->data[ai] >> 1 & 63) <= 31) {
         CHECK(slice < ta->slices && slice < 128);
         if (slice >= ta->slices || slice >= 128) return;
         size_t ah = payload_offset(a->data + ai, an, ta->header_end[slice]);
         size_t bh = payload_offset(b->data + bi, bn, tb->header_end[slice]);
         CHECK(ah <= an && bh <= bn);
         if (ah <= an && bh <= bn) {
            CHECK(an - ah == bn - bh);
            if (an - ah == bn - bh) CHECK(!memcmp(a->data + ai + ah, b->data + bi + bh, an - ah));
         }
         slice++;
      }
      ai += an; bi += bn;
   }
   CHECK(ai == a->size && bi == b->size && slice == ta->slices);
}

static bool describe_frame(struct trace_state *t, int *previous)
{
   struct virgl_h265_picture_desc *p = &t->picture;
   unsigned bits = p->pps.sps.log2_max_pic_order_cnt_lsb_minus4 + 4;
   unsigned modulus = 1u << bits;
   int msb = *previous - ((unsigned)*previous & (modulus - 1));
   unsigned prev_lsb = (unsigned)*previous & (modulus - 1);
   if (t->lsb < prev_lsb && prev_lsb - t->lsb >= modulus / 2) msb += modulus;
   else if (t->lsb > prev_lsb && t->lsb - prev_lsb > modulus / 2) msb -= modulus;
   p->IDRPicFlag = t->type == 19 || t->type == 20;
   p->RAPPicFlag = t->type >= 16 && t->type <= 23;
   p->CurrPicOrderCntVal = p->IDRPicFlag ? 0 : msb + t->lsb;
   if (t->tid == 1 && t->type != 6 && t->type != 7 && t->type != 8 && t->type != 9)
      *previous = p->CurrPicOrderCntVal;
   if (t->negative + t->positive > 15) return false;
   unsigned idx = 0;
   for (unsigned group = 0; group < 2; group++) {
      int poc = p->CurrPicOrderCntVal;
      unsigned n = group ? t->positive : t->negative;
      for (unsigned i = 0; i < n; i++, idx++) {
         poc += (group ? 1 : -1) * (int)t->delta[group][i];
         p->ref[idx] = poc + 1000; p->PicOrderCntVal[idx] = poc;
         if (t->used[group][i]) {
            if (group) p->RefPicSetStCurrAfter[p->NumPocStCurrAfter++] = idx;
            else p->RefPicSetStCurrBefore[p->NumPocStCurrBefore++] = idx;
         }
      }
   }
   p->NumPocTotalCurr = p->NumPocStCurrBefore + p->NumPocStCurrAfter;
   return true;
}

static void compare_pixels(CVPixelBufferRef a, CVPixelBufferRef b)
{
   CHECK(a && b);
   if (!a || !b) return;
   CHECK(CVPixelBufferGetWidth(a) == CVPixelBufferGetWidth(b));
   CHECK(CVPixelBufferGetHeight(a) == CVPixelBufferGetHeight(b));
   CHECK(CVPixelBufferGetPixelFormatType(a) == CVPixelBufferGetPixelFormatType(b));
   CHECK(CVPixelBufferLockBaseAddress(a, kCVPixelBufferLock_ReadOnly) == 0);
   CHECK(CVPixelBufferLockBaseAddress(b, kCVPixelBufferLock_ReadOnly) == 0);
   bool different = false;
   bool high = CVPixelBufferGetPixelFormatType(a) == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
   for (unsigned plane = 0; plane < 2; plane++) {
      size_t rows = CVPixelBufferGetHeightOfPlane(a, plane);
      size_t bytes = CVPixelBufferGetWidthOfPlane(a, plane) * (plane ? 2 : 1) * (high ? 2 : 1);
      for (size_t y = 0; y < rows; y++) {
         const uint8_t *pa = CVPixelBufferGetBaseAddressOfPlane(a, plane);
         const uint8_t *pb = CVPixelBufferGetBaseAddressOfPlane(b, plane);
         if (memcmp(pa + y * CVPixelBufferGetBytesPerRowOfPlane(a, plane),
                    pb + y * CVPixelBufferGetBytesPerRowOfPlane(b, plane), bytes)) different = true;
      }
   }
   if (different) { pixel_differences++; fprintf(stderr, "DIAGNOSTIC: frame %u pixels differ\n", frames); }
   CVPixelBufferUnlockBaseAddress(a, kCVPixelBufferLock_ReadOnly);
   CVPixelBufferUnlockBaseAddress(b, kCVPixelBufferLock_ReadOnly);
}

int main(int argc, char **argv)
{
   if (argc != 2) return 2;
   AVFormatContext *input = NULL;
   CHECK(avformat_open_input(&input, argv[1], NULL, NULL) == 0);
   CHECK(avformat_find_stream_info(input, NULL) >= 0);
   int index = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
   CHECK(index >= 0);
   if (failures) return 1;
   AVCodecParameters *par = input->streams[index]->codecpar;
   struct trace_state original = {0}; traced = &original;
   av_log_set_callback(trace_log); av_log_set_level(AV_LOG_TRACE);
   AVBSFContext *bsf = NULL;
   CHECK(av_bsf_list_parse_str("hevc_mp4toannexb,trace_headers", &bsf) == 0);
   avcodec_parameters_copy(bsf->par_in, par);
   CHECK(av_bsf_init(bsf) == 0);
   struct virgl_video_codec original_vt = {.width = par->width, .height = par->height};
   struct virgl_video_codec rebuilt_vt = original_vt;
   struct virgl_video_callbacks callbacks = {.decode_completed = completed};
   CHECK(virgl_video_init(-1, &callbacks, 0) == 0);
   struct virgl_video_codec *backend = NULL;
   struct virgl_video_buffer *backend_target = NULL;
   int previous_poc = 0, normalized_poc = 0;
   AVBSFContext *normal_bsf = NULL;
   CHECK(av_bsf_alloc(av_bsf_get_by_name("trace_headers"), &normal_bsf) == 0);
   normal_bsf->par_in->codec_id = AV_CODEC_ID_HEVC;
   CHECK(av_bsf_init(normal_bsf) == 0);
   AVPacket *packet = av_packet_alloc(), *filtered = av_packet_alloc();
   while (!failures && av_read_frame(input, packet) >= 0) {
      if (packet->stream_index != index) { av_packet_unref(packet); continue; }
      traced = &original;
      struct virgl_h265_pps saved = original.picture.pps;
      original = (struct trace_state){0}; original.picture.pps = saved;
      CHECK(av_bsf_send_packet(bsf, packet) == 0);
      CHECK(av_bsf_receive_packet(bsf, filtered) == 0);
      frames++;
      CHECK(describe_frame(&original, &previous_poc));
      struct virgl_h265_picture_desc *desc = &original.picture;
      /* This corpus oracle currently expects explicit short-term RPS. Other
       * forms are exercised separately; never silently invent missing syntax. */
      CHECK(!desc->pps.sps.num_short_term_ref_pic_sets);
      enum pipe_format format = desc->pps.sps.bit_depth_luma_minus8 ? PIPE_FORMAT_P010 : PIPE_FORMAT_NV12;
      rebuilt_vt.profile = original_vt.profile = format == PIPE_FORMAT_P010 ? PIPE_VIDEO_PROFILE_HEVC_MAIN_10 : PIPE_VIDEO_PROFILE_HEVC_MAIN;
      struct byte_buffer sample = {0}, vps = {0}, sps = {0}, pps = {0};
      const void *data = filtered->data; unsigned size = filtered->size, pps_id;
      CHECK(build_h26x_sample(true, 1, &data, &size, &sample, &vps, &sps, &pps, &pps_id));
      if (vps.size && sps.size && pps.size) CHECK(ensure_hevc_decoder(&original_vt, format, &vps, &sps, &pps));
      bb_clear(&vps); bb_clear(&sps); bb_clear(&pps);
      CHECK(synthesize_hevc_parameter_sets(&rebuilt_vt, desc, original.pps_id, &vps, &sps, &pps));
      CHECK(ensure_hevc_decoder(&rebuilt_vt, format, &vps, &sps, &pps));
      struct byte_buffer rewritten = {0};
      unsigned slice_index = 0;
      for (size_t off = 0; off < sample.size && !failures;) {
         unsigned n = read_u32be(sample.data + off); off += 4;
         CHECK(n <= sample.size - off);
         const uint8_t *nal = sample.data + off;
         struct virgl_video_bitstream out = {0};
         if ((nal[0] >> 1 & 63) <= 31) {
            CHECK(virgl_video_hevc_rewrite(desc, nal, n, &out));
            if (out.data) CHECK(bb_append_u32be(&rewritten, out.size) && bb_append(&rewritten, out.data, out.size));
            if (out.data && original.rps_begin[slice_index]) {
               /* No original SPS accompanies the call. The same real CABAC
                * data now refers to an SPS set instead of an inline RPS. */
               struct virgl_h265_picture_desc indexed = *desc;
               indexed.pps.sps.num_short_term_ref_pic_sets = 1;
               struct byte_buffer from_sps = {0};
               struct virgl_video_bitstream restored = {0};
               CHECK(refer_to_sps(nal, n, &original, slice_index, &from_sps));
               CHECK(virgl_video_hevc_rewrite(&indexed, from_sps.data, from_sps.size, &restored));
               CHECK(restored.size == out.size && !memcmp(restored.data, out.data, out.size));
               bb_clear(&from_sps); free(restored.data);
            }
            slice_index++;
         } else CHECK(bb_append_u32be(&rewritten, n) && bb_append(&rewritten, nal, n));
         free(out.data); off += n;
      }
      struct virgl_video_buffer a = {.format = format, .width = par->width, .height = par->height};
      struct virgl_video_buffer b = a;
      if (!failures) {
         if (!backend) {
            struct virgl_video_create_codec_args args = {.profile = rebuilt_vt.profile,
               .entrypoint = PIPE_VIDEO_ENTRYPOINT_BITSTREAM, .chroma_format = PIPE_VIDEO_CHROMA_FORMAT_420,
               .width = par->width, .height = par->height, .max_references = 16};
            backend = virgl_video_create_codec(&args);
            struct virgl_video_create_buffer_args buffer_args = {.format = format,
               .width = par->width, .height = par->height};
            backend_target = virgl_video_create_buffer(&buffer_args);
         }
         CHECK(backend && backend_target);
         if (backend && backend_target) {
            union virgl_picture_desc wire = {0}; wire.h265 = *desc; wire.base.profile = rebuilt_vt.profile;
            const void *bytes = sample.data; unsigned length = sample.size;
            CHECK(virgl_video_begin_frame(backend, backend_target) == 0);
            CHECK(virgl_video_decode_bitstream(backend, backend_target, &wire, 1, &bytes, &length) == 0);
            CHECK(test_decode_wait(backend, backend_target, NULL, completed) == 0);
         }
         struct byte_buffer annexb = {0};
         append_annexb_nal(&annexb, vps.data, vps.size);
         append_annexb_nal(&annexb, sps.data, sps.size);
         append_annexb_nal(&annexb, pps.data, pps.size);
         for (size_t off = 0; off < rewritten.size;) {
            unsigned n = read_u32be(rewritten.data + off); off += 4;
            append_annexb_nal(&annexb, rewritten.data + off, n); off += n;
         }
         struct trace_state normalized = {0}; traced = &normalized;
         AVPacket *norm = av_packet_alloc(); av_new_packet(norm, annexb.size);
         memcpy(norm->data, annexb.data, annexb.size);
         CHECK(av_bsf_send_packet(normal_bsf, norm) == 0);
         CHECK(av_bsf_receive_packet(normal_bsf, norm) == 0);
         av_packet_free(&norm); bb_clear(&annexb); traced = &original;
         CHECK(describe_frame(&normalized, &normalized_poc));
         CHECK(desc->CurrPicOrderCntVal == normalized.picture.CurrPicOrderCntVal);
         CHECK(desc->NumPocStCurrBefore == normalized.picture.NumPocStCurrBefore);
         CHECK(desc->NumPocStCurrAfter == normalized.picture.NumPocStCurrAfter);
         CHECK(!memcmp(desc->PicOrderCntVal, normalized.picture.PicOrderCntVal, sizeof(desc->PicOrderCntVal)));
         if (desc->pps.sps.scaling_list_enabled_flag) {
#define MATRIX(field) CHECK(!memcmp(desc->pps.sps.field, normalized.picture.pps.sps.field, sizeof(desc->pps.sps.field)))
            MATRIX(ScalingList4x4); MATRIX(ScalingList8x8); MATRIX(ScalingList16x16);
            MATRIX(ScalingList32x32); MATRIX(ScalingListDCCoeff16x16); MATRIX(ScalingListDCCoeff32x32);
#undef MATRIX
         }
         compare_payloads(&sample, &rewritten, &original, &normalized);
         CHECK(test_decode_wait(&original_vt, &a, &sample, NULL) == 0);
         CHECK(test_decode_wait(&rebuilt_vt, &b, &rewritten, NULL) == 0);
         if (a.pixel_buffer && b.pixel_buffer) compare_pixels(a.pixel_buffer, b.pixel_buffer);
         if (a.pixel_buffer && backend_target && backend_target->pixel_buffer)
            compare_pixels(a.pixel_buffer, backend_target->pixel_buffer);
      }
      if (a.pixel_buffer) CVPixelBufferRelease(a.pixel_buffer);
      if (b.pixel_buffer) CVPixelBufferRelease(b.pixel_buffer);
      bb_clear(&sample); bb_clear(&rewritten); bb_clear(&vps); bb_clear(&sps); bb_clear(&pps);
      av_packet_unref(filtered);
   }
   destroy_decoder(&original_vt); destroy_decoder(&rebuilt_vt);
   virgl_video_destroy_buffer(backend_target); virgl_video_destroy_codec(backend);
   virgl_video_destroy();
   av_packet_free(&packet); av_packet_free(&filtered); av_bsf_free(&bsf); av_bsf_free(&normal_bsf); avformat_close_input(&input);
   CHECK(frames > 0);
   CHECK(completed_frames == frames);
   printf("HEVC normalization: %u frames, %u checks, %u failures; %u pixel differences (diagnostic)\n",
          frames, checks, failures, pixel_differences);
   return failures != 0;
}
