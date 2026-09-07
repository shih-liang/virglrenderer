/* SPDX-License-Identifier: MIT
 * Copyright 2026 NativePipe contributors
 * H.265 sections 7.3.6/7.3.7: materialize the resolved DPB as a slice RPS.
 * Header coding choices are replaceable; reference ordering, prediction,
 * filtering and the entropy-coded payload are not.
 */
#include "virgl_video_bits.h"

struct hevc_reader {
   const uint8_t *data;
   size_t size, byte, count;
   unsigned bit, zeroes;
   bool failed;
   struct video_bits *copy;
};

static uint32_t hr_u(struct hevc_reader *r, unsigned n)
{
   uint32_t v = 0;
   if (n > 32) { r->failed = true; return 0; }
   unsigned width = n;
   while (n-- && !r->failed) {
      if (!r->bit && r->zeroes >= 2 && r->byte < r->size && r->data[r->byte] == 3) {
         r->byte++; r->zeroes = 0;
         if (r->byte == r->size || r->data[r->byte] > 3) r->failed = true;
      }
      if (r->byte == r->size) { r->failed = true; break; }
      v = (v << 1) | ((r->data[r->byte] >> (7 - r->bit)) & 1);
      r->count++;
      if (++r->bit == 8) {
         r->zeroes = r->data[r->byte] ? 0 : r->zeroes + 1;
         r->byte++; r->bit = 0;
      }
   }
   if (r->copy) vb_put(r->copy, v, width);
   return v;
}

static uint32_t hr_ue(struct hevc_reader *r)
{
   unsigned n = 0;
   while (!r->failed && !hr_u(r, 1)) {
      if (++n == 32) { r->failed = true; return 0; }
   }
   return ((1u << n) - 1) + hr_u(r, n);
}

static void hr_skip(struct hevc_reader *r, size_t bits)
{
   if (bits > (r->size - r->byte) * 8) { r->failed = true; return; }
   while (bits && !r->failed) {
      unsigned n = bits > 32 ? 32 : bits;
      hr_u(r, n); bits -= n;
   }
}

struct hevc_rps {
   unsigned negative[16], positive[16], lt[16];
   unsigned nneg, npos, nlt, total;
   unsigned old_list[2][16], new_list[2][16];
   bool used[16];
};

static bool make_rps(const struct virgl_h265_picture_desc *p, struct hevc_rps *rps)
{
   unsigned counts[] = {p->NumPocStCurrBefore, p->NumPocStCurrAfter, p->NumPocLtCurr};
   const uint8_t *sets[] = {p->RefPicSetStCurrBefore, p->RefPicSetStCurrAfter, p->RefPicSetLtCurr};
   for (unsigned group = 0; group < 3; group++) {
      if (counts[group] > 8) return false;
      for (unsigned i = 0; i < counts[group]; i++) {
         unsigned idx = sets[group][i];
         if (idx >= 16 || !p->ref[idx] || rps->used[idx] ||
             !!p->IsLongTerm[idx] != (group == 2)) return false;
         int32_t poc = p->PicOrderCntVal[idx];
         if (group == 0 && poc >= p->CurrPicOrderCntVal) return false;
         if (group == 1 && poc <= p->CurrPicOrderCntVal) return false;
         rps->used[idx] = true;
      }
      rps->total += counts[group];
   }
   if (rps->total > 15) return false;
   for (unsigned i = 0; i < 16; i++) if (p->ref[i]) {
      for (unsigned j = 0; j < i; j++) if (p->ref[j] &&
         (p->ref[i] == p->ref[j] || p->PicOrderCntVal[i] == p->PicOrderCntVal[j])) return false;
      unsigned *list, *n;
      if (p->IsLongTerm[i]) { list = rps->lt; n = &rps->nlt; }
      else if (p->PicOrderCntVal[i] < p->CurrPicOrderCntVal) { list = rps->negative; n = &rps->nneg; }
      else if (p->PicOrderCntVal[i] > p->CurrPicOrderCntVal) { list = rps->positive; n = &rps->npos; }
      else return false;
      unsigned pos = (*n)++;
      bool descending = list != rps->positive;
      while (pos && (descending ? p->PicOrderCntVal[list[pos-1]] < p->PicOrderCntVal[i]
                                : p->PicOrderCntVal[list[pos-1]] > p->PicOrderCntVal[i])) {
         list[pos] = list[pos-1]; pos--;
      }
      list[pos] = i;
   }
   if (rps->nneg + rps->npos + rps->nlt > 15) return false;
   for (unsigned l = 0; l < 2; l++) {
      unsigned old = 0, next = 0;
      for (unsigned k = 0; k < 3; k++) {
         unsigned group = k == 2 ? 2 : k ^ l;
         for (unsigned i = 0; i < counts[group]; i++) rps->old_list[l][old++] = sets[group][i];
         const unsigned *set = group == 0 ? rps->negative : group == 1 ? rps->positive : rps->lt;
         unsigned n = group == 0 ? rps->nneg : group == 1 ? rps->npos : rps->nlt;
         for (unsigned i = 0; i < n; i++) if (rps->used[set[i]]) rps->new_list[l][next++] = set[i];
      }
      if (old != rps->total || next != old) return false;
   }
   return true;
}

