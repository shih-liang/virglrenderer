/* SPDX-License-Identifier: MIT
 * Real Metal writer + MoltenVK reader, through the Venus dispatch handlers.
 * The producer completes first; Vulkan has no video-specific synchronization. */
#include <Metal/Metal.h>
#include <CoreVideo/CoreVideo.h>
#include "venus/vkr_device.h"
#include "venus/vkr_physical_device.h"
#include "venus/vkr_command_buffer.h"
#include "venus/vkr_image.h"
#include "venus/vkr_queue.h"
#include "venus/venus-protocol/vn_protocol_renderer_queue.h"
#include "venus/venus-protocol/vn_protocol_renderer_command_buffer.h"
#include "virgl_video_metal.h"

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)
#define VK(x) do { VkResult r = (x); CHECK(r == VK_SUCCESS); if (r != VK_SUCCESS) { fprintf(stderr, "VkResult=%d\n", r); exit(2); } } while (0)
static void retired(uint32_t ctx, uint32_t ring, uint64_t id)
{ (void)ctx; (void)ring; (void)id; CHECK(false); /* this test does not request a renderer fence */ }
static uint32_t memory_type(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags flags)
{
   VkPhysicalDeviceMemoryProperties props; vkGetPhysicalDeviceMemoryProperties(physical, &props);
   for (uint32_t i = 0; i < props.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
   fprintf(stderr, "No memory type\n"); exit(2);
}

int main(void) { @autoreleasepool {
   VkInstance instance;
   const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
   const VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app}; /* Static ICD, not the Vulkan loader. */
   VK(vkCreateInstance(&instance_info, NULL, &instance));
   VkPhysicalDevice physical; uint32_t count = 1;
   VK(vkEnumeratePhysicalDevices(instance, &count, &physical));
   float priority = 1;
   const VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority};
   const char *extensions[] = {VK_EXT_METAL_OBJECTS_EXTENSION_NAME,
      VK_EXT_EXTERNAL_MEMORY_METAL_EXTENSION_NAME, "VK_KHR_portability_subset"};
   VkPhysicalDeviceSynchronization2Features sync2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
      .synchronization2 = VK_TRUE};
   const VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &sync2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
      .enabledExtensionCount = 3, .ppEnabledExtensionNames = extensions};
   struct vkr_device dev = {.base = {.type = VK_OBJECT_TYPE_DEVICE, .id = 5}};
   VK(vkCreateDevice(physical, &device_info, NULL, &dev.base.handle.device));
   VkDevice device = dev.base.handle.device;
#define PROC(name) dev.proc_table.name = vk##name
   PROC(CreateFence); PROC(DestroyFence); PROC(ResetFences); PROC(WaitForFences);
   PROC(QueueSubmit); PROC(QueueSubmit2);
   PROC(CmdPipelineBarrier); PROC(CmdPipelineBarrier2); PROC(BeginCommandBuffer);
   PROC(ResetCommandBuffer); PROC(ResetCommandPool); PROC(CmdExecuteCommands);
