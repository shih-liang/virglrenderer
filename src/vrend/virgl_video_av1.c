/* SPDX-License-Identifier: MIT
 * Copyright 2026 NativePipe contributors
 *
 * Serialize the effective AV1 decode state, not the original header syntax.
 * AV1 specification sections 5.5, 5.9 and 5.11 define the syntax below. Tile
 * entropy bytes are opaque: never change a tool, CDF selection, or reference
 * relationship merely to make a header easier to write.
 */
#include "virgl_video_bits.h"

typedef __typeof__(((struct virgl_av1_picture_desc *)0)->picture_parameter) av1_pic;
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static bool sequence_header(const av1_pic *p,
                            struct virgl_video_bitstream *sequence)
{
   uint8_t bytes[128];
   struct video_bits b = {.data = bytes, .capacity = sizeof(bytes)};
   unsigned w = p->max_width, h = p->max_height;
   if (!w || !h || p->profile || p->bit_depth_idx > 1 ||
       p->seq_info_fields.mono_chrome || p->order_hint_bits_minus_1 > 7)
      return false;
   unsigned wb = MAX(vb_log2(w), 1), hb = MAX(vb_log2(h), 1);
   vb_put(&b, 0, 3); /* Main profile, 4:2:0 */
   vb_put(&b, 0, 1); /* still_picture */
   vb_put(&b, 0, 1); /* reduced_still_picture_header */
   vb_put(&b, 0, 1); /* timing_info_present_flag */
   vb_put(&b, 0, 1); /* initial_display_delay_present_flag */
   vb_put(&b, 0, 5); /* one operating point */
   vb_put(&b, 0, 12); /* all temporal/spatial layers */
   /* The descriptor has no level/timing/bitrate constraints. Annex A defines
    * 31 as Maximum parameters, not level 2.0 (the zero-initialized codec level
    * used by Mesa's VA frontend). Hardware session creation remains the gate. */
   vb_put(&b, 31, 5);
   vb_put(&b, 0, 1); /* main tier */
   vb_put(&b, wb - 1, 4); vb_put(&b, hb - 1, 4);
   vb_put(&b, w - 1, wb); vb_put(&b, h - 1, hb);
   vb_put(&b, 0, 1); /* frame IDs are tracked as resource IDs by the caller */
   vb_put(&b, p->seq_info_fields.use_128x128_superblock, 1);
   vb_put(&b, p->seq_info_fields.enable_filter_intra, 1);
   vb_put(&b, p->seq_info_fields.enable_intra_edge_filter, 1);
   vb_put(&b, p->seq_info_fields.enable_interintra_compound, 1);
   vb_put(&b, p->seq_info_fields.enable_masked_compound, 1);
   vb_put(&b, 1, 1); /* warped motion permitted; actual use is per frame */
   vb_put(&b, p->seq_info_fields.enable_dual_filter, 1);
   vb_put(&b, p->seq_info_fields.enable_order_hint, 1);
   if (p->seq_info_fields.enable_order_hint) {
      vb_put(&b, p->seq_info_fields.enable_jnt_comp, 1);
      vb_put(&b, p->seq_info_fields.ref_frame_mvs, 1);
   }
   vb_put(&b, 1, 1); /* SELECT_SCREEN_CONTENT_TOOLS */
   vb_put(&b, 1, 1); /* SELECT_INTEGER_MV */
   if (p->seq_info_fields.enable_order_hint)
      vb_put(&b, p->order_hint_bits_minus_1, 3);
   vb_put(&b, 1, 1); /* superres permitted, not forced */
   vb_put(&b, p->seq_info_fields.enable_cdef, 1);
   vb_put(&b, 1, 1); /* restoration permitted, not forced */
   vb_put(&b, p->bit_depth_idx, 1);
   vb_put(&b, 0, 1); /* monochrome */
   vb_put(&b, 1, 1); /* color description */
   vb_put(&b, 2, 8); vb_put(&b, 2, 8); /* unspecified primaries/transfer */
   vb_put(&b, p->matrix_coefficients, 8);
   vb_put(&b, 0, 1); /* range is not carried by the VirGL descriptor */
   vb_put(&b, 0, 2); /* unspecified chroma sample position */
   vb_put(&b, 1, 1); /* permit independent U/V deltas */
   vb_put(&b, p->seq_info_fields.film_grain_params_present, 1);
   vb_align(&b, true);
   return !b.failed && vs_obu(sequence, 1, bytes, b.count / 8);
}

