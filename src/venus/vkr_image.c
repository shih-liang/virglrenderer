/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_image.h"

#include "vkr_image_gen.h"
#include "vkr_device_memory.h"
#include "vkr_instance.h"
#include "vkr_physical_device.h"

static void
vkr_image_track_memory(struct vkr_image *img,
					   struct vkr_device_memory *mem,
					   VkDeviceSize offset)
{
	vkr_image_release(img);
	img->bound_memory = mem;
	img->memory_offset = offset;
	list_addtail(&img->memory_head, &mem->bound_images);
	vkr_device_memory_publish_metal_texture(mem);
}

void
vkr_image_release(struct vkr_image *img)
{
	if (!img || !img->bound_memory)
		return;

	struct vkr_device_memory *mem = img->bound_memory;
	list_del(&img->memory_head);
	list_inithead(&img->memory_head);
	img->bound_memory = NULL;
	img->memory_offset = 0;
	vkr_device_memory_publish_metal_texture(mem);
}

static void
vkr_image_fix_create_info(struct vkr_device *dev,
                          VkImageCreateInfo *pCreateInfo)
{
   VkExternalMemoryImageCreateInfo *ext_create_info;

   ext_create_info = vkr_find_struct(
            pCreateInfo, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
   if (ext_create_info) {
      const VkExternalMemoryHandleTypeFlags fd_types =
         VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT |
         VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
      const VkExternalMemoryHandleTypeFlags guest_types =
         ext_create_info->handleTypes;
      if (dev->physical_device->is_metal_export_supported &&
          (guest_types & fd_types)) {
         /* Linux fd handle types are guest transport semantics. MoltenVK uses
          * an MTLHeap for the corresponding ordinary external allocation. */
         ext_create_info->handleTypes &= ~fd_types;
         ext_create_info->handleTypes |=
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT;
      }
   }
}

static VkResult
vkr_image_fix_drm_format(struct vkr_device *dev,
                         VkImageCreateInfo *pCreateInfo)
{
   const VkImageDrmFormatModifierExplicitCreateInfoEXT* drm_format_info =
            vkr_find_struct(pCreateInfo, VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT);
   const VkImageDrmFormatModifierListCreateInfoEXT* drm_format_list =
            vkr_find_struct(pCreateInfo, VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT);

   if (pCreateInfo->tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT || (!drm_format_info && !drm_format_list)) {
      return VK_SUCCESS;
   }

   if (drm_format_info && drm_format_info->drmFormatModifier == DRM_FORMAT_MOD_LINEAR) {
      pCreateInfo->tiling = VK_IMAGE_TILING_LINEAR;
      return VK_SUCCESS;
   }

   for (int i = 0; drm_format_list && i < drm_format_list->drmFormatModifierCount; i++) {
      if (drm_format_list->pDrmFormatModifiers[i] == DRM_FORMAT_MOD_LINEAR) {
         pCreateInfo->tiling = VK_IMAGE_TILING_LINEAR;
         return VK_SUCCESS;
      }
   }

   vkr_log("only DRM_FORMAT_MOD_LINEAR is supported");
   return VK_ERROR_FORMAT_NOT_SUPPORTED;
}

static VkResult
vkr_image_emulate_drm_format_modifier_properties(UNUSED struct vkr_device *dev,
                                                 UNUSED VkImage image,
                                                 VkImageDrmFormatModifierPropertiesEXT* pProperties)
{
   pProperties->drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
   return VK_SUCCESS;
}

static void
vkr_dispatch_vkCreateImage(struct vn_dispatch_context *dispatch,
                           struct vn_command_vkCreateImage *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   VkImageCreateInfo *create_info = (VkImageCreateInfo *)args->pCreateInfo;
   const bool drm_format_modifier_emulated =
      !dev->physical_device->EXT_image_drm_format_modifier &&
      create_info->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;

   const VkExternalMemoryImageCreateInfo *guest_external_info =
      vkr_find_struct(create_info->pNext,
                      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
   const VkExternalMemoryHandleTypeFlags guest_external_handle_types =
      guest_external_info ? guest_external_info->handleTypes : 0;
   /* if host does not natively support dmabuf we need to patch create info */
   if (dev->physical_device->is_dma_buf_emulated) {
      vkr_image_fix_create_info(dev, create_info);
   }

   if (!dev->physical_device->EXT_image_drm_format_modifier) {
      args->ret = vkr_image_fix_drm_format(dev, create_info);
      if (args->ret != VK_SUCCESS) {
         return;
      }
   }

   /* XXX If VkExternalMemoryImageCreateInfo is chained by the app, all is
    * good.  If it is not chained, we might still bind an external memory to
    * the image, because vkr_dispatch_vkAllocateMemory makes any HOST_VISIBLE
    * memory external.  That is a spec violation.
    *
    * The discussions in vkr_dispatch_vkCreateBuffer are applicable to both
    * buffers and images.  Additionally, drivers usually use
    * VkExternalMemoryImageCreateInfo to pick a well-defined image layout for
    * interoperability with foreign queues.  However, a well-defined layout
    * might not exist for some images.  When it does, it might still require a
    * dedicated allocation or might have a degraded performance.
    *
    * On the other hand, binding an external memory to an image created
    * without VkExternalMemoryImageCreateInfo usually works.  Yes, it will
    * explode if the external memory is accessed by foreign queues due to the
    * lack of a well-defined image layout.  But we never end up in that
    * situation because the app does not consider the memory external.
    */

   struct vkr_image *img = vkr_image_create_and_add(ctx, args);
   /* pImage now contains the driver VkImage handle, not a vkr_image pointer.
    * Keep renderer-only metadata on the object returned by the generator. */
	if (img && args->ret == VK_SUCCESS && args->pCreateInfo) {
		list_inithead(&img->memory_head);
      img->width = args->pCreateInfo->extent.width;
      img->height = args->pCreateInfo->extent.height;
      img->format = args->pCreateInfo->format;
      img->usage = args->pCreateInfo->usage;
      img->external_handle_types = guest_external_handle_types;
      img->drm_format_modifier_emulated = drm_format_modifier_emulated;
   }
}

static void
vkr_dispatch_vkDestroyImage(struct vn_dispatch_context *dispatch,
                            struct vn_command_vkDestroyImage *args)
{
	struct vkr_image *img = vkr_image_from_handle(args->image);
	vkr_image_release(img);
	vkr_image_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetImageMemoryRequirements(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageMemoryRequirements *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageMemoryRequirements_args_handle(args);
   vk->GetImageMemoryRequirements(args->device, args->image, args->pMemoryRequirements);
}

static void
vkr_dispatch_vkGetImageMemoryRequirements2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageMemoryRequirements2 *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageMemoryRequirements2_args_handle(args);
   vk->GetImageMemoryRequirements2(args->device, args->pInfo, args->pMemoryRequirements);
}

static void
vkr_dispatch_vkGetImageSparseMemoryRequirements(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageSparseMemoryRequirements *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageSparseMemoryRequirements_args_handle(args);
   vk->GetImageSparseMemoryRequirements(args->device, args->image,
                                        args->pSparseMemoryRequirementCount,
                                        args->pSparseMemoryRequirements);
}

static void
vkr_dispatch_vkGetImageSparseMemoryRequirements2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageSparseMemoryRequirements2 *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageSparseMemoryRequirements2_args_handle(args);
   vk->GetImageSparseMemoryRequirements2(args->device, args->pInfo,
                                         args->pSparseMemoryRequirementCount,
                                         args->pSparseMemoryRequirements);
}

static void
vkr_dispatch_vkBindImageMemory(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkBindImageMemory *args)
{
	struct vkr_device *dev = vkr_device_from_handle(args->device);
	struct vn_device_proc_table *vk = &dev->proc_table;
	struct vkr_image *img = vkr_image_from_handle(args->image);
	struct vkr_device_memory *mem = vkr_device_memory_from_handle(args->memory);
	const VkDeviceSize offset = args->memoryOffset;

	vn_replace_vkBindImageMemory_args_handle(args);
	args->ret =
		vk->BindImageMemory(args->device, args->image, args->memory, args->memoryOffset);
	if (args->ret == VK_SUCCESS && img && mem)
		vkr_image_track_memory(img, mem, offset);
}

static void
vkr_dispatch_vkBindImageMemory2(struct vn_dispatch_context *dispatch,
                                struct vn_command_vkBindImageMemory2 *args)
{
	struct vkr_device *dev = vkr_device_from_handle(args->device);
	struct vn_device_proc_table *vk = &dev->proc_table;
	struct image_memory_binding {
		struct vkr_image *img;
		struct vkr_device_memory *mem;
		VkDeviceSize offset;
	};
	struct image_memory_binding *bindings =
		calloc(args->bindInfoCount, sizeof(*bindings));
	if (args->bindInfoCount && !bindings) {
		args->ret = VK_ERROR_OUT_OF_HOST_MEMORY;
		return;
	}
	for (uint32_t i = 0; i < args->bindInfoCount; i++) {
		bindings[i].img = vkr_image_from_handle(args->pBindInfos[i].image);
		bindings[i].mem = vkr_device_memory_from_handle(args->pBindInfos[i].memory);
		bindings[i].offset = args->pBindInfos[i].memoryOffset;
	}

	vn_replace_vkBindImageMemory2_args_handle(args);
	args->ret = vk->BindImageMemory2(args->device, args->bindInfoCount, args->pBindInfos);
	if (args->ret == VK_SUCCESS) {
		for (uint32_t i = 0; i < args->bindInfoCount; i++) {
			if (bindings[i].img && bindings[i].mem)
				vkr_image_track_memory(bindings[i].img, bindings[i].mem,
									   bindings[i].offset);
		}
	}
	free(bindings);
}

static void
vkr_dispatch_vkGetImageSubresourceLayout(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageSubresourceLayout *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;
   const struct vkr_image *img = vkr_image_from_handle(args->image);

   if (img && img->drm_format_modifier_emulated) {
      VkImageSubresource *subresource = (VkImageSubresource *)args->pSubresource;
      if (subresource->aspectMask == VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT)
         subresource->aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   }

   vn_replace_vkGetImageSubresourceLayout_args_handle(args);
   vk->GetImageSubresourceLayout(args->device, args->image, args->pSubresource,
                                 args->pLayout);
}

static void
vkr_dispatch_vkGetImageSubresourceLayout2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageSubresourceLayout2 *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;
   const struct vkr_image *img = vkr_image_from_handle(args->image);

   if (img && img->drm_format_modifier_emulated) {
      VkImageSubresource2 *subresource = (VkImageSubresource2 *)args->pSubresource;
      if (subresource->imageSubresource.aspectMask ==
          VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT)
         subresource->imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   }

   vn_replace_vkGetImageSubresourceLayout2_args_handle(args);
   vk->GetImageSubresourceLayout2(args->device, args->image, args->pSubresource,
                                  args->pLayout);
}

