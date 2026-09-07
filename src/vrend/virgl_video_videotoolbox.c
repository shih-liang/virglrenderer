/**************************************************************************
 *
 * Copyright (C) 2026 NativePipe contributors
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 **************************************************************************/

/*
 * macOS implementation of the virgl video interface. VideoToolbox consumes
 * and produces IOSurface-backed CVPixelBuffers. A native Metal transfer
 * copies their planes to/from the guest's fixed shared video resources.
 *
 * Capabilities are the intersection of the Gallium video protocol and the
 * hardware codecs reported by VideoToolbox.  Never advertise a VideoToolbox
 * codec merely because CoreMedia defines its fourcc. Gallium's stateless
 * descriptors are converted to equivalent stateful headers where the decode
 * state is complete; unsupported configurations fail explicitly.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pipe/p_video_state.h"
#include "util/u_memory.h"
#include "virgl_hw.h"
#include "virgl_video.h"
#include "virgl_video_hw.h"
#include "virgl_video_bitstream.h"
#include "virgl_util.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
                            ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define DRM_FORMAT_NV12 FOURCC('N', 'V', '1', '2')
#define DRM_FORMAT_P010 FOURCC('P', '0', '1', '0')
#define DRM_FORMAT_R8   FOURCC('R', '8', ' ', ' ')
#define DRM_FORMAT_GR88 FOURCC('G', 'R', '8', '8')
#define DRM_FORMAT_R16  FOURCC('R', '1', '6', ' ')
#define DRM_FORMAT_GR32 FOURCC('G', 'R', '3', '2')
#define MAX_CODED_BYTES (128u * 1024u * 1024u)

struct decode_completion {
   uint32_t width, height;
   virgl_video_decode_done callback;
   void *data;
};

static void decode_complete(struct decode_completion *frame, void *pixels)
{
   frame->callback(frame->data, pixels);
   free(frame);
}

struct byte_buffer {
   uint8_t *data;
   size_t size;
   size_t capacity;
};

struct bit_writer {
   uint8_t *data;
   size_t capacity;
   size_t bit_count;
   bool failed;
};

struct virgl_video_buffer {
   enum pipe_format format;
   uint32_t width;
   uint32_t height;
   uint32_t id;
   CVPixelBufferRef pixel_buffer;
   OSStatus status;
   void *opaque;
};

struct virgl_video_codec {
   enum pipe_video_profile profile;
   enum pipe_video_entrypoint entrypoint;
   enum pipe_video_chroma_format chroma_format;
   uint32_t level;
   uint32_t width;
   uint32_t height;
   uint32_t max_references;
   void *opaque;
   uint32_t frame_buffer_id;
   bool frame_ready;
   bool frame_failed;
   struct byte_buffer frame_data;
   union virgl_picture_desc frame_desc;
   bool parameter_sets_in_band;
   struct virgl_av1_rewrite_state av1_state;

   VTDecompressionSessionRef decoder;
   CMVideoFormatDescriptionRef decode_format;
   CMVideoCodecType decode_codec_type;
   enum pipe_format decode_surface_format;
   uint8_t *decode_config;
   size_t decode_config_size;
   uint8_t *vps;
   size_t vps_size;
   uint8_t *sps;
   size_t sps_size;
   uint8_t *pps;
   size_t pps_size;

   VTCompressionSessionRef encoder;
   CMTime next_timestamp;
   uint8_t *coded_data;
   size_t coded_size;
   OSStatus encode_status;
};

static struct virgl_video_callbacks *video_callbacks;
static uint32_t next_buffer_id = 1;

static bool h264_profile_supported(enum pipe_video_profile profile);
static bool hevc_profile_supported(enum pipe_video_profile profile);

static bool bb_reserve(struct byte_buffer *buffer, size_t additional)
{
   size_t required;
   size_t capacity;
   uint8_t *data;

   if (additional > MAX_CODED_BYTES || buffer->size > MAX_CODED_BYTES - additional)
      return false;
   required = buffer->size + additional;
   if (required <= buffer->capacity)
      return true;
   capacity = buffer->capacity ? buffer->capacity : 4096;
   while (capacity < required) {
      if (capacity > MAX_CODED_BYTES / 2)
         capacity = MAX_CODED_BYTES;
      else
         capacity *= 2;
   }
   data = realloc(buffer->data, capacity);
   if (!data)
      return false;
   buffer->data = data;
   buffer->capacity = capacity;
   return true;
}

static bool bb_append(struct byte_buffer *buffer, const void *data, size_t size)
{
   if (!size)
      return true;
   if (!data || !bb_reserve(buffer, size))
      return false;
   memcpy(buffer->data + buffer->size, data, size);
   buffer->size += size;
   return true;
}

static bool bb_append_u32be(struct byte_buffer *buffer, uint32_t value)
{
   uint8_t bytes[4] = {
      (uint8_t)(value >> 24), (uint8_t)(value >> 16),
      (uint8_t)(value >> 8), (uint8_t)value,
   };
   return bb_append(buffer, bytes, sizeof(bytes));
}

static bool bb_append_u16be(struct byte_buffer *buffer, uint16_t value)
{
   uint8_t bytes[2] = {(uint8_t)(value >> 8), (uint8_t)value};
   return bb_append(buffer, bytes, sizeof(bytes));
}

static bool bb_append_u8(struct byte_buffer *buffer, uint8_t value)
{
   return bb_append(buffer, &value, sizeof(value));
}

static void bb_clear(struct byte_buffer *buffer)
{
   free(buffer->data);
   memset(buffer, 0, sizeof(*buffer));
}

static void bw_bit(struct bit_writer *writer, unsigned bit)
{
   size_t byte = writer->bit_count / 8;
   unsigned shift = 7 - (writer->bit_count % 8);
   if (byte >= writer->capacity) {
      writer->failed = true;
      return;
   }
   if ((writer->bit_count % 8) == 0)
      writer->data[byte] = 0;
   writer->data[byte] |= (bit & 1u) << shift;
   writer->bit_count++;
}

static void bw_bits(struct bit_writer *writer, uint32_t value, unsigned bits)
{
   if (bits > 32) {
      writer->failed = true;
      return;
   }
   while (bits--)
      bw_bit(writer, value >> bits);
}

static void bw_ue(struct bit_writer *writer, uint32_t value)
{
   if (value == UINT32_MAX) {
      writer->failed = true;
      return;
   }
   uint32_t code_num = value + 1;
   unsigned bits = 0;
   uint32_t copy = code_num;
   while (copy) {
      bits++;
      copy >>= 1;
   }
   for (unsigned i = 1; i < bits; i++)
      bw_bit(writer, 0);
   bw_bits(writer, code_num, bits);
}

static void bw_se(struct bit_writer *writer, int32_t value)
{
   if (value == INT32_MIN) {
      writer->failed = true;
      return;
   }
   uint32_t mapped = value <= 0 ? (uint32_t)(-2 * (int64_t)value)
                                : (uint32_t)(2 * (int64_t)value - 1);
   bw_ue(writer, mapped);
}

static size_t bw_finish(struct bit_writer *writer)
{
   bw_bit(writer, 1);
   while (!writer->failed && writer->bit_count % 8)
      bw_bit(writer, 0);
   return writer->failed ? 0 : writer->bit_count / 8;
}

static size_t nal_escape(uint8_t header, const uint8_t *rbsp, size_t rbsp_size,
                         uint8_t *output, size_t capacity)
{
   size_t count = 0;
   unsigned zeroes = 0;
   if (!capacity)
      return 0;
   output[count++] = header;
   for (size_t i = 0; i < rbsp_size; i++) {
      uint8_t byte = rbsp[i];
      if (zeroes >= 2 && byte <= 3) {
         if (count >= capacity)
            return 0;
         output[count++] = 3;
         zeroes = 0;
      }
      if (count >= capacity)
         return 0;
      output[count++] = byte;
      zeroes = byte == 0 ? zeroes + 1 : 0;
   }
   return count;
}

static size_t hevc_nal_escape(uint8_t nal_type, const uint8_t *rbsp,
                              size_t rbsp_size, uint8_t *output, size_t capacity)
{
   if (capacity < 2)
      return 0;
   output[0] = nal_type << 1;
   size_t size = nal_escape(1, rbsp, rbsp_size, output + 1, capacity - 1);
   return size ? size + 1 : 0;
}

static uint8_t h264_profile_idc(enum pipe_video_profile profile)
{
   switch (profile) {
   case PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE:
   case PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE:
      return 66;
   case PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN:
      return 77;
   case PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH:
      return 100;
   default:
      return 0;
   }
}

/* Gallium/VA-API carry the effective scaling lists in scan order. Emit the
 * resolved lists in the PPS; there is no need to recreate SPS fallback rules. */
static void write_h264_scaling_list(struct bit_writer *writer,
                                    const uint8_t *list, unsigned count)
{
   int last = 8;
   for (unsigned i = 0; i < count; i++) {
      if (!list[i]) {
         writer->failed = true;
         return;
      }
      int delta = ((int)list[i] - last + 128 + 256) % 256 - 128;
      bw_se(writer, delta);
      last = list[i];
   }
}

static bool synthesize_h264_parameter_sets(
      const struct virgl_video_codec *codec,
      const struct virgl_h264_picture_desc *picture,
      uint32_t pps_id,
      struct byte_buffer *sps_nal,
      struct byte_buffer *pps_nal)
{
   const struct virgl_h264_pps *pps = &picture->pps;
   const struct virgl_h264_sps *sps = &pps->sps;
   uint8_t profile = h264_profile_idc(codec->profile);
   uint8_t rbsp[1024];
   uint8_t nal[1200];
   struct bit_writer writer;
   size_t rbsp_size;
   size_t nal_size;
   uint32_t width_mbs;
   uint32_t height_map_units;
   uint32_t coded_width;
   uint32_t coded_height;
   uint32_t crop_right;
   uint32_t crop_bottom;
   bool scaling = pps->ScalingList4x4[0][0] != 0;

   if (!profile || pps_id > 255 || sps->pic_order_cnt_type > 2 ||
       sps->log2_max_frame_num_minus4 > 12 || sps->log2_max_pic_order_cnt_lsb_minus4 > 12 ||
       sps->chroma_format_idc != 1 || sps->bit_depth_luma_minus8 ||
       sps->bit_depth_chroma_minus8 ||
       (sps->seq_scaling_matrix_present_flag && !scaling) ||
       pps->num_slice_groups_minus1 ||
       picture->num_ref_frames > 16 ||
       picture->num_ref_idx_l0_active_minus1 > 31 ||
       picture->num_ref_idx_l1_active_minus1 > 31 ||
       !sps->frame_mbs_only_flag)
      return false;

   memset(&writer, 0, sizeof(writer));
   writer.data = rbsp;
   writer.capacity = sizeof(rbsp);
   bw_bits(&writer, profile, 8);
   bw_bits(&writer,
           codec->profile == PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE
              ? 0xc0 : 0,
           8);
   bw_bits(&writer, sps->level_idc ? sps->level_idc :
            codec->level && codec->level <= UINT8_MAX ? codec->level : 40, 8);
   bw_ue(&writer, 0); /* seq_parameter_set_id */
   if (profile >= 100) {
      bw_ue(&writer, sps->chroma_format_idc ? sps->chroma_format_idc : 1);
      bw_ue(&writer, sps->bit_depth_luma_minus8);
      bw_ue(&writer, sps->bit_depth_chroma_minus8);
      bw_bit(&writer, 0); /* qpprime_y_zero_transform_bypass_flag */
      bw_bit(&writer, 0); /* seq_scaling_matrix_present_flag */
   }
   bw_ue(&writer, sps->log2_max_frame_num_minus4);
   bw_ue(&writer, sps->pic_order_cnt_type);
   if (sps->pic_order_cnt_type == 0) {
      bw_ue(&writer, sps->log2_max_pic_order_cnt_lsb_minus4);
   } else if (sps->pic_order_cnt_type == 1) {
      bw_bit(&writer, sps->delta_pic_order_always_zero_flag);
      bw_se(&writer, sps->offset_for_non_ref_pic);
      bw_se(&writer, sps->offset_for_top_to_bottom_field);
      bw_ue(&writer, sps->num_ref_frames_in_pic_order_cnt_cycle);
      for (unsigned i = 0; i < sps->num_ref_frames_in_pic_order_cnt_cycle; i++)
         bw_se(&writer, sps->offset_for_ref_frame[i]);
   }
   /* VA supplies the resolved values on the picture, not these SPS/PPS
    * fields. Mesa leaves max_num_ref_frames and default_active_minus1 zero.
    * Emitting those zeros misparses P/B slice headers and loses references. */
   bw_ue(&writer, picture->num_ref_frames);
   bw_bit(&writer, 0); /* gaps_in_frame_num_value_allowed_flag */