static unsigned tile_log2(unsigned unit, unsigned target)
{
   unsigned bits = 0;
   while (unit < target) { unit *= 2; bits++; }
   return bits;
}

static bool tile_info(struct video_bits *b, const av1_pic *p,
                      unsigned coded_width, unsigned *tile_bits)
{
   unsigned sb = p->seq_info_fields.use_128x128_superblock ? 128 : 64;
   unsigned cols = (coded_width + sb - 1) / sb;
   unsigned rows = (p->frame_height + sb - 1) / sb;
   unsigned max_width = 4096 / sb, max_area = (4096 * 2304) / (sb * sb);
   unsigned min_cols = tile_log2(max_width, cols);
   unsigned max_cols = tile_log2(1, MIN(cols, 64));
   unsigned max_rows = tile_log2(1, MIN(rows, 64));
   unsigned min_tiles = MAX(min_cols, tile_log2(max_area, cols * rows));
   unsigned col_bits = vb_log2(p->tile_cols), row_bits = vb_log2(p->tile_rows);
   if (!p->tile_cols || !p->tile_rows || p->tile_cols > 64 || p->tile_rows > 64 ||
       (unsigned)p->tile_cols * p->tile_rows > 256) return false;
   vb_put(b, p->pic_info_fields.uniform_tile_spacing_flag, 1);
   if (p->pic_info_fields.uniform_tile_spacing_flag) {
      vb_increment(b, col_bits, min_cols, max_cols);
      vb_increment(b, row_bits, min_tiles > col_bits ? min_tiles - col_bits : 0, max_rows);
      unsigned tw = (cols + (1u << col_bits) - 1) >> col_bits;
      unsigned th = (rows + (1u << row_bits) - 1) >> row_bits;
      if (!tw || !th || (cols + tw - 1) / tw != p->tile_cols ||
          (rows + th - 1) / th != p->tile_rows) return false;
   } else {
      unsigned used = 0, widest = 0;
      for (unsigned i = 0; i < p->tile_cols; i++) {
         unsigned width = p->width_in_sbs[i];
         if (!width || used >= cols) return false;
         vb_ns(b, width - 1, MIN(cols - used, max_width));
         widest = MAX(widest, width);
         used += width;
      }
      if (used != cols) return false;
      max_area = min_tiles ? cols * rows >> (min_tiles + 1) : cols * rows;
      unsigned max_height = MAX(max_area / widest, 1);
      used = 0;
      for (unsigned i = 0; i < p->tile_rows; i++) {
         unsigned height = p->height_in_sbs[i];
         if (!height || used >= rows) return false;
         vb_ns(b, height - 1, MIN(rows - used, max_height));
         used += height;
      }
      if (used != rows) return false;
   }
   *tile_bits = col_bits + row_bits;
   if (*tile_bits) {
      if (p->context_update_tile_id >= p->tile_cols * p->tile_rows) return false;
      vb_put(b, p->context_update_tile_id, *tile_bits);
      vb_put(b, 3, 2); /* four-byte tile lengths in our new tile group */
   }
   return !b->failed;
}

static void delta_q(struct video_bits *b, int value)
{
   vb_put(b, value != 0, 1);
   if (value) vb_signed(b, value, 7);
}

