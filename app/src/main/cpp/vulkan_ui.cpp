#include "vulkan_ui.h"
#include "ui_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>

#include "url_analyzer.h"
#include "http_download.h"
#include "js_quickjs.h"
#include "platform.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "mp4_metadata.h"
#include "default_album_art.h"

/* Forward declarations for cleanup reset functions */
extern "C" void browser_api_impl_reset(void);

#ifdef _MSC_VER
#include <windows.h>
static inline double get_time_seconds(void) {
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart / (double)freq.QuadPart;
}
#define __atomic_load_n(ptr, order) (*(ptr))
#else
static inline double get_time_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
#endif

#ifdef BE_PLATFORM_ANDROID
#include <android/log.h>
#include <android_native_app_glue.h>
#include "media_store.h"
#define LOG_TAG "minimalvulkan"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOG_TAG "minimalvulkan"
#define LOGI(...) platform_log(LOG_LEVEL_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) platform_log(LOG_LEVEL_ERROR, LOG_TAG, __VA_ARGS__)
#endif

/* File logging */
void file_log(const char *fmt, ...) {
    static int log_fd = -1;
    if (log_fd < 0) {
#ifdef BE_PLATFORM_ANDROID
        log_fd = open("/data/data/com.bgmdwldr.vulkan/main_debug.log",
                      O_WRONLY | O_CREAT | O_APPEND, 0644);
#else
        const char *home = getenv("HOME");
#ifdef _WIN32
        if (!home) home = getenv("USERPROFILE");
#endif
        if (home) {
            char path[512];
            snprintf(path, sizeof(path), "%s/.bgmdwnldr_debug.log", home);
            log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        }
#endif
    }
    if (log_fd >= 0) {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        int n = vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        if (n > 0 && n < (int)sizeof(buf) - 1) {
            buf[n++] = '\n';
            write(log_fd, buf, n);
        }
    }
}

VulkanApp *g_app = NULL;
NativeInputState g_input = {0};

void free_shader(ShaderBlob *blob) {
    if (blob->data) {
        free(blob->data);
        blob->data = NULL;
        blob->size = 0;
    }
}


uint32_t find_memory_type(VulkanApp *app, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(app->physicalDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    /* Fallback: some emulators (e.g., SwiftShader) expose HOST_VISIBLE without HOST_COHERENT */
    if (properties == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
            if ((typeFilter & (1u << i)) &&
                (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                return i;
            }
        }
    }
    return UINT32_MAX;
}

bool create_buffer(VulkanApp *app, VkDeviceSize size, VkBufferUsageFlags usage,
                          VkMemoryPropertyFlags properties, VkBuffer *buffer,
                          VkDeviceMemory *bufferMemory) {
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkResult result = vkCreateBuffer(app->device, &bufferInfo, NULL, buffer);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateBuffer failed: %d", result);
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(app->device, *buffer, &memRequirements);

    uint32_t memoryType = find_memory_type(app, memRequirements.memoryTypeBits, properties);
    if (memoryType == UINT32_MAX) {
        LOGE("No suitable memory type");
        return false;
    }

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memRequirements.size,
        .memoryTypeIndex = memoryType
    };
    result = vkAllocateMemory(app->device, &allocInfo, NULL, bufferMemory);
    if (result != VK_SUCCESS) {
        LOGE("vkAllocateMemory failed: %d", result);
        return false;
    }

    vkBindBufferMemory(app->device, *buffer, *bufferMemory, 0);
    return true;
}


VkCommandBuffer begin_single_time_commands(VulkanApp *app) {
    VkCommandBufferAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = app->commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(app->device, &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    return commandBuffer;
}


void end_single_time_commands(VulkanApp *app, VkCommandBuffer commandBuffer) {
    vkEndCommandBuffer(commandBuffer);
    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &commandBuffer
    };
    vkQueueSubmit(app->graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(app->graphicsQueue);
    vkFreeCommandBuffers(app->device, app->commandPool, 1, &commandBuffer);
}


bool create_image(VulkanApp *app, uint32_t width, uint32_t height, VkFormat format,
                    VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties,
                    VkImage *image, VkDeviceMemory *imageMemory) {
    VkImageCreateInfo imageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = tiling,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };

    VkResult result = vkCreateImage(app->device, &imageInfo, NULL, image);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateImage failed: %d", result);
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(app->device, *image, &memRequirements);

    uint32_t memoryType = find_memory_type(app, memRequirements.memoryTypeBits, properties);
    if (memoryType == UINT32_MAX) {
        LOGE("No suitable image memory type");
        return false;
    }

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memRequirements.size,
        .memoryTypeIndex = memoryType
    };
    result = vkAllocateMemory(app->device, &allocInfo, NULL, imageMemory);
    if (result != VK_SUCCESS) {
        LOGE("vkAllocateMemory failed: %d", result);
        return false;
    }

    vkBindImageMemory(app->device, *image, *imageMemory, 0);
    return true;
}


VkImageView create_image_view(VulkanApp *app, VkImage image, VkFormat format) {
    VkImageViewCreateInfo viewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };
    VkImageView imageView;
    VkResult result = vkCreateImageView(app->device, &viewInfo, NULL, &imageView);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateImageView failed: %d", result);
        return VK_NULL_HANDLE;
    }
    return imageView;
}


void transition_image_layout(VulkanApp *app, VkImage image, VkImageLayout oldLayout,
                                    VkImageLayout newLayout) {
    VkCommandBuffer commandBuffer = begin_single_time_commands(app);
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = oldLayout,
        .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };

    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
        newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }

    vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0,
                         0, NULL, 0, NULL, 1, &barrier);
    end_single_time_commands(app, commandBuffer);
}


void copy_buffer_to_image(VulkanApp *app, VkBuffer buffer, VkImage image,
                                 uint32_t width, uint32_t height) {
    VkCommandBuffer commandBuffer = begin_single_time_commands(app);
    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1
        },
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, 1}
    };
    vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region);
    end_single_time_commands(app, commandBuffer);
}

/* Upload RGBA pixel data as a Vulkan texture. Replaces any previous album art.
   pixels must be width*height*4 bytes, tightly packed. */
bool upload_album_art(VulkanApp *app, const uint8_t *pixels, uint32_t width, uint32_t height) {
    if (!app || !pixels || width == 0 || height == 0) return false;

    /* Clean up previous album art */
    if (app->albumArtView != VK_NULL_HANDLE) {
        vkDestroyImageView(app->device, app->albumArtView, NULL);
        app->albumArtView = VK_NULL_HANDLE;
    }
    if (app->albumArtImage != VK_NULL_HANDLE) {
        vkDestroyImage(app->device, app->albumArtImage, NULL);
        app->albumArtImage = VK_NULL_HANDLE;
    }
    if (app->albumArtMemory != VK_NULL_HANDLE) {
        vkFreeMemory(app->device, app->albumArtMemory, NULL);
        app->albumArtMemory = VK_NULL_HANDLE;
    }
    if (app->albumArtSampler != VK_NULL_HANDLE) {
        vkDestroySampler(app->device, app->albumArtSampler, NULL);
        app->albumArtSampler = VK_NULL_HANDLE;
    }

    VkDeviceSize imageSize = (VkDeviceSize)width * height * 4;

    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    if (!create_buffer(app, imageSize,
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       &stagingBuffer, &stagingMemory)) {
        LOGE("upload_album_art: failed to create staging buffer");
        return false;
    }

    void *data;
    vkMapMemory(app->device, stagingMemory, 0, imageSize, 0, &data);
    memcpy(data, pixels, (size_t)imageSize);
    vkUnmapMemory(app->device, stagingMemory);

    if (!create_image(app, width, height, VK_FORMAT_R8G8B8A8_UNORM,
                      VK_IMAGE_TILING_OPTIMAL,
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                      &app->albumArtImage, &app->albumArtMemory)) {
        LOGE("upload_album_art: failed to create image");
        vkDestroyBuffer(app->device, stagingBuffer, NULL);
        vkFreeMemory(app->device, stagingMemory, NULL);
        return false;
    }

    transition_image_layout(app, app->albumArtImage,
                            VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copy_buffer_to_image(app, stagingBuffer, app->albumArtImage, width, height);
    transition_image_layout(app, app->albumArtImage,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(app->device, stagingBuffer, NULL);
    vkFreeMemory(app->device, stagingMemory, NULL);

    app->albumArtView = create_image_view(app, app->albumArtImage, VK_FORMAT_R8G8B8A8_UNORM);
    if (app->albumArtView == VK_NULL_HANDLE) {
        LOGE("upload_album_art: failed to create image view");
        return false;
    }

    VkSamplerCreateInfo samplerInfo = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .compareEnable = VK_FALSE,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE
    };
    VkResult result = vkCreateSampler(app->device, &samplerInfo, NULL, &app->albumArtSampler);
    if (result != VK_SUCCESS) {
        LOGE("upload_album_art: failed to create sampler: %d", result);
        return false;
    }

    app->albumArtWidth = width;
    app->albumArtHeight = height;
    app->albumArtVisible = true;

    /* Allocate and update descriptor set for album art */
    if (app->albumArtDescriptorSet == VK_NULL_HANDLE) {
        VkDescriptorSetAllocateInfo allocInfo = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = app->descriptorPool,
            .descriptorSetCount = 1,
            .pSetLayouts = &app->descriptorSetLayout
        };
        result = vkAllocateDescriptorSets(app->device, &allocInfo, &app->albumArtDescriptorSet);
        if (result != VK_SUCCESS) {
            LOGE("upload_album_art: failed to allocate descriptor set: %d", result);
            return false;
        }
    }

    VkDescriptorImageInfo imageInfo = {
        .sampler = app->albumArtSampler,
        .imageView = app->albumArtView,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = app->albumArtDescriptorSet,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &imageInfo
    };
    vkUpdateDescriptorSets(app->device, 1, &write, 0, NULL);

    return true;
}

bool pick_device_and_queue(VulkanApp *app) {
    if (app->device != VK_NULL_HANDLE) {
        return true;
    }
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(app->instance, &deviceCount, NULL);
    if (deviceCount == 0) {
        LOGE("No Vulkan devices found");
        return false;
    }
    VkPhysicalDevice *devices = (VkPhysicalDevice *)malloc(sizeof(VkPhysicalDevice) * deviceCount);
    vkEnumeratePhysicalDevices(app->instance, &deviceCount, devices);

    VkPhysicalDevice selected = VK_NULL_HANDLE;
    uint32_t selectedQueueFamily = 0;

    for (uint32_t i = 0; i < deviceCount; ++i) {
        uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &queueCount, NULL);
        VkQueueFamilyProperties *queues =
            (VkQueueFamilyProperties *)malloc(sizeof(VkQueueFamilyProperties) * queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &queueCount, queues);
        for (uint32_t q = 0; q < queueCount; ++q) {
            VkBool32 presentSupported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], q, app->surface, &presentSupported);
            if ((queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presentSupported) {
                selected = devices[i];
                selectedQueueFamily = q;
                break;
            }
        }
        free(queues);
        if (selected != VK_NULL_HANDLE) {
            break;
        }
    }
    free(devices);
    if (selected == VK_NULL_HANDLE) {
        LOGE("No suitable Vulkan device found");
        return false;
    }

    app->physicalDevice = selected;
    app->graphicsQueueFamily = selectedQueueFamily;

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = app->graphicsQueueFamily,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority
    };
    const char *deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo deviceCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueCreateInfo,
        .enabledExtensionCount = (uint32_t)(sizeof(deviceExtensions) / sizeof(deviceExtensions[0])),
        .ppEnabledExtensionNames = deviceExtensions
    };

    VkResult result = vkCreateDevice(app->physicalDevice, &deviceCreateInfo, NULL, &app->device);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateDevice failed: %d", result);
        return false;
    }
    vkGetDeviceQueue(app->device, app->graphicsQueueFamily, 0, &app->graphicsQueue);
    return true;
}


bool create_swapchain(VulkanApp *app) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(app->physicalDevice, app->surface, &caps);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(app->physicalDevice, app->surface, &formatCount, NULL);
    VkSurfaceFormatKHR *formats =
        (VkSurfaceFormatKHR *)malloc(sizeof(VkSurfaceFormatKHR) * formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(app->physicalDevice, app->surface, &formatCount, formats);

    VkSurfaceFormatKHR chosenFormat = formats[0];
    for (uint32_t i = 0; i < formatCount; ++i) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            chosenFormat = formats[i];
            break;
        }
    }
    free(formats);

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX || extent.height == UINT32_MAX) {
        /* Some platforms (e.g. Wayland) report undefined extent; fall back
           to the window dimensions we tracked from the platform. */
        extent.width = (uint32_t)app->windowWidth;
        extent.height = (uint32_t)app->windowHeight;
        if (extent.width < caps.minImageExtent.width) extent.width = caps.minImageExtent.width;
        if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
        if (extent.width > caps.maxImageExtent.width) extent.width = caps.maxImageExtent.width;
        if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    }
#ifdef BE_PLATFORM_MACOS
    /* On macOS, caps.currentExtent can lag behind the actual framebuffer
       size during live resize. GLFW's framebuffer size is updated
       immediately, so use it as the authoritative size. */
    if ((int)extent.width != app->windowWidth || (int)extent.height != app->windowHeight) {
        LOGI("create_swapchain: overriding extent %dx%d -> %dx%d (GLFW framebuffer)",
             extent.width, extent.height, app->windowWidth, app->windowHeight);
        extent.width = (uint32_t)app->windowWidth;
        extent.height = (uint32_t)app->windowHeight;
        if (extent.width < caps.minImageExtent.width) extent.width = caps.minImageExtent.width;
        if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
        if (extent.width > caps.maxImageExtent.width) extent.width = caps.maxImageExtent.width;
        if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    }
#endif
    LOGI("create_swapchain: window=%dx%d caps.currentExtent=%dx%d chosen=%dx%d",
         app->windowWidth, app->windowHeight,
         caps.currentExtent.width, caps.currentExtent.height,
         extent.width, extent.height);
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR swapInfo = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = app->surface,
        .minImageCount = imageCount,
        .imageFormat = chosenFormat.format,
        .imageColorSpace = chosenFormat.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = VK_NULL_HANDLE
    };
    VkResult result = vkCreateSwapchainKHR(app->device, &swapInfo, NULL, &app->swapchain);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateSwapchainKHR failed: %d", result);
        return false;
    }

    vkGetSwapchainImagesKHR(app->device, app->swapchain, &imageCount, NULL);
    app->images = (VkImage *)malloc(sizeof(VkImage) * imageCount);
    vkGetSwapchainImagesKHR(app->device, app->swapchain, &imageCount, app->images);
    app->imageCount = imageCount;
    app->swapchainFormat = chosenFormat.format;
    app->swapchainExtent = extent;
    app->windowWidth = (int32_t)extent.width;
    app->windowHeight = (int32_t)extent.height;
    app->surfaceTransform = caps.currentTransform;
    app->mirrorX = false;
    switch (caps.currentTransform) {
        case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_BIT_KHR:
        case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_90_BIT_KHR:
        case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_180_BIT_KHR:
        case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_270_BIT_KHR:
            app->mirrorX = true;
            break;
        default:
            break;
    }
    LOGE("Swapchain transform=%u mirrorX=%d", caps.currentTransform, app->mirrorX ? 1 : 0);

    app->imageViews = (VkImageView *)malloc(sizeof(VkImageView) * imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
        VkImageViewCreateInfo viewInfo = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = app->images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = app->swapchainFormat,
            .components = {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY
            },
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };
        result = vkCreateImageView(app->device, &viewInfo, NULL, &app->imageViews[i]);
        if (result != VK_SUCCESS) {
            LOGE("vkCreateImageView failed: %d", result);
            return false;
        }
    }
    return true;
}