   width_mbs = (codec->width + 15) / 16;
   height_map_units = (codec->height + (sps->frame_mbs_only_flag ? 15 : 31)) /
                      (sps->frame_mbs_only_flag ? 16 : 32);
   bw_ue(&writer, width_mbs - 1);
   bw_ue(&writer, height_map_units - 1);
   bw_bit(&writer, sps->frame_mbs_only_flag);
   if (!sps->frame_mbs_only_flag)
      bw_bit(&writer, sps->mb_adaptive_frame_field_flag);
   bw_bit(&writer, sps->direct_8x8_inference_flag);

   coded_width = width_mbs * 16;
   coded_height = height_map_units * (sps->frame_mbs_only_flag ? 16 : 32);
   crop_right = (coded_width - codec->width) / 2;
   crop_bottom = (coded_height - codec->height) /
                 (2 * (sps->frame_mbs_only_flag ? 1 : 2));
   bw_bit(&writer, crop_right || crop_bottom);
   if (crop_right || crop_bottom) {
      bw_ue(&writer, 0);
      bw_ue(&writer, crop_right);
      bw_ue(&writer, 0);
      bw_ue(&writer, crop_bottom);
   }
   bw_bit(&writer, 0); /* vui_parameters_present_flag */
   rbsp_size = bw_finish(&writer);
   nal_size = nal_escape(0x67, rbsp, rbsp_size, nal, sizeof(nal));
   if (!rbsp_size || !nal_size || !bb_append(sps_nal, nal, nal_size))
      return false;

   memset(&writer, 0, sizeof(writer));
   writer.data = rbsp;
   writer.capacity = sizeof(rbsp);
   bw_ue(&writer, pps_id);
   bw_ue(&writer, 0); /* seq_parameter_set_id */
   bw_bit(&writer, pps->entropy_coding_mode_flag);
   bw_bit(&writer, pps->bottom_field_pic_order_in_frame_present_flag);
   bw_ue(&writer, 0); /* num_slice_groups_minus1 */
   bw_ue(&writer, picture->num_ref_idx_l0_active_minus1);
   bw_ue(&writer, picture->num_ref_idx_l1_active_minus1);
   bw_bit(&writer, pps->weighted_pred_flag);
   bw_bits(&writer, pps->weighted_bipred_idc, 2);
   bw_se(&writer, pps->pic_init_qp_minus26);
   bw_se(&writer, pps->pic_init_qs_minus26);
   bw_se(&writer, pps->chroma_qp_index_offset);
   bw_bit(&writer, pps->deblocking_filter_control_present_flag);
   bw_bit(&writer, pps->constrained_intra_pred_flag);
   bw_bit(&writer, pps->redundant_pic_cnt_present_flag);
   if (profile >= 100 || pps->second_chroma_qp_index_offset != pps->chroma_qp_index_offset) {
      bw_bit(&writer, pps->transform_8x8_mode_flag);
      bw_bit(&writer, scaling);
      if (scaling) {
         unsigned lists = pps->transform_8x8_mode_flag ? 8 : 6;
         for (unsigned i = 0; i < lists; i++) {
            bw_bit(&writer, 1); /* pic_scaling_list_present_flag */
            write_h264_scaling_list(&writer, i < 6 ? pps->ScalingList4x4[i] :
                                    pps->ScalingList8x8[i - 6], i < 6 ? 16 : 64);
         }
      }
      bw_se(&writer, pps->second_chroma_qp_index_offset);
   }
   rbsp_size = bw_finish(&writer);
   nal_size = nal_escape(0x68, rbsp, rbsp_size, nal, sizeof(nal));
   return rbsp_size && nal_size && bb_append(pps_nal, nal, nal_size);
}

struct bit_reader {
   const uint8_t *data;
   size_t size;
   size_t byte;
   unsigned bit;
   unsigned zeroes;
   bool failed;
};

static unsigned br_bit(struct bit_reader *reader)
{
   uint8_t value;
   while (reader->byte < reader->size && reader->bit == 0 &&
          reader->zeroes >= 2 && reader->data[reader->byte] == 3) {
      reader->byte++;
      reader->zeroes = 0;
   }
   if (reader->byte >= reader->size) {
      reader->failed = true;
      return 0;
   }
   value = reader->data[reader->byte];
   unsigned result = (value >> (7 - reader->bit)) & 1u;
   reader->bit++;
   if (reader->bit == 8) {
      reader->zeroes = value == 0 ? reader->zeroes + 1 : 0;
      reader->byte++;
      reader->bit = 0;
   }
   return result;
}

static uint32_t br_ue(struct bit_reader *reader)
{
   unsigned leading = 0;
   uint32_t suffix = 0;
   while (!reader->failed && br_bit(reader) == 0 && leading < 31)
      leading++;
   for (unsigned i = 0; i < leading; i++)
      suffix = (suffix << 1) | br_bit(reader);
   return reader->failed ? 0 : ((1u << leading) - 1u + suffix);
}

static uint32_t slice_pps_id(const uint8_t *nal, size_t size)
{
   struct bit_reader reader;
   if (size < 2)
      return UINT32_MAX;
   memset(&reader, 0, sizeof(reader));
   reader.data = nal + 1;
   reader.size = size - 1;
   (void)br_ue(&reader); /* first_mb_in_slice */
   (void)br_ue(&reader); /* slice_type */
   uint32_t id = br_ue(&reader);
   return reader.failed || id > 255 ? UINT32_MAX : id;
}

static size_t find_start_code(const uint8_t *data, size_t size, size_t start,
                              size_t *prefix_size)
{
   for (size_t i = start; i + 3 <= size; i++) {
      if (data[i] || data[i + 1])
         continue;
      if (data[i + 2] == 1) {
         *prefix_size = 3;
         return i;
      }
      if (i + 4 <= size && data[i + 2] == 0 && data[i + 3] == 1) {
         *prefix_size = 4;
         return i;
      }
   }
   return SIZE_MAX;
}

static uint8_t hevc_profile_idc(enum pipe_video_profile profile)
{
   switch (profile) {
   case PIPE_VIDEO_PROFILE_HEVC_MAIN:
   case PIPE_VIDEO_PROFILE_HEVC_MAIN_STILL:
      return 1;
   case PIPE_VIDEO_PROFILE_HEVC_MAIN_10:
      return 2;
   default:
      return 0;
   }
}

static void write_hevc_profile_tier_level(struct bit_writer *writer,
                                          uint8_t profile_idc,
                                          uint8_t level_idc)
{
   bw_bits(writer, 0, 2); /* general_profile_space */
   bw_bit(writer, 0);     /* general_tier_flag */
   bw_bits(writer, profile_idc, 5);
   bw_bits(writer, profile_idc < 32 ? 1u << (31 - profile_idc) : 0, 32);
   bw_bits(writer, 0xb0, 8); /* progressive, non-packed, frame-only */
   bw_bits(writer, 0, 32);
   bw_bits(writer, 0, 8);
   bw_bits(writer, level_idc, 8);
   /* Permit all seven temporal sublayers. The descriptor does not carry the
    * original maximum, and VCL temporal IDs must not be flattened: doing so
    * changes POC derivation for reordered pictures. No sublayer overrides. */
   bw_bits(writer, 0, 16);
}

static void write_hevc_scaling_lists(struct bit_writer *writer,
                                      const struct virgl_h265_sps *sps)
{
   /* Gallium's resolved matrices are already in diagonal scan order. */
   for (unsigned size = 0; size < 4; size++) {
      unsigned matrices = size == 3 ? 2 : 6;
      for (unsigned matrix = 0; matrix < matrices; matrix++) {
         const uint8_t *values = size == 0 ? sps->ScalingList4x4[matrix] :
            size == 1 ? sps->ScalingList8x8[matrix] :
            size == 2 ? sps->ScalingList16x16[matrix] : sps->ScalingList32x32[matrix];
         unsigned previous = 8;
         bw_bit(writer, 1); /* scaling_list_pred_mode_flag: explicit values */
         if (size >= 2) {
            previous = size == 2 ? sps->ScalingListDCCoeff16x16[matrix] :
                                  sps->ScalingListDCCoeff32x32[matrix];
            if (!previous) { writer->failed = true; return; }
            bw_se(writer, (int)previous - 8);
         }
         for (unsigned i = 0; i < (size ? 64u : 16u); i++) {
            if (!values[i]) { writer->failed = true; return; }
            int delta = (values[i] - previous + 128) & 255;
            bw_se(writer, delta - 128);
            previous = values[i];
         }
      }
   }
}

static bool append_hevc_rbsp(struct byte_buffer *output, uint8_t nal_type,
                             struct bit_writer *writer, uint8_t *rbsp,
                             uint8_t *nal, size_t nal_capacity)
{
   size_t rbsp_size = bw_finish(writer);
   size_t nal_size = rbsp_size
      ? hevc_nal_escape(nal_type, rbsp, rbsp_size, nal, nal_capacity) : 0;
   return nal_size && bb_append(output, nal, nal_size);
}

static bool synthesize_hevc_parameter_sets(
      const struct virgl_video_codec *codec,
      const struct virgl_h265_picture_desc *picture,
      uint32_t pps_id,
      struct byte_buffer *vps_nal,
      struct byte_buffer *sps_nal,
      struct byte_buffer *pps_nal)
{
   const struct virgl_h265_pps *pps = &picture->pps;
   const struct virgl_h265_sps *sps = &pps->sps;
   uint8_t profile_idc = hevc_profile_idc(codec->profile);
   uint8_t level_idc = codec->level
      ? (uint8_t)MIN2(codec->level <= 63 ? codec->level * 3 : codec->level, 255)
      : 153;
   uint8_t rbsp[8192];
   uint8_t nal[9216];
   struct bit_writer writer;
   uint32_t width = sps->pic_width_in_luma_samples
      ? sps->pic_width_in_luma_samples : codec->width;
   uint32_t height = sps->pic_height_in_luma_samples
      ? sps->pic_height_in_luma_samples : codec->height;
   uint32_t crop_right = width > codec->width ? width - codec->width : 0;
   uint32_t crop_bottom = height > codec->height ? height - codec->height : 0;
   uint32_t poc_bits = sps->log2_max_pic_order_cnt_lsb_minus4 + 4;

   if (!profile_idc || pps_id > 63 || sps->chroma_format_idc != 1 ||
       sps->separate_colour_plane_flag || sps->sps_max_dec_pic_buffering_minus1 > 15 ||
       !width || !height || width > 16384 || height > 16384 ||
       width < codec->width || height < codec->height ||
       sps->bit_depth_luma_minus8 != sps->bit_depth_chroma_minus8 ||
       sps->bit_depth_luma_minus8 > 2 || poc_bits > 16 ||
       (codec->profile == PIPE_VIDEO_PROFILE_HEVC_MAIN &&
        sps->bit_depth_luma_minus8 != 0) ||
       (codec->profile == PIPE_VIDEO_PROFILE_HEVC_MAIN_10 &&
        sps->bit_depth_luma_minus8 != 2) ||
       (crop_right & 1) || (crop_bottom & 1) ||
       picture->NumPocStCurrBefore > ARRAY_SIZE(picture->RefPicSetStCurrBefore) ||
       picture->NumPocStCurrAfter > ARRAY_SIZE(picture->RefPicSetStCurrAfter) ||
       picture->NumPocLtCurr > ARRAY_SIZE(picture->RefPicSetLtCurr) ||
       sps->num_short_term_ref_pic_sets > 64 || sps->num_long_term_ref_pics_sps > 32)
      return false;

