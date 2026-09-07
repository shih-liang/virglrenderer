/* SPDX-License-Identifier: MIT
 * Independent parser/decoder oracle. FFmpeg/dav1d are test dependencies only.
 * Input is IVF. Each original AV1 frame is decoded by dav1d; its public parsed
 * header is translated to the same resolved descriptor used by guest Mesa.
 * FFmpeg's trace_headers locates tile bytes independently of the rewriter.
 */
#include <dav1d/dav1d.h>
#include <libavcodec/bsf.h>
#include <libavutil/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "vrend/virgl_video_bitstream.h"
#ifdef TEST_VIDEOTOOLBOX
#include "vrend/virgl_video_videotoolbox.c"
#include "video_decode_test.h"
static struct virgl_video_codec vt_codec;
static unsigned vt_frames;

static void check_vt(const struct virgl_video_bitstream *sample,
                     const struct virgl_video_bitstream *config,
                     const Dav1dPicture *picture);
#endif

static unsigned frames, failures, checks;
static size_t header_end;
static bool tracing_frame;
static bool show_existing;
static unsigned pixel_differences;
static char log_line[4096];
static size_t log_size;
#define CHECK(c) do { checks++; if (!(c)) { \
   fprintf(stderr, "FAIL frame %u line %d: %s\n", frames, __LINE__, #c); failures++; \
} } while (0)

static void trace_line(void)
{
   if (strstr(log_line, "Frame Header")) { tracing_frame = true; header_end = 0; }
   if (tracing_frame) {
      size_t pos; char name[128], bits[256];
      if (sscanf(log_line, "%zu %127s %255s", &pos, name, bits) == 3 &&
          strspn(bits, "01") == strlen(bits)) {
         header_end = pos + strlen(bits);
         if (!strcmp(name, "show_existing_frame")) show_existing = bits[0] == '1';
      }
   }
   if (strstr(log_line, "Invalid") || strstr(log_line, "Failed")) fputs(log_line, stderr);
}

static void trace_log(void *unused, int level, const char *format, va_list args)
{
   (void)unused;
   if (level > AV_LOG_TRACE) return;
   char part[2048];
   vsnprintf(part, sizeof(part), format, args);
   for (const char *p = part; *p; p++) {
      if (log_size + 1 < sizeof(log_line)) log_line[log_size++] = *p;
      if (*p == '\n') { log_line[log_size] = 0; trace_line(); log_size = 0; }
   }
}