bool create_render_pass(VulkanApp *app) {
    VkAttachmentDescription colorAttachment = {
        .format = app->swapchainFormat,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
    };
    VkAttachmentReference colorRef = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorRef
    };
    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
    };
    VkRenderPassCreateInfo renderPassInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &colorAttachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency
    };
    VkResult result = vkCreateRenderPass(app->device, &renderPassInfo, NULL, &app->renderPass);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateRenderPass failed: %d", result);
        return false;
    }
    return true;
}


VkShaderModule create_shader_module(VulkanApp *app, const uint8_t *data, size_t size) {
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode = (const uint32_t *)data
    };
    VkShaderModule module;
    VkResult result = vkCreateShaderModule(app->device, &info, NULL, &module);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateShaderModule failed: %d", result);
        return VK_NULL_HANDLE;
    }
    return module;
}


bool create_font_resources(VulkanApp *app) {
    uint8_t *atlas = (uint8_t *)malloc(FONT_ATLAS_W * FONT_ATLAS_H);
    if (!atlas) {
        LOGE("Failed to allocate font atlas");
        return false;
    }
    build_font_atlas(atlas);

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    if (!create_buffer(app, FONT_ATLAS_W * FONT_ATLAS_H,
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       &stagingBuffer, &stagingMemory)) {
        free(atlas);
        return false;
    }

    void *data = NULL;
    vkMapMemory(app->device, stagingMemory, 0, FONT_ATLAS_W * FONT_ATLAS_H, 0, &data);
    memcpy(data, atlas, FONT_ATLAS_W * FONT_ATLAS_H);
    VkMappedMemoryRange stagingRange = {};
    stagingRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    stagingRange.memory = stagingMemory;
    stagingRange.offset = 0;
    stagingRange.size = VK_WHOLE_SIZE;
    vkFlushMappedMemoryRanges(app->device, 1, &stagingRange);
    vkUnmapMemory(app->device, stagingMemory);
    free(atlas);

    if (!create_image(app, FONT_ATLAS_W, FONT_ATLAS_H, VK_FORMAT_R8_UNORM,
                      VK_IMAGE_TILING_OPTIMAL,
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                      &app->fontImage, &app->fontImageMemory)) {
        return false;
    }
    transition_image_layout(app, app->fontImage, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copy_buffer_to_image(app, stagingBuffer, app->fontImage, FONT_ATLAS_W, FONT_ATLAS_H);
    transition_image_layout(app, app->fontImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(app->device, stagingBuffer, NULL);
    vkFreeMemory(app->device, stagingMemory, NULL);

    app->fontImageView = create_image_view(app, app->fontImage, VK_FORMAT_R8_UNORM);
    if (app->fontImageView == VK_NULL_HANDLE) {
        return false;
    }

    VkSamplerCreateInfo samplerInfo = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .compareEnable = VK_FALSE,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE
    };
    VkResult result = vkCreateSampler(app->device, &samplerInfo, NULL, &app->fontSampler);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateSampler failed: %d", result);
        return false;
    }

    VkDescriptorSetLayoutBinding samplerBinding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
    };
    VkDescriptorSetLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &samplerBinding
    };
    result = vkCreateDescriptorSetLayout(app->device, &layoutInfo, NULL, &app->descriptorSetLayout);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateDescriptorSetLayout failed: %d", result);
        return false;
    }

    VkDescriptorPoolSize poolSize = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 2
    };
    VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize
    };
    result = vkCreateDescriptorPool(app->device, &poolInfo, NULL, &app->descriptorPool);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateDescriptorPool failed: %d", result);
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = app->descriptorPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &app->descriptorSetLayout
    };
    result = vkAllocateDescriptorSets(app->device, &allocInfo, &app->descriptorSet);
    if (result != VK_SUCCESS) {
        LOGE("vkAllocateDescriptorSets failed: %d", result);
        return false;
    }

    VkDescriptorImageInfo imageInfo = {
        .sampler = app->fontSampler,
        .imageView = app->fontImageView,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = app->descriptorSet,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &imageInfo
    };
    vkUpdateDescriptorSets(app->device, 1, &write, 0, NULL);

    return true;
}


bool create_vertex_buffer(VulkanApp *app) {
    app->vertexCapacity = 4096;
    VkDeviceSize bufferSize = app->vertexCapacity * sizeof(Vertex);
    return create_buffer(app, bufferSize,
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         &app->vertexBuffer, &app->vertexBufferMemory);
}


void ui_set_status(VulkanApp *app, const char *status) {
    pthread_mutex_lock(&app->uiMutex);
    snprintf(app->statusText, sizeof(app->statusText), "%s", status);
    pthread_mutex_unlock(&app->uiMutex);
}

void ui_set_progress(VulkanApp *app, float progress) {
    pthread_mutex_lock(&app->uiMutex);
    app->progress = progress;
    pthread_mutex_unlock(&app->uiMutex);
}


void ui_snapshot(VulkanApp *app, char *urlOut, size_t urlLen,
                        char *statusOut, size_t statusLen, float *progressOut) {
    pthread_mutex_lock(&app->uiMutex);
    snprintf(urlOut, urlLen, "%s", app->urlInput);
    if (app->statusText[0] == '\0') {
        snprintf(statusOut, statusLen, "Idle");
    } else {
        snprintf(statusOut, statusLen, "%s", app->statusText);
    }
    if (progressOut) {
        *progressOut = app->progress;
    }
    pthread_mutex_unlock(&app->uiMutex);

    /* If a download is active, poll DownloadState and override display */
    if (app->workerRunning) {
        size_t downloaded = __atomic_load_n(&app->downloadState.bytes_downloaded, __ATOMIC_RELAXED);
        size_t total = __atomic_load_n(&app->downloadState.bytes_total, __ATOMIC_RELAXED);
        int state = __atomic_load_n(&app->downloadState.state, __ATOMIC_RELAXED);

        if (total > 0 && progressOut) {
            *progressOut = (float)downloaded / (float)total;
        }

        /* Only override status text when the download has actually started
         * (state != 0). When state == 0, the worker thread is still doing
         * setup/url analysis and has set its own status text.
         * Add an animated ellipsis so the user knows the app is alive. */
        if (state == 0) {
            double now = get_time_seconds();
            int dots = (int)(now * 2.0) % 4;
            pthread_mutex_lock(&app->uiMutex);
            /* Only animate if the current status looks like a loading message */
            if (strncmp(app->statusText, "Analyzing", 9) == 0 ||
                strncmp(app->statusText, "Loading", 7) == 0 ||
                strncmp(app->statusText, "Getting", 7) == 0) {
                char base[128];
                snprintf(base, sizeof(base), "%s", app->statusText);
                /* Strip any existing dots */
                size_t len = strlen(base);
                while (len > 0 && base[len-1] == '.') {
                    base[len-1] = '\0';
                    len--;
                }
                char dots_str[5] = {0};
                for (int i = 0; i < dots; i++) dots_str[i] = '.';
                snprintf(app->statusText, sizeof(app->statusText), "%s%s", base, dots_str);
            }
            pthread_mutex_unlock(&app->uiMutex);
        } else if (state != 0) {
            double now = get_time_seconds();

            pthread_mutex_lock(&app->uiMutex);
            double elapsed = now - app->prev_download_time;
            if (elapsed >= 0.3 && app->prev_download_time > 0) {
                size_t delta = downloaded - app->prev_downloaded_bytes;
                double speed = (double)delta / elapsed;
                const char *unit = "B/s";
                double display_speed = speed;
                if (display_speed >= 1024.0 * 1024.0 * 1024.0) {
                    display_speed /= 1024.0 * 1024.0 * 1024.0;
                    unit = "GB/s";
                } else if (display_speed >= 1024.0 * 1024.0) {
                    display_speed /= 1024.0 * 1024.0;
                    unit = "MB/s";
                } else if (display_speed >= 1024.0) {
                    display_speed /= 1024.0;
                    unit = "KB/s";
                }
                int pct = (total > 0) ? (int)((float)downloaded / (float)total * 100.0f) : 0;
                if (state == 1) {
                    snprintf(app->statusText, sizeof(app->statusText), "Getting file size...");
                } else if (state == 3) {
                    snprintf(app->statusText, sizeof(app->statusText), "Finishing... %d%%", pct);
                } else if (state == 4) {
                    snprintf(app->statusText, sizeof(app->statusText), "Download complete");
                } else if (state == 5) {
                    snprintf(app->statusText, sizeof(app->statusText), "Download failed");
                } else {
                    snprintf(app->statusText, sizeof(app->statusText), "Downloading... %d%% %.1f %s", pct, display_speed, unit);
                }
                app->prev_downloaded_bytes = downloaded;
                app->prev_download_time = now;
            } else if (app->prev_download_time == 0.0) {
                app->prev_downloaded_bytes = downloaded;
                app->prev_download_time = now;
            }
            pthread_mutex_unlock(&app->uiMutex);
        }
    }
}

// DEPRECATED: Use show_soft_keyboard/hide_soft_keyboard from new input system

typedef struct WorkerArgs {
    VulkanApp *app;
    char url[2048];
} WorkerArgs;


static void compute_and_set_status(VulkanApp *app, size_t downloaded, size_t total, int state) {
    double now = get_time_seconds();

    pthread_mutex_lock(&app->uiMutex);
    double elapsed = now - app->prev_download_time;
    if (elapsed >= 0.3 && app->prev_download_time > 0) {
        size_t delta = downloaded - app->prev_downloaded_bytes;
        double speed = (double)delta / elapsed;
        const char *unit = "B/s";
        double display_speed = speed;
        if (display_speed >= 1024.0 * 1024.0 * 1024.0) {
            display_speed /= 1024.0 * 1024.0 * 1024.0;
            unit = "GB/s";
        } else if (display_speed >= 1024.0 * 1024.0) {
            display_speed /= 1024.0 * 1024.0;
            unit = "MB/s";
        } else if (display_speed >= 1024.0) {
            display_speed /= 1024.0;
            unit = "KB/s";
        }
        int pct = (total > 0) ? (int)((float)downloaded / (float)total * 100.0f) : 0;
        if (state == 1) {
            snprintf(app->statusText, sizeof(app->statusText), "Getting file size...");
        } else if (state == 3) {
            snprintf(app->statusText, sizeof(app->statusText), "Finishing... %d%%", pct);
        } else if (state == 4) {
            snprintf(app->statusText, sizeof(app->statusText), "Download complete");
        } else if (state == 5) {
            snprintf(app->statusText, sizeof(app->statusText), "Download failed");
        } else {
            snprintf(app->statusText, sizeof(app->statusText), "Downloading... %d%% %.1f %s", pct, display_speed, unit);
        }
        app->prev_downloaded_bytes = downloaded;
        app->prev_download_time = now;
    } else if (app->prev_download_time == 0.0) {
        app->prev_downloaded_bytes = downloaded;
        app->prev_download_time = now;
    }
    if (total > 0) {
        app->progress = (float)downloaded / (float)total;
    }
    pthread_mutex_unlock(&app->uiMutex);
}

/* Extract video ID from YouTube URL for filename fallback */

static void extract_video_id(const char *url, char *out, size_t out_len) {
    const char *v = strstr(url, "v=");
    if (v) {
        v += 2;
        size_t i = 0;
        while (v[i] && v[i] != '&' && v[i] != '#' && i < 32 && i < out_len - 1) {
            out[i] = v[i];
            i++;
        }
        out[i] = '\0';
    } else {
        const char *last_slash = strrchr(url, '/');
        if (last_slash) {
            snprintf(out, out_len, "%s", last_slash + 1);
        } else {
            snprintf(out, out_len, "download");
        }
    }
}

/* Sanitize title into a filesystem-safe short name.
   Keeps alphanumerics, spaces, dashes, underscores, dots, and valid UTF-8 sequences.
   Replaces dangerous chars with '_' and collapses multiples. */
static void sanitize_filename(const char *title, char *out, size_t out_len) {
    size_t j = 0;
    int last_was_special = 1; /* suppress leading underscores */
    for (size_t i = 0; title[i] && j < out_len - 1; ) {
        unsigned char c = (unsigned char)title[i];

        /* Detect UTF-8 multi-byte sequence length */
        size_t seq_len = 1;
        if ((c & 0x80) != 0) {
            if ((c & 0xE0) == 0xC0) seq_len = 2;
            else if ((c & 0xF0) == 0xE0) seq_len = 3;
            else if ((c & 0xF8) == 0xF0) seq_len = 4;

            /* Validate continuation bytes */
            bool valid = true;
            for (size_t k = 1; k < seq_len && valid; k++) {
                unsigned char next = (unsigned char)title[i + k];
                if (next == '\0' || (next & 0xC0) != 0x80) valid = false;
            }
            if (!valid) seq_len = 1;
        }

        if (seq_len > 1) {
            /* Preserve valid UTF-8 sequences (safe for modern filesystems) */
            if (j + seq_len < out_len) {
                memcpy(out + j, title + i, seq_len);
                j += seq_len;
                last_was_special = 0;
            }
            i += seq_len;
        } else {
            /* ASCII single byte */
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '(' || c == ')') {
                out[j++] = (char)c;
                last_was_special = 0;
            } else if (c == ' ' && !last_was_special && j < out_len - 1) {
                out[j++] = '_';
                last_was_special = 1;
            } else if (!last_was_special && j < out_len - 1) {
                out[j++] = '_';
                last_was_special = 1;
            }
            i++;
        }
    }
    /* trim trailing underscores */
    while (j > 0 && out[j - 1] == '_') {
        j--;
    }
    out[j] = '\0';
}

/* Expand $TITLE wildcard in a filename template.
   Replaces $TITLE with the sanitized video title.
   out must hold at least out_len bytes. */
static void expand_filename_template(const char *template_str, const char *title,
                                      char *out, size_t out_len) {
    const char *ph = "$TITLE";
    size_t ph_len = 6;
    size_t out_pos = 0;
    out[0] = '\0';

    const char *p = template_str;
    while (*p && out_pos < out_len - 1) {
        if (strncmp(p, ph, ph_len) == 0) {
            char safe_title[256];
            sanitize_filename(title, safe_title, sizeof(safe_title));
            size_t st_len = strlen(safe_title);
            if (out_pos + st_len >= out_len) {
                st_len = out_len - out_pos - 1;
            }
            memcpy(out + out_pos, safe_title, st_len);
            out_pos += st_len;
            p += ph_len;
        } else {
            out[out_pos++] = *p++;
        }
    }
    out[out_pos] = '\0';
}

/* Generate a unique filename from video title.
   If title is empty, falls back to video ID from URL.
   Appends _v2, _v3, etc. if the file already exists in save_path.
   out must hold at least out_len bytes (including .m4a). */
