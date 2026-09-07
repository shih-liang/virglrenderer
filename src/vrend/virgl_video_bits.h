/* SPDX-License-Identifier: MIT
 * Copyright 2026 NativePipe contributors
 */
#ifndef VIRGL_VIDEO_BITS_H
#define VIRGL_VIDEO_BITS_H

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "virgl_video_bitstream.h"

#define VIDEO_BITSTREAM_LIMIT (128u * 1024u * 1024u)

struct video_bits {
   uint8_t *data;
   size_t capacity, count;
   bool failed;
};

static inline void vb_put(struct video_bits *b, uint32_t value, unsigned n)
{
   if (n > 32 || (n < 32 && (value >> n)) ||
       b->count > b->capacity * 8 || n > b->capacity * 8 - b->count) {
      b->failed = true;
      return;
   }
   while (n--) {
      size_t byte = b->count / 8;
      unsigned bit = 7 - b->count % 8;
      if (bit == 7) b->data[byte] = 0;
      b->data[byte] |= ((value >> n) & 1) << bit;
      b->count++;
   }
}

static inline void vb_signed(struct video_bits *b, int32_t value, unsigned n)
{
   if (!n || n > 31 || value < -(1 << (n - 1)) || value >= (1 << (n - 1))) {
      b->failed = true;
      return;
   }
   vb_put(b, (uint32_t)value & ((1u << n) - 1), n);
}

static inline void vb_ue(struct video_bits *b, uint32_t value)
{
   if (value == UINT32_MAX) { b->failed = true; return; }
   uint32_t code = value + 1;
   unsigned n = 0;
   for (uint32_t v = code; v > 1; v >>= 1) n++;
   vb_put(b, 0, n); vb_put(b, code, n + 1);
}

static inline unsigned vb_log2(unsigned value)
{
   unsigned n = 0;
   if (value) value--;
   while (value) { value >>= 1; n++; }
   return n;
}

static inline void vb_align(struct video_bits *b, bool trailing_one)
{
   if (trailing_one) vb_put(b, 1, 1);
   while (!b->failed && b->count % 8) vb_put(b, 0, 1);
}

static inline void vb_ns(struct video_bits *b, unsigned value, unsigned n)
{
   if (!n || value >= n) { b->failed = true; return; }
   unsigned width = vb_log2(n);
   unsigned m = (1u << width) - n;
   if (!width) return;
   if (value < m) vb_put(b, value, width - 1);
   else {
      unsigned v = value + m;
      vb_put(b, v >> 1, width - 1);
      vb_put(b, v & 1, 1);
   }
}

static inline void vb_increment(struct video_bits *b, unsigned value,
                                unsigned lo, unsigned hi)
{
   if (value < lo || value > hi) { b->failed = true; return; }
   for (unsigned i = lo; i < hi; i++) {
      vb_put(b, value > i, 1);
      if (value == i) break;
   }
}

static inline bool vs_append(struct virgl_video_bitstream *s,
                             const void *data, size_t n)
{
   if (!n) return true;
   if (!data || n > VIDEO_BITSTREAM_LIMIT || s->size > VIDEO_BITSTREAM_LIMIT - n)
      return false;
   uint8_t *p = realloc(s->data, s->size + n);
   if (!p) return false;
   s->data = p;
   memcpy(p + s->size, data, n);
   s->size += n;
   return true;
}

static inline void vs_clear(struct virgl_video_bitstream *s)
{
   free(s->data);
   memset(s, 0, sizeof(*s));
}

static inline bool vs_obu(struct virgl_video_bitstream *s, unsigned type,
                          const uint8_t *data, size_t n)
{
   uint8_t header[9];
   size_t count = 1, value = n;
   header[0] = (type << 3) | 2;
   do {
      if (count == sizeof(header)) return false;
      header[count] = value & 127;
      value >>= 7;
      if (value) header[count] |= 128;
      count++;
   } while (value);
   return vs_append(s, header, count) && vs_append(s, data, n);
}

#endif