static void describe(struct virgl_av1_picture_desc *d, const Dav1dPicture *pic,
                     const uint32_t refs[8], uint32_t id)
{
   const Dav1dSequenceHeader *s = pic->seq_hdr;
   const Dav1dFrameHeader *f = pic->frame_hdr;
   __typeof__(d->picture_parameter) *p = &d->picture_parameter;
   memset(d, 0, sizeof(*d));
   memcpy(d->ref, refs, 8 * sizeof(*refs));
   p->profile = s->profile; p->order_hint_bits_minus_1 = s->order_hint_n_bits ? s->order_hint_n_bits - 1 : 0;
   p->bit_depth_idx = s->hbd;
   p->seq_info_fields.use_128x128_superblock = s->sb128;
   p->seq_info_fields.enable_filter_intra = s->filter_intra;
   p->seq_info_fields.enable_intra_edge_filter = s->intra_edge_filter;
   p->seq_info_fields.enable_interintra_compound = s->inter_intra;
   p->seq_info_fields.enable_masked_compound = s->masked_compound;
   p->seq_info_fields.enable_dual_filter = s->dual_filter;
   p->seq_info_fields.enable_order_hint = s->order_hint;
   p->seq_info_fields.enable_jnt_comp = s->jnt_comp;
   p->seq_info_fields.enable_cdef = s->cdef;
   p->seq_info_fields.mono_chrome = s->monochrome;
   p->seq_info_fields.ref_frame_mvs = s->ref_frame_mvs;
   p->seq_info_fields.film_grain_params_present = s->film_grain_present;
   p->current_frame_id = id;
   p->frame_width = f->width[1]; p->frame_height = f->height;
   p->max_width = s->max_width; p->max_height = s->max_height;
   for (unsigned i = 0; i < 7; i++) p->ref_frame_idx[i] = f->refidx[i];
   p->primary_ref_frame = f->primary_ref_frame; p->order_hint = f->frame_offset;
   p->seg_info.segment_info_fields.enabled = f->segmentation.enabled;
   p->seg_info.segment_info_fields.update_map = f->segmentation.update_map;
   p->seg_info.segment_info_fields.update_data = f->segmentation.update_data;
   p->seg_info.segment_info_fields.temporal_update = f->segmentation.temporal;
   for (unsigned i = 0; i < 8; i++) {
      const Dav1dSegmentationData *sd = &f->segmentation.seg_data.d[i];
      int v[] = {sd->delta_q, sd->delta_lf_y_v, sd->delta_lf_y_h,
                 sd->delta_lf_u, sd->delta_lf_v, sd->ref, sd->skip, sd->globalmv};
      for (unsigned j = 0; j < 8; j++) {
         bool enabled = j == 5 ? v[j] >= 0 : v[j] != 0;
         if (enabled) p->seg_info.feature_mask[i] |= 1u << j;
         p->seg_info.feature_data[i][j] = j < 6 && enabled ? v[j] : 0;
      }
   }
   __typeof__(p->film_grain_info) *g = &p->film_grain_info;
   const Dav1dFilmGrainData *fg = &f->film_grain.data;
   g->film_grain_info_fields.apply_grain = f->film_grain.present;
   g->film_grain_info_fields.chroma_scaling_from_luma = fg->chroma_scaling_from_luma;
   g->film_grain_info_fields.grain_scaling_minus_8 = fg->scaling_shift - 8;
   g->film_grain_info_fields.ar_coeff_lag = fg->ar_coeff_lag;
   g->film_grain_info_fields.ar_coeff_shift_minus_6 = fg->ar_coeff_shift - 6;
   g->film_grain_info_fields.grain_scale_shift = fg->grain_scale_shift;
   g->film_grain_info_fields.overlap_flag = fg->overlap_flag;
   g->film_grain_info_fields.clip_to_restricted_range = fg->clip_to_restricted_range;
   g->grain_seed = fg->seed; g->num_y_points = fg->num_y_points;
   g->num_cb_points = fg->num_uv_points[0]; g->num_cr_points = fg->num_uv_points[1];
   for (unsigned i = 0; i < 14; i++) {
      g->point_y_value[i] = fg->y_points[i][0]; g->point_y_scaling[i] = fg->y_points[i][1];
   }
   for (unsigned i = 0; i < 10; i++) {
      g->point_cb_value[i] = fg->uv_points[0][i][0]; g->point_cb_scaling[i] = fg->uv_points[0][i][1];
      g->point_cr_value[i] = fg->uv_points[1][i][0]; g->point_cr_scaling[i] = fg->uv_points[1][i][1];
   }
   memcpy(g->ar_coeffs_y, fg->ar_coeffs_y, 24);
   memcpy(g->ar_coeffs_cb, fg->ar_coeffs_uv[0], 25);
   memcpy(g->ar_coeffs_cr, fg->ar_coeffs_uv[1], 25);
   if (fg->num_uv_points[0]) {
      g->cb_mult = fg->uv_mult[0] + 128;
      g->cb_luma_mult = fg->uv_luma_mult[0] + 128;
      g->cb_offset = fg->uv_offset[0] + 256;
   }
   if (fg->num_uv_points[1]) {
      g->cr_mult = fg->uv_mult[1] + 128;
      g->cr_luma_mult = fg->uv_luma_mult[1] + 128;
      g->cr_offset = fg->uv_offset[1] + 256;
   }
   p->tile_cols = f->tiling.cols; p->tile_rows = f->tiling.rows;
   for (unsigned i = 0; i <= f->tiling.cols; i++) p->tile_col_start_sb[i] = f->tiling.col_start_sb[i];
   for (unsigned i = 0; i <= f->tiling.rows; i++) p->tile_row_start_sb[i] = f->tiling.row_start_sb[i];
   for (unsigned i = 0; i < f->tiling.cols; i++) p->width_in_sbs[i] = f->tiling.col_start_sb[i+1] - f->tiling.col_start_sb[i];
   for (unsigned i = 0; i < f->tiling.rows; i++) p->height_in_sbs[i] = f->tiling.row_start_sb[i+1] - f->tiling.row_start_sb[i];
   p->context_update_tile_id = f->tiling.update;
   p->pic_info_fields.frame_type = f->frame_type;
   p->pic_info_fields.show_frame = f->show_frame;
   p->pic_info_fields.showable_frame = f->showable_frame;
   p->pic_info_fields.error_resilient_mode = f->error_resilient_mode;
   p->pic_info_fields.disable_cdf_update = f->disable_cdf_update;
   p->pic_info_fields.allow_screen_content_tools = f->allow_screen_content_tools;
   p->pic_info_fields.force_integer_mv = f->force_integer_mv;
   p->pic_info_fields.allow_intrabc = f->allow_intrabc;
   p->pic_info_fields.use_superres = f->super_res.enabled;
   p->pic_info_fields.allow_high_precision_mv = f->hp;
   p->pic_info_fields.is_motion_mode_switchable = f->switchable_motion_mode;
   p->pic_info_fields.use_ref_frame_mvs = f->use_ref_frame_mvs;
   p->pic_info_fields.disable_frame_end_update_cdf = !f->refresh_context;
   p->pic_info_fields.uniform_tile_spacing_flag = f->tiling.uniform;
   p->pic_info_fields.allow_warped_motion = f->warp_motion;
   p->superres_scale_denominator = f->super_res.width_scale_denominator;
   p->interp_filter = f->subpel_filter_mode;
   memcpy(p->filter_level, f->loopfilter.level_y, 2);
   p->filter_level_u = f->loopfilter.level_u; p->filter_level_v = f->loopfilter.level_v;
   p->loop_filter_info_fields.sharpness_level = f->loopfilter.sharpness;
   p->loop_filter_info_fields.mode_ref_delta_enabled = f->loopfilter.mode_ref_delta_enabled;
   p->loop_filter_info_fields.mode_ref_delta_update = f->loopfilter.mode_ref_delta_update;
   memcpy(p->ref_deltas, f->loopfilter.mode_ref_deltas.ref_delta, 8);
   memcpy(p->mode_deltas, f->loopfilter.mode_ref_deltas.mode_delta, 2);
   p->base_qindex = f->quant.yac; p->y_dc_delta_q = f->quant.ydc_delta;
   p->u_dc_delta_q = f->quant.udc_delta; p->u_ac_delta_q = f->quant.uac_delta;
   p->v_dc_delta_q = f->quant.vdc_delta; p->v_ac_delta_q = f->quant.vac_delta;
   p->qmatrix_fields.using_qmatrix = f->quant.qm;
   p->qmatrix_fields.qm_y = f->quant.qm_y; p->qmatrix_fields.qm_u = f->quant.qm_u; p->qmatrix_fields.qm_v = f->quant.qm_v;
   p->mode_control_fields.delta_q_present_flag = f->delta.q.present;
   p->mode_control_fields.log2_delta_q_res = f->delta.q.res_log2;
   p->mode_control_fields.delta_lf_present_flag = f->delta.lf.present;
   p->mode_control_fields.log2_delta_lf_res = f->delta.lf.res_log2;
   p->mode_control_fields.delta_lf_multi = f->delta.lf.multi;
   p->mode_control_fields.tx_mode = f->txfm_mode;
   p->mode_control_fields.reference_select = f->switchable_comp_refs;
   p->mode_control_fields.reduced_tx_set_used = f->reduced_txtp_set;
   p->mode_control_fields.skip_mode_present = f->skip_mode_enabled;
   p->cdef_damping_minus_3 = f->cdef.damping - 3; p->cdef_bits = f->cdef.n_bits;
   memcpy(p->cdef_y_strengths, f->cdef.y_strength, 8); memcpy(p->cdef_uv_strengths, f->cdef.uv_strength, 8);
   p->loop_restoration_fields.yframe_restoration_type = f->restoration.type[0];
   p->loop_restoration_fields.cbframe_restoration_type = f->restoration.type[1];
   p->loop_restoration_fields.crframe_restoration_type = f->restoration.type[2];
   if (f->restoration.type[0] || f->restoration.type[1] || f->restoration.type[2]) {
      p->loop_restoration_fields.lr_unit_shift = f->restoration.unit_size[0] - 6;
      if (f->restoration.type[1] || f->restoration.type[2])
         p->loop_restoration_fields.lr_uv_shift = f->restoration.unit_size[0] - f->restoration.unit_size[1];
      for (unsigned i = 0; i < 3; i++) p->lr_unit_size[i] = 1u << f->restoration.unit_size[i];
   }
   for (unsigned i = 0; i < 7; i++) {
      p->wm[i].wmtype = f->gmv[i].type;
      memcpy(p->wm[i].wmmat, f->gmv[i].matrix, 6 * sizeof(int32_t));
   }
   p->refresh_frame_flags = f->refresh_frame_flags; p->matrix_coefficients = s->mtrx;
}