static void generate_filename(const char *title, const char *url,
                              const char *save_path,
                              char *out, size_t out_len,
                              const char *ext) {
    char base[256] = {0};
    if (title && title[0] != '\0') {
        sanitize_filename(title, base, sizeof(base));
    }
    if (base[0] == '\0') {
        extract_video_id(url, base, sizeof(base));
    }
    /* limit base length to keep room for suffix + extension */
    if (strlen(base) > 50) {
        /* Trim to last complete UTF-8 sequence before 50 bytes */
        size_t trim = 50;
        while (trim > 0 && ((unsigned char)base[trim] & 0xC0) == 0x80) {
            trim--;
        }
        base[trim] = '\0';
        /* trim trailing spaces/underscores after cut */
        size_t b = strlen(base);
        while (b > 0 && (base[b - 1] == ' ' || base[b - 1] == '_')) {
            b--;
        }
        base[b] = '\0';
    }

    if (!ext) ext = ".m4a";
    size_t path_len = save_path ? strlen(save_path) : 0;
    int version = 1;
    for (;;) {
        if (version == 1) {
            snprintf(out, out_len, "%s%s", base, ext);
        } else {
            char suffix[16];
            snprintf(suffix, sizeof(suffix), "_v%d", version);
            size_t base_keep = strlen(base);
            /* make room for suffix so total stays reasonable */
            while (base_keep + strlen(suffix) > 50) {
                /* Step back to the start of the previous UTF-8 sequence */
                while (base_keep > 0 && ((unsigned char)base[base_keep - 1] & 0xC0) == 0x80) {
                    base_keep--;
                }
                if (base_keep > 0) base_keep--;
            }
            char trimmed[256];
            strncpy(trimmed, base, base_keep);
            trimmed[base_keep] = '\0';
            /* trim trailing spaces/underscores */
            while (base_keep > 0 && (trimmed[base_keep - 1] == ' ' || trimmed[base_keep - 1] == '_')) {
                base_keep--;
            }
            trimmed[base_keep] = '\0';
            snprintf(out, out_len, "%s%s%s", trimmed, suffix, ext);
        }
        /* Check if file already exists (skip on Android where save_path is NULL) */
        if (!save_path || path_len == 0) {
            break;
        }
        char fullpath[512];
        if (save_path[path_len - 1] == '/') {
            snprintf(fullpath, sizeof(fullpath), "%s%s", save_path, out);
        } else {
            snprintf(fullpath, sizeof(fullpath), "%s/%s", save_path, out);
        }
        if (!platform_file_exists(fullpath)) {
            break; /* file does not exist, we can use this name */
        }
        version++;
        if (version > 999) {
            /* safety break */
            snprintf(out, out_len, "%s_v%d%s", base, version, ext);
            break;
        }
    }
}


void *worker_thread(void *arg) {
    WorkerArgs *args = (WorkerArgs *)arg;
    VulkanApp *app = args->app;
    ui_set_progress(app, 0.0f);
    
    LOGI("Processing URL: %s", args->url);
    file_log("Processing URL: %s", args->url);
    ui_set_status(app, "Initializing...");
    
    time_t start_time = time(NULL);
    const int max_total_time = 300;  // 5 minute total timeout for entire operation
    char err[512] = {0};
    MediaUrl media = {0};
    char filename[256] = {0};
    char status_msg[280];
    char save_err[512] = {0};

#ifdef BE_PLATFORM_ANDROID
    MediaStoreHandle handle = {0};
    JavaVM *vm = NULL;
    jobject activity = NULL;
#endif
    
    /* Initialize platform and HTTP */
    platform_init();
    platform_http_init();
    
    /* Clear any previous session cookies */
    http_clear_youtube_cookies();

    /* Initialize QuickJS runtime for this download session */
    LOGI("Initializing QuickJS...");
    file_log("Initializing QuickJS...");
    if (!js_quickjs_init()) {
        LOGE("QuickJS initialization failed!");
        file_log("QuickJS initialization failed!");
        ui_set_status(app, "JS init failed");
        app->workerRunning = false;
        free(args);
        return NULL;
    }
    
    LOGI("Creating QuickJS runtime...");
    file_log("Creating QuickJS runtime...");
    if (!js_quickjs_create_runtime()) {
        LOGE("QuickJS runtime creation failed!");
        file_log("QuickJS runtime creation failed!");
        ui_set_status(app, "JS runtime failed");
        js_quickjs_cleanup();
        app->workerRunning = false;
        free(args);
        return NULL;
    }
    
    LOGI("Setting up DOM...");
    file_log("Setting up DOM...");
    js_quickjs_setup_initial_dom();
    
    ui_set_status(app, "Analyzing URL...");

    /* Step 1: Analyze URL and extract media info */
    file_log("Calling url_analyze...");
    if (!url_analyze_with_options(args->url, &media, err, sizeof(err), app->keepVideo)) {
        LOGE("URL analysis failed: %s", err);
        file_log("URL analysis failed: %s", err);
        snprintf(status_msg, sizeof(status_msg), "Analysis failed: %.200s", err);
        ui_set_status(app, status_msg);
        goto cleanup;
    }

    LOGI("Media URL found: %.300s", media.url);
    file_log("Media URL found: %.300s", media.url);
    if (media.title[0]) {
        LOGI("Video title: %.200s", media.title);
        file_log("Video title: %s", media.title);
        pthread_mutex_lock(&app->uiMutex);
        snprintf(app->videoTitle, sizeof(app->videoTitle), "%s", media.title);
        pthread_mutex_unlock(&app->uiMutex);
    }
    // Check for overall timeout
    if (time(NULL) - start_time > max_total_time) {
        LOGE("Operation timed out after %d seconds", max_total_time);
        ui_set_status(app, "Operation timeout");
        goto cleanup;
    }
    
    /* Validate the URL before downloading */
    if (strstr(media.url, "googlevideo.com") == NULL) {
        LOGE("URL does not contain googlevideo.com - may be invalid");
    } else {
        /* Check if URL has required parameters */
        if (strstr(media.url, "sig=") == NULL && strstr(media.url, "signature=") == NULL) {
            LOGI("URL has no signature parameter");
        } else {
            LOGI("URL has signature parameter");
        }
    }
    
    /* Step 2: Download the media file */
#ifdef BE_PLATFORM_ANDROID
    /* Expand filename template (handles $TITLE wildcard) */
    expand_filename_template(app->customFilename, media.title, filename, sizeof(filename));
    if (!filename[0]) {
        generate_filename(media.title, args->url, NULL, filename, sizeof(filename),
                          app->keepVideo ? ".mp4" : ".m4a");
    }
    vm = app->androidApp->activity->vm;
    activity = app->androidApp->activity->clazz;
    if (media_store_create_audio(vm, activity, filename,
                                 media.mime[0] ? media.mime : "audio/mp4",
                                 &handle, err, sizeof(err))) {
        HttpBuffer buffer = {0};
        if (http_get_to_memory(media.url, &buffer, err, sizeof(err))) {
            ssize_t written = write(handle.fd, buffer.data, buffer.size);
            if (written == (ssize_t)buffer.size) {
                media_store_finalize(vm, activity, &handle, err, sizeof(err));
                LOGI("Saved to MediaStore: %s (%zu bytes)", filename, buffer.size);
                file_log("Saved to MediaStore: %s (%zu bytes)", filename, buffer.size);
                char uriStr[512] = {0};
                if (media_store_get_uri_string(vm, &handle, uriStr, sizeof(uriStr))) {
                    pthread_mutex_lock(&app->uiMutex);
                    snprintf(app->downloadPath, sizeof(app->downloadPath), "%s", uriStr);
                    pthread_mutex_unlock(&app->uiMutex);
                }
                ui_set_status(app, "Download complete");
            } else {
                LOGE("Write failed: expected %zu, wrote %zd", buffer.size, written);
                file_log("Write failed: expected %zu, wrote %zd", buffer.size, written);
                ui_set_status(app, "Save failed");
            }
        } else {
            LOGE("Download failed: %s", err);
            snprintf(status_msg, sizeof(status_msg), "Download failed: %.200s", err);
            ui_set_status(app, status_msg);
        }
        http_free_buffer(&buffer);
        media_store_close(vm, &handle);
    } else {
        LOGE("MediaStore create failed: %s", err);
        file_log("MediaStore create failed: %s", err);
        ui_set_status(app, "MediaStore failed");
    }
#else
    {
    if (app->keepVideo) {
        if (!platform_media_save_init_video("BGMDWLDR", save_err, sizeof(save_err))) {
            LOGE("platform_media_save_init_video failed: %s", save_err);
            file_log("platform_media_save_init_video failed: %s", save_err);
            ui_set_status(app, "Save init failed");
            ui_set_progress(app, 0.0f);
            goto cleanup;
        }
    } else {
        if (!platform_media_save_init("BGMDWLDR", save_err, sizeof(save_err))) {
            LOGE("platform_media_save_init failed: %s", save_err);
            file_log("platform_media_save_init failed: %s", save_err);
            ui_set_status(app, "Save init failed");
            ui_set_progress(app, 0.0f);
            goto cleanup;
        }
    }
    
    /* Expand filename template (handles $TITLE wildcard) */
    expand_filename_template(app->customFilename, media.title, filename, sizeof(filename));
    if (!filename[0]) {
        generate_filename(media.title, args->url, platform_media_save_get_path(), filename, sizeof(filename),
                          app->keepVideo ? ".mp4" : ".m4a");
    }
    
    char fullPath[512];
    const char *savePath = platform_media_save_get_path();
    snprintf(fullPath, sizeof(fullPath), "%s/%s", savePath, filename);
    
    download_state_reset(&app->downloadState);
    if (!http_download_to_file(media.url, fullPath, &app->downloadState, err, sizeof(err))) {
        LOGE("Download failed: %s", err);
        snprintf(status_msg, sizeof(status_msg), "Download failed: %.200s", err);
        ui_set_status(app, status_msg);
        ui_set_progress(app, 0.0f);
        platform_media_save_cleanup();
        goto cleanup;
    }
    
    LOGI("Saved to %s", fullPath);
    file_log("Saved to %s", fullPath);

    pthread_mutex_lock(&app->uiMutex);
    snprintf(app->downloadPath, sizeof(app->downloadPath), "%s", fullPath);
    pthread_mutex_unlock(&app->uiMutex);
    ui_set_status(app, "Download complete");
    ui_set_progress(app, 1.0f);

    if (!app->keepVideo) {
        /* Embed default album art into m4a using native MP4 metadata injection */
        LOGI("Embedding default album art into %s (%zu bytes, JPEG)",
             fullPath, DEFAULT_ALBUM_ART_JPG_SIZE);
        file_log("Embedding default album art into %s", fullPath);
        if (mp4_embed_album_art(fullPath,
                                DEFAULT_ALBUM_ART_JPG,
                                DEFAULT_ALBUM_ART_JPG_SIZE, true)) {
            LOGI("Default album art embedded successfully");
            file_log("Default album art embedded successfully");
        } else {
            LOGE("Default album art embedding failed");
            file_log("Default album art embedding failed");
        }
    }
    
    /* Set title metadata to match user-specified filename */
    char titleFromFilename[256];
    size_t fn_len = strlen(filename);
    const char *used_ext = app->keepVideo ? ".mp4" : ".m4a";
    size_t ext_len = strlen(used_ext);
    if (fn_len > ext_len) {
        size_t base_len = fn_len - ext_len;
        if (base_len >= sizeof(titleFromFilename)) base_len = sizeof(titleFromFilename) - 1;
        memcpy(titleFromFilename, filename, base_len);
        titleFromFilename[base_len] = '\0';
    } else {
        snprintf(titleFromFilename, sizeof(titleFromFilename), "%s", filename);
    }
    LOGI("Setting title to: %s", titleFromFilename);
    file_log("Setting title to: %s", titleFromFilename);
    if (mp4_set_title(fullPath, titleFromFilename)) {
        LOGI("Title set successfully");
        file_log("Title set successfully");
    } else {
        LOGE("Title setting failed");
        file_log("Title setting failed");
    }
    
    platform_media_save_cleanup();
    }
#endif

cleanup:
    /* Cleanup resources */
    LOGI("Cleaning up QuickJS...");
    file_log("Cleaning up QuickJS...");
    browser_api_impl_reset();
    js_quickjs_reset_class_ids();
    js_quickjs_cleanup();
    platform_http_cleanup();
    platform_cleanup();
    
    app->workerRunning = false;
    free(args);
    return NULL;
}


void start_worker(VulkanApp *app) {
    LOGI("start_worker called, workerRunning=%d", app->workerRunning ? 1 : 0);
    file_log("start_worker called, workerRunning=%d", app->workerRunning ? 1 : 0);
    if (app->workerRunning) {
        LOGI("start_worker: worker already running, returning");
        file_log("start_worker: worker already running, returning");
        return;
    }
    WorkerArgs *args = (WorkerArgs *)malloc(sizeof(WorkerArgs));
    if (!args) {
        ui_set_status(app, "Out of memory");
        return;
    }
    args->app = app;
    pthread_mutex_lock(&app->uiMutex);
    snprintf(args->url, sizeof(args->url), "%s", app->urlInput);
    pthread_mutex_unlock(&app->uiMutex);
    LOGI("start_worker: URL='%s'", args->url);
    if (args->url[0] == '\0') {
        ui_set_status(app, "Enter a URL");
        free(args);
        return;
    }
    pthread_mutex_lock(&app->uiMutex);
    app->downloadPath[0] = '\0';
    app->videoTitle[0] = '\0';
    app->thumbnailUrl[0] = '\0';
    app->albumArtVisible = false;
    app->prev_downloaded_bytes = 0;
    app->prev_download_time = 0.0;
    pthread_mutex_unlock(&app->uiMutex);
    download_state_reset(&app->downloadState);
    LOGI("start_worker: calling pthread_create...");
    file_log("start_worker: calling pthread_create...");
    int pt_result = pthread_create(&app->workerThread, NULL, worker_thread, args);
    app->workerRunning = (pt_result == 0);
    LOGI("start_worker: pthread_create returned %d, workerRunning=%d", pt_result, app->workerRunning ? 1 : 0);
    file_log("start_worker: pthread_create returned %d, workerRunning=%d", pt_result, app->workerRunning ? 1 : 0);
    if (!app->workerRunning) {
        ui_set_status(app, "Worker start failed");
        free(args);
    }
}

/* Draw a string of text at (x,y) with given color. Returns x after last glyph. */

/* Test mode: when non-NULL, draw_frame renders this text instead of the full UI */
const char *g_testText = NULL;

void update_test_text_vertices(VulkanApp *app, const char *text) {
    Vertex vertices[1024];
    uint32_t count = generate_test_text(vertices, 1024, text, 10.0f, 10.0f,
                                         app->densityScale,
                                         (float)app->swapchainExtent.width,
                                         (float)app->swapchainExtent.height,
                                         1.0f, 1.0f, 1.0f);
    app->vertexCount = count;
    void *data = NULL;
    vkMapMemory(app->device, app->vertexBufferMemory, 0,
                app->vertexCapacity * sizeof(Vertex), 0, &data);
    memcpy(data, vertices, count * sizeof(Vertex));
    vkUnmapMemory(app->device, app->vertexBufferMemory);
}