static void
vkr_dispatch_vkGetDeviceImageSubresourceLayout(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetDeviceImageSubresourceLayout *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   /* if host does not natively support dmabuf we need to patch create info */
   if (dev->physical_device->is_dma_buf_emulated) {
      vkr_image_fix_create_info(dev,
                                (VkImageCreateInfo *)args->pInfo->pCreateInfo);
   }

   if (!dev->physical_device->EXT_image_drm_format_modifier &&
       args->pInfo->pCreateInfo->tiling == VK_IMAGE_TILING_LINEAR) {
      VkImageSubresource2 *subresource =
         (VkImageSubresource2 *)args->pInfo->pSubresource;
      if (subresource->imageSubresource.aspectMask ==
          VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT)
         subresource->imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   }

   vn_replace_vkGetDeviceImageSubresourceLayout_args_handle(args);
   vk->GetDeviceImageSubresourceLayout(args->device, args->pInfo, args->pLayout);
}

static void
vkr_dispatch_vkGetImageDrmFormatModifierPropertiesEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageDrmFormatModifierPropertiesEXT *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageDrmFormatModifierPropertiesEXT_args_handle(args);

   if (dev->physical_device->EXT_image_drm_format_modifier) {
      args->ret = vk->GetImageDrmFormatModifierPropertiesEXT(args->device, args->image,
                                                            args->pProperties);
   } else {
      args->ret = vkr_image_emulate_drm_format_modifier_properties(dev, args->image,
                                                                   args->pProperties);
   }
}