   memset(&writer, 0, sizeof(writer));
   writer.data = rbsp;
   writer.capacity = sizeof(rbsp);
   bw_bits(&writer, 0, 4);  /* vps_video_parameter_set_id */
   bw_bit(&writer, 1);      /* vps_base_layer_internal_flag */
   bw_bit(&writer, 1);      /* vps_base_layer_available_flag */
   bw_bits(&writer, 0, 6);  /* vps_max_layers_minus1 */
   bw_bits(&writer, 6, 3);  /* vps_max_sub_layers_minus1 */
   bw_bit(&writer, 1);      /* vps_temporal_id_nesting_flag */
   bw_bits(&writer, 0xffff, 16);
   write_hevc_profile_tier_level(&writer, profile_idc, level_idc);
   bw_bit(&writer, 0); /* vps_sub_layer_ordering_info_present_flag */
   bw_ue(&writer, sps->sps_max_dec_pic_buffering_minus1);
   bw_ue(&writer, sps->sps_max_dec_pic_buffering_minus1);
   bw_ue(&writer, 0); /* vps_max_latency_increase_plus1 */
   bw_bits(&writer, 0, 6); /* vps_max_layer_id */
   bw_ue(&writer, 0);      /* vps_num_layer_sets_minus1 */
   bw_bit(&writer, 0);     /* vps_timing_info_present_flag */
   bw_bit(&writer, 0);     /* vps_extension_flag */
   if (!append_hevc_rbsp(vps_nal, 32, &writer, rbsp, nal, sizeof(nal)))
      return false;

   memset(&writer, 0, sizeof(writer));
   writer.data = rbsp;
   writer.capacity = sizeof(rbsp);
   bw_bits(&writer, 0, 4); /* sps_video_parameter_set_id */
   bw_bits(&writer, 6, 3); /* sps_max_sub_layers_minus1 */
   bw_bit(&writer, 1);     /* sps_temporal_id_nesting_flag */
   write_hevc_profile_tier_level(&writer, profile_idc, level_idc);
   bw_ue(&writer, 0); /* sps_seq_parameter_set_id */
   bw_ue(&writer, sps->chroma_format_idc);
   bw_ue(&writer, width);
   bw_ue(&writer, height);
   bw_bit(&writer, crop_right || crop_bottom);
   if (crop_right || crop_bottom) {
      bw_ue(&writer, 0);
      bw_ue(&writer, crop_right / 2);
      bw_ue(&writer, 0);
      bw_ue(&writer, crop_bottom / 2);
   }
   bw_ue(&writer, sps->bit_depth_luma_minus8);
   bw_ue(&writer, sps->bit_depth_chroma_minus8);
   bw_ue(&writer, sps->log2_max_pic_order_cnt_lsb_minus4);
   bw_bit(&writer, 0); /* sps_sub_layer_ordering_info_present_flag */
   bw_ue(&writer, sps->sps_max_dec_pic_buffering_minus1);
   bw_ue(&writer, sps->sps_max_dec_pic_buffering_minus1);
   bw_ue(&writer, 0); /* sps_max_latency_increase_plus1 */
   bw_ue(&writer, sps->log2_min_luma_coding_block_size_minus3);
   bw_ue(&writer, sps->log2_diff_max_min_luma_coding_block_size);
   bw_ue(&writer, sps->log2_min_transform_block_size_minus2);
   bw_ue(&writer, sps->log2_diff_max_min_transform_block_size);
   bw_ue(&writer, sps->max_transform_hierarchy_depth_inter);
   bw_ue(&writer, sps->max_transform_hierarchy_depth_intra);
   bw_bit(&writer, sps->scaling_list_enabled_flag);
   if (sps->scaling_list_enabled_flag) {
      bw_bit(&writer, 1); /* sps_scaling_list_data_present_flag */
      write_hevc_scaling_lists(&writer, sps);
   }
   bw_bit(&writer, sps->amp_enabled_flag);
   bw_bit(&writer, sps->sample_adaptive_offset_enabled_flag);
   bw_bit(&writer, sps->pcm_enabled_flag);
   if (sps->pcm_enabled_flag) {
      bw_bits(&writer, sps->pcm_sample_bit_depth_luma_minus1, 4);
      bw_bits(&writer, sps->pcm_sample_bit_depth_chroma_minus1, 4);
      bw_ue(&writer, sps->log2_min_pcm_luma_coding_block_size_minus3);
      bw_ue(&writer, sps->log2_diff_max_min_pcm_luma_coding_block_size);
      bw_bit(&writer, sps->pcm_loop_filter_disabled_flag);
   }
   /* Resolved RPS values are serialized in each rewritten slice. */
   bw_ue(&writer, 0); /* num_short_term_ref_pic_sets */
   bw_bit(&writer, 1); /* long_term_ref_pics_present_flag */
   bw_ue(&writer, 0); /* num_long_term_ref_pics_sps */
   bw_bit(&writer, sps->sps_temporal_mvp_enabled_flag);
   bw_bit(&writer, sps->strong_intra_smoothing_enabled_flag);
   bw_bit(&writer, 0); /* vui_parameters_present_flag */
   bw_bit(&writer, 0); /* sps_extension_present_flag */
   if (!append_hevc_rbsp(sps_nal, 33, &writer, rbsp, nal, sizeof(nal)))
      return false;

   memset(&writer, 0, sizeof(writer));
   writer.data = rbsp;
   writer.capacity = sizeof(rbsp);
   bw_ue(&writer, pps_id);
   bw_ue(&writer, 0); /* pps_seq_parameter_set_id */
   bw_bit(&writer, pps->dependent_slice_segments_enabled_flag);
   bw_bit(&writer, pps->output_flag_present_flag);
   bw_bits(&writer, pps->num_extra_slice_header_bits, 3);
   bw_bit(&writer, pps->sign_data_hiding_enabled_flag);
   bw_bit(&writer, pps->cabac_init_present_flag);
   bw_ue(&writer, pps->num_ref_idx_l0_default_active_minus1);
   bw_ue(&writer, pps->num_ref_idx_l1_default_active_minus1);
   bw_se(&writer, pps->init_qp_minus26);
   bw_bit(&writer, pps->constrained_intra_pred_flag);
   bw_bit(&writer, pps->transform_skip_enabled_flag);
   bw_bit(&writer, pps->cu_qp_delta_enabled_flag);
   if (pps->cu_qp_delta_enabled_flag)
      bw_ue(&writer, pps->diff_cu_qp_delta_depth);
   bw_se(&writer, pps->pps_cb_qp_offset);
   bw_se(&writer, pps->pps_cr_qp_offset);
   bw_bit(&writer, pps->pps_slice_chroma_qp_offsets_present_flag);
   bw_bit(&writer, pps->weighted_pred_flag);
   bw_bit(&writer, pps->weighted_bipred_flag);
   bw_bit(&writer, pps->transquant_bypass_enabled_flag);
   bw_bit(&writer, pps->tiles_enabled_flag);
   bw_bit(&writer, pps->entropy_coding_sync_enabled_flag);
   if (pps->tiles_enabled_flag) {
      if (pps->num_tile_columns_minus1 >= ARRAY_SIZE(pps->column_width_minus1) ||
          pps->num_tile_rows_minus1 >= ARRAY_SIZE(pps->row_height_minus1))
         return false;
      bw_ue(&writer, pps->num_tile_columns_minus1);
      bw_ue(&writer, pps->num_tile_rows_minus1);
      bw_bit(&writer, pps->uniform_spacing_flag);
      if (!pps->uniform_spacing_flag) {
         for (unsigned i = 0; i < pps->num_tile_columns_minus1; i++)
            bw_ue(&writer, pps->column_width_minus1[i]);
         for (unsigned i = 0; i < pps->num_tile_rows_minus1; i++)
            bw_ue(&writer, pps->row_height_minus1[i]);
      }
      bw_bit(&writer, pps->loop_filter_across_tiles_enabled_flag);
   }
   bw_bit(&writer, pps->pps_loop_filter_across_slices_enabled_flag);
   bw_bit(&writer, pps->deblocking_filter_control_present_flag);
   if (pps->deblocking_filter_control_present_flag) {
      bw_bit(&writer, pps->deblocking_filter_override_enabled_flag);
      bw_bit(&writer, pps->pps_deblocking_filter_disabled_flag);
      if (!pps->pps_deblocking_filter_disabled_flag) {
         bw_se(&writer, pps->pps_beta_offset_div2);
         bw_se(&writer, pps->pps_tc_offset_div2);
      }
   }
   bw_bit(&writer, 0); /* pps_scaling_list_data_present_flag */
   bw_bit(&writer, 1); /* slices preserve the effective reference list order */
   bw_ue(&writer, pps->log2_parallel_merge_level_minus2);
   bw_bit(&writer, pps->slice_segment_header_extension_present_flag);
   bw_bit(&writer, 0); /* pps_extension_present_flag */
   return append_hevc_rbsp(pps_nal, 34, &writer, rbsp, nal, sizeof(nal));
}

static uint32_t hevc_slice_pps_id(const uint8_t *nal, size_t size)
{
   struct bit_reader reader;
   uint8_t nal_type;
   if (size < 3)
      return UINT32_MAX;
   nal_type = (nal[0] >> 1) & 0x3f;
   memset(&reader, 0, sizeof(reader));
   reader.data = nal + 2;
   reader.size = size - 2;
   (void)br_bit(&reader); /* first_slice_segment_in_pic_flag */
   if (nal_type >= 16 && nal_type <= 23)
      (void)br_bit(&reader); /* no_output_of_prior_pics_flag */
   uint32_t id = br_ue(&reader);
   return reader.failed || id > 63 ? UINT32_MAX : id;
}

static uint32_t read_u32be(const uint8_t *data)
{
   return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
          ((uint32_t)data[2] << 8) | data[3];
}

static bool append_h26x_nal(bool hevc, struct byte_buffer *sample,
                            struct byte_buffer *vps, struct byte_buffer *sps,
                            struct byte_buffer *pps, const uint8_t *nal,
                            size_t size, uint32_t *pps_id)
{
   if (size < (hevc ? 2u : 1u) || size > UINT32_MAX || (nal[0] & 0x80) ||
       (hevc && ((nal[0] & 1) || (nal[1] & 0xf8) || !(nal[1] & 7))))
      return false;
   unsigned type = hevc ? (nal[0] >> 1) & 0x3f : nal[0] & 0x1f;
   struct byte_buffer *parameter = type == (hevc ? 33u : 7u) ? sps :
                                  type == (hevc ? 34u : 8u) ? pps :
                                  hevc && type == 32 ? vps : NULL;
   if (parameter) {
      /* One active parameter-set group per access unit. Never silently
       * combine one SPS with a different PPS from a later group. */
      if (parameter->size)
         return parameter->size == size && !memcmp(parameter->data, nal, size);
      return bb_append(parameter, nal, size);
   }
   if (hevc ? type <= 31 : type == 1 || type == 5) {
      uint32_t id = hevc ? hevc_slice_pps_id(nal, size) : slice_pps_id(nal, size);
      if (id == UINT32_MAX || (*pps_id != UINT32_MAX && *pps_id != id))
         return false;
      *pps_id = id;
   }
   return bb_append_u32be(sample, size) && bb_append(sample, nal, size);
}

/* Annex B, 4-byte length-prefixed, and one raw NAL per input buffer all share
 * the same validation. In the usual one-resource command, don't join/copy it. */