static void draw_context_menu(Vertex *vertices, uint32_t *count, uint32_t capacity,
                              float screenW, float screenH, bool mirrorX,
                              float glyphW, float glyphH, float densityScale) {
    if (!g_input.menuVisible) {
        g_input.menuX0 = g_input.menuY0 = g_input.menuX1 = g_input.menuY1 = -1.0f;
        return;
    }

    const char *items[4] = {"Cut", "Copy", "Paste", "Select All"};
    const float C_WHITE[3]     = {1.00f, 1.00f, 1.00f};
    const float C_CYAN_DIM[3]  = {0.05f, 0.50f, 0.65f};
    const float C_BG_LIGHT[3]  = {0.10f, 0.10f, 0.16f};
    const float C_HOVER[3]     = {0.15f, 0.25f, 0.40f};

    float padX = 12.0f * densityScale;
    float padY = 6.0f * densityScale;
    float itemH = glyphH + padY * 2.0f;
    float sepH = 4.0f * densityScale;
    float borderThick = 1.0f * densityScale;

    /* Compute width from longest label */
    float maxTextW = 0.0f;
    for (int i = 0; i < 4; i++) {
        float w = string_width(items[i], glyphW);
        if (w > maxTextW) maxTextW = w;
    }
    float menuW = maxTextW + padX * 2.0f;
    float menuH = 4.0f * itemH + sepH;

    /* Reflow to stay inside window bounds */
    float mx = g_input.menuX;
    float my = g_input.menuY;
    if (mx + menuW > screenW) mx = screenW - menuW;
    if (my + menuH > screenH) my = screenH - menuH;
    if (mx < 0.0f) mx = 0.0f;
    if (my < 0.0f) my = 0.0f;

    /* Store computed bounds for hit testing */
    g_input.menuX0 = mx;
    g_input.menuY0 = my;
    g_input.menuX1 = mx + menuW;
    g_input.menuY1 = my + menuH;

    /* Background */
    append_rect(vertices, count, mx, my, menuW, menuH,
                screenW, screenH, mirrorX,
                C_BG_LIGHT[0], C_BG_LIGHT[1], C_BG_LIGHT[2]);

    /* Border */
    append_border(vertices, count, mx, my, menuW, menuH, borderThick,
                  screenW, screenH, mirrorX,
                  C_CYAN_DIM[0], C_CYAN_DIM[1], C_CYAN_DIM[2]);

    /* Items */
    for (int i = 0; i < 4; i++) {
        float rowY = my + i * itemH;
        if (i >= 2) rowY += sepH; /* shift down after separator */

        /* Highlight */
        if (g_input.menuHoveredItem == i) {
            append_rect(vertices, count, mx + borderThick, rowY,
                        menuW - borderThick * 2.0f, itemH,
                        screenW, screenH, mirrorX,
                        C_HOVER[0], C_HOVER[1], C_HOVER[2]);
        }

        /* Text (vertically centered in row) */
        float textY = rowY + (itemH - glyphH) * 0.5f;
        draw_string(vertices, count, capacity, items[i],
                    mx + padX, textY, glyphW, glyphH,
                    screenW, screenH, mirrorX,
                    C_WHITE[0], C_WHITE[1], C_WHITE[2]);
    }

    /* Separator line between Copy (1) and Paste (2) */
    float sepY = my + 2.0f * itemH + sepH * 0.5f;
    append_hline(vertices, count, mx + padX, sepY, menuW - padX * 2.0f, 1.0f * densityScale,
                 screenW, screenH, mirrorX,
                 C_CYAN_DIM[0], C_CYAN_DIM[1], C_CYAN_DIM[2]);
}

