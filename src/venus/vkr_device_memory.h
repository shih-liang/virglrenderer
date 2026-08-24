/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_DEVICE_MEMORY_H
#define VKR_DEVICE_MEMORY_H

#include "vkr_common.h"

struct gbm_bo;

struct vkr_device_memory {
   struct vkr_object base;

   struct vkr_device *device;

   bool might_export;
   bool metal_buffer_exportable;

   uint32_t property_flags;
   uint32_t valid_fd_types;

   /* gbm bo backing non-external mappable memory */
   struct gbm_bo *gbm_bo;

   /* udmabuf backing non-external mappable memory */
   int udmabuf_fd;

   uint64_t allocation_size;
   uint32_t memory_type_index;

	bool exported;
	/* Shared states, rather than numeric resource ids, connect bound images to
	 * scanout without creation-order or id-reuse races. Import and export stay
	 * distinct because Vulkan permits compatible external memory to be
	 * re-exported as a different resource. */
	struct virgl_resource_metal_texture_state *import_metal_state;
	struct virgl_resource_metal_texture_state *export_metal_state;

	/* Images actually bound to this allocation. An exported Metal texture is
	 * available only when this list identifies one unambiguous external image. */
	struct list_head bound_images;
};
VKR_DEFINE_OBJECT_CAST(device_memory, VK_OBJECT_TYPE_DEVICE_MEMORY, VkDeviceMemory)

void
vkr_context_init_device_memory_dispatch(struct vkr_context *ctx);

void
vkr_device_memory_release(struct vkr_device_memory *mem);

void
vkr_device_memory_publish_metal_texture(struct vkr_device_memory *mem);

void
vkr_device_memory_publish_metal_buffer(struct vkr_device_memory *mem);

bool
vkr_device_memory_export_blob(struct vkr_device_memory *mem,
                              uint64_t blob_size,
                              uint32_t blob_flags,
                              struct virgl_context_blob *out_blob);

#endif /* VKR_DEVICE_MEMORY_H */