static void read_old_rps(struct hevc_reader *r, const struct virgl_h265_picture_desc *p)
{
   const struct virgl_h265_sps *s = &p->pps.sps;
   if (hr_u(r, 1)) {
      if (!s->num_short_term_ref_pic_sets) { r->failed = true; return; }
      unsigned index = hr_u(r, vb_log2(s->num_short_term_ref_pic_sets));
      if (index >= s->num_short_term_ref_pic_sets) r->failed = true;
   } else {
      /* VA-API supplies the exact RBSP length when the slice RPS predicts an
       * SPS RPS whose syntax is no longer present. Do not guess that RPS. */
      unsigned bits = p->NumShortTermPictureSliceHeaderBits ?
         p->NumShortTermPictureSliceHeaderBits : p->pps.st_rps_bits;
      if (bits) {
         if (bits > 4096) { r->failed = true; return; }
         hr_skip(r, bits);
      } else if (s->num_short_term_ref_pic_sets && hr_u(r, 1)) {
         if (hr_ue(r) >= s->num_short_term_ref_pic_sets || p->NumDeltaPocsOfRefRpsIdx > 15) {
            r->failed = true; return;
         }
         hr_u(r, 1); hr_ue(r);
         for (unsigned i = 0; i <= p->NumDeltaPocsOfRefRpsIdx; i++)
            if (!hr_u(r, 1)) hr_u(r, 1);
      } else {
         unsigned n = hr_ue(r), pos = hr_ue(r);
         if (n > 15 || pos > 15 - n) { r->failed = true; return; }
         for (unsigned i = 0; i < n + pos; i++) { hr_ue(r); hr_u(r, 1); }
      }
   }
   if (s->long_term_ref_pics_present_flag) {
      unsigned from_sps = s->num_long_term_ref_pics_sps ? hr_ue(r) : 0;
      unsigned from_slice = hr_ue(r);
      if (from_sps > s->num_long_term_ref_pics_sps || from_sps > 15 || from_slice > 15 - from_sps) {
         r->failed = true; return;
      }
      for (unsigned i = 0; i < from_sps + from_slice; i++) {
         if (i < from_sps) hr_u(r, vb_log2(s->num_long_term_ref_pics_sps));
         else { hr_u(r, s->log2_max_pic_order_cnt_lsb_minus4 + 4); hr_u(r, 1); }
         if (hr_u(r, 1)) hr_ue(r);
      }
   }
}

static void write_rps(struct video_bits *b, const struct virgl_h265_picture_desc *p,
                      const struct hevc_rps *rps)
{
   vb_put(b, 0, 1); /* explicit short-term RPS, no SPS set index */
   vb_ue(b, rps->nneg); vb_ue(b, rps->npos);
   for (unsigned group = 0; group < 2; group++) {
      int64_t previous = p->CurrPicOrderCntVal;
      const unsigned *set = group ? rps->positive : rps->negative;
      unsigned n = group ? rps->npos : rps->nneg;
      for (unsigned i = 0; i < n; i++) {
         int64_t poc = p->PicOrderCntVal[set[i]];
         int64_t delta = group ? poc - previous : previous - poc;
         if (delta <= 0 || delta > UINT32_MAX) { b->failed = true; return; }
         vb_ue(b, delta - 1); vb_put(b, rps->used[set[i]], 1);
         previous = poc;
      }
   }
   /* The new SPS permits long-term refs with an empty SPS list. */
   vb_ue(b, rps->nlt);
   unsigned bits = p->pps.sps.log2_max_pic_order_cnt_lsb_minus4 + 4;
   int64_t modulus = 1 << bits;
   int64_t current_msb = (int64_t)p->CurrPicOrderCntVal - ((uint32_t)p->CurrPicOrderCntVal & (modulus - 1));
   int64_t previous_cycle = 0;
   for (unsigned i = 0; i < rps->nlt; i++) {
      unsigned idx = rps->lt[i];
      uint32_t lsb = (uint32_t)p->PicOrderCntVal[idx] & (modulus - 1);
      int64_t cycle = (current_msb + lsb - (int64_t)p->PicOrderCntVal[idx]) / modulus;
      if (cycle < 0) {
         /* A future-MSB long-term picture can only use unambiguous LSB
          * matching. It cannot be expressed as an unsigned past-MSB cycle. */
         for (unsigned j = 0; j < 16; j++)
            if (j != idx && p->ref[j] && ((uint32_t)p->PicOrderCntVal[j] & (modulus - 1)) == lsb) {
               b->failed = true; return;
            }
         vb_put(b, lsb, bits); vb_put(b, rps->used[idx], 1); vb_put(b, 0, 1);
         continue;
      }
      if (cycle < previous_cycle || cycle - previous_cycle >= UINT32_MAX) { b->failed = true; return; }
      vb_put(b, lsb, bits); vb_put(b, rps->used[idx], 1);
      vb_put(b, 1, 1); /* explicit MSB: no ambiguous same-LSB references */
      vb_ue(b, cycle - previous_cycle);
      previous_cycle = cycle;
   }
}