static bool quantization(struct video_bits *b, const av1_pic *p, unsigned primary)
{
   vb_put(b, p->base_qindex, 8);
   delta_q(b, p->y_dc_delta_q);
   vb_put(b, 1, 1); /* diff_uv_delta: always serialize the effective values */
   delta_q(b, p->u_dc_delta_q); delta_q(b, p->u_ac_delta_q);
   delta_q(b, p->v_dc_delta_q); delta_q(b, p->v_ac_delta_q);
   vb_put(b, p->qmatrix_fields.using_qmatrix, 1);
   if (p->qmatrix_fields.using_qmatrix) {
      vb_put(b, p->qmatrix_fields.qm_y, 4);
      vb_put(b, p->qmatrix_fields.qm_u, 4);
      vb_put(b, p->qmatrix_fields.qm_v, 4);
   }
   vb_put(b, p->seg_info.segment_info_fields.enabled, 1);
   if (p->seg_info.segment_info_fields.enabled) {
      if (primary != 7) {
         vb_put(b, p->seg_info.segment_info_fields.update_map, 1);
         if (p->seg_info.segment_info_fields.update_map)
            vb_put(b, p->seg_info.segment_info_fields.temporal_update, 1);
         /* Feature values are resolved by the frontend. Re-emit all of them,
          * while preserving map/CDF reuse, instead of relying on deltas. */
         vb_put(b, 1, 1);
      }
      static const uint8_t bits[] = {9, 7, 7, 7, 7, 3, 0, 0};
      for (unsigned i = 0; i < 8; i++) for (unsigned j = 0; j < 8; j++) {
         bool on = p->seg_info.feature_mask[i] & (1u << j);
         vb_put(b, on, 1);
         if (on && bits[j]) {
            int value = p->seg_info.feature_data[i][j];
            if (j < 5) vb_signed(b, value, bits[j]);
            else vb_put(b, value, bits[j]);
         }
      }
   }
   if (p->base_qindex) vb_put(b, p->mode_control_fields.delta_q_present_flag, 1);
   else if (p->mode_control_fields.delta_q_present_flag) b->failed = true;
   if (p->mode_control_fields.delta_q_present_flag) {
      vb_put(b, p->mode_control_fields.log2_delta_q_res, 2);
      if (!p->pic_info_fields.allow_intrabc)
         vb_put(b, p->mode_control_fields.delta_lf_present_flag, 1);
      if (p->mode_control_fields.delta_lf_present_flag) {
         if (p->pic_info_fields.allow_intrabc) b->failed = true;
         vb_put(b, p->mode_control_fields.log2_delta_lf_res, 2);
         vb_put(b, p->mode_control_fields.delta_lf_multi, 1);
      }
   }
   bool lossless = !p->y_dc_delta_q && !p->u_dc_delta_q && !p->u_ac_delta_q &&
                   !p->v_dc_delta_q && !p->v_ac_delta_q;
   for (unsigned i = 0; i < 8; i++) {
      int q = p->base_qindex;
      if (p->seg_info.segment_info_fields.enabled && (p->seg_info.feature_mask[i] & 1))
         q += p->seg_info.feature_data[i][0];
      if (q > 0) lossless = false;
   }
   return lossless;
}

static void filters(struct video_bits *b, const av1_pic *p, bool lossless)
{
   bool intrabc = p->pic_info_fields.allow_intrabc;
   if (!lossless && !intrabc) {
      vb_put(b, p->filter_level[0], 6); vb_put(b, p->filter_level[1], 6);
      if (p->filter_level[0] || p->filter_level[1]) {
         vb_put(b, p->filter_level_u, 6); vb_put(b, p->filter_level_v, 6);
      }
      vb_put(b, p->loop_filter_info_fields.sharpness_level, 3);
      vb_put(b, p->loop_filter_info_fields.mode_ref_delta_enabled, 1);
      if (p->loop_filter_info_fields.mode_ref_delta_enabled) {
         vb_put(b, 1, 1); /* update all resolved deltas */
         for (unsigned i = 0; i < 8; i++) {
            vb_put(b, 1, 1); vb_signed(b, p->ref_deltas[i], 7);
         }
         for (unsigned i = 0; i < 2; i++) {
            vb_put(b, 1, 1); vb_signed(b, p->mode_deltas[i], 7);
         }
      }
      if (p->seq_info_fields.enable_cdef) {
         vb_put(b, p->cdef_damping_minus_3, 2); vb_put(b, p->cdef_bits, 2);
         if (p->cdef_bits > 3) { b->failed = true; return; }
         for (unsigned i = 0; i < (1u << p->cdef_bits); i++) {
            vb_put(b, p->cdef_y_strengths[i], 6);
            vb_put(b, p->cdef_uv_strengths[i], 6);
         }
      }
   }
   if (!(lossless && !p->pic_info_fields.use_superres) && !intrabc) {
      unsigned types[] = {p->loop_restoration_fields.yframe_restoration_type,
                          p->loop_restoration_fields.cbframe_restoration_type,
                          p->loop_restoration_fields.crframe_restoration_type};
      for (unsigned i = 0; i < 3; i++) vb_put(b, types[i], 2);
      if (types[0] || types[1] || types[2]) {
         vb_increment(b, p->loop_restoration_fields.lr_unit_shift,
                       p->seq_info_fields.use_128x128_superblock ? 1 : 0, 2);
         if (types[1] || types[2]) vb_put(b, p->loop_restoration_fields.lr_uv_shift, 1);
      }
   }
   if (!lossless) vb_increment(b, p->mode_control_fields.tx_mode, 1, 2);
   else if (p->mode_control_fields.tx_mode) b->failed = true;
}