void update_text_vertices(VulkanApp *app) {
    char url[256];
    char status[128];
    char downloadPath[512];
    char videoTitle[256];
    float progress = 0.0f;
    ui_snapshot(app, url, sizeof(url), status, sizeof(status), &progress);

    pthread_mutex_lock(&app->uiMutex);
    snprintf(downloadPath, sizeof(downloadPath), "%s", app->downloadPath);
    snprintf(videoTitle, sizeof(videoTitle), "%s", app->videoTitle);
    pthread_mutex_unlock(&app->uiMutex);

    Vertex *vertices = NULL;
    vkMapMemory(app->device, app->vertexBufferMemory, 0,
                app->vertexCapacity * sizeof(Vertex), 0, (void **)&vertices);
    uint32_t count = 0;

    VkMappedMemoryRange vtxRange = {};
    vtxRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    vtxRange.memory = app->vertexBufferMemory;
    vtxRange.offset = 0;
    vtxRange.size = VK_WHOLE_SIZE;

    float scale = 2.0f * app->densityScale;
    float glyphW = FONT_GLYPH_W * scale;
    float glyphH = FONT_GLYPH_H * scale;
    float screenW = (float)app->swapchainExtent.width;
    float screenH = (float)app->swapchainExtent.height;
    static float lastScreenW = 0, lastScreenH = 0, lastGlyphW = 0;
    if (screenW != lastScreenW || screenH != lastScreenH || glyphW != lastGlyphW) {
        LOGI("update_text_vertices: densityScale=%.2f glyph=%.1fx%.1f screen=%.0fx%.0f",
             app->densityScale, glyphW, glyphH, screenW, screenH);
        lastScreenW = screenW; lastScreenH = screenH; lastGlyphW = glyphW;
    }

    /* Layout constants in dp */
    float marginX = 20.0f * app->densityScale;
    float marginTop = 36.0f * app->densityScale;
    float marginBottom = 48.0f * app->densityScale;
    float lineGap = 6.0f * app->densityScale;
    float sectionGap = 18.0f * app->densityScale;
    float contentW = screenW - marginX * 2.0f;
    if (contentW < 160.0f) contentW = 160.0f;
    float contentX = marginX;

    /* Cyberpunk color palette */
    const float C_CYAN[3]      = {0.10f, 0.95f, 1.00f};
    const float C_CYAN_DIM[3]  = {0.05f, 0.50f, 0.65f};
    const float C_WHITE[3]     = {1.00f, 1.00f, 1.00f};
    const float C_WHITE_DIM[3] = {0.70f, 0.75f, 0.80f};
    const float C_PLACEHOLDER[3] = {0.30f, 0.40f, 0.45f};
    const float C_BG[3]        = {0.06f, 0.06f, 0.10f};
    const float C_BG_LIGHT[3]  = {0.10f, 0.10f, 0.16f};
    const float C_MAGENTA[3]   = {1.00f, 0.20f, 0.80f};
    const float C_GREEN[3]     = {0.20f, 1.00f, 0.40f};
    const float C_RED[3]       = {1.00f, 0.20f, 0.20f};
    const float C_YELLOW[3]    = {1.00f, 0.80f, 0.10f};

    /* Global blink timer for text cursors */
    double nowSec = get_time_seconds();

    /* Pre-compute URL box height for shift calculation */
#ifdef BE_PLATFORM_ANDROID
    float urlBoxH = glyphH * 10 + 20.0f * app->densityScale;
#else
    float urlBoxH = glyphH * 5;  /* desktop: minimum, will expand to fill */
#endif

    /* Compute render shift: keyboard focus auto-scroll + user pan */
    float visibleBottom = screenH - app->keyboardHeightPx - 24.0f * app->densityScale;
    if (visibleBottom < 24.0f * app->densityScale) visibleBottom = 24.0f * app->densityScale;

    /* Auto-scroll to keep focused field above keyboard when requested */
    if (g_input.keyboardVisible && app->keyboardHeightPx > 0.0f && app->uiScrollNeedsFocus) {
        float focusBottom = 0.0f;
        if (g_input.focus == FOCUS_URL) {
            focusBottom = app->uiUrlY1 - app->uiScrollOffsetY;  /* unshifted */
        } else if (g_input.focus == FOCUS_FILENAME) {
            focusBottom = app->uiFilenameY1 - app->uiScrollOffsetY;  /* unshifted */
        }
        float targetShift = 0.0f;
        if (focusBottom > visibleBottom - 24.0f * app->densityScale) {
            targetShift = visibleBottom - 24.0f * app->densityScale - focusBottom;
        }
        app->uiScrollOffsetY = targetShift;
        app->uiScrollNeedsFocus = false;
    }

    /* Clamp scroll offset so content stays reachable */
    ui_clamp_scroll_offset();
    float shiftY = app->uiScrollOffsetY;

    /* Track actual content bottom as we render */
    float contentBottomY = 0.0f;

    /* ========== CORNER DECORATIONS ========== */
    float cornerSize = 14.0f * app->densityScale;
    float cornerThick = 2.0f * app->densityScale;
    float cy = marginTop + shiftY;
    /* Top-left */
    append_rect(vertices, &count, marginX, cy, cornerThick, cornerSize, screenW, screenH, app->mirrorX, C_CYAN_DIM[0], C_CYAN_DIM[1], C_CYAN_DIM[2]);
    append_rect(vertices, &count, marginX, cy, cornerSize, cornerThick, screenW, screenH, app->mirrorX, C_CYAN_DIM[0], C_CYAN_DIM[1], C_CYAN_DIM[2]);
    /* Top-right */
    append_rect(vertices, &count, screenW - marginX - cornerThick, cy, cornerThick, cornerSize, screenW, screenH, app->mirrorX, C_CYAN_DIM[0], C_CYAN_DIM[1], C_CYAN_DIM[2]);
    append_rect(vertices, &count, screenW - marginX - cornerSize, cy, cornerSize, cornerThick, screenW, screenH, app->mirrorX, C_CYAN_DIM[0], C_CYAN_DIM[1], C_CYAN_DIM[2]);
    /* Bottom-left */
    float by = screenH - marginBottom;
    append_rect(vertices, &count, marginX, by - cornerSize, cornerThick, cornerSize, screenW, screenH, app->mirrorX, C_CYAN_DIM[0], C_CYAN_DIM[1], C_CYAN_DIM[2]);
    append_rect(vertices, &count, marginX, by - cornerThick, cornerSize, cornerThick, screenW, screenH, app->mirrorX, C_CYAN_DIM[0], C_CYAN_DIM[1], C_CYAN_DIM[2]);
    /* Bottom-right */
    append_rect(vertices, &count, screenW - marginX - cornerThick, by - cornerSize, cornerThick, cornerSize, screenW, screenH, app->mirrorX, C_CYAN_DIM[0], C_CYAN_DIM[1], C_CYAN_DIM[2]);
    append_rect(vertices, &count, screenW - marginX - cornerSize, by - cornerThick, cornerSize, cornerThick, screenW, screenH, app->mirrorX, C_CYAN_DIM[0], C_CYAN_DIM[1], C_CYAN_DIM[2]);

    /* ========== TITLE ========== */
    float titleY = marginTop + sectionGap + shiftY;
    const char *title = "BGMDWLDR";
    float titleW = string_width(title, glyphW);
    float titleX = (screenW - titleW) * 0.5f;
    if (titleX < marginX) titleX = marginX;
    draw_string(vertices, &count, app->vertexCapacity, title, titleX, titleY,
                glyphW, glyphH, screenW, screenH, app->mirrorX,
                C_CYAN[0], C_CYAN[1], C_CYAN[2]);

    /* Title underline */
    float underlineY = titleY + glyphH + 4.0f * app->densityScale;
    float underlineW = titleW + 24.0f * app->densityScale;
    float underlineX = (screenW - underlineW) * 0.5f;
    append_rect(vertices, &count, underlineX, underlineY, underlineW, 2.0f * app->densityScale,
                screenW, screenH, app->mirrorX, C_CYAN_DIM[0], C_CYAN_DIM[1], C_CYAN_DIM[2]);
    contentBottomY = underlineY + 2.0f * app->densityScale;

    /* ========== FILENAME SECTION ========== */
    float fnLabelY = underlineY + sectionGap;
    const char *fnLabel = "FILENAME";
    float fnLabelW = string_width(fnLabel, glyphW);
    float fnLabelX = (screenW - fnLabelW) * 0.5f;
    draw_string(vertices, &count, app->vertexCapacity, fnLabel, fnLabelX, fnLabelY,
                glyphW, glyphH, screenW, screenH, app->mirrorX,
                C_CYAN_DIM[0], C_CYAN_DIM[1], C_CYAN_DIM[2]);

    /* Filename input box - single line */
    float fnBoxH = glyphH + 16.0f * app->densityScale;
    float fnBoxY = fnLabelY + glyphH + lineGap;
    float fnBoxW = contentW;
    float fnBoxX = contentX;
    float fnBoxPadX = 10.0f * app->densityScale;
    float fnTextX = fnBoxX + fnBoxPadX;
    float fnTextY = fnBoxY + (fnBoxH - glyphH) * 0.5f;
    float fnTextMaxW = fnBoxW - fnBoxPadX * 2.0f;
    if (fnTextMaxW < glyphW) fnTextMaxW = glyphW;
    int fnCharsPerLine = (int)(fnTextMaxW / glyphW);
    if (fnCharsPerLine < 1) fnCharsPerLine = 1;

    /* Filename box background */
    append_rect(vertices, &count, fnBoxX, fnBoxY, fnBoxW, fnBoxH,
                screenW, screenH, app->mirrorX, C_BG[0], C_BG[1], C_BG[2]);
    /* Filename box border */
    append_border(vertices, &count, fnBoxX, fnBoxY, fnBoxW, fnBoxH, 2.0f * app->densityScale,
                  screenW, screenH, app->mirrorX,
                  C_CYAN[0], C_CYAN[1], C_CYAN[2]);

    /* Filename selection highlight */
    if (has_filename_selection()) {
        int fselStart = g_input.filenameField.selStart < g_input.filenameField.selEnd ?
                        g_input.filenameField.selStart : g_input.filenameField.selEnd;
        int fselEnd = g_input.filenameField.selStart < g_input.filenameField.selEnd ?
                      g_input.filenameField.selEnd : g_input.filenameField.selStart;
        float fsx = fnTextX + fselStart * glyphW;
        float fsw = (fselEnd - fselStart) * glyphW;
        append_rect(vertices, &count, fsx, fnTextY, fsw, glyphH,
                    screenW, screenH, app->mirrorX,
                    0.25f, 0.45f, 0.75f);
    }

    /* Filename text */
    draw_string(vertices, &count, app->vertexCapacity,
                g_input.filenameField.buffer, fnTextX, fnTextY,
                glyphW, glyphH, screenW, screenH, app->mirrorX,
                C_WHITE[0], C_WHITE[1], C_WHITE[2]);

    /* Filename cursor (block) - visible when active */
    bool filenameCaretOn = (g_input.filenameActive || g_input.focus == FOCUS_FILENAME);
    if (filenameCaretOn) {
        float caretX = fnTextX + g_input.filenameField.cursorPos * glyphW;
        append_rect(vertices, &count, caretX, fnTextY, glyphW, glyphH,
                    screenW, screenH, app->mirrorX,
                    C_WHITE[0], C_WHITE[1], C_WHITE[2]);
        /* Invert character under cursor */
        if (g_input.filenameField.cursorPos < g_input.filenameField.length) {
            unsigned char c = (unsigned char)g_input.filenameField.buffer[g_input.filenameField.cursorPos];
            if (c >= 32 && c <= 127) {
                int glyphIndex = (int)c - 32;
                int gcol = glyphIndex % FONT_COLS;
                int grow = glyphIndex / FONT_COLS;
                float u0 = (float)(gcol * FONT_GLYPH_W) / (float)FONT_ATLAS_W;
                float v0 = (float)(grow * FONT_GLYPH_H) / (float)FONT_ATLAS_H;
                float u1 = (float)((gcol + 1) * FONT_GLYPH_W) / (float)FONT_ATLAS_W;
                float v1 = (float)((grow + 1) * FONT_GLYPH_H) / (float)FONT_ATLAS_H;
                append_glyph(vertices, &count, caretX, fnTextY, glyphW, glyphH,
                             u0, v0, u1, v1,
                             screenW, screenH, app->mirrorX,
                             C_BG[0], C_BG[1], C_BG[2]);
            }
        }
    }

    /* Store filename bounds for hit testing */
    pthread_mutex_lock(&app->uiMutex);
    app->uiFilenameX0 = fnBoxX;
    app->uiFilenameX1 = fnBoxX + fnBoxW;
    app->uiFilenameY0 = fnBoxY;
    app->uiFilenameY1 = fnBoxY + fnBoxH;
    app->uiFilenameTextX = fnTextX;
    app->uiFilenameTextY = fnTextY;
    app->uiFilenameGlyphW = glyphW;
    app->uiFilenameGlyphH = glyphH;
    app->uiFilenameCharsPerLine = fnCharsPerLine;
    pthread_mutex_unlock(&app->uiMutex);
    g_input.filenameX0 = fnBoxX;
    g_input.filenameY0 = fnBoxY;
    g_input.filenameX1 = fnBoxX + fnBoxW;
    g_input.filenameY1 = fnBoxY + fnBoxH;
    if (fnBoxY + fnBoxH > contentBottomY) contentBottomY = fnBoxY + fnBoxH;

    /* ========== KEEP VIDEO CHECKBOX ========== */
    float checkY = fnBoxY + fnBoxH + lineGap;
    float checkSize = glyphH;
    float checkX = contentX;

    /* Checkbox box background */
    append_rect(vertices, &count, checkX, checkY, checkSize, checkSize,
                screenW, screenH, app->mirrorX, C_BG[0], C_BG[1], C_BG[2]);
    /* Checkbox box border */
    float checkBorderColor[3] = {C_CYAN[0], C_CYAN[1], C_CYAN[2]};
    if (g_input.keepVideoHovered) {
        checkBorderColor[0] = C_MAGENTA[0];
        checkBorderColor[1] = C_MAGENTA[1];
        checkBorderColor[2] = C_MAGENTA[2];
    }
    append_border(vertices, &count, checkX, checkY, checkSize, checkSize, 2.0f * app->densityScale,
                  screenW, screenH, app->mirrorX, checkBorderColor[0], checkBorderColor[1], checkBorderColor[2]);

    /* Checkmark (if checked) */
    if (app->keepVideo) {
        append_rect(vertices, &count, checkX + checkSize*0.25f, checkY + checkSize*0.25f,
                    checkSize*0.5f, checkSize*0.5f,
                    screenW, screenH, app->mirrorX, C_MAGENTA[0], C_MAGENTA[1], C_MAGENTA[2]);
    }

    /* Checkbox label */
    float checkLabelX = checkX + checkSize + 8.0f * app->densityScale;
    draw_string(vertices, &count, app->vertexCapacity, "Keep video (32kbps audio)", checkLabelX, checkY,
                glyphW, glyphH, screenW, screenH, app->mirrorX,
                C_WHITE[0], C_WHITE[1], C_WHITE[2]);

    /* Store checkbox bounds for hit testing */
    pthread_mutex_lock(&app->uiMutex);
    app->uiKeepVideoX0 = checkX;
    app->uiKeepVideoY0 = checkY;
    float checkLabelW = string_width("Keep video (32kbps audio)", glyphW);
    app->uiKeepVideoX1 = checkX + checkSize + 8.0f * app->densityScale + checkLabelW;
    app->uiKeepVideoY1 = checkY + checkSize;
    pthread_mutex_unlock(&app->uiMutex);
    if (checkY + checkSize > contentBottomY) contentBottomY = checkY + checkSize;

    /* ========== URL SECTION ========== */
    float urlLabelY = checkY + checkSize + lineGap;
    const char *urlLabel = "TARGET URL";
    float urlLabelW = string_width(urlLabel, glyphW);
    float urlLabelX = (screenW - urlLabelW) * 0.5f;
    draw_string(vertices, &count, app->vertexCapacity, urlLabel, urlLabelX, urlLabelY,
                glyphW, glyphH, screenW, screenH, app->mirrorX,
                C_CYAN_DIM[0], C_CYAN_DIM[1], C_CYAN_DIM[2]);

    /* URL input box - full width minus margins, multiline tall */
    float urlBoxY = urlLabelY + glyphH + lineGap;
    float urlBoxW = contentW;
    float urlBoxX = contentX;
    float urlBoxPadX = 10.0f * app->densityScale;
    float urlBoxPadY = 10.0f * app->densityScale;
    float urlTextY = urlBoxY + urlBoxPadY;
    float urlTextMaxW = urlBoxW - urlBoxPadX * 2.0f;
    if (urlTextMaxW < glyphW) urlTextMaxW = glyphW;
    int urlCharsPerLine = (int)(urlTextMaxW / glyphW);
    if (urlCharsPerLine < 1) urlCharsPerLine = 1;

#ifndef BE_PLATFORM_ANDROID
    /* Desktop: expand URL box to fill remaining vertical space */
    {
        /* Footer area */
        float bottomReserve = marginBottom + cornerSize + glyphH + 8.0f * app->densityScale;
        /* Helper text + gap to footer */
        bottomReserve += glyphH + 4.0f * app->densityScale;
        /* Hint + gap to helper */
        bottomReserve += glyphH + lineGap + sectionGap;
        /* Download path (multi-line estimate) */
        if (strlen(downloadPath) > 0) {
            float pathGlyphW = glyphW * 0.75f;
            float pathGlyphH = glyphH * 0.75f;
            int pathCharsPerLine = (int)(contentW / pathGlyphW);
            if (pathCharsPerLine < 1) pathCharsPerLine = 1;
            int pathLines = (int)((strlen(downloadPath) + pathCharsPerLine - 1) / pathCharsPerLine);
            if (pathLines < 1) pathLines = 1;
            bottomReserve += pathLines * (pathGlyphH + 2.0f) + lineGap;
        }
        /* Status (multi-line estimate) */
        {
            char statusText[128];
            snprintf(statusText, sizeof(statusText), "%.120s", status);
            if (strlen(statusText) == 0) snprintf(statusText, sizeof(statusText), "Ready");
            float statusWrapWidth = contentW - (8.0f * app->densityScale + 8.0f * app->densityScale);
            int statusCharsPerLine = (int)(statusWrapWidth / glyphW);
            if (statusCharsPerLine < 1) statusCharsPerLine = 1;
            int statusLines = (int)((strlen(statusText) + statusCharsPerLine - 1) / statusCharsPerLine);
            if (statusLines < 1) statusLines = 1;
            bottomReserve += statusLines * (glyphH + 2.0f) + lineGap;
        }
        /* Progress bar */
        if (progress > 0.0f || app->workerRunning) {
            bottomReserve += glyphH + lineGap + 10.0f * app->densityScale + sectionGap;
        }
        /* Video title */
        if (videoTitle[0] != '\0') {
            int titleCharsPerLine = (int)(contentW / glyphW);
            if (titleCharsPerLine < 1) titleCharsPerLine = 1;
            int titleLines = (int)((strlen(videoTitle) + titleCharsPerLine - 1) / titleCharsPerLine);
            if (titleLines < 1) titleLines = 1;
            if (titleLines > 2) titleLines = 2;  /* cap estimate at 2 lines */
            bottomReserve += glyphH + lineGap + titleLines * (glyphH + 2.0f) + sectionGap;
        }
        /* Gap after URL box */
        bottomReserve += sectionGap;

        urlBoxH = screenH - urlBoxY - bottomReserve;
        if (urlBoxH < glyphH * 5) urlBoxH = glyphH * 5;
        if (urlBoxH > screenH * 0.75f) urlBoxH = screenH * 0.75f;
        urlTextY = urlBoxY + urlBoxPadY;
    }
#endif

    /* URL box background */
    append_rect(vertices, &count, urlBoxX, urlBoxY, urlBoxW, urlBoxH,
                screenW, screenH, app->mirrorX, C_BG[0], C_BG[1], C_BG[2]);
    /* URL box border */
    append_border(vertices, &count, urlBoxX, urlBoxY, urlBoxW, urlBoxH, 2.0f * app->densityScale,
                  screenW, screenH, app->mirrorX, C_CYAN_DIM[0], C_CYAN_DIM[1], C_CYAN_DIM[2]);

    /* URL text with caret */
    bool caretOn = app->inputActive;

    char urlDisplay[256];
    float urlTextX = urlBoxX + urlBoxPadX;

    /* Selection highlight (drawn behind text) */
    if (tf_has_selection(&g_input.urlField)) {
        int selStart = g_input.urlField.selStart < g_input.urlField.selEnd ? g_input.urlField.selStart : g_input.urlField.selEnd;
        int selEnd = g_input.urlField.selStart < g_input.urlField.selEnd ? g_input.urlField.selEnd : g_input.urlField.selStart;
        int startLine = selStart / urlCharsPerLine;
        int endLine = selEnd / urlCharsPerLine;
        float lineH = glyphH + 2.0f;
        for (int line = startLine; line <= endLine; line++) {
            int lineStart = line * urlCharsPerLine;
            int lineEnd = lineStart + urlCharsPerLine;
            int s = selStart > lineStart ? selStart : lineStart;
            int e = selEnd < lineEnd ? selEnd : lineEnd;
            if (s >= e) continue;
            float sx = urlTextX + (s - lineStart) * glyphW;
            float sy = urlTextY + line * lineH;
            float sw = (e - s) * glyphW;
            append_rect(vertices, &count, sx, sy, sw, glyphH,
                        screenW, screenH, app->mirrorX,
                        0.25f, 0.45f, 0.75f);
        }
    }

    if (strlen(url) == 0) {
        if (app->inputActive) {
            if (caretOn) {
                /* Full-width block cursor on empty line */
                append_rect(vertices, &count, urlTextX, urlTextY, glyphW, glyphH,
                            screenW, screenH, app->mirrorX,
                            C_WHITE[0], C_WHITE[1], C_WHITE[2]);
            } else {
                snprintf(urlDisplay, sizeof(urlDisplay), "%s", " ");
                draw_string(vertices, &count, app->vertexCapacity, urlDisplay, urlTextX, urlTextY,
                            glyphW, glyphH, screenW, screenH, app->mirrorX,
                            C_WHITE[0], C_WHITE[1], C_WHITE[2]);
            }
        } else {
            const char *placeholder = "Tap to enter URL...";
            int maxPlaceChars = (int)((urlBoxW - urlBoxPadX * 2.0f) / glyphW);
            if (maxPlaceChars < 4) maxPlaceChars = 4;
            if ((int)strlen(placeholder) > maxPlaceChars) {
                snprintf(urlDisplay, sizeof(urlDisplay), "%.*s...", maxPlaceChars - 3, placeholder);
            } else {
                snprintf(urlDisplay, sizeof(urlDisplay), "%s", placeholder);
            }
            draw_string(vertices, &count, app->vertexCapacity, urlDisplay, urlTextX, urlTextY,
                        glyphW, glyphH, screenW, screenH, app->mirrorX,
                        C_PLACEHOLDER[0], C_PLACEHOLDER[1], C_PLACEHOLDER[2]);
        }
    } else {
        snprintf(urlDisplay, sizeof(urlDisplay), "%s", url);
        draw_wrapped_text(vertices, &count, app->vertexCapacity,
                          urlDisplay, urlTextX, urlTextY,
                          glyphW, glyphH, urlTextMaxW,
                          screenW, screenH, app->mirrorX,
                          C_WHITE[0], C_WHITE[1], C_WHITE[2]);
        if (caretOn && app->inputActive) {
            int cursorLine = g_input.urlField.cursorPos / urlCharsPerLine;
            int cursorCol = g_input.urlField.cursorPos % urlCharsPerLine;
            if (cursorCol == 0 && g_input.urlField.cursorPos > 0 && g_input.urlField.cursorPos == g_input.urlField.length) {
                cursorLine--;
                cursorCol = urlCharsPerLine;
            }
            float caretX = urlTextX + cursorCol * glyphW;
            float caretY = urlTextY + cursorLine * (glyphH + 2.0f);

            /* Full-width block cursor */
            append_rect(vertices, &count, caretX, caretY, glyphW, glyphH,
                        screenW, screenH, app->mirrorX,
                        C_WHITE[0], C_WHITE[1], C_WHITE[2]);

            /* Invert character under cursor */
            if (g_input.urlField.cursorPos < g_input.urlField.length) {
                unsigned char c = (unsigned char)urlDisplay[g_input.urlField.cursorPos];
                if (c >= 32 && c <= 127) {
                    int glyphIndex = (int)c - 32;
                    int gcol = glyphIndex % FONT_COLS;
                    int grow = glyphIndex / FONT_COLS;
                    float u0 = (float)(gcol * FONT_GLYPH_W) / (float)FONT_ATLAS_W;
                    float v0 = (float)(grow * FONT_GLYPH_H) / (float)FONT_ATLAS_H;
                    float u1 = (float)((gcol + 1) * FONT_GLYPH_W) / (float)FONT_ATLAS_W;
                    float v1 = (float)((grow + 1) * FONT_GLYPH_H) / (float)FONT_ATLAS_H;
                    append_glyph(vertices, &count, caretX, caretY, glyphW, glyphH,
                                 u0, v0, u1, v1,
                                 screenW, screenH, app->mirrorX,
                                 C_BG[0], C_BG[1], C_BG[2]);
                }
            }
        }
    }

    /* Update touch detection bounds and text metrics */
    pthread_mutex_lock(&app->uiMutex);
    app->uiUrlX0 = urlBoxX;
    app->uiUrlX1 = urlBoxX + urlBoxW;
    app->uiUrlY0 = urlBoxY;
    app->uiUrlY1 = urlBoxY + urlBoxH;
    app->uiUrlTextX = urlTextX;
    app->uiUrlTextY = urlTextY;
    app->uiUrlGlyphW = glyphW;
    app->uiUrlGlyphH = glyphH;
    app->uiUrlCharsPerLine = urlCharsPerLine;
    pthread_mutex_unlock(&app->uiMutex);
    if (urlBoxY + urlBoxH > contentBottomY) contentBottomY = urlBoxY + urlBoxH;

    /* ========== VIDEO TITLE ========== */

    float sectionY = urlBoxY + urlBoxH + sectionGap;
    if (videoTitle[0] != '\0') {
        float titleLabelY = sectionY;
        draw_string(vertices, &count, app->vertexCapacity, "TITLE", contentX, titleLabelY,
                    glyphW, glyphH, screenW, screenH, app->mirrorX,
                    C_CYAN_DIM[0], C_CYAN_DIM[1], C_CYAN_DIM[2]);
        float titleTextY = titleLabelY + glyphH + lineGap;
        draw_wrapped_text(vertices, &count, app->vertexCapacity,
                          videoTitle, contentX, titleTextY,
                          glyphW, glyphH, contentW,
                          screenW, screenH, app->mirrorX,
                          C_WHITE[0], C_WHITE[1], C_WHITE[2]);
        sectionY = titleTextY + glyphH + sectionGap;
        if (sectionY > contentBottomY) contentBottomY = sectionY;
    }

    /* ========== PROGRESS BAR ========== */
    if (progress > 0.0f || app->workerRunning) {
        float progressLabelY = sectionY;
        const char *progressLabel = "PROGRESS";
        draw_string(vertices, &count, app->vertexCapacity, progressLabel, contentX, progressLabelY,
                    glyphW, glyphH, screenW, screenH, app->mirrorX,
                    C_CYAN_DIM[0], C_CYAN_DIM[1], C_CYAN_DIM[2]);

        float progressBarY = progressLabelY + glyphH + lineGap;
        float progressBarH = 10.0f * app->densityScale;
        /* Background */
        append_rect(vertices, &count, contentX, progressBarY, contentW, progressBarH,
                    screenW, screenH, app->mirrorX, C_BG_LIGHT[0], C_BG_LIGHT[1], C_BG_LIGHT[2]);
        /* Border */
        append_border(vertices, &count, contentX, progressBarY, contentW, progressBarH, 1.0f * app->densityScale,
                      screenW, screenH, app->mirrorX, C_CYAN_DIM[0], C_CYAN_DIM[1], C_CYAN_DIM[2]);
        /* Fill */
        float fillW = contentW * progress;
        if (fillW > 0.0f) {
            append_rect(vertices, &count, contentX, progressBarY, fillW, progressBarH,
                        screenW, screenH, app->mirrorX, C_MAGENTA[0], C_MAGENTA[1], C_MAGENTA[2]);
        }
        /* Percentage */
        char pctStr[16];
        snprintf(pctStr, sizeof(pctStr), "%.0f%%", progress * 100.0f);
        float pctW = string_width(pctStr, glyphW);
        float pctX = contentX + contentW - pctW;
        draw_string(vertices, &count, app->vertexCapacity, pctStr, pctX, progressLabelY,
                    glyphW, glyphH, screenW, screenH, app->mirrorX,
                    C_MAGENTA[0], C_MAGENTA[1], C_MAGENTA[2]);

        sectionY = progressBarY + progressBarH + sectionGap;
        if (sectionY > contentBottomY) contentBottomY = sectionY;
    }

    /* ========== STATUS ========== */
    float statusY = sectionY;
    const float *statusColor = C_WHITE_DIM;
    if (app->workerRunning) {
        statusColor = C_YELLOW;
    } else if (strlen(status) > 0) {
        if (strstr(status, "Success") || strstr(status, "Saved") || strstr(status, "Complete") || strstr(status, "Done")) {
            statusColor = C_GREEN;
        } else if (strstr(status, "Error") || strstr(status, "Fail") || strstr(status, "Invalid") || strstr(status, "timeout")) {
            statusColor = C_RED;
        }
    }

    /* Status indicator dot */
    float dotSize = 8.0f * app->densityScale;
    float dotY = statusY + (glyphH - dotSize) * 0.5f;
    append_rect(vertices, &count, contentX, dotY, dotSize, dotSize,
                screenW, screenH, app->mirrorX, statusColor[0], statusColor[1], statusColor[2]);

    /* Status text with wrapping */
    char statusText[128];
    snprintf(statusText, sizeof(statusText), "%.120s", status);
    if (strlen(statusText) == 0) {
        snprintf(statusText, sizeof(statusText), "Ready");
    }
    float statusTextX = contentX + dotSize + 8.0f * app->densityScale;
    float statusWrapWidth = contentW - (dotSize + 8.0f * app->densityScale);
    float statusBottomY = draw_wrapped_text(vertices, &count, app->vertexCapacity,
                                            statusText, statusTextX, statusY,
                                            glyphW, glyphH, statusWrapWidth,
                                            screenW, screenH, app->mirrorX,
                                            C_WHITE_DIM[0], C_WHITE_DIM[1], C_WHITE_DIM[2]);
    if (statusBottomY > contentBottomY) contentBottomY = statusBottomY;

    /* ========== DOWNLOAD PATH ========== */
    float pathY = statusBottomY + lineGap;
    if (strlen(downloadPath) > 0) {
        float pathWrapWidth = contentW;
        float pathGlyphW = glyphW * 0.75f;
        float pathGlyphH = glyphH * 0.75f;

        /* Selection highlight (drawn behind text) */
        if (g_input.pathSelected) {
            /* Compute actual height after draw */
            float pathH = pathGlyphH + 2.0f; /* at least one line */
            append_rect(vertices, &count, contentX, pathY, pathWrapWidth, pathH,
                        screenW, screenH, app->mirrorX,
                        0.25f, 0.45f, 0.75f);
        }

        float pathTopY = pathY;
        float pathBottomY = draw_wrapped_text(vertices, &count, app->vertexCapacity,
                                              downloadPath, contentX, pathY,
                                              pathGlyphW, pathGlyphH, pathWrapWidth,
                                              screenW, screenH, app->mirrorX,
                                              C_GREEN[0], C_GREEN[1], C_GREEN[2]);

        /* Track bounds for click detection using actual rendered extents */
        g_input.pathX0 = contentX;
        g_input.pathY0 = pathTopY;
        g_input.pathX1 = contentX + pathWrapWidth;
        g_input.pathY1 = pathBottomY;

        pathY = pathBottomY + lineGap;
        if (pathY > contentBottomY) contentBottomY = pathY;
    } else {
        g_input.pathX0 = g_input.pathY0 = g_input.pathX1 = g_input.pathY1 = -1.0f;
        g_input.pathSelected = false;
    }

    /* ========== ACTION HINT ========== */
    float hintY = pathY + sectionGap;
    const char *hint = app->workerRunning ? "WORKING..." : "PRESS ENTER TO START";
    const float *hintColor = app->workerRunning ? C_YELLOW : C_CYAN_DIM;
    float hintW = string_width(hint, glyphW);
    float hintX = (screenW - hintW) * 0.5f;
    if (hintX < marginX) hintX = marginX;
    draw_string(vertices, &count, app->vertexCapacity, hint, hintX, hintY,
                glyphW, glyphH, screenW, screenH, app->mirrorX,
                hintColor[0], hintColor[1], hintColor[2]);
    if (hintY + glyphH > contentBottomY) contentBottomY = hintY + glyphH;

    /* ========== FOOTER (compute Y early for clamping) ========== */
    float footerY = screenH - marginBottom - cornerSize - glyphH - 8.0f * app->densityScale;

    /* ========== HELPER TEXT ========== */
    float helperY = hintY + glyphH + lineGap;
    /* Clamp so helper text never overlaps footer */
    float minHelperY = footerY - glyphH - 4.0f * app->densityScale;
    if (helperY > minHelperY) helperY = minHelperY;
    if (helperY < hintY + glyphH) helperY = hintY + glyphH;
    const char *helperText = "Feel free to use this like a normal UI";
    float helperW = string_width(helperText, glyphW);
    float helperX = (screenW - helperW) * 0.5f;
    if (helperX < marginX) helperX = marginX;
    draw_string(vertices, &count, app->vertexCapacity, helperText, helperX, helperY,
                glyphW, glyphH, screenW, screenH, app->mirrorX,
                C_CYAN_DIM[0], C_CYAN_DIM[1], C_CYAN_DIM[2]);
    if (helperY + glyphH > contentBottomY) contentBottomY = helperY + glyphH;

    /* ========== FOOTER ========== */
    const char *footerFull = "v1.0  YOUTUBE BGM DOWNLOADER";
    char footerDisplay[64];
    int maxFooterChars = (int)((screenW - marginX * 2.0f) / glyphW);
    if (maxFooterChars < 1) maxFooterChars = 1;
    if ((int)strlen(footerFull) > maxFooterChars) {
        snprintf(footerDisplay, sizeof(footerDisplay), "%.*s...", maxFooterChars - 3, footerFull);
    } else {
        snprintf(footerDisplay, sizeof(footerDisplay), "%s", footerFull);
    }
    float footerW = string_width(footerDisplay, glyphW);
    float footerX = (screenW - footerW) * 0.5f;
    draw_string(vertices, &count, app->vertexCapacity, footerDisplay, footerX, footerY,
                glyphW, glyphH, screenW, screenH, app->mirrorX,
                C_CYAN_DIM[0], C_CYAN_DIM[1], C_CYAN_DIM[2]);
    if (footerY + glyphH > contentBottomY) contentBottomY = footerY + glyphH;

    /* Store unshifted content bottom for scroll clamping */
    app->uiContentBottom = contentBottomY - shiftY;

    /* ========== CONTEXT MENU ========== */
    draw_context_menu(vertices, &count, app->vertexCapacity,
                      screenW, screenH, app->mirrorX,
                      glyphW, glyphH, app->densityScale);

    vkFlushMappedMemoryRanges(app->device, 1, &vtxRange);
    vkUnmapMemory(app->device, app->vertexBufferMemory);
    app->vertexCount = count;
}