static void ref_lists(struct hevc_reader *r, struct video_bits *b,
                      const struct virgl_h265_picture_desc *p, const struct hevc_rps *rps,
                      unsigned type, const unsigned active[2])
{
   if (!rps->total || rps->total > 15) { r->failed = true; return; }
   if (rps->total == 1) return;
   r->copy = NULL;
   for (unsigned l = 0; l < (type == 0 ? 2u : 1u); l++) {
      bool modified = p->pps.lists_modification_present_flag && hr_u(r, 1);
      unsigned width = vb_log2(rps->total);
      vb_put(b, 1, 1); /* explicitly retain the original effective list order */
      for (unsigned i = 0; i < active[l]; i++) {
         unsigned old = modified ? hr_u(r, width) : i % rps->total;
         if (old >= rps->total) { r->failed = true; return; }
         unsigned id = rps->old_list[l][old], next;
         for (next = 0; next < rps->total && rps->new_list[l][next] != id; next++);
         if (next == rps->total) { r->failed = true; return; }
         vb_put(b, next, width);
      }
   }
   r->copy = b;
}

static void weights(struct hevc_reader *r, unsigned type, const unsigned active[2])
{
   if (hr_ue(r) > 7) { r->failed = true; return; }
   hr_ue(r); /* se(delta_chroma_log2_weight_denom): same code length as ue */
   for (unsigned l = 0; l < (type == 0 ? 2u : 1u); l++) {
      bool y[15], uv[15];
      for (unsigned i = 0; i < active[l]; i++) y[i] = hr_u(r, 1);
      for (unsigned i = 0; i < active[l]; i++) uv[i] = hr_u(r, 1);
      for (unsigned i = 0; i < active[l]; i++) {
         if (y[i]) { hr_ue(r); hr_ue(r); }
         if (uv[i]) for (unsigned j = 0; j < 4; j++) hr_ue(r);
      }
   }
}