static bool build_h26x_sample(bool hevc, unsigned count, const void *const *buffers,
                              const unsigned *sizes, struct byte_buffer *sample,
                              struct byte_buffer *vps, struct byte_buffer *sps,
                              struct byte_buffer *pps, uint32_t *pps_id)
{
   struct byte_buffer joined = {0};
   bool result = false;
   *pps_id = UINT32_MAX;
   if (!count || !buffers || !sizes)
      return false;
   for (unsigned i = 0; i < count; i++)
      if (!buffers[i] || !sizes[i] || sizes[i] > MAX_CODED_BYTES ||
          (count > 1 && !bb_append(&joined, buffers[i], sizes[i])))
         goto out;
   const uint8_t *data = count == 1 ? buffers[0] : joined.data;
   size_t size = count == 1 ? sizes[0] : joined.size;
   /* Test lengths first: a valid AVCC length may itself contain 00 00 01. */
   size_t offset = 0;
   while (offset + 4 <= size) {
      uint32_t length = read_u32be(data + offset);
      if (length < (hevc ? 2u : 1u) || length > size - offset - 4)
         break;
      offset += 4 + length;
   }
   if (offset == size) {
      for (offset = 0; offset < size;) {
         uint32_t length = read_u32be(data + offset);
         offset += 4;
         if (!append_h26x_nal(hevc, sample, vps, sps, pps, data + offset, length, pps_id))
            goto out;
         offset += length;
      }
   } else {
      size_t prefix = 0, current = find_start_code(data, size, 0, &prefix);
      if (current != SIZE_MAX) {
         for (size_t i = 0; i < current; i++) if (data[i]) goto out;
         while (current != SIZE_MAX) {
            size_t start = current + prefix;
            size_t next = find_start_code(data, size, start, &prefix);
            size_t end = next == SIZE_MAX ? size : next;
            /* trailing_zero_8bits belong to Annex B only. Do not trim raw
             * or length-prefixed NAL payloads. */
            while (end > start && !data[end - 1]) end--;
            if (!append_h26x_nal(hevc, sample, vps, sps, pps,
                                 data + start, end - start, pps_id))
               goto out;
            current = next;
         }
      } else {
         for (unsigned i = 0; i < count; i++)
            if (!append_h26x_nal(hevc, sample, vps, sps, pps, buffers[i], sizes[i], pps_id))
               goto out;
      }
   }
   result = sample->size && *pps_id != UINT32_MAX;
out:
   bb_clear(&joined);
   return result;
}

static bool join_bitstream(unsigned num_buffers,
                           const void *const *buffers,
                           const unsigned *sizes,
                           struct byte_buffer *joined)
{
   if (!num_buffers || !buffers || !sizes)
      return false;
   for (unsigned i = 0; i < num_buffers; i++) {
      if (!buffers[i] || !sizes[i] || !bb_append(joined, buffers[i], sizes[i]))
         return false;
   }
   return joined->size != 0;
}

static bool jpeg_marker(struct byte_buffer *sample, uint8_t marker,
                        const void *payload, size_t payload_size)
{
   static const uint8_t prefix = 0xff;
   if (payload_size > UINT16_MAX - 2)
      return false;
   return bb_append_u8(sample, prefix) && bb_append_u8(sample, marker) &&
          bb_append_u16be(sample, (uint16_t)(payload_size + 2)) &&
          bb_append(sample, payload, payload_size);
}

static bool build_jpeg_sample(const struct virgl_mjpeg_picture_desc *picture,
                              unsigned num_buffers,
                              const void *const *buffers,
                              const unsigned *sizes,
                              struct byte_buffer *sample)
{
   const uint8_t soi[2] = {0xff, 0xd8};
   const uint8_t eoi[2] = {0xff, 0xd9};
   struct byte_buffer entropy = {0};
   uint8_t payload[512];
   bool quant_used[4] = {false};
   bool huffman_used[2] = {false};
   uint8_t components;
   bool ok = false;

   if (!join_bitstream(num_buffers, buffers, sizes, &entropy))
      goto out;
   if (entropy.size >= 2 && entropy.data[0] == 0xff &&
       entropy.data[1] == 0xd8) {
      ok = bb_append(sample, entropy.data, entropy.size);
      if (ok && (entropy.size < 2 ||
                 entropy.data[entropy.size - 2] != 0xff ||
                 entropy.data[entropy.size - 1] != 0xd9))
         ok = bb_append(sample, eoi, sizeof(eoi));
      goto out;
   }

   components = picture->picture_parameter.num_components;
   if (!components || components > 4 ||
       picture->slice_parameter.num_components != components ||
       !picture->picture_parameter.picture_width ||
       !picture->picture_parameter.picture_height)
      goto out;
   if (!bb_append(sample, soi, sizeof(soi)))
      goto out;
   {
      static const uint8_t jfif[] = {
         'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0,
      };
      if (!jpeg_marker(sample, 0xe0, jfif, sizeof(jfif)))
         goto out;
   }

   for (unsigned i = 0; i < components; i++) {
      unsigned table = picture->picture_parameter.components[i]
                          .quantiser_table_selector;
      if (table >= ARRAY_SIZE(quant_used))
         goto out;
      quant_used[table] = true;
   }
   for (unsigned table = 0; table < ARRAY_SIZE(quant_used); table++) {
      if (!quant_used[table])
         continue;
      payload[0] = table;
      memcpy(payload + 1,
             picture->quantization_table.quantiser_table[table], 64);
      if (!jpeg_marker(sample, 0xdb, payload, 65))
         goto out;
   }

   payload[0] = 8;
   payload[1] = picture->picture_parameter.picture_height >> 8;
   payload[2] = picture->picture_parameter.picture_height;
   payload[3] = picture->picture_parameter.picture_width >> 8;
   payload[4] = picture->picture_parameter.picture_width;
   payload[5] = components;
   for (unsigned i = 0; i < components; i++) {
      payload[6 + i * 3] =
         picture->picture_parameter.components[i].component_id;
      payload[7 + i * 3] =
         (picture->picture_parameter.components[i].h_sampling_factor << 4) |
          picture->picture_parameter.components[i].v_sampling_factor;
      payload[8 + i * 3] =
         picture->picture_parameter.components[i].quantiser_table_selector;
   }
   if (!jpeg_marker(sample, 0xc0, payload, 6 + components * 3))
      goto out;

   for (unsigned i = 0; i < components; i++) {
      unsigned dc = picture->slice_parameter.components[i].dc_table_selector;
      unsigned ac = picture->slice_parameter.components[i].ac_table_selector;
      if (dc >= ARRAY_SIZE(huffman_used) || ac >= ARRAY_SIZE(huffman_used))
         goto out;
      huffman_used[dc] = true;
      huffman_used[ac] = true;
   }
   for (unsigned table = 0; table < ARRAY_SIZE(huffman_used); table++) {
      size_t dc_count = 0;
      size_t ac_count = 0;
      size_t offset = 0;
      if (!huffman_used[table])
         continue;
      for (unsigned i = 0; i < 16; i++) {
         dc_count += picture->huffman_table.table[table].num_dc_codes[i];
         ac_count += picture->huffman_table.table[table].num_ac_codes[i];
      }
      if (dc_count > sizeof(picture->huffman_table.table[table].dc_values) ||
          ac_count > sizeof(picture->huffman_table.table[table].ac_values))
         goto out;
      payload[offset++] = table;
      memcpy(payload + offset,
             picture->huffman_table.table[table].num_dc_codes, 16);
      offset += 16;
      memcpy(payload + offset,
             picture->huffman_table.table[table].dc_values, dc_count);
      offset += dc_count;
      payload[offset++] = 0x10 | table;
      memcpy(payload + offset,
             picture->huffman_table.table[table].num_ac_codes, 16);
      offset += 16;
      memcpy(payload + offset,
             picture->huffman_table.table[table].ac_values, ac_count);
      offset += ac_count;
      if (!jpeg_marker(sample, 0xc4, payload, offset))
         goto out;
   }

   if (picture->slice_parameter.restart_interval) {
      payload[0] = picture->slice_parameter.restart_interval >> 8;
      payload[1] = picture->slice_parameter.restart_interval;
      if (!jpeg_marker(sample, 0xdd, payload, 2))
         goto out;
   }
   payload[0] = components;
   for (unsigned i = 0; i < components; i++) {
      payload[1 + i * 2] =
         picture->slice_parameter.components[i].component_selector;
      payload[2 + i * 2] =
         (picture->slice_parameter.components[i].dc_table_selector << 4) |
          picture->slice_parameter.components[i].ac_table_selector;
   }
   payload[1 + components * 2] = 0;
   payload[2 + components * 2] = 63;
   payload[3 + components * 2] = 0;
   if (!jpeg_marker(sample, 0xda, payload, 4 + components * 2) ||
       !bb_append(sample, entropy.data, entropy.size) ||
       !bb_append(sample, eoi, sizeof(eoi)))
      goto out;
   ok = true;

out:
   bb_clear(&entropy);
   return ok;
}

static bool build_vp9_configuration(const struct virgl_video_codec *codec,
                                    const struct virgl_vp9_picture_desc *picture,
                                    struct byte_buffer *configuration)
{
   uint8_t data[12] = {1, 0, 0, 0};
   uint8_t profile = picture->picture_parameter.profile;
   uint8_t bit_depth = picture->picture_parameter.bit_depth;
   unsigned subsampling;

   if (profile != (codec->profile == PIPE_VIDEO_PROFILE_VP9_PROFILE2 ? 2 : 0) ||
       bit_depth != (profile == 2 ? 10 : 8) ||
       !picture->picture_parameter.pic_fields.subsampling_x ||
       !picture->picture_parameter.pic_fields.subsampling_y)
      return false;
   if (picture->picture_parameter.pic_fields.subsampling_x &&
       picture->picture_parameter.pic_fields.subsampling_y)
      subsampling = 1; /* 4:2:0, chroma colocated with luma */
   else if (picture->picture_parameter.pic_fields.subsampling_x)
      subsampling = 2; /* 4:2:2 */
   else
      subsampling = 3; /* 4:4:4 */
   data[4] = profile;
   data[5] = (uint8_t)MIN2(codec->level, 255);
   data[6] = (bit_depth << 4) | (subsampling << 1);
   data[7] = 2; /* color primaries unspecified */
   data[8] = 2; /* transfer characteristics unspecified */
   data[9] = 2; /* matrix coefficients unspecified */
   return bb_append(configuration, data, sizeof(data));
}

static bool read_leb128(const uint8_t *data, size_t size,
                        size_t *value, size_t *length)
{
   size_t result = 0;
   unsigned shift = 0;
   for (size_t i = 0; i < size && i < 8; i++) {
      uint8_t byte = data[i];
      if (shift >= sizeof(size_t) * 8 ||
          (size_t)(byte & 0x7f) > (SIZE_MAX >> shift))
         return false;
      result |= (size_t)(byte & 0x7f) << shift;
      if (!(byte & 0x80)) {
         *value = result;
         *length = i + 1;
         return true;
      }
      shift += 7;
   }
   return false;
}

static bool find_av1_sequence_obu(const uint8_t *data, size_t size,
                                  const uint8_t **sequence,
                                  size_t *sequence_size)
{
   size_t offset = 0;
   while (offset < size) {
      size_t start = offset;
      uint8_t header = data[offset++];
      unsigned type;
      bool extension;
      bool has_size;
      size_t payload_size;
      size_t leb_size;
      if (header & 0x81)
         return false;
      type = (header >> 3) & 0x0f;
      extension = header & 0x04;
      has_size = header & 0x02;
      if (extension && offset >= size)
         return false;
      if (extension)
         offset++;
      if (has_size) {
         if (!read_leb128(data + offset, size - offset,
                          &payload_size, &leb_size))
            return false;
         offset += leb_size;
         if (payload_size > size - offset)
            return false;
      } else {
         payload_size = size - offset;
      }
      offset += payload_size;
      if (type == 1) {
         *sequence = data + start;
         *sequence_size = offset - start;
         return true;
      }
      if (!has_size)
         break;
   }
   return false;
}

static bool build_av1_configuration(const struct virgl_video_codec *codec,
                                    const struct virgl_av1_picture_desc *picture,
                                    const uint8_t *sample, size_t sample_size,
                                    struct byte_buffer *configuration)
{
   const uint8_t *sequence;
   size_t sequence_size;
   uint8_t header[4];
   uint8_t bit_depth_index = picture->picture_parameter.bit_depth_idx;
   bool mono = picture->picture_parameter.seq_info_fields.mono_chrome;
   bool subsampling_x = codec->chroma_format == PIPE_VIDEO_CHROMA_FORMAT_420 ||
                        codec->chroma_format == PIPE_VIDEO_CHROMA_FORMAT_422;
   bool subsampling_y = codec->chroma_format == PIPE_VIDEO_CHROMA_FORMAT_420;