/**
 * Native AInputEvent handler - DEPRECATED
 * All input handling now goes through Java dispatchTouchEvent/dispatchKeyEvent
 * which forward to nativeOnTouch/nativeOnKey JNI methods.
 * This function remains for compatibility but returns 0 (not consumed).
 */

bool create_pipeline(VulkanApp *app, const uint8_t *vert_data, size_t vert_size,
                     const uint8_t *frag_data, size_t frag_size) {
    ShaderBlob vert = { (uint8_t *)vert_data, vert_size };
    ShaderBlob frag = { (uint8_t *)frag_data, frag_size };
    if (!vert.data || !frag.data) {
                return false;
    }

    VkShaderModule vertModule = create_shader_module(app, vert.data, vert.size);
    VkShaderModule fragModule = create_shader_module(app, frag.data, frag.size);
    if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE) {
        return false;
    }

    VkPipelineShaderStageCreateInfo shaderStages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertModule,
            .pName = "main"
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragModule,
            .pName = "main"
        }
    };

    VkVertexInputBindingDescription binding = {
        .binding = 0,
        .stride = sizeof(Vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };
    VkVertexInputAttributeDescription attributes[3] = {
        {
            .location = 0,
            .binding = 0,
            .format = VK_FORMAT_R32G32_SFLOAT,
            .offset = (uint32_t)offsetof(Vertex, pos)
        },
        {
            .location = 1,
            .binding = 0,
            .format = VK_FORMAT_R32G32_SFLOAT,
            .offset = (uint32_t)offsetof(Vertex, uv)
        },
        {
            .location = 2,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offset = (uint32_t)offsetof(Vertex, color)
        }
    };
    VkPipelineVertexInputStateCreateInfo vertexInput = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 3,
        .pVertexAttributeDescriptions = attributes
    };
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    };

    VkViewport viewport = {
        .x = 0.0f,
        .y = 0.0f,
        .width = (float)app->swapchainExtent.width,
        .height = (float)app->swapchainExtent.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    VkRect2D scissor = {
        .offset = {0, 0},
        .extent = app->swapchainExtent
    };
    VkPipelineViewportStateCreateInfo viewportState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor
    };

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f
    };

    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };

    VkPipelineColorBlendAttachmentState colorBlendAttachment = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                          VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT |
                          VK_COLOR_COMPONENT_A_BIT
    };
    VkPipelineColorBlendStateCreateInfo colorBlending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &colorBlendAttachment
    };

    VkPushConstantRange pushRange = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(int)
    };
    VkPipelineLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &app->descriptorSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushRange
    };
    VkResult result = vkCreatePipelineLayout(app->device, &layoutInfo, NULL, &app->pipelineLayout);
    if (result != VK_SUCCESS) {
        LOGE("vkCreatePipelineLayout failed: %d", result);
        return false;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = shaderStages,
        .pVertexInputState = &vertexInput,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisample,
        .pColorBlendState = &colorBlending,
        .layout = app->pipelineLayout,
        .renderPass = app->renderPass,
        .subpass = 0
    };
    result = vkCreateGraphicsPipelines(app->device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL,
                                       &app->pipeline);
    vkDestroyShaderModule(app->device, vertModule, NULL);
    vkDestroyShaderModule(app->device, fragModule, NULL);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateGraphicsPipelines failed: %d", result);
        return false;
    }
    return true;
}


bool create_framebuffers(VulkanApp *app) {
    app->framebuffers = (VkFramebuffer *)malloc(sizeof(VkFramebuffer) * app->imageCount);
    for (uint32_t i = 0; i < app->imageCount; ++i) {
        VkImageView attachments[] = { app->imageViews[i] };
        VkFramebufferCreateInfo fbInfo = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = app->renderPass,
            .attachmentCount = 1,
            .pAttachments = attachments,
            .width = app->swapchainExtent.width,
            .height = app->swapchainExtent.height,
            .layers = 1
        };
        VkResult result = vkCreateFramebuffer(app->device, &fbInfo, NULL, &app->framebuffers[i]);
        if (result != VK_SUCCESS) {
            LOGE("vkCreateFramebuffer failed: %d", result);
            return false;
        }
    }
    return true;
}


bool create_command_pool_and_buffers(VulkanApp *app) {
    VkCommandPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = app->graphicsQueueFamily
    };
    VkResult result = vkCreateCommandPool(app->device, &poolInfo, NULL, &app->commandPool);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateCommandPool failed: %d", result);
        return false;
    }

    app->commandBuffers = (VkCommandBuffer *)malloc(sizeof(VkCommandBuffer) * app->imageCount);
    VkCommandBufferAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = app->commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = app->imageCount
    };
    result = vkAllocateCommandBuffers(app->device, &allocInfo, app->commandBuffers);
    if (result != VK_SUCCESS) {
        LOGE("vkAllocateCommandBuffers failed: %d", result);
        return false;
    }

    return true;
}


void record_command_buffers(VulkanApp *app) {
    VkDeviceSize offsets[] = {0};
    for (uint32_t i = 0; i < app->imageCount; ++i) {
        VkCommandBufferBeginInfo beginInfo = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
        };
        vkBeginCommandBuffer(app->commandBuffers[i], &beginInfo);
        VkClearValue clearColor = { .color = {{0.02f, 0.02f, 0.03f, 1.0f}} };
        VkRenderPassBeginInfo renderPassInfo = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = app->renderPass,
            .framebuffer = app->framebuffers[i],
            .renderArea = {
                .offset = {0, 0},
                .extent = app->swapchainExtent
            },
            .clearValueCount = 1,
            .pClearValues = &clearColor
        };
        vkCmdBeginRenderPass(app->commandBuffers[i], &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(app->commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, app->pipeline);

        /* Text / UI rendering (mode = 0: R channel as alpha) */
        int mode = 0;
        vkCmdPushConstants(app->commandBuffers[i], app->pipelineLayout,
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(int), &mode);
        vkCmdBindDescriptorSets(app->commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                app->pipelineLayout, 0, 1, &app->descriptorSet, 0, NULL);
        vkCmdBindVertexBuffers(app->commandBuffers[i], 0, 1, &app->vertexBuffer, offsets);
        vkCmdDraw(app->commandBuffers[i], app->vertexCount, 1, 0, 0);

        /* Album art rendering (mode = 1: RGBA texture) */
        if (app->albumArtVisible && app->albumArtVertexCount > 0) {
            mode = 1;
            vkCmdPushConstants(app->commandBuffers[i], app->pipelineLayout,
                               VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(int), &mode);
            vkCmdBindDescriptorSets(app->commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    app->pipelineLayout, 0, 1, &app->albumArtDescriptorSet, 0, NULL);
            vkCmdDraw(app->commandBuffers[i], app->albumArtVertexCount, 1, app->albumArtVertexOffset, 0);
        }

        vkCmdEndRenderPass(app->commandBuffers[i]);
        vkEndCommandBuffer(app->commandBuffers[i]);
    }
}


bool create_sync_objects(VulkanApp *app) {
    VkSemaphoreCreateInfo semInfo = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkResult result = vkCreateSemaphore(app->device, &semInfo, NULL, &app->imageAvailableSemaphore);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateSemaphore failed: %d", result);
        return false;
    }
    result = vkCreateSemaphore(app->device, &semInfo, NULL, &app->renderFinishedSemaphore);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateSemaphore failed: %d", result);
        return false;
    }
    VkFenceCreateInfo fenceInfo = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT
    };
    result = vkCreateFence(app->device, &fenceInfo, NULL, &app->inFlightFences[0]);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateFence[0] failed: %d", result);
        return false;
    }
    fenceInfo.flags = 0; /* second fence starts unsignaled */
    result = vkCreateFence(app->device, &fenceInfo, NULL, &app->inFlightFences[1]);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateFence[1] failed: %d", result);
        return false;
    }
    app->currentFenceIndex = 0;
    return true;
}