static bool send(Dav1dContext *c, const uint8_t *bytes, size_t size)
{
   Dav1dData data = {0};
   uint8_t *p = dav1d_data_create(&data, size);
   if (!p) return false;
   memcpy(p, bytes, size);
   int result = dav1d_send_data(c, &data);
   dav1d_data_unref(&data);
   return result == 0;
}

static bool same_picture(const Dav1dPicture *a, const Dav1dPicture *b)
{
   if (a->p.w != b->p.w || a->p.h != b->p.h || a->p.bpc != b->p.bpc) return false;
   for (unsigned plane = 0; plane < 3; plane++) {
      unsigned width = plane ? (a->p.w + 1) / 2 : a->p.w;
      unsigned height = plane ? (a->p.h + 1) / 2 : a->p.h;
      size_t bytes = width * (a->p.bpc > 8 ? 2 : 1);
      for (unsigned row = 0; row < height; row++)
         if (memcmp((uint8_t *)a->data[plane] + row * a->stride[plane != 0],
                    (uint8_t *)b->data[plane] + row * b->stride[plane != 0], bytes)) return false;
   }
   return true;
}

static void check_semantics(const Dav1dPicture *original, const Dav1dPicture *rebuilt,
                            const uint32_t refs[8], uint32_t id)
{
   struct virgl_av1_picture_desc a, b;
   describe(&a, original, refs, id);
   describe(&b, rebuilt, refs, id);
   /* These bits select how resolved data is transmitted, not its value. */
   a.picture_parameter.seg_info.segment_info_fields.update_data = 0;
   b.picture_parameter.seg_info.segment_info_fields.update_data = 0;
   a.picture_parameter.loop_filter_info_fields.mode_ref_delta_update = 0;
   b.picture_parameter.loop_filter_info_fields.mode_ref_delta_update = 0;
#define PARAM(field) do { \
   bool equal = !memcmp(&a.picture_parameter.field, &b.picture_parameter.field, \
                       sizeof(a.picture_parameter.field)); \
   if (!equal) fprintf(stderr, "semantic mismatch: %s\n", #field); \
   CHECK(equal); \
} while (0)
   PARAM(seq_info_fields); PARAM(frame_width); PARAM(frame_height);
   PARAM(ref_frame_idx); PARAM(primary_ref_frame); PARAM(order_hint);
   PARAM(seg_info); PARAM(film_grain_info); PARAM(pic_info_fields);
   PARAM(tile_cols); PARAM(tile_rows); PARAM(width_in_sbs); PARAM(height_in_sbs);
   PARAM(superres_scale_denominator); PARAM(interp_filter);
   PARAM(filter_level); PARAM(filter_level_u); PARAM(filter_level_v);
   PARAM(loop_filter_info_fields); PARAM(ref_deltas); PARAM(mode_deltas);
   PARAM(base_qindex); PARAM(y_dc_delta_q); PARAM(u_dc_delta_q); PARAM(u_ac_delta_q);
   PARAM(v_dc_delta_q); PARAM(v_ac_delta_q); PARAM(qmatrix_fields);
   PARAM(mode_control_fields); PARAM(cdef_damping_minus_3); PARAM(cdef_bits);
   PARAM(cdef_y_strengths); PARAM(cdef_uv_strengths); PARAM(loop_restoration_fields);
   PARAM(wm); PARAM(refresh_frame_flags);
#undef PARAM
   CHECK(original->p.w == rebuilt->p.w && original->p.h == rebuilt->p.h &&
         original->p.bpc == rebuilt->p.bpc);
   /* Same-decoder equality is useful evidence for header-only rewriting, but
    * is deliberately not a universal software-versus-hardware acceptance rule.
    * Any observed difference must be investigated against codec semantics. */
   if (!same_picture(original, rebuilt)) {
      pixel_differences++;
      fprintf(stderr, "DIAGNOSTIC frame %u: decoded samples differ\n", frames);
   }
}