   if (bit_depth_index > 1 || picture->picture_parameter.profile != 0 || mono ||
       !find_av1_sequence_obu(sample, sample_size, &sequence, &sequence_size))
      return false;
   header[0] = 0x81; /* marker and configurationVersion */
   header[1] = (picture->picture_parameter.profile << 5) |
               MIN2(codec->level, 31);
   header[2] = (bit_depth_index > 0 ? 0x40 : 0) |
               (bit_depth_index > 1 ? 0x20 : 0) |
               (mono ? 0x10 : 0) |
               (subsampling_x ? 0x08 : 0) |
               (subsampling_y ? 0x04 : 0);
   header[3] = 0;
   return bb_append(configuration, header, sizeof(header)) &&
          bb_append(configuration, sequence, sequence_size);
}

static bool same_bytes(const uint8_t *a, size_t a_size,
                       const uint8_t *b, size_t b_size)
{
   return a_size == b_size && (!a_size || !memcmp(a, b, a_size));
}

static void decoder_output(void *decompression_refcon,
                           void *source_frame_refcon,
                           OSStatus status,
                           VTDecodeInfoFlags info_flags,
                           CVImageBufferRef image_buffer,
                           CMTime presentation_time,
                           CMTime presentation_duration)
{
   struct decode_completion *frame = source_frame_refcon;
   (void)decompression_refcon;
   (void)info_flags;
   (void)presentation_time;
   (void)presentation_duration;
   if (status != noErr || !image_buffer ||
       (info_flags & kVTDecodeInfo_FrameDropped) ||
       CVPixelBufferGetWidth(image_buffer) < frame->width ||
       CVPixelBufferGetHeight(image_buffer) < frame->height) {
      virgl_error("VideoToolbox output rejected: status=%d flags=%u image=%zux%zu target=%ux%u\n",
         (int)status, (unsigned)info_flags,
         image_buffer ? CVPixelBufferGetWidth(image_buffer) : 0,
         image_buffer ? CVPixelBufferGetHeight(image_buffer) : 0,
         frame->width, frame->height);
      decode_complete(frame, NULL);
      return;
   }
   decode_complete(frame, image_buffer);
}

static OSType cv_pixel_format(enum pipe_format format)
{
   switch (format) {
   case PIPE_FORMAT_IYUV:
   case PIPE_FORMAT_YV12:
   case PIPE_FORMAT_NV12:
      return kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
   case PIPE_FORMAT_P010:
      return kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
   default:
      return 0;
   }
}

static CFDictionaryRef create_pixel_buffer_attributes(enum pipe_format format)
{
   CFMutableDictionaryRef attributes;
   CFMutableDictionaryRef iosurface;
   int32_t pixel_format = (int32_t)cv_pixel_format(format);
   CFNumberRef format_number;

   if (!pixel_format)
      return NULL;

   attributes = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
   iosurface = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
   format_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type,
                                  &pixel_format);
   if (!attributes || !iosurface || !format_number) {
      if (attributes) CFRelease(attributes);
      if (iosurface) CFRelease(iosurface);
      if (format_number) CFRelease(format_number);
      return NULL;
   }
   CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey,
                        iosurface);
   CFDictionarySetValue(attributes, kCVPixelBufferMetalCompatibilityKey,
                        kCFBooleanTrue);
   CFDictionarySetValue(attributes, kCVPixelBufferPixelFormatTypeKey,
                        format_number);
   CFRelease(iosurface);
   CFRelease(format_number);
   return attributes;
}

static void destroy_decoder(struct virgl_video_codec *codec)
{
   if (codec->decoder) {
      /* Drain only when destroying/replacing a session, never after each
       * picture. Submitted callbacks own their targets independently. */
      VTDecompressionSessionWaitForAsynchronousFrames(codec->decoder);
      VTDecompressionSessionInvalidate(codec->decoder);
      CFRelease(codec->decoder);
      codec->decoder = NULL;
   }
   if (codec->decode_format) {
      CFRelease(codec->decode_format);
      codec->decode_format = NULL;
   }
   free(codec->decode_config);
   free(codec->vps);
   free(codec->sps);
   free(codec->pps);
   codec->decode_config = NULL;
   codec->decode_config_size = 0;
   codec->vps = NULL;
   codec->vps_size = 0;
   codec->sps = NULL;
   codec->pps = NULL;
   codec->sps_size = 0;
   codec->pps_size = 0;
   codec->decode_codec_type = 0;
   codec->decode_surface_format = PIPE_FORMAT_NONE;
}

static bool copy_bytes(uint8_t **destination, size_t *destination_size,
                       const uint8_t *source, size_t source_size)
{
   uint8_t *copy;
   if (!source_size) {
      free(*destination);
      *destination = NULL;
      *destination_size = 0;
      return true;
   }
   copy = malloc(source_size);
   if (!copy)
      return false;
   memcpy(copy, source, source_size);
   free(*destination);
   *destination = copy;
   *destination_size = source_size;
   return true;
}

static bool create_decoder_session(struct virgl_video_codec *codec,
                                   CMVideoCodecType codec_type,
                                   enum pipe_format surface_format)
{
   VTDecompressionOutputCallbackRecord callback;
   CFMutableDictionaryRef specification;
   CFDictionaryRef attributes;
   OSStatus status;

   callback.decompressionOutputCallback = decoder_output;
   callback.decompressionOutputRefCon = NULL;
   specification = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
   attributes = create_pixel_buffer_attributes(surface_format);
   if (!specification || !attributes) {
      if (specification) CFRelease(specification);
      if (attributes) CFRelease(attributes);
      return false;
   }
   CFDictionarySetValue(
      specification,
      kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder,
      kCFBooleanTrue);
   status = VTDecompressionSessionCreate(
      kCFAllocatorDefault, codec->decode_format, specification, attributes,
      &callback, &codec->decoder);
   CFRelease(specification);
   CFRelease(attributes);
   if (status != noErr) {
      destroy_decoder(codec);
      return false;
   }
   codec->decode_codec_type = codec_type;
   codec->decode_surface_format = surface_format;
   return true;
}

/* CoreMedia owns the parameter sets in the format description. A compatible
 * update must preserve the decoder's DPB, not recreate the session per PPS. */
static bool accept_decoder_format(struct virgl_video_codec *codec,
                                   CMVideoFormatDescriptionRef format,
                                   enum pipe_format surface_format)
{
   CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions(format);
   if (dimensions.width <= 0 || dimensions.height <= 0 ||
       dimensions.width > 16384 || dimensions.height > 16384) {
      CFRelease(format);
      return false;
   }
   CMVideoCodecType type = CMFormatDescriptionGetMediaSubType(format);
   bool compatible = codec->decoder &&
      codec->decode_surface_format == surface_format &&
      VTDecompressionSessionCanAcceptFormatDescription(codec->decoder, format);
   if (!compatible) {
      if (codec->decoder) {
         VTDecompressionSessionInvalidate(codec->decoder);
         CFRelease(codec->decoder);
         codec->decoder = NULL;
      }
   }
   if (codec->decode_format)
      CFRelease(codec->decode_format);
   codec->decode_format = format;
   return compatible || create_decoder_session(codec, type, surface_format);
}

static bool ensure_h264_decoder(struct virgl_video_codec *codec,
                                const struct byte_buffer *sps,
                                const struct byte_buffer *pps)
{
   const uint8_t *sets[] = {sps->data, pps->data};
   size_t sizes[] = {sps->size, pps->size};
   CMVideoFormatDescriptionRef format = NULL;
   if (codec->decoder && same_bytes(codec->sps, codec->sps_size, sps->data, sps->size) &&
       same_bytes(codec->pps, codec->pps_size, pps->data, pps->size))
      return true;
   if (CMVideoFormatDescriptionCreateFromH264ParameterSets(
          kCFAllocatorDefault, 2, sets, sizes, 4, &format) != noErr)
      return false;
   return accept_decoder_format(codec, format, PIPE_FORMAT_NV12) &&
          copy_bytes(&codec->sps, &codec->sps_size, sps->data, sps->size) &&
          copy_bytes(&codec->pps, &codec->pps_size, pps->data, pps->size);
}

static bool ensure_hevc_decoder(struct virgl_video_codec *codec,
                                enum pipe_format surface_format,
                                const struct byte_buffer *vps,
                                const struct byte_buffer *sps,
                                const struct byte_buffer *pps)
{
   const uint8_t *sets[] = {vps->data, sps->data, pps->data};
   size_t sizes[] = {vps->size, sps->size, pps->size};
   CMVideoFormatDescriptionRef format = NULL;
   if (codec->decoder && codec->decode_surface_format == surface_format &&
       same_bytes(codec->vps, codec->vps_size, vps->data, vps->size) &&
       same_bytes(codec->sps, codec->sps_size, sps->data, sps->size) &&
       same_bytes(codec->pps, codec->pps_size, pps->data, pps->size))
      return true;
   if (CMVideoFormatDescriptionCreateFromHEVCParameterSets(
          kCFAllocatorDefault, 3, sets, sizes, 4, NULL, &format) != noErr)
      return false;
   return accept_decoder_format(codec, format, surface_format) &&
          copy_bytes(&codec->vps, &codec->vps_size, vps->data, vps->size) &&
          copy_bytes(&codec->sps, &codec->sps_size, sps->data, sps->size) &&
          copy_bytes(&codec->pps, &codec->pps_size, pps->data, pps->size);
}

static bool ensure_atom_decoder(struct virgl_video_codec *codec,
                                CMVideoCodecType codec_type,
                                enum pipe_format surface_format,
                                CFStringRef atom_name,
                                const uint8_t *configuration,
                                size_t configuration_size)
{
   if (!configuration || !configuration_size || configuration_size > MAX_CODED_BYTES)
      return false;
   if (codec->decoder && codec->decode_codec_type == codec_type &&
       codec->decode_surface_format == surface_format &&
       same_bytes(codec->decode_config, codec->decode_config_size,
                  configuration, configuration_size))
      return true;
   CFDataRef data = CFDataCreate(kCFAllocatorDefault, configuration, configuration_size);
   if (!data)
      return false;
   const void *atom_keys[] = {atom_name}, *atom_values[] = {data};
   CFDictionaryRef atoms = CFDictionaryCreate(kCFAllocatorDefault,
      atom_keys, atom_values, 1, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
   const void *keys[] = {kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms};
   const void *values[] = {atoms};
   CFDictionaryRef extensions = atoms ? CFDictionaryCreate(kCFAllocatorDefault,
      keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks) : NULL;
   CMVideoFormatDescriptionRef format = NULL;
   bool result = extensions && CMVideoFormatDescriptionCreate(
      kCFAllocatorDefault, codec_type, codec->width, codec->height, extensions, &format) == noErr;
   if (result)
      result = accept_decoder_format(codec, format, surface_format) &&
         copy_bytes(&codec->decode_config, &codec->decode_config_size,
                    CFDataGetBytePtr(data), configuration_size);
   if (extensions) CFRelease(extensions);
   if (atoms) CFRelease(atoms);
   CFRelease(data);
   return result;
}

static bool ensure_plain_decoder(struct virgl_video_codec *codec,
                                 CMVideoCodecType type, enum pipe_format surface_format)
{
   if (codec->decoder && codec->decode_codec_type == type &&
       codec->decode_surface_format == surface_format)
      return true;
   CMVideoFormatDescriptionRef format = NULL;
   if (CMVideoFormatDescriptionCreate(kCFAllocatorDefault, type,
       codec->width, codec->height, NULL, &format) != noErr)
      return false;
   return accept_decoder_format(codec, format, surface_format);
}

static bool append_annexb_nal(struct byte_buffer *output,
                              const uint8_t *nal, size_t size)
{
   static const uint8_t start_code[4] = {0, 0, 0, 1};
   return bb_append(output, start_code, sizeof(start_code)) &&
          bb_append(output, nal, size);
}

static bool encoded_sample_to_annexb(const struct virgl_video_codec *codec,
                                     CMSampleBufferRef sample,
                                     struct byte_buffer *output)
{
   CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample), contiguous = NULL;
   CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
   CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
   bool keyframe = true, result = false;
   int nal_length = 0;
   size_t count = 0, size, offset = 0;
   const char *bytes = NULL;
   if (!block || !format)
      return false;
   if (attachments && CFArrayGetCount(attachments)) {
      CFDictionaryRef attachment = CFArrayGetValueAtIndex(attachments, 0);
      keyframe = CFDictionaryGetValue(attachment, kCMSampleAttachmentKey_NotSync) != kCFBooleanTrue;
   }
   bool h264 = h264_profile_supported(codec->profile);
   OSStatus status = h264 ?
      CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, NULL, NULL, &count, &nal_length) :
      CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, 0, NULL, NULL, &count, &nal_length);
   if (status != noErr || (nal_length != 1 && nal_length != 2 && nal_length != 4))
      return false;
   if (keyframe) {
      for (size_t i = 0; i < count; i++) {
         const uint8_t *parameter = NULL;
         size_t length = 0;
         status = h264 ?
            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, i, &parameter, &length, NULL, NULL) :
            CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, i, &parameter, &length, NULL, NULL);
         if (status != noErr || !append_annexb_nal(output, parameter, length))
            return false;
      }
   }
   size = CMBlockBufferGetDataLength(block);
   if (!size || size > MAX_CODED_BYTES)
      return false;
   if (!CMBlockBufferIsRangeContiguous(block, 0, size)) {
      if (CMBlockBufferCreateContiguous(kCFAllocatorDefault, block, kCFAllocatorDefault,
             NULL, 0, size, 0, &contiguous) != noErr)
         return false;
      block = contiguous;
   }
   char *pointer = NULL;
   if (CMBlockBufferGetDataPointer(block, 0, NULL, NULL, &pointer) != noErr)
      goto out;
   bytes = pointer;
   while (offset + nal_length <= size) {
      uint32_t length = 0;
      for (int i = 0; i < nal_length; i++)
         length = (length << 8) | (uint8_t)bytes[offset++];
      if (!length || length > size - offset ||
          !append_annexb_nal(output, (const uint8_t *)bytes + offset, length))
         goto out;
      offset += length;
   }
   result = offset == size && output->size;