void cleanup_swapchain(VulkanApp *app) {
    if (app->device == VK_NULL_HANDLE) {
        return;
    }
    if (app->framebuffers) {
        for (uint32_t i = 0; i < app->imageCount; ++i) {
            if (app->framebuffers[i] != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(app->device, app->framebuffers[i], NULL);
            }
        }
        free(app->framebuffers);
        app->framebuffers = NULL;
    }
    if (app->commandBuffers) {
        vkFreeCommandBuffers(app->device, app->commandPool, app->imageCount, app->commandBuffers);
        free(app->commandBuffers);
        app->commandBuffers = NULL;
    }
    if (app->commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(app->device, app->commandPool, NULL);
        app->commandPool = VK_NULL_HANDLE;
    }
    if (app->pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(app->device, app->pipeline, NULL);
        app->pipeline = VK_NULL_HANDLE;
    }
    if (app->pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(app->device, app->pipelineLayout, NULL);
        app->pipelineLayout = VK_NULL_HANDLE;
    }
    if (app->renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(app->device, app->renderPass, NULL);
        app->renderPass = VK_NULL_HANDLE;
    }
    if (app->imageViews) {
        for (uint32_t i = 0; i < app->imageCount; ++i) {
            if (app->imageViews[i] != VK_NULL_HANDLE) {
                vkDestroyImageView(app->device, app->imageViews[i], NULL);
            }
        }
        free(app->imageViews);
        app->imageViews = NULL;
    }
    if (app->swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(app->device, app->swapchain, NULL);
        app->swapchain = VK_NULL_HANDLE;
    }
    if (app->images) {
        free(app->images);
        app->images = NULL;
    }
    app->imageCount = 0;
}


void cleanup_device(VulkanApp *app) {
    cleanup_swapchain(app);
    if (app->vertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(app->device, app->vertexBuffer, NULL);
        app->vertexBuffer = VK_NULL_HANDLE;
    }
    if (app->vertexBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(app->device, app->vertexBufferMemory, NULL);
        app->vertexBufferMemory = VK_NULL_HANDLE;
    }
    if (app->fontSampler != VK_NULL_HANDLE) {
        vkDestroySampler(app->device, app->fontSampler, NULL);
        app->fontSampler = VK_NULL_HANDLE;
    }
    if (app->fontImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(app->device, app->fontImageView, NULL);
        app->fontImageView = VK_NULL_HANDLE;
    }
    if (app->fontImage != VK_NULL_HANDLE) {
        vkDestroyImage(app->device, app->fontImage, NULL);
        app->fontImage = VK_NULL_HANDLE;
    }
    if (app->fontImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(app->device, app->fontImageMemory, NULL);
        app->fontImageMemory = VK_NULL_HANDLE;
    }
    if (app->albumArtView != VK_NULL_HANDLE) {
        vkDestroyImageView(app->device, app->albumArtView, NULL);
        app->albumArtView = VK_NULL_HANDLE;
    }
    if (app->albumArtImage != VK_NULL_HANDLE) {
        vkDestroyImage(app->device, app->albumArtImage, NULL);
        app->albumArtImage = VK_NULL_HANDLE;
    }
    if (app->albumArtMemory != VK_NULL_HANDLE) {
        vkFreeMemory(app->device, app->albumArtMemory, NULL);
        app->albumArtMemory = VK_NULL_HANDLE;
    }
    if (app->albumArtSampler != VK_NULL_HANDLE) {
        vkDestroySampler(app->device, app->albumArtSampler, NULL);
        app->albumArtSampler = VK_NULL_HANDLE;
    }
    if (app->descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(app->device, app->descriptorPool, NULL);
        app->descriptorPool = VK_NULL_HANDLE;
    }
    if (app->descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(app->device, app->descriptorSetLayout, NULL);
        app->descriptorSetLayout = VK_NULL_HANDLE;
    }
    if (app->imageAvailableSemaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(app->device, app->imageAvailableSemaphore, NULL);
        app->imageAvailableSemaphore = VK_NULL_HANDLE;
    }
    if (app->renderFinishedSemaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(app->device, app->renderFinishedSemaphore, NULL);
        app->renderFinishedSemaphore = VK_NULL_HANDLE;
    }
    if (app->inFlightFences[0] != VK_NULL_HANDLE) {
        vkDestroyFence(app->device, app->inFlightFences[0], NULL);
        app->inFlightFences[0] = VK_NULL_HANDLE;
    }
    if (app->inFlightFences[1] != VK_NULL_HANDLE) {
        vkDestroyFence(app->device, app->inFlightFences[1], NULL);
        app->inFlightFences[1] = VK_NULL_HANDLE;
    }
    if (app->device != VK_NULL_HANDLE) {
        vkDestroyDevice(app->device, NULL);
        app->device = VK_NULL_HANDLE;
    }
    if (app->surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(app->instance, app->surface, NULL);
        app->surface = VK_NULL_HANDLE;
    }
    if (app->instance != VK_NULL_HANDLE) {
        vkDestroyInstance(app->instance, NULL);
        app->instance = VK_NULL_HANDLE;
    }
    if (app->vertShaderData) {
        free((void *)app->vertShaderData);
        app->vertShaderData = NULL;
    }
    if (app->fragShaderData) {
        free((void *)app->fragShaderData);
        app->fragShaderData = NULL;
    }
}


bool vulkan_ui_init(VulkanApp *app, VkInstance instance, VkSurfaceKHR surface,
                    const uint8_t *vert_data, size_t vert_size,
                    const uint8_t *frag_data, size_t frag_size) {
    app->instance = instance;
    app->surface = surface;
    app->vertShaderData = (const uint8_t *)malloc(vert_size);
    if (app->vertShaderData) memcpy((void *)app->vertShaderData, vert_data, vert_size);
    app->vertShaderSize = vert_size;
    app->fragShaderData = (const uint8_t *)malloc(frag_size);
    if (app->fragShaderData) memcpy((void *)app->fragShaderData, frag_data, frag_size);
    app->fragShaderSize = frag_size;
    if (!app->instance || !app->surface) {
        return false;
    }
    if (!pick_device_and_queue(app)) {
        return false;
    }
    if (!create_swapchain(app)) {
        return false;
    }
    if (!create_render_pass(app)) {
        return false;
    }
    if (!create_command_pool_and_buffers(app)) {
        return false;
    }
    if (!create_font_resources(app)) {
        return false;
    }
    if (!create_pipeline(app, vert_data, vert_size, frag_data, frag_size)) {
        return false;
    }
    if (!create_vertex_buffer(app)) {
        return false;
    }
    if (!create_framebuffers(app)) {
        return false;
    }
    app->uiScrollOffsetY = 0.0f;
    app->uiScrollNeedsFocus = false;
    app->uiContentBottom = 0.0f;
    /* Set default filename: $TITLE.m4a */
    strncpy(g_input.filenameField.buffer, "$TITLE.m4a",
            sizeof(g_input.filenameField.buffer) - 1);
    g_input.filenameField.buffer[sizeof(g_input.filenameField.buffer) - 1] = '\0';
    g_input.filenameField.length = (int)strlen(g_input.filenameField.buffer);
    g_input.filenameField.cursorPos = g_input.filenameField.length;
    g_input.filenameField.selStart = -1;
    g_input.filenameField.selEnd = -1;
    g_input.filenameField.isDragging = false;
    g_input.filenameField.dragAnchor = 0;
    g_input.urlField.buffer[0] = '\0';
    g_input.urlField.length = 0;
    g_input.urlField.cursorPos = 0;
    g_input.urlField.selStart = -1;
    g_input.urlField.selEnd = -1;
    g_input.urlField.isDragging = false;
    g_input.urlField.dragAnchor = 0;

    update_text_vertices(app);
    record_command_buffers(app);
    if (!create_sync_objects(app)) {
        return false;
    }
    app->ready = true;
    return true;
}


bool recreate_swapchain(VulkanApp *app) {
#ifdef BE_PLATFORM_ANDROID
    if (!app->window) {
        return false;
    }
#endif
    LOGI("recreate_swapchain: old=%dx%d", app->swapchainExtent.width, app->swapchainExtent.height);
    vkDeviceWaitIdle(app->device);
    cleanup_swapchain(app);
    if (!create_swapchain(app)) {
        return false;
    }
    if (!create_render_pass(app)) {
        return false;
    }
    if (!create_command_pool_and_buffers(app)) {
        return false;
    }
    if (!create_pipeline(app, app->vertShaderData, app->vertShaderSize, app->fragShaderData, app->fragShaderSize)) {
        return false;
    }
    if (!create_framebuffers(app)) {
        return false;
    }
    update_text_vertices(app);
    record_command_buffers(app);
    return true;
}


void draw_frame(VulkanApp *app) {
    if (!app->ready) {
        return;
    }
    /* Ping-pong between two fences.  We wait on the fence from the frame
     * before last, reset it, then submit with it so each fence is only
     * ever in-flight for one frame at a time. */
    VkFence waitFence = app->inFlightFences[app->currentFenceIndex];
    VkFence signalFence = app->inFlightFences[1 - app->currentFenceIndex];

    VkResult fenceResult = vkWaitForFences(app->device, 1, &waitFence, VK_TRUE, UINT64_MAX);
    if (fenceResult != VK_SUCCESS) {
        LOGE("vkWaitForFences failed: %d", fenceResult);
        if (fenceResult == VK_ERROR_DEVICE_LOST) {
            LOGE("GPU device lost - attempting recovery...");
            recreate_swapchain(app);
        }
        return;
    }

    if (g_testText) {
        update_test_text_vertices(app, g_testText);
    } else {
        update_text_vertices(app);
    }
    record_command_buffers(app);

    uint32_t imageIndex = 0;
    /* Use a 1-second timeout so we don't hang forever when the window is
     * minimized or the surface is temporarily unavailable on Windows. */
    VkResult result = vkAcquireNextImageKHR(app->device, app->swapchain, 1000000000ULL,
                                            app->imageAvailableSemaphore, VK_NULL_HANDLE,
                                            &imageIndex);
    if (result == VK_TIMEOUT) {
        /* Surface temporarily unavailable (e.g. window minimized). Skip
         * this frame without submitting anything. */
        return;
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        LOGI("vkAcquireNextImageKHR: %s (recreating swapchain)",
             result == VK_ERROR_OUT_OF_DATE_KHR ? "OUT_OF_DATE" : "SUBOPTIMAL");
        recreate_swapchain(app);
        return;
    }
    if (result != VK_SUCCESS) {
        LOGE("vkAcquireNextImageKHR failed: %d", result);
        return;
    }

    VkSemaphore waitSemaphores[] = { app->imageAvailableSemaphore };
    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSemaphore signalSemaphores[] = { app->renderFinishedSemaphore };

    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = waitSemaphores,
        .pWaitDstStageMask = waitStages,
        .commandBufferCount = 1,
        .pCommandBuffers = &app->commandBuffers[imageIndex],
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = signalSemaphores
    };

    /* Reset the fence before submitting so the driver can signal it again.
     * This is required by the Vulkan spec. */
    vkResetFences(app->device, 1, &signalFence);

    result = vkQueueSubmit(app->graphicsQueue, 1, &submitInfo, signalFence);
    if (result != VK_SUCCESS) {
        LOGE("vkQueueSubmit failed: %d", result);
        return;
    }
    app->currentFenceIndex = 1 - app->currentFenceIndex;

    VkPresentInfoKHR presentInfo = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = signalSemaphores,
        .swapchainCount = 1,
        .pSwapchains = &app->swapchain,
        .pImageIndices = &imageIndex
    };
    result = vkQueuePresentKHR(app->graphicsQueue, &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        recreate_swapchain(app);
    } else if (result != VK_SUCCESS) {
        LOGE("vkQueuePresentKHR failed: %d", result);
    }
}


/* Capture a swapchain image to a CPU buffer.  Caller must free(outPixels).
   Returns true on success.  This acquires any available swapchain image,
   copies it, and presents it again. */
bool capture_swapchain_image(VulkanApp *app, uint8_t **outPixels, uint32_t *outW, uint32_t *outH) {
    if (!app->ready || app->imageCount == 0) {
        return false;
    }

    uint32_t w = app->swapchainExtent.width;
    uint32_t h = app->swapchainExtent.height;
    VkDeviceSize imageSize = (VkDeviceSize)w * h * 4;

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    if (!create_buffer(app, imageSize,
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       &stagingBuffer, &stagingMemory)) {
        LOGE("capture: failed to create staging buffer");
        return false;
    }

    /* Acquire any available image */
    uint32_t imageIndex = 0;
    VkResult result = vkAcquireNextImageKHR(app->device, app->swapchain, UINT64_MAX,
                                            app->imageAvailableSemaphore, VK_NULL_HANDLE,
                                            &imageIndex);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        LOGE("capture: vkAcquireNextImageKHR failed: %d", result);
        vkDestroyBuffer(app->device, stagingBuffer, NULL);
        vkFreeMemory(app->device, stagingMemory, NULL);
        return false;
    }

    VkCommandBuffer cmd = begin_single_time_commands(app);

    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = app->images[imageIndex];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);

    VkBufferImageCopy region = {};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {w, h, 1};

    vkCmdCopyImageToBuffer(cmd, app->images[imageIndex],
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuffer, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);

    end_single_time_commands(app, cmd);

    *outPixels = (uint8_t *)malloc((size_t)imageSize);
    if (!*outPixels) {
        vkDestroyBuffer(app->device, stagingBuffer, NULL);
        vkFreeMemory(app->device, stagingMemory, NULL);
        return false;
    }

    void *data = NULL;
    vkMapMemory(app->device, stagingMemory, 0, imageSize, 0, &data);
    memcpy(*outPixels, data, (size_t)imageSize);
    vkUnmapMemory(app->device, stagingMemory);

    vkDestroyBuffer(app->device, stagingBuffer, NULL);
    vkFreeMemory(app->device, stagingMemory, NULL);

    *outW = w;
    *outH = h;

    /* Present the image again so the swapchain stays balanced */
    VkPresentInfoKHR presentInfo = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .swapchainCount = 1,
        .pSwapchains = &app->swapchain,
        .pImageIndices = &imageIndex
    };
    vkQueuePresentKHR(app->graphicsQueue, &presentInfo);

    return true;
}

void update_input_bounds(void) {
    if (!g_app) return;
    
    pthread_mutex_lock(&g_app->uiMutex);
    g_input.urlX0 = g_app->uiUrlX0;
    g_input.urlY0 = g_app->uiUrlY0;
    g_input.urlX1 = g_app->uiUrlX1;
    g_input.urlY1 = g_app->uiUrlY1;
    
    // Sync text buffer
    if (g_app->urlInput[0] != '\0' && g_input.urlField.buffer[0] == '\0') {
        // Copy from app to input (initialization)
        strncpy(g_input.urlField.buffer, g_app->urlInput, sizeof(g_input.urlField.buffer) - 1);
        g_input.urlField.buffer[sizeof(g_input.urlField.buffer) - 1] = '\0';
        g_input.urlField.length = strlen(g_input.urlField.buffer);
        g_input.urlField.cursorPos = g_input.urlField.length;
    } else if (g_input.urlField.buffer[0] != '\0') {
        // Copy from input to app (normal operation)
        strncpy(g_app->urlInput, g_input.urlField.buffer, sizeof(g_app->urlInput) - 1);
        g_app->urlInput[sizeof(g_app->urlInput) - 1] = '\0';
        g_app->urlLen = g_input.urlField.length;
    }
    pthread_mutex_unlock(&g_app->uiMutex);
}

/**
 * Check if touch is inside URL input box
 */