static void check_rejections(const struct virgl_av1_rewrite_state *state,
                              const struct virgl_av1_picture_desc *valid,
                              const uint8_t *data, size_t size)
{
   struct virgl_av1_picture_desc bad = *valid;
   struct virgl_av1_rewrite_state next, untouched;
   memset(&untouched, 0x5a, sizeof(untouched)); next = untouched;
   struct virgl_video_bitstream output = {0}, config = {0};
   bad.slice_parameter.slice_data_offset[0] = UINT32_MAX;
   CHECK(!virgl_video_av1_rewrite(state, &next, &bad, frames, data, size, &output, &config));
   CHECK(!output.data && !output.size && !config.data && !config.size);
   CHECK(!memcmp(&next, &untouched, sizeof(next)));
   bad = *valid;
   if (bad.picture_parameter.pic_info_fields.frame_type == 1) {
      bad.picture_parameter.wm[0].wmtype = 3;
      bad.picture_parameter.wm[0].wmmat[2] = INT32_MIN;
      CHECK(!virgl_video_av1_rewrite(state, &next, &bad, frames, data, size, &output, &config));
      CHECK(!output.data && !config.data);
      CHECK(!memcmp(&next, &untouched, sizeof(next)));
      bad = *valid;
      bad.picture_parameter.wm[0].wmtype = 1;
      bad.picture_parameter.wm[0].wmmat[2] = 0; /* translation implies identity affine part */
      CHECK(!virgl_video_av1_rewrite(state, &next, &bad, frames, data, size, &output, &config));
      CHECK(!output.data && !config.data);
      CHECK(!memcmp(&next, &untouched, sizeof(next)));
   }
}