static void
vkr_dispatch_vkCreateImageView(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCreateImageView *args)
{
   vkr_image_view_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroyImageView(struct vn_dispatch_context *dispatch,
                                struct vn_command_vkDestroyImageView *args)
{
   vkr_image_view_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkCreateSampler(struct vn_dispatch_context *dispatch,
                             struct vn_command_vkCreateSampler *args)
{
   vkr_sampler_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroySampler(struct vn_dispatch_context *dispatch,
                              struct vn_command_vkDestroySampler *args)
{
   vkr_sampler_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkCreateSamplerYcbcrConversion(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkCreateSamplerYcbcrConversion *args)
{
   vkr_sampler_ycbcr_conversion_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroySamplerYcbcrConversion(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkDestroySamplerYcbcrConversion *args)
{
   vkr_sampler_ycbcr_conversion_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetDeviceImageMemoryRequirements(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetDeviceImageMemoryRequirements *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   /* if host does not natively support dmabuf we need to patch create info */
   if (dev->physical_device->is_dma_buf_emulated) {
      vkr_image_fix_create_info(dev,
                                (VkImageCreateInfo *)args->pInfo->pCreateInfo);
   }

   vn_replace_vkGetDeviceImageMemoryRequirements_args_handle(args);
   vk->GetDeviceImageMemoryRequirements(args->device, args->pInfo,
                                        args->pMemoryRequirements);
}

static void
vkr_dispatch_vkGetDeviceImageSparseMemoryRequirements(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetDeviceImageSparseMemoryRequirements *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   /* if host does not natively support dmabuf we need to patch create info */
   if (dev->physical_device->is_dma_buf_emulated) {
      vkr_image_fix_create_info(dev,
                                (VkImageCreateInfo *)args->pInfo->pCreateInfo);
   }

   vn_replace_vkGetDeviceImageSparseMemoryRequirements_args_handle(args);
   vk->GetDeviceImageSparseMemoryRequirements(args->device, args->pInfo,
                                              args->pSparseMemoryRequirementCount,
                                              args->pSparseMemoryRequirements);
}

void
vkr_context_init_image_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateImage = vkr_dispatch_vkCreateImage;
   dispatch->dispatch_vkDestroyImage = vkr_dispatch_vkDestroyImage;
   dispatch->dispatch_vkGetImageMemoryRequirements =
      vkr_dispatch_vkGetImageMemoryRequirements;
   dispatch->dispatch_vkGetImageMemoryRequirements2 =
      vkr_dispatch_vkGetImageMemoryRequirements2;
   dispatch->dispatch_vkGetImageSparseMemoryRequirements =
      vkr_dispatch_vkGetImageSparseMemoryRequirements;
   dispatch->dispatch_vkGetImageSparseMemoryRequirements2 =
      vkr_dispatch_vkGetImageSparseMemoryRequirements2;
   dispatch->dispatch_vkBindImageMemory = vkr_dispatch_vkBindImageMemory;
   dispatch->dispatch_vkBindImageMemory2 = vkr_dispatch_vkBindImageMemory2;
   dispatch->dispatch_vkGetImageSubresourceLayout =
      vkr_dispatch_vkGetImageSubresourceLayout;
   dispatch->dispatch_vkGetImageSubresourceLayout2 =
      vkr_dispatch_vkGetImageSubresourceLayout2;
   dispatch->dispatch_vkGetDeviceImageSubresourceLayout =
      vkr_dispatch_vkGetDeviceImageSubresourceLayout;

   dispatch->dispatch_vkGetImageDrmFormatModifierPropertiesEXT =
      vkr_dispatch_vkGetImageDrmFormatModifierPropertiesEXT;
   dispatch->dispatch_vkGetDeviceImageMemoryRequirements =
      vkr_dispatch_vkGetDeviceImageMemoryRequirements;
   dispatch->dispatch_vkGetDeviceImageSparseMemoryRequirements =
      vkr_dispatch_vkGetDeviceImageSparseMemoryRequirements;
}

void
vkr_context_init_image_view_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateImageView = vkr_dispatch_vkCreateImageView;
   dispatch->dispatch_vkDestroyImageView = vkr_dispatch_vkDestroyImageView;
}

void
vkr_context_init_sampler_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateSampler = vkr_dispatch_vkCreateSampler;
   dispatch->dispatch_vkDestroySampler = vkr_dispatch_vkDestroySampler;
}

void
vkr_context_init_sampler_ycbcr_conversion_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateSamplerYcbcrConversion =
      vkr_dispatch_vkCreateSamplerYcbcrConversion;
   dispatch->dispatch_vkDestroySamplerYcbcrConversion =
      vkr_dispatch_vkDestroySamplerYcbcrConversion;
}