out:
   if (contiguous) CFRelease(contiguous);
   return result;
}

static void encoder_output(void *output_refcon,
                           void *source_frame_refcon,
                           OSStatus status,
                           VTEncodeInfoFlags info_flags,
                           CMSampleBufferRef sample_buffer)
{
   struct virgl_video_codec *codec = output_refcon;
   struct byte_buffer output = {0};
   (void)source_frame_refcon;
   (void)info_flags;
   codec->encode_status = status;
   free(codec->coded_data);
   codec->coded_data = NULL;
   codec->coded_size = 0;
   if (status != noErr || !sample_buffer ||
       !CMSampleBufferDataIsReady(sample_buffer) ||
       !encoded_sample_to_annexb(codec, sample_buffer, &output)) {
      bb_clear(&output);
      if (status == noErr)
         codec->encode_status = kVTVideoEncoderMalfunctionErr;
      return;
   }
   codec->coded_data = output.data;
   codec->coded_size = output.size;
}

static CFStringRef encoder_profile(enum pipe_video_profile profile)
{
   switch (profile) {
   case PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE:
   case PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE:
      return kVTProfileLevel_H264_Baseline_AutoLevel;
   case PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN:
      return kVTProfileLevel_H264_Main_AutoLevel;
   case PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH:
      return kVTProfileLevel_H264_High_AutoLevel;
   case PIPE_VIDEO_PROFILE_HEVC_MAIN:
   case PIPE_VIDEO_PROFILE_HEVC_MAIN_STILL:
      return kVTProfileLevel_HEVC_Main_AutoLevel;
   case PIPE_VIDEO_PROFILE_HEVC_MAIN_10:
      return kVTProfileLevel_HEVC_Main10_AutoLevel;
   default:
      return NULL;
   }
}

static CMVideoCodecType codec_type_for_profile(enum pipe_video_profile profile)
{
   if (h264_profile_supported(profile))
      return kCMVideoCodecType_H264;
   if (hevc_profile_supported(profile))
      return kCMVideoCodecType_HEVC;
   switch (profile) {
   case PIPE_VIDEO_PROFILE_JPEG_BASELINE:
      return kCMVideoCodecType_JPEG;
   case PIPE_VIDEO_PROFILE_VP9_PROFILE0:
   case PIPE_VIDEO_PROFILE_VP9_PROFILE2:
      return kCMVideoCodecType_VP9;
   case PIPE_VIDEO_PROFILE_AV1_MAIN:
      return kCMVideoCodecType_AV1;
   default:
      return 0;
   }
}

static enum pipe_format surface_format_for_profile(
      enum pipe_video_profile profile)
{
   switch (profile) {
   case PIPE_VIDEO_PROFILE_HEVC_MAIN_10:
   case PIPE_VIDEO_PROFILE_VP9_PROFILE2:
      return PIPE_FORMAT_P010;
   default:
      return PIPE_FORMAT_NV12;
   }
}

static bool create_encoder(struct virgl_video_codec *codec)
{
   CFStringRef profile = encoder_profile(codec->profile);
   CMVideoCodecType codec_type = codec_type_for_profile(codec->profile);
   CFMutableDictionaryRef specification;
   OSStatus status;
   if (!profile || !codec_type)
      return false;
   specification = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
   if (!specification)
      return false;
   CFDictionarySetValue(
      specification,
      kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder,
      kCFBooleanTrue);
   status = VTCompressionSessionCreate(
      kCFAllocatorDefault, codec->width, codec->height,
      codec_type, specification, NULL, NULL,
      encoder_output, codec, &codec->encoder);
   CFRelease(specification);
   if (status != noErr)
      return false;
   status = VTSessionSetProperty(codec->encoder,
                                 kVTCompressionPropertyKey_RealTime,
                                 kCFBooleanTrue);
   if (status == noErr)
      status = VTSessionSetProperty(
         codec->encoder, kVTCompressionPropertyKey_AllowFrameReordering,
         kCFBooleanFalse);
   if (status == noErr)
      status = VTSessionSetProperty(
         codec->encoder, kVTCompressionPropertyKey_ProfileLevel, profile);
   if (status == noErr)
      status = VTCompressionSessionPrepareToEncodeFrames(codec->encoder);
   if (status == noErr)
      return true;
   VTCompressionSessionInvalidate(codec->encoder);
   CFRelease(codec->encoder);
   codec->encoder = NULL;
   return false;
}

static bool h264_profile_supported(enum pipe_video_profile profile)
{
   return profile == PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE ||
          profile == PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE ||
          profile == PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN ||
          profile == PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH;
}

static bool hevc_profile_supported(enum pipe_video_profile profile)
{
   return profile == PIPE_VIDEO_PROFILE_HEVC_MAIN ||
          profile == PIPE_VIDEO_PROFILE_HEVC_MAIN_10 ||
          profile == PIPE_VIDEO_PROFILE_HEVC_MAIN_STILL;
}

static bool decode_profile_supported(enum pipe_video_profile profile)
{
   return h264_profile_supported(profile) || hevc_profile_supported(profile) ||
          profile == PIPE_VIDEO_PROFILE_JPEG_BASELINE ||
          profile == PIPE_VIDEO_PROFILE_VP9_PROFILE0 ||
          profile == PIPE_VIDEO_PROFILE_VP9_PROFILE2 ||
          profile == PIPE_VIDEO_PROFILE_AV1_MAIN;
}

static bool encode_profile_supported(enum pipe_video_profile profile)
{
   return h264_profile_supported(profile) ||
          profile == PIPE_VIDEO_PROFILE_HEVC_MAIN ||
          profile == PIPE_VIDEO_PROFILE_HEVC_MAIN_10;
}

static bool hardware_encoder_supported(enum pipe_video_profile profile)
{
   CFStringRef requested = encoder_profile(profile);
   if (!requested)
      return false;
   const void *keys[] = {kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder};
   const void *values[] = {kCFBooleanTrue};
   CFDictionaryRef specification = CFDictionaryCreate(kCFAllocatorDefault,
      keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
   if (!specification)
      return false;
   CFDictionaryRef properties = NULL;
   OSStatus status = VTCopySupportedPropertyDictionaryForEncoder(
      1920, 1080, codec_type_for_profile(profile), specification, NULL, &properties);
   CFRelease(specification);
   bool supported = false;
   if (status == noErr && properties) {
      CFDictionaryRef property = CFDictionaryGetValue(properties, kVTCompressionPropertyKey_ProfileLevel);
      CFArrayRef values = property ? CFDictionaryGetValue(property, kVTPropertySupportedValueListKey) : NULL;
      supported = values && CFArrayContainsValue(values, CFRangeMake(0, CFArrayGetCount(values)), requested);
   }
   if (properties) CFRelease(properties);
   return supported;
}

static bool make_pixel_buffer(enum pipe_format format,
                              uint32_t width, uint32_t height,
                              CVPixelBufferRef *pixel_buffer)
{
   OSType pixel_format = cv_pixel_format(format);
   CFDictionaryRef attributes = create_pixel_buffer_attributes(format);
   if (!pixel_format || !attributes)
      return false;
   CVReturn status = CVPixelBufferCreate(
      kCFAllocatorDefault, width, height, pixel_format,
      attributes, pixel_buffer);
   if (attributes)
      CFRelease(attributes);
   return status == kCVReturnSuccess;
}

static bool call_with_planes(struct virgl_video_codec *codec,
                             struct virgl_video_buffer *buffer,
                             uint32_t flags,
                             int (*callback)(struct virgl_video_codec *,
                                              const struct virgl_video_dma_buf *))
{
   struct virgl_video_dma_buf mapped;
   size_t planes;

   if (!buffer->pixel_buffer || !callback)
      return false;
   memset(&mapped, 0, sizeof(mapped));
   mapped.buf = buffer;
   mapped.native_frame = buffer->pixel_buffer;
   mapped.drm_format = buffer->format == PIPE_FORMAT_P010
      ? DRM_FORMAT_P010 : DRM_FORMAT_NV12;
   mapped.width = buffer->width;
   mapped.height = buffer->height;
   mapped.flags = flags;
   planes = CVPixelBufferGetPlaneCount(buffer->pixel_buffer);
   if (planes != 2 || CVPixelBufferGetPixelFormatType(buffer->pixel_buffer) !=
                       cv_pixel_format(buffer->format)) {
      return false;
   }
   mapped.num_planes = planes;
   for (unsigned i = 0; i < mapped.num_planes; i++) {
      size_t width = CVPixelBufferGetWidthOfPlane(buffer->pixel_buffer, i);
      size_t height = CVPixelBufferGetHeightOfPlane(buffer->pixel_buffer, i);
      size_t pitch = CVPixelBufferGetBytesPerRowOfPlane(buffer->pixel_buffer, i);
      if (!width || !height || width > UINT32_MAX || height > UINT32_MAX ||
          pitch > UINT32_MAX / height) {
         return false;
      }
      mapped.planes[i].fd = -1;
      if (buffer->format == PIPE_FORMAT_P010)
         mapped.planes[i].drm_format = i ? DRM_FORMAT_GR32 : DRM_FORMAT_R16;
      else
         mapped.planes[i].drm_format = i ? DRM_FORMAT_GR88 : DRM_FORMAT_R8;
      mapped.planes[i].width = width;
      mapped.planes[i].height = height;
      mapped.planes[i].pitch = pitch;
      mapped.planes[i].size = pitch * height;
   }
   return callback(codec, &mapped) == 0;
}

int virgl_video_init(int drm_fd, struct virgl_video_callbacks *callbacks,
                     unsigned int flags)
{
   (void)drm_fd;
   (void)flags;
   if (!callbacks)
      return -1;
   video_callbacks = callbacks;
   VTRegisterSupplementalVideoDecoderIfAvailable(kCMVideoCodecType_VP9);
   VTRegisterSupplementalVideoDecoderIfAvailable(kCMVideoCodecType_AV1);
   return 0;
}

void virgl_video_destroy(void)
{
   video_callbacks = NULL;
}

static void add_cap(union virgl_caps *caps,
                    enum pipe_video_profile profile,
                    enum pipe_video_entrypoint entrypoint,
                    enum pipe_format format,
                    uint32_t max_level,
                    uint32_t max_dimension)
{
   struct virgl_video_caps *cap;
   if (caps->v2.num_video_caps >= ARRAY_SIZE(caps->v2.video_caps))
      return;
   cap = &caps->v2.video_caps[caps->v2.num_video_caps++];
   memset(cap, 0, sizeof(*cap));
   cap->profile = profile;
   cap->entrypoint = entrypoint;
   cap->max_level = max_level;
   cap->max_width = max_dimension;
   cap->max_height = max_dimension;
   cap->prefered_format = format;
   cap->max_macroblocks = UINT16_MAX;
   cap->npot_texture = 1;
   cap->supports_progressive = 1;
   cap->max_temporal_layers = 1;
}

int virgl_video_fill_caps(union virgl_caps *caps)
{
   static const enum pipe_video_profile h264_profiles[] = {
      PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE,
      PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE,
      PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN,
      PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH,
   };
   static const enum pipe_video_profile hevc_profiles[] = {
      PIPE_VIDEO_PROFILE_HEVC_MAIN,
      PIPE_VIDEO_PROFILE_HEVC_MAIN_10,
   };
   if (!caps || !video_callbacks)
      return -1;
   caps->v2.num_video_caps = 0;
   for (unsigned i = 0; i < ARRAY_SIZE(h264_profiles); i++) {
      if (VTIsHardwareDecodeSupported(kCMVideoCodecType_H264))
         add_cap(caps, h264_profiles[i], PIPE_VIDEO_ENTRYPOINT_BITSTREAM,
                 PIPE_FORMAT_NV12, 52, 4096);
      if (hardware_encoder_supported(h264_profiles[i]))
         add_cap(caps, h264_profiles[i], PIPE_VIDEO_ENTRYPOINT_ENCODE,
                 PIPE_FORMAT_NV12, 52, 4096);
   }
   for (unsigned i = 0; i < ARRAY_SIZE(hevc_profiles); i++) {
      enum pipe_format format = surface_format_for_profile(hevc_profiles[i]);
      if (VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC))
         add_cap(caps, hevc_profiles[i], PIPE_VIDEO_ENTRYPOINT_BITSTREAM,
                 format, 62, 8192);
      if (hardware_encoder_supported(hevc_profiles[i]))
         add_cap(caps, hevc_profiles[i], PIPE_VIDEO_ENTRYPOINT_ENCODE,
                 format, 62, 8192);
   }
   if (VTIsHardwareDecodeSupported(kCMVideoCodecType_JPEG))
      add_cap(caps, PIPE_VIDEO_PROFILE_JPEG_BASELINE,
              PIPE_VIDEO_ENTRYPOINT_BITSTREAM, PIPE_FORMAT_NV12, 0, 16384);
   if (VTIsHardwareDecodeSupported(kCMVideoCodecType_VP9)) {
      add_cap(caps, PIPE_VIDEO_PROFILE_VP9_PROFILE0,
              PIPE_VIDEO_ENTRYPOINT_BITSTREAM, PIPE_FORMAT_NV12, 62, 8192);
      add_cap(caps, PIPE_VIDEO_PROFILE_VP9_PROFILE2,
              PIPE_VIDEO_ENTRYPOINT_BITSTREAM, PIPE_FORMAT_P010, 62, 8192);
   }
   /* AV1 descriptor-to-OBU reconstruction is available, but general guest
    * support also needs VT hidden-frame output and distinct film-grain target
    * delivery. Do not advertise those unverified lifecycle semantics. */
   return 0;
}