#ifdef TEST_VIDEOTOOLBOX
static void check_vt(const struct virgl_video_bitstream *sample,
                     const struct virgl_video_bitstream *config,
                     const Dav1dPicture *picture)
{
   vt_codec.width = picture->p.w; vt_codec.height = picture->p.h;
   enum pipe_format format = picture->p.bpc == 10 ? PIPE_FORMAT_P010 : PIPE_FORMAT_NV12;
   bool ready = ensure_atom_decoder(&vt_codec, kCMVideoCodecType_AV1, format,
                                    CFSTR("av1C"), config->data, config->size);
   CHECK(ready);
   if (!ready) return;
   struct virgl_video_buffer target = {.format = format,
      .width = picture->p.w, .height = picture->p.h};
   struct byte_buffer bytes = {0};
   CHECK(bb_append(&bytes, sample->data, sample->size));
   int result = test_decode_wait(&vt_codec, &target, &bytes, NULL);
   if (result) fprintf(stderr, "VideoToolbox frame %u type=%u show=%u status=%d\n",
      frames, picture->frame_hdr->frame_type, picture->frame_hdr->show_frame, target.status);
   CHECK(result == 0);
   if (!result) vt_frames++;
   if (target.pixel_buffer) CVPixelBufferRelease(target.pixel_buffer);
   bb_clear(&bytes);
}
#endif