bool is_inside_url_box(float x, float y) {
    return (x >= g_input.urlX0 && x <= g_input.urlX1 &&
            y >= g_input.urlY0 && y <= g_input.urlY1);
}

bool is_inside_filename_box(float x, float y) {
    return (x >= g_input.filenameX0 && x <= g_input.filenameX1 &&
            y >= g_input.filenameY0 && y <= g_input.filenameY1);
}

bool is_inside_keep_video_checkbox(float x, float y) {
    if (!g_app) return false;
    return (x >= g_app->uiKeepVideoX0 && x <= g_app->uiKeepVideoX1 &&
            y >= g_app->uiKeepVideoY0 && y <= g_app->uiKeepVideoY1);
}

float ui_get_visible_bottom(void) {
    if (!g_app) return 0.0f;
    float screenH = (float)g_app->windowHeight;
    if (screenH <= 0.0f) screenH = (float)g_app->swapchainExtent.height;
    float visibleBottom = screenH - g_app->keyboardHeightPx - 24.0f * g_app->densityScale;
    if (visibleBottom < 24.0f * g_app->densityScale) visibleBottom = 24.0f * g_app->densityScale;
    return visibleBottom;
}

void ui_clamp_scroll_offset(void) {
    if (!g_app) return;
    float screenH = (float)g_app->windowHeight;
    if (screenH <= 0.0f) screenH = (float)g_app->swapchainExtent.height;
    float visibleBottom = ui_get_visible_bottom();
    float contentBottom = g_app->uiContentBottom;
    if (contentBottom <= 0.0f) contentBottom = screenH;
    float minShift = visibleBottom - contentBottom;
    if (minShift > 0.0f) minShift = 0.0f;
    if (g_app->uiScrollOffsetY < minShift) g_app->uiScrollOffsetY = minShift;
    if (g_app->uiScrollOffsetY > 0.0f) g_app->uiScrollOffsetY = 0.0f;
}

void ui_scroll_to_focus(InputFocus focus) {
    if (!g_app) return;
    float visibleBottom = ui_get_visible_bottom();
    float focusBottom = 0.0f;
    if (focus == FOCUS_URL) {
        focusBottom = g_app->uiUrlY1 - g_app->uiScrollOffsetY;
    } else if (focus == FOCUS_FILENAME) {
        focusBottom = g_app->uiFilenameY1 - g_app->uiScrollOffsetY;
    } else {
        g_app->uiScrollOffsetY = 0.0f;
        ui_clamp_scroll_offset();
        return;
    }
    float margin = 24.0f * g_app->densityScale;
    float targetShift = 0.0f;
    if (focusBottom > visibleBottom - margin) {
        targetShift = visibleBottom - margin - focusBottom;
    }
    g_app->uiScrollOffsetY = targetShift;
    ui_clamp_scroll_offset();
}

void ui_apply_scroll_delta(float deltaY) {
    if (!g_app) return;
    g_app->uiScrollOffsetY += deltaY;
    ui_clamp_scroll_offset();
}

/**
 * Show soft keyboard via JNI
 */

void show_soft_keyboard(void) {
    platform_show_keyboard(g_app, false);
}

void hide_soft_keyboard(void) {
    platform_hide_keyboard(g_app);
}

/* ============================================================================
 * Generic TextField editing functions (operate on any TextField)
 * ============================================================================ */

bool tf_has_selection(TextField *tf) {
    return tf->selStart >= 0 && tf->selEnd >= 0 && tf->selStart != tf->selEnd;
}

void tf_clear_selection(TextField *tf) {
    tf->selStart = -1;
    tf->selEnd = -1;
}

void tf_delete_selection(TextField *tf) {
    if (!tf_has_selection(tf)) return;
    int start = tf->selStart < tf->selEnd ? tf->selStart : tf->selEnd;
    int end = tf->selStart < tf->selEnd ? tf->selEnd : tf->selStart;
    int len = end - start;
    if (end < tf->length) {
        memmove(&tf->buffer[start],
                &tf->buffer[end],
                tf->length - end);
    }
    tf->length -= len;
    tf->buffer[tf->length] = '\0';
    tf->cursorPos = start;
    tf_clear_selection(tf);
}

void tf_insert_char(TextField *tf, char c) {
    tf_delete_selection(tf);
    if (tf->length >= (int)sizeof(tf->buffer) - 1) {
        return; // Buffer full
    }
    if (tf->cursorPos < tf->length) {
        memmove(&tf->buffer[tf->cursorPos + 1],
                &tf->buffer[tf->cursorPos],
                tf->length - tf->cursorPos);
    }
    tf->buffer[tf->cursorPos] = c;
    tf->length++;
    tf->cursorPos++;
    tf->buffer[tf->length] = '\0';
}

void tf_backspace(TextField *tf) {
    if (tf_has_selection(tf)) {
        tf_delete_selection(tf);
        return;
    }
    if (tf->cursorPos <= 0 || tf->length <= 0) {
        return;
    }
    if (tf->cursorPos < tf->length) {
        memmove(&tf->buffer[tf->cursorPos - 1],
                &tf->buffer[tf->cursorPos],
                tf->length - tf->cursorPos);
    }
    tf->cursorPos--;
    tf->length--;
    tf->buffer[tf->length] = '\0';
}

void tf_cursor_left(TextField *tf) {
    if (tf_has_selection(tf)) {
        int start = tf->selStart < tf->selEnd ? tf->selStart : tf->selEnd;
        tf->cursorPos = start;
        tf_clear_selection(tf);
        return;
    }
    if (tf->cursorPos > 0) {
        tf->cursorPos--;
    }
}

void tf_cursor_right(TextField *tf) {
    if (tf_has_selection(tf)) {
        int end = tf->selStart < tf->selEnd ? tf->selEnd : tf->selStart;
        tf->cursorPos = end;
        tf_clear_selection(tf);
        return;
    }
    if (tf->cursorPos < tf->length) {
        tf->cursorPos++;
    }
}

void tf_select_all(TextField *tf) {
    tf->cursorPos = tf->length;
    tf->selStart = 0;
    tf->selEnd = tf->length;
}

void tf_copy(TextField *tf) {
    if (!tf_has_selection(tf)) return;
    int start = tf->selStart < tf->selEnd ? tf->selStart : tf->selEnd;
    int end = tf->selStart < tf->selEnd ? tf->selEnd : tf->selStart;
    int len = end - start;
    if (len <= 0) return;
    char *tmp = (char *)malloc(len + 1);
    if (!tmp) return;
    memcpy(tmp, &tf->buffer[start], len);
    tmp[len] = '\0';
    platform_set_clipboard(tmp);
    free(tmp);
}

void tf_cut(TextField *tf) {
    tf_copy(tf);
    tf_delete_selection(tf);
}

void tf_paste(TextField *tf, const char *text) {
    if (!text) return;
    int textLen = (int)strlen(text);
    if (textLen <= 0) return;
    tf_delete_selection(tf);
    int available = (int)sizeof(tf->buffer) - 1 - tf->length;
    if (textLen > available) textLen = available;
    if (textLen <= 0) return;
    if (tf->cursorPos < tf->length) {
        memmove(&tf->buffer[tf->cursorPos + textLen],
                &tf->buffer[tf->cursorPos],
                tf->length - tf->cursorPos);
    }
    memcpy(&tf->buffer[tf->cursorPos], text, textLen);
    tf->length += textLen;
    tf->cursorPos += textLen;
    tf->buffer[tf->length] = '\0';
}

int tf_char_pos_from_mouse(TextField *tf, float mouseX, float mouseY,
                           float textX, float textY,
                           float glyphW, float glyphH, int charsPerLine) {
    float relX = mouseX - textX;
    float relY = mouseY - textY;
    float lineH = glyphH + 2.0f;
    int line = (int)(relY / lineH);
    if (line < 0) line = 0;
    int col = (int)(relX / glyphW + 0.5f);
    if (col < 0) col = 0;
    int pos = line * charsPerLine + col;
    if (pos < 0) pos = 0;
    if (pos > tf->length) pos = tf->length;
    return pos;
}

void tf_update_selection_drag(TextField *tf, float mouseX, float mouseY,
                              float textX, float textY,
                              float glyphW, float glyphH, int charsPerLine) {
    if (!tf->isDragging) return;
    int pos = tf_char_pos_from_mouse(tf, mouseX, mouseY, textX, textY, glyphW, glyphH, charsPerLine);
    tf->selStart = tf->dragAnchor;
    tf->selEnd = pos;
    tf->cursorPos = pos;
}

/* ============================================================================
 * .m4a extension enforcement
 * ============================================================================ */

static bool ends_with_m4a(const char *str, int len) {
    if (len < 4) return false;
    const char *suffix = ".m4a";
    for (int i = 0; i < 4; i++) {
        char a = str[len - 4 + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
        if (a != b) return false;
    }
    return true;
}

void enforce_media_extension(TextField *tf) {
    const char *suffix = (g_app && g_app->keepVideo) ? ".mp4" : ".m4a";
    int suffix_len = 4;
    int len = tf->length;
    bool already_correct = false;
    if (len >= suffix_len) {
        already_correct = true;
        for (int i = 0; i < suffix_len; i++) {
            char a = tf->buffer[len - suffix_len + i];
            char b = suffix[i];
            if (a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
            if (a != b) { already_correct = false; break; }
        }
    }
    if (already_correct) {
        return;
    }
    char *last_dot = strrchr(tf->buffer, '.');
    if (last_dot && last_dot >= tf->buffer + len - 10) {
        *last_dot = '\0';
        tf->length = (int)(last_dot - tf->buffer);
    }
    int remaining = (int)sizeof(tf->buffer) - tf->length - 1;
    if (remaining >= suffix_len) {
        strncat(tf->buffer, suffix, remaining);
        tf->length = strlen(tf->buffer);
    }
    int ext_start = tf->length - suffix_len;
    if (tf->cursorPos > ext_start) tf->cursorPos = ext_start;
    if (tf->selStart > ext_start) tf->selStart = ext_start;
    if (tf->selEnd > ext_start) tf->selEnd = ext_start;
}

/* ============================================================================
 * URL field wrappers (backward compat)
 * ============================================================================ */

void handle_character_input(char c) {
    tf_insert_char(&g_input.urlField, c);
    LOGI("Input: '%c' -> buffer: '%s'", c, g_input.urlField.buffer);
    file_log("Input: '%c' -> buffer: '%s'", c, g_input.urlField.buffer);
}

void handle_backspace(void) {
    tf_backspace(&g_input.urlField);
    LOGI("Backspace -> buffer: '%s'", g_input.urlField.buffer);
}

void handle_cursor_left(void) {
    tf_cursor_left(&g_input.urlField);
}

void handle_cursor_right(void) {
    tf_cursor_right(&g_input.urlField);
}

bool has_selection(void) {
    return tf_has_selection(&g_input.urlField);
}

void clear_selection(void) {
    tf_clear_selection(&g_input.urlField);
    g_input.pathSelected = false;
}

void delete_selection(void) {
    tf_delete_selection(&g_input.urlField);
}

void handle_select_all(void) {
    tf_select_all(&g_input.urlField);
}

void handle_copy(void) {
    if (g_input.pathSelected && g_app) {
        pthread_mutex_lock(&g_app->uiMutex);
        const char *path = g_app->downloadPath;
        pthread_mutex_unlock(&g_app->uiMutex);
        if (path[0] != '\0') {
            platform_set_clipboard(path);
        }
        return;
    }
    tf_copy(&g_input.urlField);
}

void handle_cut(void) {
    handle_copy();
    delete_selection();
}

void handle_paste(const char *text) {
    tf_paste(&g_input.urlField, text);
}

void sync_input_to_app(void) {
    if (!g_app) return;
    pthread_mutex_lock(&g_app->uiMutex);
    strncpy(g_app->urlInput, g_input.urlField.buffer, sizeof(g_app->urlInput) - 1);
    g_app->urlInput[sizeof(g_app->urlInput) - 1] = '\0';
    g_app->urlLen = g_input.urlField.length;
    strncpy(g_app->customFilename, g_input.filenameField.buffer, sizeof(g_app->customFilename) - 1);
    g_app->customFilename[sizeof(g_app->customFilename) - 1] = '\0';
    pthread_mutex_unlock(&g_app->uiMutex);
}

/**
 * Handle submit (Enter key)
 */

void handle_submit(void) {
    LOGI("Submit triggered, URL: '%s'", g_input.urlField.buffer);
    file_log("Submit triggered, URL: '%s'", g_input.urlField.buffer);
    
    if (!g_app) {
        LOGE("handle_submit: g_app is NULL!");
        return;
    }
    
    // Copy URL and filename to app buffer
    sync_input_to_app();
    
    // Deactivate input
    g_input.inputActive = false;
    g_input.filenameActive = false;
    g_input.focus = FOCUS_NONE;
    if (g_app) {
        g_app->inputActive = false;
        g_app->uiScrollOffsetY = 0.0f;
    }
    hide_soft_keyboard();
    
    // Start worker
    start_worker(g_app);
}

/* ============================================================================
 * Filename field wrappers (with .m4a enforcement)
 * ============================================================================ */

bool has_filename_selection(void) {
    return tf_has_selection(&g_input.filenameField);
}

void clear_filename_selection(void) {
    tf_clear_selection(&g_input.filenameField);
}

void delete_filename_selection(void) {
    tf_delete_selection(&g_input.filenameField);
    enforce_media_extension(&g_input.filenameField);
}

void handle_filename_select_all(void) {
    tf_select_all(&g_input.filenameField);
}

void handle_filename_copy(void) {
    tf_copy(&g_input.filenameField);
}

void handle_filename_cut(void) {
    tf_cut(&g_input.filenameField);
    enforce_media_extension(&g_input.filenameField);
}

void handle_filename_paste(const char *text) {
    tf_paste(&g_input.filenameField, text);
    enforce_media_extension(&g_input.filenameField);
}

void handle_filename_character(char c) {
    tf_insert_char(&g_input.filenameField, c);
    enforce_media_extension(&g_input.filenameField);
}

void handle_filename_backspace(void) {
    tf_backspace(&g_input.filenameField);
    enforce_media_extension(&g_input.filenameField);
}

void handle_filename_cursor_left(void) {
    tf_cursor_left(&g_input.filenameField);
}

void handle_filename_cursor_right(void) {
    tf_cursor_right(&g_input.filenameField);
}

/* ============================================================================
 * Mouse helpers (backward compat wrappers for URL field)
 * ============================================================================ */

int char_pos_from_mouse(float mouseX, float mouseY,
                         float urlTextX, float urlTextY,
                         float glyphW, float glyphH, int charsPerLine) {
    return tf_char_pos_from_mouse(&g_input.urlField, mouseX, mouseY,
                                  urlTextX, urlTextY, glyphW, glyphH, charsPerLine);
}

void set_cursor_from_mouse(float mouseX, float mouseY, float urlTextX, float urlTextY,
                           float glyphW, float glyphH, int charsPerLine) {
    g_input.urlField.cursorPos = char_pos_from_mouse(mouseX, mouseY, urlTextX, urlTextY, glyphW, glyphH, charsPerLine);
}

void update_selection_drag(float mouseX, float mouseY, float urlTextX, float urlTextY,
                           float glyphW, float glyphH, int charsPerLine) {
    tf_update_selection_drag(&g_input.urlField, mouseX, mouseY,
                             urlTextX, urlTextY, glyphW, glyphH, charsPerLine);
}

/* ============================================================================
 * JNI EVENT CALLBACKS (Called from Java)
 * ============================================================================ */