struct virgl_video_codec *virgl_video_create_codec(
      const struct virgl_video_create_codec_args *args)
{
   struct virgl_video_codec *codec;
   if (!args ||
       ((args->entrypoint == PIPE_VIDEO_ENTRYPOINT_BITSTREAM &&
         !decode_profile_supported(args->profile)) ||
        (args->entrypoint == PIPE_VIDEO_ENTRYPOINT_ENCODE &&
         !encode_profile_supported(args->profile))) ||
       args->chroma_format != PIPE_VIDEO_CHROMA_FORMAT_420 ||
       !args->width || !args->height || args->width > 16384 ||
       args->height > 16384 ||
       (args->entrypoint != PIPE_VIDEO_ENTRYPOINT_BITSTREAM &&
        args->entrypoint != PIPE_VIDEO_ENTRYPOINT_ENCODE))
      return NULL;
   if (args->entrypoint == PIPE_VIDEO_ENTRYPOINT_BITSTREAM &&
       !VTIsHardwareDecodeSupported(codec_type_for_profile(args->profile)))
      return NULL;
   codec = calloc(1, sizeof(*codec));
   if (!codec)
      return NULL;
   codec->profile = args->profile;
   codec->entrypoint = args->entrypoint;
   codec->chroma_format = args->chroma_format;
   codec->level = args->level;
   codec->width = args->width;
   codec->height = args->height;
   codec->max_references = args->max_references;
   codec->opaque = args->opaque;
   codec->next_timestamp = kCMTimeZero;
   if (args->entrypoint == PIPE_VIDEO_ENTRYPOINT_ENCODE &&
       !create_encoder(codec)) {
      free(codec);
      return NULL;
   }
   return codec;
}

void virgl_video_destroy_codec(struct virgl_video_codec *codec)
{
   if (!codec)
      return;
   destroy_decoder(codec);
   if (codec->encoder) {
      VTCompressionSessionInvalidate(codec->encoder);
      CFRelease(codec->encoder);
   }
   free(codec->coded_data);
   bb_clear(&codec->frame_data);
   free(codec);
}

enum pipe_video_profile virgl_video_codec_profile(
      const struct virgl_video_codec *codec)
{
   return codec ? codec->profile : PIPE_VIDEO_PROFILE_UNKNOWN;
}

void *virgl_video_codec_opaque_data(struct virgl_video_codec *codec)
{
   return codec ? codec->opaque : NULL;
}

struct virgl_video_buffer *virgl_video_create_buffer(
      const struct virgl_video_create_buffer_args *args)
{
   struct virgl_video_buffer *buffer;
   if (!args || (args->format != PIPE_FORMAT_NV12 && args->format != PIPE_FORMAT_IYUV &&
                 args->format != PIPE_FORMAT_YV12 && args->format != PIPE_FORMAT_P010) || !args->width ||
       !args->height || args->width > 16384 || args->height > 16384 ||
       args->interlaced || !next_buffer_id)
      return NULL;
   buffer = calloc(1, sizeof(*buffer));
   if (!buffer)
      return NULL;
   buffer->format = args->format;
   buffer->width = args->width;
   buffer->height = args->height;
   buffer->id = next_buffer_id++;
   buffer->opaque = args->opaque;
   return buffer;
}

void virgl_video_destroy_buffer(struct virgl_video_buffer *buffer)
{
   if (!buffer)
      return;
   if (buffer->pixel_buffer)
      CVPixelBufferRelease(buffer->pixel_buffer);
   free(buffer);
}

uint32_t virgl_video_buffer_id(const struct virgl_video_buffer *buffer)
{
   return buffer ? buffer->id : 0;
}

void *virgl_video_buffer_opaque_data(struct virgl_video_buffer *buffer)
{
   return buffer ? buffer->opaque : NULL;
}

int virgl_video_begin_frame(struct virgl_video_codec *codec,
                            struct virgl_video_buffer *target)
{
   if (!codec || !target || codec->frame_buffer_id ||
       codec->width != target->width || codec->height != target->height)
      return -1;
   codec->frame_ready = false;
   codec->frame_failed = false;
   target->status = kVTVideoDecoderBadDataErr;
   if (codec->entrypoint == PIPE_VIDEO_ENTRYPOINT_ENCODE) {
      if (cv_pixel_format(target->format) != cv_pixel_format(surface_format_for_profile(codec->profile)) ||
          (!target->pixel_buffer && !make_pixel_buffer(
              target->format, target->width, target->height, &target->pixel_buffer)) ||
          !video_callbacks || !call_with_planes(
              codec, target, VIRGL_VIDEO_DMABUF_WRITE_ONLY,
              video_callbacks->encode_upload_picture))
         return -1;
   }
   codec->frame_buffer_id = target->id;
   return 0;
}

static int submit_decode_sample(struct virgl_video_codec *codec,
                                struct virgl_video_buffer *target,
                                struct byte_buffer *sample,
                                virgl_video_decode_done completion, void *data)
{
   CMBlockBufferRef block = NULL;
   CMSampleBufferRef sample_buffer = NULL;
   size_t sample_size;
   OSStatus status = -1;

   if (!codec->decoder || !codec->decode_format || !sample->size)
      return -1;

   sample_size = sample->size;
   status = CMBlockBufferCreateWithMemoryBlock(
      kCFAllocatorDefault, sample->data, sample_size, kCFAllocatorMalloc, NULL,
      0, sample_size, 0, &block);
   if (status != noErr)
      goto out;
   /* Transfer ownership of the malloc buffer to CoreMedia. The session may
    * retain it; no temporary borrowed pointer and no extra bitstream copy. */
   memset(sample, 0, sizeof(*sample));
   status = CMSampleBufferCreateReady(
      kCFAllocatorDefault, block, codec->decode_format, 1, 0, NULL, 1,
      &sample_size, &sample_buffer);
   if (status != noErr)
      goto out;
   struct decode_completion *frame = calloc(1, sizeof(*frame));
   if (!frame) { status = -1; goto out; }
   frame->width = target->width; frame->height = target->height;
   frame->callback = completion; frame->data = data;
   status = VTDecompressionSessionDecodeFrame(
      codec->decoder, sample_buffer, kVTDecodeFrame_EnableAsynchronousDecompression,
      frame, NULL);
   if (status != noErr)
      virgl_error("VideoToolbox asynchronous decode rejected: status=%d profile=%u\n",
                  (int)status, codec->profile);
   /* Apple's contract: an error means no callback; success guarantees one
    * (possibly inline). Each sample here contains exactly one picture. Only
    * that callback, or synchronous rejection, owns and frees frameRefcon. */
   if (status != noErr)
      decode_complete(frame, NULL);
   /* Once VT was called, completion owns the outcome (including rejection). */
   status = noErr;

out:
   if (sample_buffer) CFRelease(sample_buffer);
   if (block) CFRelease(block);
   if (status != noErr)
      virgl_error("VideoToolbox decode failed: status=%d profile=%u size=%ux%u\n",
                  (int)status, codec->profile, codec->width, codec->height);
   return status == noErr ? 0 : -1;
}