static int relative_dist(unsigned a, unsigned b, unsigned bits)
{
   unsigned mask = (1u << bits) - 1, sign = 1u << (bits - 1);
   unsigned diff = (a - b) & mask;
   return (int)(diff & (sign - 1)) - (int)(diff & sign);
}

static bool skip_allowed(const av1_pic *p, const struct virgl_av1_rewrite_state *s)
{
   if (!p->seq_info_fields.enable_order_hint || !p->mode_control_fields.reference_select)
      return false;
   unsigned bits = p->order_hint_bits_minus_1 + 1;
   int before = INT_MIN, after = INT_MAX;
   for (unsigned i = 0; i < 7; i++) {
      int d = relative_dist(s->refs[p->ref_frame_idx[i]].order_hint, p->order_hint, bits);
      if (d < 0) before = MAX(before, d);
      if (d > 0) after = MIN(after, d);
   }
   if (before == INT_MIN) return false;
   if (after != INT_MAX) return true;
   for (unsigned i = 0; i < 7; i++)
      if (relative_dist(s->refs[p->ref_frame_idx[i]].order_hint, p->order_hint, bits) < before)
         return true;
   return false;
}

static unsigned recenter(unsigned r, unsigned v)
{
   if (v > 2 * r) return v;
   return v >= r ? 2 * (v - r) : 2 * (r - v) - 1;
}

static void signed_subexp(struct video_bits *b, int value, int ref, unsigned bits)
{
   int bound = 1 << bits;
   unsigned n = 2 * bound + 1;
   if (value < -bound || value > bound || ref < -bound || ref > bound) {
      b->failed = true; return;
   }
   unsigned r = ref + bound, v = value + bound;
   v = 2 * r <= n ? recenter(r, v) : recenter(n - 1 - r, n - 1 - v);
   for (unsigned i = 0, used = 0; !b->failed; i++) {
      unsigned width = i ? i + 2 : 3, count = 1u << width;
      if (n <= used + 3 * count) { vb_ns(b, v - used, n - used); break; }
      bool more = v >= used + count;
      vb_put(b, more, 1);
      if (!more) { vb_put(b, v - used, width); break; }
      used += count;
   }
}

static void global_motion(struct video_bits *b, const av1_pic *p,
                          const struct virgl_av1_reference *primary)
{
   const int32_t identity[] = {0, 0, 65536, 0, 0, 65536};
   for (unsigned i = 0; i < 7; i++) {
      unsigned type = p->wm[i].wmtype;
      if (type > 3) { b->failed = true; return; }
      vb_put(b, type != 0, 1);
      if (!type) continue;
      vb_put(b, type == 2, 1);
      if (type != 2) vb_put(b, type == 1, 1);
      const int32_t *m = p->wm[i].wmmat;
      const int32_t *r = primary ? primary->gm[i] : identity;
      if (type == 1 && (m[2] != 65536 || m[3] || m[4] || m[5] != 65536)) {
         b->failed = true; return;
      }
      if (type >= 2) {
         if ((m[2] & 1) || (m[3] & 1) || (type == 3 && ((m[4] & 1) || (m[5] & 1)))) {
            b->failed = true; return;
         }
         if (type == 2 && ((int64_t)m[4] != -(int64_t)m[3] || m[5] != m[2])) {
            b->failed = true; return;
         }
         signed_subexp(b, ((int64_t)m[2] - 65536) / 2, ((int64_t)r[2] - 65536) >> 1, 12);
         signed_subexp(b, m[3] / 2, r[3] >> 1, 12);
         if (type == 3) {
            signed_subexp(b, m[4] / 2, r[4] >> 1, 12);
            signed_subexp(b, ((int64_t)m[5] - 65536) / 2, ((int64_t)r[5] - 65536) >> 1, 12);
         }
      }
      unsigned hp = p->pic_info_fields.allow_high_precision_mv;
      unsigned shift = type == 1 ? 14 - hp : 10;
      unsigned bits = type == 1 ? 8 + hp : 12;
      for (unsigned j = 0; j < 2; j++) {
         if (m[j] % (1 << shift)) b->failed = true;
         signed_subexp(b, m[j] / (1 << shift), r[j] >> shift, bits);
      }
   }
}

