/* SPDX-License-Identifier: MIT
 * Copyright 2026 NativePipe contributors
 */
#ifndef VIRGL_VIDEO_BITSTREAM_H
#define VIRGL_VIDEO_BITSTREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "virgl_video_hw.h"

/* Owned malloc buffers. On failure outputs remain empty. No pixel decoding or
 * additional runtime codec library is involved in these transformations. */
struct virgl_video_bitstream {
   uint8_t *data;
   size_t size;
};

struct virgl_av1_reference {
   uint32_t id;
   uint8_t order_hint;
   int32_t gm[7][6];
};

struct virgl_av1_rewrite_state {
   struct virgl_av1_reference refs[8];
   uint8_t sequence[128];
   size_t sequence_size;
};

/* Commit next_state only after the downstream decoder has accepted the frame.
 * The descriptor's resource IDs, not stream frame_id syntax, identify pictures. */
bool virgl_video_av1_rewrite(const struct virgl_av1_rewrite_state *state,
                            struct virgl_av1_rewrite_state *next_state,
                            const struct virgl_av1_picture_desc *picture,
                            uint32_t target_id,
                            const uint8_t *data, size_t size,
                            struct virgl_video_bitstream *sample,
                            struct virgl_video_bitstream *configuration);

/* One raw HEVC VCL NAL. The associated SPS has no stored RPS, permits
 * long-term references, and the PPS permits reference-list modification.
 * Rewrites header syntax only; escaped CABAC bytes are copied unchanged. */
bool virgl_video_hevc_rewrite(const struct virgl_h265_picture_desc *picture,
                             const uint8_t *nal, size_t size,
                             struct virgl_video_bitstream *output);

#endif