/* Decode once per begin/end frame, not once per slice-data command. */
static int decode_frame(struct virgl_video_codec *codec,
                         struct virgl_video_buffer *target,
                         virgl_video_decode_done completion, void *data)
{
   const union virgl_picture_desc *desc = &codec->frame_desc;
   unsigned num_buffers = 1;
   const void *buffers[] = {codec->frame_data.data};
   unsigned sizes[] = {codec->frame_data.size};
   struct byte_buffer sample = {0};
   struct byte_buffer configuration = {0};
   struct byte_buffer vps = {0};
   struct byte_buffer sps = {0};
   struct byte_buffer pps = {0};
   enum pipe_format surface_format;
   uint32_t pps_id;
   int result = -1;
   struct virgl_av1_rewrite_state next_av1;
   bool rewritten_av1 = false;

   surface_format = surface_format_for_profile(codec->profile);
   if (codec->profile == PIPE_VIDEO_PROFILE_AV1_MAIN)
      surface_format = target->format == PIPE_FORMAT_P010 ? PIPE_FORMAT_P010 : PIPE_FORMAT_NV12;
   if (cv_pixel_format(target->format) != cv_pixel_format(surface_format))
      goto out;

   if (h264_profile_supported(codec->profile)) {
      if (!build_h26x_sample(false, num_buffers, buffers, sizes, &sample, &vps, &sps, &pps,
                             &pps_id))
         goto out;
      if (sps.size && pps.size)
         codec->parameter_sets_in_band = true;
      if (codec->parameter_sets_in_band &&
          ((!sps.size && !bb_append(&sps, codec->sps, codec->sps_size)) ||
           (!pps.size && !bb_append(&pps, codec->pps, codec->pps_size))))
         goto out;
      if (!sps.size || !pps.size) {
         bb_clear(&sps);
         bb_clear(&pps);
         if (!synthesize_h264_parameter_sets(
                codec, &desc->h264, pps_id == UINT32_MAX ? 0 : pps_id,
                &sps, &pps)) {
            virgl_error("VideoToolbox H.264 stream has no usable SPS/PPS\n");
            goto out;
         }
      }
      if (!ensure_h264_decoder(codec, &sps, &pps))
         goto out;
   } else if (hevc_profile_supported(codec->profile)) {
      if (!build_h26x_sample(true, num_buffers, buffers, sizes, &sample, &vps, &sps,
                             &pps, &pps_id))
         goto out;
      if (vps.size && sps.size && pps.size)
         codec->parameter_sets_in_band = true;
      if (codec->parameter_sets_in_band &&
          ((!vps.size && !bb_append(&vps, codec->vps, codec->vps_size)) ||
           (!sps.size && !bb_append(&sps, codec->sps, codec->sps_size)) ||
           (!pps.size && !bb_append(&pps, codec->pps, codec->pps_size))))
         goto out;
      if (!vps.size || !sps.size || !pps.size) {
         bb_clear(&vps);
         bb_clear(&sps);
         bb_clear(&pps);
         if (!synthesize_hevc_parameter_sets(
                codec, &desc->h265, pps_id == UINT32_MAX ? 0 : pps_id,
                &vps, &sps, &pps)) {
            virgl_error("VideoToolbox HEVC stream has no usable VPS/SPS/PPS\n");
            goto out;
         }
         struct byte_buffer normalized = {0};
         size_t pos = 0;
         bool complete = true;
         while (pos + 4 <= sample.size) {
            unsigned n = read_u32be(sample.data + pos);
            pos += 4;
            if (n < 2 || n > sample.size - pos) { complete = false; break; }
            unsigned type = (sample.data[pos] >> 1) & 63;
            struct virgl_video_bitstream slice = {0};
            bool ok = type > 31 || virgl_video_hevc_rewrite(&desc->h265, sample.data + pos, n, &slice);
            if (ok) ok = bb_append_u32be(&normalized, slice.data ? slice.size : n) &&
               bb_append(&normalized, slice.data ? slice.data : sample.data + pos, slice.data ? slice.size : n);
            free(slice.data);
            if (!ok) { complete = false; break; }
            pos += n;
         }
         if (!complete || pos != sample.size) { bb_clear(&normalized); goto out; }
         bb_clear(&sample);
         sample = normalized;
      }
      if (!ensure_hevc_decoder(codec, surface_format, &vps, &sps, &pps))
         goto out;
   } else if (codec->profile == PIPE_VIDEO_PROFILE_JPEG_BASELINE) {
      if (!build_jpeg_sample(&desc->mjpeg, num_buffers, buffers, sizes,
                             &sample) ||
          !ensure_plain_decoder(codec, kCMVideoCodecType_JPEG,
                                PIPE_FORMAT_NV12))
         goto out;
   } else if (codec->profile == PIPE_VIDEO_PROFILE_VP9_PROFILE0 ||
              codec->profile == PIPE_VIDEO_PROFILE_VP9_PROFILE2) {
      if (!join_bitstream(num_buffers, buffers, sizes, &sample) ||
          !build_vp9_configuration(codec, &desc->vp9, &configuration) ||
          !ensure_atom_decoder(codec, kCMVideoCodecType_VP9, surface_format,
                               CFSTR("vpcC"), configuration.data,
                               configuration.size))
         goto out;
   } else if (codec->profile == PIPE_VIDEO_PROFILE_AV1_MAIN) {
      const uint8_t *config_data;
      size_t config_size;
      if (desc->av1.film_grain_target && desc->av1.film_grain_target != target->id) {
         virgl_error("VideoToolbox AV1 needs separate reference/display target delivery\n");
         goto out;
      }
      if (desc->av1.picture_parameter.profile != 0 ||
          desc->av1.picture_parameter.bit_depth_idx > 1 ||
          desc->av1.picture_parameter.seq_info_fields.mono_chrome ||
          cv_pixel_format(target->format) != cv_pixel_format(desc->av1.picture_parameter.bit_depth_idx ?
                              PIPE_FORMAT_P010 : PIPE_FORMAT_NV12))
         goto out;
      if (desc->av1.slice_parameter.slice_count) {
         struct virgl_video_bitstream rewritten = {0}, config = {0};
         if (!virgl_video_av1_rewrite(&codec->av1_state, &next_av1, &desc->av1,
              target->id, codec->frame_data.data, codec->frame_data.size,
              &rewritten, &config)) goto out;
         sample = (struct byte_buffer){.data = rewritten.data, .size = rewritten.size};
         configuration = (struct byte_buffer){.data = config.data, .size = config.size};
         rewritten_av1 = true;
      } else if (!join_bitstream(num_buffers, buffers, sizes, &sample))
         goto out;
      if (rewritten_av1 || build_av1_configuration(codec, &desc->av1, sample.data, sample.size,
                                  &configuration)) {
         config_data = configuration.data;
         config_size = configuration.size;
      } else {
         config_data = codec->decode_config;
         config_size = codec->decode_config_size;
      }
      if (!ensure_atom_decoder(codec, kCMVideoCodecType_AV1, surface_format,
                               CFSTR("av1C"), config_data, config_size))
         goto out;
   } else {
      goto out;
   }
   result = submit_decode_sample(codec, target, &sample, completion, data);
   if (!result && rewritten_av1) codec->av1_state = next_av1;

out:
   bb_clear(&sample);
   bb_clear(&configuration);
   bb_clear(&vps);
   bb_clear(&sps);
   bb_clear(&pps);
   return result;
}

int virgl_video_decode_bitstream(struct virgl_video_codec *codec,
                                 struct virgl_video_buffer *target,
                                 const union virgl_picture_desc *desc,
                                 unsigned num_buffers, const void *const *buffers,
                                 const unsigned *sizes)
{
   if (!codec || !target || codec->frame_buffer_id != target->id ||
       codec->entrypoint != PIPE_VIDEO_ENTRYPOINT_BITSTREAM)
      return -1;
   if (!desc || desc->base.profile != codec->profile || !num_buffers ||
       !buffers || !sizes || codec->frame_failed)
      goto fail;
   for (unsigned i = 0; i < num_buffers; i++)
      if (!buffers[i] || !sizes[i] ||
          !bb_append(&codec->frame_data, buffers[i], sizes[i]))
         goto fail;
   codec->frame_desc = *desc;
   return 0;
fail:
   codec->frame_failed = true;
   return -1;
}

int virgl_video_encode_bitstream(struct virgl_video_codec *codec,
                                 struct virgl_video_buffer *source,
                                 const union virgl_picture_desc *desc)
{
   CFMutableDictionaryRef properties = NULL;
   CFNumberRef bitrate = NULL;
   CFNumberRef frame_rate = NULL;
   uint32_t target_bitrate;
   uint32_t frame_rate_num;
   uint32_t frame_rate_den;
   uint8_t picture_type;
   double fps = 60.0;
   CMTime timestamp;
   CMTime duration = CMTimeMake(1, 60);
   OSStatus status;

   if (!codec || !source || !desc || !codec->encoder ||
       codec->frame_buffer_id != source->id ||
       codec->entrypoint != PIPE_VIDEO_ENTRYPOINT_ENCODE ||
       desc->base.profile != codec->profile)
      return -1;
   if (cv_pixel_format(source->format) != cv_pixel_format(surface_format_for_profile(codec->profile)))
      return -1;
   if (h264_profile_supported(codec->profile)) {
      target_bitrate = desc->h264_enc.rate_ctrl[0].target_bitrate;
      frame_rate_num = desc->h264_enc.rate_ctrl[0].frame_rate_num;
      frame_rate_den = desc->h264_enc.rate_ctrl[0].frame_rate_den;
      picture_type = desc->h264_enc.picture_type;
   } else {
      target_bitrate = desc->h265_enc.rc.target_bitrate;
      frame_rate_num = desc->h265_enc.rc.frame_rate_num;
      frame_rate_den = desc->h265_enc.rc.frame_rate_den;
      picture_type = desc->h265_enc.picture_type;
   }
   if ((frame_rate_num == 0) != (frame_rate_den == 0) ||
       frame_rate_num > INT32_MAX || frame_rate_den > INT32_MAX)
      return -1;
   status = noErr;
   if (target_bitrate) {
      int32_t value = (int32_t)MIN2(target_bitrate, INT32_MAX);
      bitrate = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
      if (bitrate)
         status = VTSessionSetProperty(codec->encoder,
                              kVTCompressionPropertyKey_AverageBitRate,
                              bitrate);
      else
         status = kVTAllocationFailedErr;
      if (status != noErr)
         goto out_encode;
   }
   if (frame_rate_num && frame_rate_den) {
      fps = (double)frame_rate_num / frame_rate_den;
      frame_rate = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &fps);
      if (frame_rate)
         status = VTSessionSetProperty(codec->encoder,
                              kVTCompressionPropertyKey_ExpectedFrameRate,
                              frame_rate);
      else
         status = kVTAllocationFailedErr;
      if (status != noErr)
         goto out_encode;
      duration = CMTimeMake(
         frame_rate_den, (int32_t)MIN2(frame_rate_num, INT32_MAX));
   }
   if (picture_type == PIPE_H2645_ENC_PICTURE_TYPE_IDR ||
       picture_type == PIPE_H2645_ENC_PICTURE_TYPE_I) {
      properties = CFDictionaryCreateMutable(
         kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
         &kCFTypeDictionaryValueCallBacks);
      if (!properties) {
         status = kVTAllocationFailedErr;
         goto out_encode;
      }
      if (properties)
         CFDictionarySetValue(properties, kVTEncodeFrameOptionKey_ForceKeyFrame,
                              kCFBooleanTrue);
   }
   timestamp = codec->next_timestamp;
   codec->next_timestamp = CMTimeAdd(timestamp, duration);
   codec->encode_status = -1;
   free(codec->coded_data);
   codec->coded_data = NULL;
   codec->coded_size = 0;
   status = VTCompressionSessionEncodeFrame(
      codec->encoder, source->pixel_buffer, timestamp, duration,
      properties, source, NULL);
   if (status == noErr)
      status = VTCompressionSessionCompleteFrames(codec->encoder,
                                                   kCMTimeInvalid);
   if (status == noErr)
      status = codec->encode_status;
out_encode:
   if (properties) CFRelease(properties);
   if (bitrate) CFRelease(bitrate);
   if (frame_rate) CFRelease(frame_rate);
   codec->frame_ready = status == noErr && codec->coded_data && codec->coded_size;
   return codec->frame_ready ? 0 : -1;
}

int virgl_video_end_frame_async(struct virgl_video_codec *codec,
                               struct virgl_video_buffer *target,
                               virgl_video_decode_done completion, void *data)
{
   if (!codec || !target || !completion || codec->frame_buffer_id != target->id ||
       codec->entrypoint != PIPE_VIDEO_ENTRYPOINT_BITSTREAM)
      return -1;
   int result = !codec->frame_failed && codec->frame_data.size
      ? decode_frame(codec, target, completion, data) : -1;
   bb_clear(&codec->frame_data);
   codec->frame_buffer_id = 0;
   return result;
}

int virgl_video_end_frame(struct virgl_video_codec *codec,
                          struct virgl_video_buffer *target)
{
   if (!codec || !target || codec->frame_buffer_id != target->id ||
       codec->entrypoint != PIPE_VIDEO_ENTRYPOINT_ENCODE)
      return -1;
   codec->frame_buffer_id = 0;
   bool ready = codec->frame_ready;
   codec->frame_ready = false;
   if (!ready || !video_callbacks)
      return -1;
   unsigned size = codec->coded_size;
   const void *data = codec->coded_data;
   if (codec->coded_size > UINT_MAX || !video_callbacks->encode_completed)
      return -1;
   return video_callbacks->encode_completed(codec, NULL, NULL, 1, &data, &size);
}