static void film_grain(struct video_bits *b, const av1_pic *p)
{
   if (!p->seq_info_fields.film_grain_params_present ||
       (!p->pic_info_fields.show_frame && !p->pic_info_fields.showable_frame)) return;
   const __typeof__(p->film_grain_info) *g = &p->film_grain_info;
   vb_put(b, g->film_grain_info_fields.apply_grain, 1);
   if (!g->film_grain_info_fields.apply_grain) return;
   vb_put(b, g->grain_seed, 16);
   if (p->pic_info_fields.frame_type == 1) vb_put(b, 1, 1); /* update_grain */
   if (g->num_y_points > 14 || g->num_cb_points > 10 || g->num_cr_points > 10) {
      b->failed = true; return;
   }
   vb_put(b, g->num_y_points, 4);
   for (unsigned i = 0; i < g->num_y_points; i++) {
      if (i && g->point_y_value[i] <= g->point_y_value[i - 1]) b->failed = true;
      vb_put(b, g->point_y_value[i], 8); vb_put(b, g->point_y_scaling[i], 8);
   }
   bool from_luma = g->film_grain_info_fields.chroma_scaling_from_luma;
   vb_put(b, from_luma, 1);
   if (!from_luma && g->num_y_points) {
      vb_put(b, g->num_cb_points, 4);
      for (unsigned i = 0; i < g->num_cb_points; i++) {
         if (i && g->point_cb_value[i] <= g->point_cb_value[i - 1]) b->failed = true;
         vb_put(b, g->point_cb_value[i], 8); vb_put(b, g->point_cb_scaling[i], 8);
      }
      vb_put(b, g->num_cr_points, 4);
      for (unsigned i = 0; i < g->num_cr_points; i++) {
         if (i && g->point_cr_value[i] <= g->point_cr_value[i - 1]) b->failed = true;
         vb_put(b, g->point_cr_value[i], 8); vb_put(b, g->point_cr_scaling[i], 8);
      }
   } else if (g->num_cb_points || g->num_cr_points) b->failed = true;
   vb_put(b, g->film_grain_info_fields.grain_scaling_minus_8, 2);
   unsigned lag = g->film_grain_info_fields.ar_coeff_lag;
   vb_put(b, lag, 2);
   unsigned luma = 2 * lag * (lag + 1), chroma = luma + !!g->num_y_points;
   if (g->num_y_points)
      for (unsigned i = 0; i < luma; i++) vb_put(b, g->ar_coeffs_y[i] + 128, 8);
   if (from_luma || g->num_cb_points)
      for (unsigned i = 0; i < chroma; i++) vb_put(b, g->ar_coeffs_cb[i] + 128, 8);
   if (from_luma || g->num_cr_points)
      for (unsigned i = 0; i < chroma; i++) vb_put(b, g->ar_coeffs_cr[i] + 128, 8);
   vb_put(b, g->film_grain_info_fields.ar_coeff_shift_minus_6, 2);
   vb_put(b, g->film_grain_info_fields.grain_scale_shift, 2);
   if (g->num_cb_points) {
      vb_put(b, g->cb_mult, 8); vb_put(b, g->cb_luma_mult, 8); vb_put(b, g->cb_offset, 9);
   }
   if (g->num_cr_points) {
      vb_put(b, g->cr_mult, 8); vb_put(b, g->cr_luma_mult, 8); vb_put(b, g->cr_offset, 9);
   }
   vb_put(b, g->film_grain_info_fields.overlap_flag, 1);
   vb_put(b, g->film_grain_info_fields.clip_to_restricted_range, 1);
}