static size_t leb(const uint8_t *p, size_t n, size_t *v)
{
   *v = 0;
   for (size_t i = 0; i < n && i < 8; i++) {
      *v |= (size_t)(p[i] & 127) << (7 * i);
      if (!(p[i] & 128)) return i + 1;
   }
   return 0;
}

static void check_tile_payloads(const struct virgl_video_bitstream *sample,
                                const struct virgl_av1_picture_desc *d,
                                const Dav1dPicture *parsed, const uint8_t *original)
{
   unsigned groups = 0, tiles = d->slice_parameter.slice_count;
   for (size_t pos = 0; pos < sample->size;) {
      unsigned type = sample->data[pos] >> 3 & 15;
      size_t header = sample->data[pos] & 4 ? 2 : 1, payload;
      CHECK(header <= sample->size - pos);
      if (header > sample->size - pos) return;
      size_t n = leb(sample->data + pos + header, sample->size - pos - header, &payload);
      CHECK(n && payload <= sample->size - pos - header - n);
      if (!n || payload > sample->size - pos - header - n) return;
      pos += header + n;
      size_t end = pos + payload;
      if (type == 4) {
         groups++;
         if (tiles > 1) {
            CHECK(pos < end && sample->data[pos] == 0); /* all tiles, aligned */
            if (pos == end) return;
            pos++;
         }
         for (unsigned i = 0; i < tiles; i++) {
            size_t length = end - pos;
            if (i + 1 < tiles) {
               unsigned bytes = parsed->frame_hdr->tiling.n_bytes;
               CHECK(bytes && bytes <= 4 && bytes <= end - pos);
               if (!bytes || bytes > 4 || bytes > end - pos) return;
               length = 0;
               for (unsigned j = 0; j < bytes; j++) length |= (size_t)sample->data[pos++] << (8 * j);
               length++;
            }
            CHECK(length == d->slice_parameter.slice_data_size[i] && length <= end - pos);
            if (length != d->slice_parameter.slice_data_size[i] || length > end - pos) return;
            CHECK(!memcmp(sample->data + pos, original + d->slice_parameter.slice_data_offset[i], length));
            pos += length;
         }
         CHECK(pos == end);
      }
      pos = end;
   }
   CHECK(groups == 1);
}