static bool slice_header(struct hevc_reader *r, struct video_bits *b,
                          const struct virgl_h265_picture_desc *p, unsigned nal_type)
{
   const struct virgl_h265_pps *pps = &p->pps;
   const struct virgl_h265_sps *s = &pps->sps;
   unsigned ctb_log2 = s->log2_min_luma_coding_block_size_minus3 + 3 +
                       s->log2_diff_max_min_luma_coding_block_size;
   if (ctb_log2 < 4 || ctb_log2 > 6 || s->log2_max_pic_order_cnt_lsb_minus4 > 12 ||
       s->chroma_format_idc != 1 || s->separate_colour_plane_flag ||
       !s->pic_width_in_luma_samples || !s->pic_height_in_luma_samples ||
       s->pic_width_in_luma_samples > 16384 || s->pic_height_in_luma_samples > 16384)
      return false;
   unsigned ctb = 1u << ctb_log2;
   unsigned rows = (s->pic_height_in_luma_samples + ctb - 1) / ctb;
   unsigned cols = (s->pic_width_in_luma_samples + ctb - 1) / ctb;
   bool first = hr_u(r, 1), dependent = false;
   if (nal_type >= 16 && nal_type <= 23) hr_u(r, 1);
   if (hr_ue(r) > 63) return false;
   if (!first) {
      if (pps->dependent_slice_segments_enabled_flag) dependent = hr_u(r, 1);
      if (hr_u(r, vb_log2(cols * rows)) >= cols * rows) return false;
   }
   if (!dependent) {
      if (pps->num_extra_slice_header_bits > 7) return false;
      hr_u(r, pps->num_extra_slice_header_bits);
      unsigned type = hr_ue(r);
      if (type > 2) return false;
      if (pps->output_flag_present_flag) hr_u(r, 1);
      bool temporal_mvp = false;
      struct hevc_rps rps = {0};
      if (nal_type != 19 && nal_type != 20) {
         unsigned poc_bits = s->log2_max_pic_order_cnt_lsb_minus4 + 4;
         if (hr_u(r, poc_bits) != ((uint32_t)p->CurrPicOrderCntVal & ((1u << poc_bits) - 1)))
            return false;
         r->copy = NULL;
         read_old_rps(r, p);
         if (r->failed || !make_rps(p, &rps)) return false;
         write_rps(b, p, &rps);
         r->copy = b;
         if (s->sps_temporal_mvp_enabled_flag) temporal_mvp = hr_u(r, 1);
      }
      bool sao_y = false, sao_uv = false;
      if (s->sample_adaptive_offset_enabled_flag) {
         sao_y = hr_u(r, 1); sao_uv = hr_u(r, 1);
      }
      if (type < 2) {
         unsigned active[2] = {pps->num_ref_idx_l0_default_active_minus1 + 1u,
                               pps->num_ref_idx_l1_default_active_minus1 + 1u};
         if (hr_u(r, 1)) {
            unsigned n = hr_ue(r);
            if (n >= 15) return false;
            active[0] = n + 1;
            if (!type) { n = hr_ue(r); if (n >= 15) return false; active[1] = n + 1; }
         }
         if (active[0] > 15 || active[1] > 15) return false;
         ref_lists(r, b, p, &rps, type, active);
         if (!type) hr_u(r, 1); /* mvd_l1_zero_flag */
         if (pps->cabac_init_present_flag) hr_u(r, 1);
         if (temporal_mvp) {
            unsigned list = type || hr_u(r, 1) ? 0 : 1;
            if (active[list] > 1 && hr_ue(r) >= active[list]) return false;
         }
         if ((pps->weighted_pred_flag && type == 1) || (pps->weighted_bipred_flag && !type))
            weights(r, type, active);
         if (hr_ue(r) > 4) return false;
      }
      hr_ue(r); /* slice_qp_delta */
      if (pps->pps_slice_chroma_qp_offsets_present_flag) { hr_ue(r); hr_ue(r); }
      bool disabled = pps->pps_deblocking_filter_disabled_flag;
      if (pps->deblocking_filter_override_enabled_flag && hr_u(r, 1)) {
         disabled = hr_u(r, 1);
         if (!disabled) { hr_ue(r); hr_ue(r); }
      }
      if (pps->pps_loop_filter_across_slices_enabled_flag && (sao_y || sao_uv || !disabled)) hr_u(r, 1);
   }
   if (pps->tiles_enabled_flag || pps->entropy_coding_sync_enabled_flag) {
      unsigned entries = hr_ue(r);
      if (entries > cols * rows) return false;
      if (entries) {
         unsigned width = hr_ue(r);
         if (width > 31) return false;
         for (unsigned i = 0; i < entries; i++) hr_u(r, width + 1);
      }
   }
   if (pps->slice_segment_header_extension_present_flag) {
      unsigned bytes = hr_ue(r);
      if (bytes > 256) return false;
      hr_skip(r, bytes * 8);
   }
   r->copy = NULL;
   if (hr_u(r, 1) != 1) return false;
   while (r->bit && !r->failed) if (hr_u(r, 1)) return false;
   vb_align(b, true);
   return !r->failed && !b->failed;
}

bool virgl_video_hevc_rewrite(const struct virgl_h265_picture_desc *p,
                             const uint8_t *nal, size_t size,
                             struct virgl_video_bitstream *output)
{
   if (!p || !nal || size < 3 || size > VIDEO_BITSTREAM_LIMIT - 4096 || !output ||
       output->data || output->size || (nal[0] & 0x81) || (nal[1] & 0xf8) || !(nal[1] & 7))
      return false;
   unsigned type = nal[0] >> 1;
   if (type > 31) return false;
   uint8_t *header = malloc(size + 4096);
   if (!header) return false;
   struct video_bits b = {.data = header, .capacity = size + 4096};
   struct hevc_reader r = {.data = nal + 2, .size = size - 2, .copy = &b};
   bool ok = slice_header(&r, &b, p, type) && r.byte < r.size;
   if (ok) {
      size_t bytes = b.count / 8;
      /* Header alignment ends in a nonzero byte, so emulation prevention for
       * the payload is independent of header length/content. Entry-point
       * offsets remain valid because not one payload byte is changed. */
      size_t capacity = 2 + bytes + bytes / 2 + (r.size - r.byte);
      if (capacity > VIDEO_BITSTREAM_LIMIT) ok = false;
      else {
         output->data = malloc(capacity);
         if (!output->data) ok = false;
         else {
            output->data[0] = nal[0]; output->data[1] = nal[1];
            size_t n = 2; unsigned zeros = 0;
            for (size_t i = 0; i < bytes; i++) {
               if (zeros == 2 && header[i] <= 3) { output->data[n++] = 3; zeros = 0; }
               output->data[n++] = header[i]; zeros = header[i] ? 0 : zeros + 1;
            }
            memcpy(output->data + n, r.data + r.byte, r.size - r.byte);
            output->size = n + r.size - r.byte;
         }
      }
   }
   free(header);
   return ok;
}