static bool frame_header(struct video_bits *b, const struct virgl_av1_picture_desc *d,
                         const struct virgl_av1_rewrite_state *s, unsigned *tile_bits)
{
   const av1_pic *p = &d->picture_parameter;
   unsigned type = p->pic_info_fields.frame_type;
   bool intra = type == 0 || type == 2;
   bool reset = type == 0 && p->pic_info_fields.show_frame;
   bool error_resilient = reset || type == 3 || p->pic_info_fields.error_resilient_mode;
   unsigned primary = intra || error_resilient ? 7 : p->primary_ref_frame;
   unsigned order_bits = p->seq_info_fields.enable_order_hint ? p->order_hint_bits_minus_1 + 1 : 0;
   unsigned width = p->frame_width, height = p->frame_height;
   if (!width || !height || width > p->max_width || height > p->max_height ||
       primary > 7 || p->pic_info_fields.large_scale_tile) return false;
   if (!intra) for (unsigned i = 0; i < 7; i++) {
      unsigned idx = p->ref_frame_idx[i];
      if (idx > 7 || !s->refs[idx].id || s->refs[idx].id != d->ref[idx]) return false;
   }
   vb_put(b, 0, 1); /* not show_existing_frame: this API receives a new picture */
   vb_put(b, type, 2); vb_put(b, p->pic_info_fields.show_frame, 1);
   if (!p->pic_info_fields.show_frame) vb_put(b, p->pic_info_fields.showable_frame, 1);
   if (!reset && type != 3) vb_put(b, error_resilient, 1);
   vb_put(b, p->pic_info_fields.disable_cdf_update, 1);
   vb_put(b, p->pic_info_fields.allow_screen_content_tools, 1);
   if (p->pic_info_fields.allow_screen_content_tools)
      vb_put(b, p->pic_info_fields.force_integer_mv, 1);
   if (type != 3) vb_put(b, 1, 1); /* explicit frame size */
   vb_put(b, p->order_hint, order_bits);
   if (!intra && !error_resilient) vb_put(b, primary, 3);
   unsigned refresh = reset || type == 3 ? 255 : p->refresh_frame_flags;
   if (!reset && type != 3) vb_put(b, refresh, 8);
   if ((!intra || refresh != 255) && error_resilient && order_bits)
      for (unsigned i = 0; i < 8; i++) vb_put(b, s->refs[i].order_hint, order_bits);
   if (!intra) {
      if (order_bits) vb_put(b, 0, 1); /* no short reference signaling */
      for (unsigned i = 0; i < 7; i++) vb_put(b, p->ref_frame_idx[i], 3);
      if (!error_resilient)
         for (unsigned i = 0; i < 7; i++) vb_put(b, 0, 1); /* found_ref */
   }
   vb_put(b, width - 1, MAX(vb_log2(p->max_width), 1));
   vb_put(b, height - 1, MAX(vb_log2(p->max_height), 1));
   vb_put(b, p->pic_info_fields.use_superres, 1);
   if (p->pic_info_fields.use_superres) {
      if (p->superres_scale_denominator < 9 || p->superres_scale_denominator > 16) return false;
      vb_put(b, p->superres_scale_denominator - 9, 3);
      width = (width * 8 + p->superres_scale_denominator / 2) / p->superres_scale_denominator;
   }
   vb_put(b, 0, 1); /* render size equals upscaled frame size */
   if (intra) {
      if (p->pic_info_fields.allow_screen_content_tools && !p->pic_info_fields.use_superres)
         vb_put(b, p->pic_info_fields.allow_intrabc, 1);
   } else {
      if (!p->pic_info_fields.force_integer_mv)
         vb_put(b, p->pic_info_fields.allow_high_precision_mv, 1);
      vb_put(b, p->interp_filter == 4, 1);
      if (p->interp_filter != 4) vb_put(b, p->interp_filter, 2);
      vb_put(b, p->pic_info_fields.is_motion_mode_switchable, 1);
      if (!error_resilient && order_bits && p->seq_info_fields.ref_frame_mvs)
         vb_put(b, p->pic_info_fields.use_ref_frame_mvs, 1);
      else if (p->pic_info_fields.use_ref_frame_mvs) return false;
   }
   if (!p->pic_info_fields.disable_cdf_update)
      vb_put(b, p->pic_info_fields.disable_frame_end_update_cdf, 1);
   if (!tile_info(b, p, width, tile_bits)) return false;
   bool lossless = quantization(b, p, primary);
   filters(b, p, lossless);
   if (!intra) {
      vb_put(b, p->mode_control_fields.reference_select, 1);
      if (skip_allowed(p, s)) vb_put(b, p->mode_control_fields.skip_mode_present, 1);
      else if (p->mode_control_fields.skip_mode_present) return false;
      if (!error_resilient) vb_put(b, p->pic_info_fields.allow_warped_motion, 1);
   }
   vb_put(b, p->mode_control_fields.reduced_tx_set_used, 1);
   if (!intra) global_motion(b, p, primary == 7 ? NULL : &s->refs[p->ref_frame_idx[primary]]);
   film_grain(b, p);
   vb_align(b, true); /* OBU_FRAME_HEADER trailing_bits, not OBU_FRAME alignment */
   return !b->failed;
}