#undef PROC
   struct vkr_physical_device physical_wrapper = {0};
   dev.physical_device = &physical_wrapper;
   mtx_init(&dev.free_sync_mutex, mtx_plain); list_inithead(&dev.free_syncs);
   struct vkr_context ctx = {.retire_fence = retired};
   ctx.dispatch.data = &ctx;
   vkr_context_init_queue_dispatch(&ctx); vkr_context_init_command_buffer_dispatch(&ctx);
   vkr_context_init_command_pool_dispatch(&ctx);
   VkQueue native_queue; vkGetDeviceQueue(device, 0, 0, &native_queue);
   struct vkr_queue *queue = vkr_queue_create(&ctx, &dev, 0, 0, 0, native_queue);
   CHECK(queue); queue->base.id = 1;
   VkExportMetalDeviceInfoEXT metal_device = {.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT};
   VkExportMetalObjectsInfoEXT metal_objects = {.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, .pNext = &metal_device};
   PFN_vkExportMetalObjectsEXT export_metal = (void *)vkGetDeviceProcAddr(device, "vkExportMetalObjectsEXT");
   export_metal(device, &metal_objects);
   id<MTLDevice> mtl = metal_device.mtlDevice;
   void *textures[2];
   for (unsigned p = 0; p < 2; p++) {
      MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
         (p ? MTLPixelFormatRG8Unorm : MTLPixelFormatR8Unorm) width:32 >> p height:32 >> p mipmapped:NO];
      d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
      textures[p] = [mtl newTextureWithDescriptor:d]; CHECK(textures[p]);
   }
   struct virgl_video_metal *video = virgl_video_metal_create(textures[0]); CHECK(video);
   CVPixelBufferRef pixels = NULL;
   NSDictionary *attrs = @{(id)kCVPixelBufferMetalCompatibilityKey:@YES, (id)kCVPixelBufferIOSurfacePropertiesKey:@{}};
   CHECK(CVPixelBufferCreate(NULL, 32, 32, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
      (CFDictionaryRef)attrs, &pixels) == kCVReturnSuccess);
   struct vkr_image image_wrapper = {.base = {.type = VK_OBJECT_TYPE_IMAGE, .id = 2}};
   VkExternalMemoryImageCreateInfo external_image = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLTEXTURE_BIT_EXT};
   VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &external_image,
      .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8_UNORM, .extent = {32,32,1},
      .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
   VK(vkCreateImage(device, &image_info, NULL, &image_wrapper.base.handle.image));
   VkMemoryRequirements req; vkGetImageMemoryRequirements(device, image_wrapper.base.handle.image, &req);
   const VkMemoryDedicatedAllocateInfo dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = image_wrapper.base.handle.image};
   VkImportMemoryMetalHandleInfoEXT import_info = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_METAL_HANDLE_INFO_EXT,
      .pNext = &dedicated, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLTEXTURE_BIT_EXT, .handle = textures[0]};
   VkMemoryMetalHandlePropertiesEXT props = {.sType = VK_STRUCTURE_TYPE_MEMORY_METAL_HANDLE_PROPERTIES_EXT};
   PFN_vkGetMemoryMetalHandlePropertiesEXT get_props = (void *)vkGetDeviceProcAddr(device, "vkGetMemoryMetalHandlePropertiesEXT");
   VK(get_props(device, import_info.handleType, textures[0], &props));
   VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &import_info,
      .allocationSize = req.size, .memoryTypeIndex = memory_type(physical, req.memoryTypeBits & props.memoryTypeBits, 0)};
   VkDeviceMemory image_memory; VK(vkAllocateMemory(device, &alloc, NULL, &image_memory));
   VK(vkBindImageMemory(device, image_wrapper.base.handle.image, image_memory, 0));
   VkBuffer buffer; VkDeviceMemory buffer_memory;
   VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = 1024, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
   VK(vkCreateBuffer(device, &buffer_info, NULL, &buffer));
   vkGetBufferMemoryRequirements(device, buffer, &req);
   alloc.pNext = NULL; alloc.allocationSize = req.size;
   alloc.memoryTypeIndex = memory_type(physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   VK(vkAllocateMemory(device, &alloc, NULL, &buffer_memory));
   VK(vkBindBufferMemory(device, buffer, buffer_memory, 0));
   uint8_t *mapped; VK(vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
   VkCommandPool pool; VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
   VK(vkCreateCommandPool(device, &pool_info, NULL, &pool));
   struct vkr_command_buffer cmd = {.base = {.type = VK_OBJECT_TYPE_COMMAND_BUFFER, .id = 3}, .device = &dev};
   VkCommandBufferAllocateInfo command_alloc = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   VK(vkAllocateCommandBuffers(device, &command_alloc, &cmd.base.handle.command_buffer));
   struct vkr_fence done = {.base = {.type = VK_OBJECT_TYPE_FENCE, .id = 4}};
   VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VK(vkCreateFence(device, &fence_info, NULL, &done.base.handle.fence));

   for (unsigned mode = 0; mode < 2; mode++) {
      VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      struct vn_command_vkBeginCommandBuffer b = {.commandBuffer = (VkCommandBuffer)&cmd, .pBeginInfo = &begin};
      ctx.dispatch.dispatch_vkBeginCommandBuffer(&ctx.dispatch, &b); VK(b.ret);
      VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT, .dstQueueFamilyIndex = 0,
         .image = (VkImage)&image_wrapper, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
      if (!mode) {
         struct vn_command_vkCmdPipelineBarrier a = {.commandBuffer = (VkCommandBuffer)&cmd,
            .srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
            .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
         ctx.dispatch.dispatch_vkCmdPipelineBarrier(&ctx.dispatch, &a);
      } else {
         VkImageMemoryBarrier2 barrier2 = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = barrier.dstAccessMask, .oldLayout = barrier.oldLayout, .newLayout = barrier.newLayout,
            .srcQueueFamilyIndex = barrier.srcQueueFamilyIndex, .dstQueueFamilyIndex = barrier.dstQueueFamilyIndex,
            .image = barrier.image, .subresourceRange = barrier.subresourceRange};
         VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier2};
         struct vn_command_vkCmdPipelineBarrier2 a = {.commandBuffer = (VkCommandBuffer)&cmd, .pDependencyInfo = &dependency};
         ctx.dispatch.dispatch_vkCmdPipelineBarrier2(&ctx.dispatch, &a);
      }
      VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}, .imageExtent = {32,32,1}};
      vkCmdCopyImageToBuffer(cmd.base.handle.command_buffer, image_wrapper.base.handle.image,
         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
      barrier.image = image_wrapper.base.handle.image;
      barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; barrier.dstAccessMask = 0;
      vkCmdPipelineBarrier(cmd.base.handle.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
      VK(vkEndCommandBuffer(cmd.base.handle.command_buffer));
      /* Record/import once, then change the producer every frame. */
      for (unsigned frame = 0; frame < 6; frame++) {
         CVPixelBufferLockBaseAddress(pixels, 0);
         uint8_t value = 31 + frame * 29 + mode;
         for (unsigned p = 0; p < 2; p++)
            memset(CVPixelBufferGetBaseAddressOfPlane(pixels, p), value,
               CVPixelBufferGetBytesPerRowOfPlane(pixels, p) * CVPixelBufferGetHeightOfPlane(pixels, p));
         CVPixelBufferUnlockBaseAddress(pixels, 0);
         id<MTLSharedEvent> gate = [mtl newSharedEvent];
         struct virgl_video_metal_fence *writer = virgl_video_metal_prepare(video, textures, 2, 32,32,false,true,gate,1);
         CHECK(writer);
         CHECK(virgl_video_metal_complete(writer, pixels));
         CHECK(virgl_video_metal_fence_wait(writer, false) == 0);
         gate.signaledValue = 1;
         /* END_FRAME owns this wait in production. Reuse the imported VkImage
          * and command buffer, with no writer state or imported semaphore. */
         CHECK(virgl_video_metal_fence_wait(writer, true) == 1);
         virgl_video_metal_fence_unref(writer); [gate release];
         VK(vkResetFences(device, 1, &done.base.handle.fence));
         VkCommandBuffer handle = (VkCommandBuffer)&cmd;
         if (!mode) {
            VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &handle};
            struct vn_command_vkQueueSubmit q = {.queue = (VkQueue)queue, .submitCount = 1,
               .pSubmits = &submit, .fence = (VkFence)&done};
            ctx.dispatch.dispatch_vkQueueSubmit(&ctx.dispatch, &q); VK(q.ret);
         } else {
            VkCommandBufferSubmitInfo cb = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = handle};
            VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cb};
            struct vn_command_vkQueueSubmit2 q = {.queue = (VkQueue)queue, .submitCount = 1,
               .pSubmits = &submit, .fence = (VkFence)&done};
            ctx.dispatch.dispatch_vkQueueSubmit2(&ctx.dispatch, &q); VK(q.ret);
         }
         VK(vkWaitForFences(device, 1, &done.base.handle.fence, true, 5000000000));
         bool equal = true; for (unsigned i = 0; i < 1024; i++) equal &= mapped[i] == value;
         CHECK(equal);
      }
   }
   VK(vkDeviceWaitIdle(device));
   queue->base.id = 0; vkr_queue_destroy(&ctx, queue);
   list_for_each_entry_safe(struct vkr_queue_sync, sync, &dev.free_syncs, head) {
      vkDestroyFence(device, sync->fence, NULL); list_del(&sync->head); free(sync);
   }
   mtx_destroy(&dev.free_sync_mutex);
   vkDestroyFence(device, done.base.handle.fence, NULL); vkDestroyCommandPool(device, pool, NULL);
   vkUnmapMemory(device, buffer_memory); vkDestroyBuffer(device, buffer, NULL); vkFreeMemory(device, buffer_memory, NULL);
   vkDestroyImage(device, image_wrapper.base.handle.image, NULL); vkFreeMemory(device, image_memory, NULL);
   virgl_video_metal_destroy(video); CVPixelBufferRelease(pixels);
   for (unsigned p = 0; p < 2; p++) [(id)textures[p] release];
   vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL);
   printf("Vulkan completed-plane import: %u checks, %u failures\n", checks, failures);
   return failures ? 1 : 0;
} }