int main(int argc, char **argv)
{
   if (argc != 2 && argc != 3) return 2;
#ifdef TEST_VIDEOTOOLBOX
   VTRegisterSupplementalVideoDecoderIfAvailable(kCMVideoCodecType_AV1);
   if (!VTIsHardwareDecodeSupported(kCMVideoCodecType_AV1)) {
      puts("SKIP: this machine has no available AV1 hardware decoder");
      return 77;
   }
#endif
   FILE *input = fopen(argv[1], "rb");
   uint8_t ivf[32];
   if (!input || fread(ivf, 1, 32, input) != 32 || memcmp(ivf, "DKIF", 4)) return 2;
   Dav1dSettings settings; dav1d_default_settings(&settings);
   settings.n_threads = 1; settings.max_frame_delay = 1;
   settings.output_invisible_frames = 1;
   Dav1dContext *original = NULL, *rewritten = NULL;
   CHECK(!dav1d_open(&original, &settings)); CHECK(!dav1d_open(&rewritten, &settings));
   AVBSFContext *bsf = NULL;
   CHECK(!av_bsf_alloc(av_bsf_get_by_name("trace_headers"), &bsf));
   bsf->par_in->codec_id = AV_CODEC_ID_AV1;
   CHECK(!av_bsf_init(bsf)); av_log_set_callback(trace_log); av_log_set_level(AV_LOG_TRACE);
   struct virgl_av1_rewrite_state state = {0}, next;
   uint32_t refs[8] = {0};
   uint8_t packet_header[12];
   while (!failures && fread(packet_header, 1, 12, input) == 12) {
      size_t n = 0;
      for (unsigned i = 0; i < 4; i++) n |= (size_t)packet_header[i] << (8 * i);
      uint8_t *packet = malloc(n);
      if (!packet || fread(packet, 1, n, input) != n) return 2;
      for (size_t off = 0; off < n && !failures;) {
         uint8_t *obu = packet + off;
         unsigned type = obu[0] >> 3 & 15;
         size_t h = (obu[0] & 4) ? 2 : 1, payload;
         size_t ls = leb(obu + h, n - off - h, &payload);
         if (!(obu[0] & 2) || !ls || payload > n - off - h - ls) return 2;
         size_t size = h + ls + payload;
         AVPacket *pkt = av_packet_alloc(); av_new_packet(pkt, size);
         memcpy(pkt->data, obu, size);
         header_end = 0; tracing_frame = false; show_existing = false;
         CHECK(!av_bsf_send_packet(bsf, pkt));
         while (!av_bsf_receive_packet(bsf, pkt)) av_packet_unref(pkt);
         av_packet_free(&pkt);
         CHECK(send(original, obu, size));
         Dav1dPicture decoded = {0};
         int got = dav1d_get_picture(original, &decoded);
         if (!got && show_existing) {
            CHECK(decoded.frame_hdr->frame_type != DAV1D_FRAME_TYPE_KEY);
            dav1d_picture_unref(&decoded);
         } else if (!got) {
            CHECK(type == 6 && header_end && !(header_end % 8));
            frames++;
            struct virgl_av1_picture_desc d;
            describe(&d, &decoded, refs, frames);
            size_t tile_offset = header_end / 8;
            unsigned tiles = d.picture_parameter.tile_cols * d.picture_parameter.tile_rows;
            d.slice_parameter.slice_count = tiles;
            for (unsigned i = 0; i < tiles; i++) {
               size_t length = size - tile_offset;
               if (i + 1 < tiles) {
                  unsigned nb = decoded.frame_hdr->tiling.n_bytes;
                  length = 0;
                  for (unsigned j = 0; j < nb; j++) length |= (size_t)obu[tile_offset++] << (8 * j);
                  length++;
               }
               CHECK(tile_offset <= size && length <= size - tile_offset);
               d.slice_parameter.slice_data_offset[i] = tile_offset;
               d.slice_parameter.slice_data_size[i] = length;
               d.slice_parameter.slice_data_row[i] = i / d.picture_parameter.tile_cols;
               d.slice_parameter.slice_data_col[i] = i % d.picture_parameter.tile_cols;
               tile_offset += length;
            }
            CHECK(tile_offset == size);
            check_rejections(&state, &d, obu, size);
            struct virgl_video_bitstream out = {0}, config = {0};
            CHECK(virgl_video_av1_rewrite(&state, &next, &d, frames, obu, size, &out, &config));
            if (out.size) {
               CHECK(send(rewritten, out.data, out.size));
               Dav1dPicture norm = {0};
               int r = dav1d_get_picture(rewritten, &norm);
               CHECK(r == 0);
               if (!r) {
                  check_semantics(&decoded, &norm, refs, frames);
                  check_tile_payloads(&out, &d, &norm, obu);
                  dav1d_picture_unref(&norm);
               }
#ifdef TEST_VIDEOTOOLBOX
               if (!failures) check_vt(&out, &config, &decoded);
#endif
               if (failures && argc == 3) {
                  FILE *bad = fopen(argv[2], "wb");
                  if (bad) { fwrite(out.data, 1, out.size, bad); fclose(bad); }
               }
            }
            free(out.data); free(config.data);
            if (!failures) state = next;
            for (unsigned i = 0; i < 8; i++)
               if (decoded.frame_hdr->refresh_frame_flags & (1u << i)) refs[i] = frames;
            dav1d_picture_unref(&decoded);
         } else CHECK(got == DAV1D_ERR(EAGAIN));
         off += size;
      }
      free(packet);
   }
   fclose(input); av_bsf_free(&bsf); dav1d_close(&original); dav1d_close(&rewritten);
   CHECK(frames > 0);
#ifdef TEST_VIDEOTOOLBOX
   destroy_decoder(&vt_codec);
   printf("VideoToolbox: %u decoded frames\n", vt_frames);
#endif
   printf("AV1 normalization: %u frames, %u checks, %u failures; "
          "%u frames with pixel differences (diagnostic)\n",
          frames, checks, failures, pixel_differences);
   return failures != 0;
}