bool virgl_video_av1_rewrite(const struct virgl_av1_rewrite_state *state,
                            struct virgl_av1_rewrite_state *next_state,
                            const struct virgl_av1_picture_desc *d,
                            uint32_t target_id,
                            const uint8_t *data, size_t size,
                            struct virgl_video_bitstream *sample,
                            struct virgl_video_bitstream *configuration)
{
   struct virgl_video_bitstream seq = {0}, group = {0};
   uint8_t bytes[8192];
   struct video_bits b = {.data = bytes, .capacity = sizeof(bytes)};
   bool ok = false;
   unsigned tile_bits;
   if (!state || !next_state || !d || !target_id || !data || !size ||
       size > VIDEO_BITSTREAM_LIMIT || !sample || !configuration ||
       sample->data || sample->size || configuration->data || configuration->size)
      return false;
   const av1_pic *p = &d->picture_parameter;
   bool reset = p->pic_info_fields.frame_type == 0 && p->pic_info_fields.show_frame;
   if (!sequence_header(p, &seq) || seq.size > sizeof(state->sequence)) goto out;
   if (state->sequence_size && (seq.size != state->sequence_size ||
       memcmp(seq.data, state->sequence, seq.size)) && !reset) goto out;
   if (!frame_header(&b, d, state, &tile_bits)) goto out;
   if (!vs_append(sample, seq.data, seq.size) || !vs_obu(sample, 3, bytes, b.count / 8)) goto out;
   unsigned tiles = p->tile_cols * p->tile_rows;
   if (d->slice_parameter.slice_count != tiles) goto out;
   int indices[256];
   for (unsigned i = 0; i < 256; i++) indices[i] = -1;
   for (unsigned i = 0; i < tiles; i++) {
      unsigned row = d->slice_parameter.slice_data_row[i], col = d->slice_parameter.slice_data_col[i];
      size_t offset = d->slice_parameter.slice_data_offset[i], length = d->slice_parameter.slice_data_size[i];
      if (row >= p->tile_rows || col >= p->tile_cols || !length || offset > size || length > size - offset)
         goto out;
      unsigned id = row * p->tile_cols + col;
      if (indices[id] != -1) goto out;
      indices[id] = i;
   }
   if (tiles > 1) { uint8_t zero = 0; if (!vs_append(&group, &zero, 1)) goto out; }
   for (unsigned i = 0; i < tiles; i++) {
      unsigned idx = indices[i];
      uint32_t length = d->slice_parameter.slice_data_size[idx];
      size_t offset = d->slice_parameter.slice_data_offset[idx];
      if (i + 1 < tiles) {
         uint32_t n = length - 1;
         uint8_t le[] = {n, n >> 8, n >> 16, n >> 24};
         if (!vs_append(&group, le, 4)) goto out;
      }
      if (!vs_append(&group, data + offset, length)) goto out;
   }
   if (!vs_obu(sample, 4, group.data, group.size)) goto out;
   uint8_t av1c[] = {0x81, 31, (p->bit_depth_idx ? 0x40 : 0) | 0x0c, 0};
   if (!vs_append(configuration, av1c, sizeof(av1c)) ||
       !vs_append(configuration, seq.data, seq.size)) goto out;
   *next_state = *state;
   memcpy(next_state->sequence, seq.data, seq.size);
   next_state->sequence_size = seq.size;
   unsigned refresh = reset || p->pic_info_fields.frame_type == 3 ? 255 : p->refresh_frame_flags;
   for (unsigned i = 0; i < 8; i++) if (refresh & (1u << i)) {
      next_state->refs[i].id = target_id;
      next_state->refs[i].order_hint = p->order_hint;
      for (unsigned j = 0; j < 7; j++) {
         int32_t *gm = next_state->refs[i].gm[j];
         if ((p->pic_info_fields.frame_type == 1 || p->pic_info_fields.frame_type == 3) && p->wm[j].wmtype)
            memcpy(gm, p->wm[j].wmmat, 6 * sizeof(*gm));
         else { memset(gm, 0, 6 * sizeof(*gm)); gm[2] = gm[5] = 65536; }
      }
   }
   ok = true;
out:
   vs_clear(&seq); vs_clear(&group);
   if (!ok) { vs_clear(sample); vs_clear(configuration); }
   return ok;
}
