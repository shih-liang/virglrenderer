/*
 * Copyright 2025 Turing Software, LLC
 * SPDX-License-Identifier: MIT
 */
#ifndef VIRGL_METAL_H
#define VIRGL_METAL_H

#include "virglrenderer.h"

typedef void *MTLDevice_id;
typedef void *MTLTexture_id;
typedef void *MTLHeap_id;
typedef void *MTLBuffer_id;

struct vrend_metal_texture_description {
   unsigned width;
   unsigned height;
   unsigned stride;
   unsigned offset;
   unsigned bind;
   unsigned usage;
   uint32_t format;
};

bool virgl_metal_create_texture(MTLDevice_id device,
                                const struct vrend_metal_texture_description *desc,
                                MTLTexture_id *tex);

bool virgl_metal_create_texture_from_heap(MTLHeap_id heap,
                                          const struct vrend_metal_texture_description *desc,
                                          MTLTexture_id *tex);

/* Create a zero-copy texture view over the exact MTLBuffer exported by Venus. */
bool virgl_metal_create_texture_from_buffer(
   MTLBuffer_id buffer,
   const struct vrend_metal_texture_description *desc,
   MTLTexture_id *tex);

/* Validate and retain an exact texture exported from the source VkImage. */
bool virgl_metal_retain_texture(MTLTexture_id source,
                                          const struct vrend_metal_texture_description *desc,
                                          MTLTexture_id *tex);

uint64_t virgl_metal_heap_size(MTLHeap_id heap);
uint64_t virgl_metal_buffer_size(MTLBuffer_id buffer);
void *virgl_metal_buffer_contents(MTLBuffer_id buffer);

void virgl_metal_release_texture(MTLTexture_id tex);
void virgl_metal_release_buffer(MTLBuffer_id buffer);

#endif
