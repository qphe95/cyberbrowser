#ifndef VULKAN_RENDERER_H
#define VULKAN_RENDERER_H

#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>

#include "display_list.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VulkanRenderer {
    VkInstance instance;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    VkQueue graphicsQueue;
    uint32_t graphicsQueueFamily;

    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat swapchainFormat;
    VkExtent2D swapchainExtent;
    uint32_t imageCount;
    VkImage *images;
    VkImageView *imageViews;
    VkFramebuffer *framebuffers;

    VkRenderPass renderPass;
    VkPipelineLayout pipelineLayout;
    VkPipeline pipeline;

    VkDescriptorSetLayout descriptorSetLayout;
    VkDescriptorPool descriptorPool;
    VkDescriptorSet descriptorSet;

    VkImage fontImage;
    VkDeviceMemory fontImageMemory;
    VkImageView fontImageView;
    VkSampler fontSampler;

    VkBuffer vertexBuffer;
    VkDeviceMemory vertexBufferMemory;
    uint32_t vertexCapacity;
    uint32_t vertexCount;

    VkCommandPool commandPool;
    VkCommandBuffer *commandBuffers;

    VkSemaphore imageAvailableSemaphore;
    VkSemaphore renderFinishedSemaphore;
    VkFence inFlightFence;

    VkBuffer captureBuffer;
    VkDeviceMemory captureBufferMemory;
    VkDeviceSize captureBufferSize;

    int32_t windowWidth;
    int32_t windowHeight;
    bool ready;

    /* Hi-res supersampling: render at 8x, box-filter down through
       4x and 2x intermediates to swapchain */
    VkImage hiResImage;
    VkDeviceMemory hiResImageMemory;
    VkImageView hiResImageView;
    VkFramebuffer hiResFramebuffer;
    VkImage midResImage1;       /* 4x */
    VkDeviceMemory midResImage1Memory;
    VkImage midResImage2;       /* 2x */
    VkDeviceMemory midResImage2Memory;

    const uint8_t *vertShaderData;
    size_t vertShaderSize;
    const uint8_t *fragShaderData;
    size_t fragShaderSize;
} VulkanRenderer;

bool vk_renderer_init(VulkanRenderer *r, VkInstance instance, VkSurfaceKHR surface,
                      const uint8_t *vert_data, size_t vert_size,
                      const uint8_t *frag_data, size_t frag_size);
void vk_renderer_cleanup(VulkanRenderer *r);
bool vk_renderer_recreate_swapchain(VulkanRenderer *r);
bool vk_renderer_update_vertices(VulkanRenderer *r, const void *vertexData, uint32_t vertexCount);
bool vk_renderer_draw(VulkanRenderer *r, const void *vertexData, uint32_t vertexCount);
bool vk_renderer_display_list(VulkanRenderer *r, const DisplayList *dl);
bool vk_renderer_capture_framebuffer(VulkanRenderer *r, const char *bmpPath);

#ifdef __cplusplus
}
#endif

#endif
