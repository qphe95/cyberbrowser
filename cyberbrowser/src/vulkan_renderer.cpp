#include "ft_vulkan_renderer.h"
#include "ft_ui_layout.h"
#include "ft_ui_display_list.h"
#include "ft_font_system.h"
#include "ft_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define LOG_TAG "freetext-vk"
#define LOGI(...) ft_platform_log(FT_LOG_LEVEL_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) ft_platform_log(FT_LOG_LEVEL_ERROR, LOG_TAG, __VA_ARGS__)

static bool g_captureRequested = false;

/* ========================================================================
   Helpers
   ======================================================================== */

static uint32_t find_memory_type(FtVulkanRenderer *r, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(r->physicalDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    if (properties == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
            if ((typeFilter & (1u << i)) && (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                return i;
            }
        }
    }
    return UINT32_MAX;
}

static bool create_buffer(FtVulkanRenderer *r, VkDeviceSize size, VkBufferUsageFlags usage,
                          VkMemoryPropertyFlags properties, VkBuffer *buffer, VkDeviceMemory *bufferMemory) {
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateBuffer(r->device, &bufferInfo, NULL, buffer);
    if (result != VK_SUCCESS) { LOGE("vkCreateBuffer failed: %d", result); return false; }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(r->device, *buffer, &memRequirements);

    uint32_t memoryType = find_memory_type(r, memRequirements.memoryTypeBits, properties);
    if (memoryType == UINT32_MAX) { LOGE("No suitable memory type"); return false; }

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = memoryType;

    result = vkAllocateMemory(r->device, &allocInfo, NULL, bufferMemory);
    if (result != VK_SUCCESS) { LOGE("vkAllocateMemory failed: %d", result); return false; }

    result = vkBindBufferMemory(r->device, *buffer, *bufferMemory, 0);
    if (result != VK_SUCCESS) { LOGE("vkBindBufferMemory failed: %d", result); return false; }
    return true;
}

static VkCommandBuffer begin_single_time_commands(FtVulkanRenderer *r) {
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = r->commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(r->device, &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    return commandBuffer;
}

static void end_single_time_commands(FtVulkanRenderer *r, VkCommandBuffer commandBuffer) {
    vkEndCommandBuffer(commandBuffer);
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    vkQueueSubmit(r->graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(r->graphicsQueue);
    vkFreeCommandBuffers(r->device, r->commandPool, 1, &commandBuffer);
}

static bool create_capture_buffer(FtVulkanRenderer *r) {
    if (r->captureBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(r->device, r->captureBuffer, NULL);
        vkFreeMemory(r->device, r->captureBufferMemory, NULL);
        r->captureBuffer = VK_NULL_HANDLE;
        r->captureBufferMemory = VK_NULL_HANDLE;
    }
    VkDeviceSize size = (VkDeviceSize)r->swapchainExtent.width * r->swapchainExtent.height * 4;
    r->captureBufferSize = size;
    if (!create_buffer(r, size,
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       &r->captureBuffer, &r->captureBufferMemory)) {
        return false;
    }
    return true;
}

static void save_bmp(const char *path, const uint8_t *pixels, uint32_t width, uint32_t height) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t rowSize = width * 4;
    uint32_t fileSize = 14 + 40 + rowSize * height;
    uint8_t header[14 + 40] = {};
    /* BMP file header */
    header[0] = 'B'; header[1] = 'M';
    header[2] = (uint8_t)(fileSize);
    header[3] = (uint8_t)(fileSize >> 8);
    header[4] = (uint8_t)(fileSize >> 16);
    header[5] = (uint8_t)(fileSize >> 24);
    header[10] = 14 + 40;
    /* DIB header */
    header[14] = 40;
    header[18] = (uint8_t)(width);
    header[19] = (uint8_t)(width >> 8);
    header[20] = (uint8_t)(width >> 16);
    header[21] = (uint8_t)(width >> 24);
    header[22] = (uint8_t)(height);
    header[23] = (uint8_t)(height >> 8);
    header[24] = (uint8_t)(height >> 16);
    header[25] = (uint8_t)(height >> 24);
    header[26] = 1; /* planes */
    header[28] = 32; /* bits per pixel */
    fwrite(header, 1, sizeof(header), f);
    /* Write bottom-up */
    for (int y = (int)height - 1; y >= 0; --y) {
        fwrite(pixels + y * rowSize, 1, rowSize, f);
    }
    fclose(f);
}

static void analyze_subpixel_pattern(const uint8_t *pixels, uint32_t width, uint32_t height) {
    /* Scan for vertical edges in bright text on dark background.
       An edge is where we go from dark (< 32) to bright (> 128) or vice versa. */
    int rgbLeftEdges = 0, bgrLeftEdges = 0;
    int rgbRightEdges = 0, bgrRightEdges = 0;
    int totalEdges = 0;

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 1; x < width - 1; ++x) {
            const uint8_t *left  = pixels + (y * width + x - 1) * 4;
            const uint8_t *mid   = pixels + (y * width + x) * 4;
            const uint8_t *right = pixels + (y * width + x + 1) * 4;

            float brightL = (left[0] + left[1] + left[2]) / 3.0f;
            float brightM = (mid[0] + mid[1] + mid[2]) / 3.0f;
            float brightR = (right[0] + right[1] + right[2]) / 3.0f;

            /* Left edge: dark -> bright */
            if (brightL < 32.0f && brightM > 128.0f) {
                float r = mid[2]; /* BMP is BGRA */
                float g = mid[1];
                float b = mid[0];
                if (r > g && r > b) rgbLeftEdges++;
                else if (b > g && b > r) bgrLeftEdges++;
                totalEdges++;
            }
            /* Right edge: bright -> dark */
            else if (brightM > 128.0f && brightR < 32.0f) {
                float r = mid[2];
                float g = mid[1];
                float b = mid[0];
                if (b > g && b > r) rgbRightEdges++;
                else if (r > g && r > b) bgrRightEdges++;
                totalEdges++;
            }
        }
    }

    printf("\n=== Subpixel Diagnosis ===\n");
    printf("Frame size: %dx%d\n", width, height);
    printf("Edges analyzed: %d\n", totalEdges);
    if (totalEdges == 0) {
        printf("No text edges found. Make sure text is visible before capturing.\n");
        return;
    }
    printf("Left-edge red-dominant  (RGB pattern): %d (%.1f%%)\n", rgbLeftEdges, 100.0f * rgbLeftEdges / totalEdges);
    printf("Left-edge blue-dominant (BGR pattern): %d (%.1f%%)\n", bgrLeftEdges, 100.0f * bgrLeftEdges / totalEdges);
    printf("Right-edge blue-dominant (RGB pattern): %d (%.1f%%)\n", rgbRightEdges, 100.0f * rgbRightEdges / totalEdges);
    printf("Right-edge red-dominant  (BGR pattern): %d (%.1f%%)\n", bgrRightEdges, 100.0f * bgrRightEdges / totalEdges);

    int rgbScore = rgbLeftEdges + rgbRightEdges;
    int bgrScore = bgrLeftEdges + bgrRightEdges;
    printf("\nRGB pattern match: %d edges\n", rgbScore);
    printf("BGR pattern match: %d edges\n", bgrScore);

    if (rgbScore > bgrScore * 1.5f) {
        printf("DIAGNOSIS: Framebuffer strongly matches RGB subpixel layout.\n");
        printf("  -> If you still see rainbows, try pressing F5 to switch to BGR\n");
        printf("     (some displays report RGB to Windows but are physically BGR).\n");
    } else if (bgrScore > rgbScore * 1.5f) {
        printf("DIAGNOSIS: Framebuffer strongly matches BGR subpixel layout.\n");
        printf("  -> If you still see rainbows, try pressing F5 to switch to RGB.\n");
    } else {
        printf("DIAGNOSIS: Edge colors are mixed / inconclusive.\n");
        printf("  -> The LCD filter may be spreading colors too evenly, or text\n");
        printf("     is too small for reliable edge detection.\n");
    }
    printf("==========================\n\n");
}

static bool create_image(FtVulkanRenderer *r, uint32_t width, uint32_t height, VkFormat format,
                         VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties,
                         VkImage *image, VkDeviceMemory *imageMemory, uint32_t mipLevels) {
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = tiling;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult result = vkCreateImage(r->device, &imageInfo, NULL, image);
    if (result != VK_SUCCESS) { LOGE("vkCreateImage failed: %d", result); return false; }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(r->device, *image, &memRequirements);

    uint32_t memoryType = find_memory_type(r, memRequirements.memoryTypeBits, properties);
    if (memoryType == UINT32_MAX) { LOGE("No suitable image memory type"); return false; }

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = memoryType;

    result = vkAllocateMemory(r->device, &allocInfo, NULL, imageMemory);
    if (result != VK_SUCCESS) { LOGE("vkAllocateMemory failed: %d", result); return false; }

    result = vkBindImageMemory(r->device, *image, *imageMemory, 0);
    if (result != VK_SUCCESS) { LOGE("vkBindImageMemory failed: %d", result); return false; }
    return true;
}

static VkImageView create_image_view(FtVulkanRenderer *r, VkImage image, VkFormat format, uint32_t mipLevels) {
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView imageView;
    VkResult result = vkCreateImageView(r->device, &viewInfo, NULL, &imageView);
    if (result != VK_SUCCESS) { LOGE("vkCreateImageView failed: %d", result); return VK_NULL_HANDLE; }
    return imageView;
}

static void transition_image_layout(FtVulkanRenderer *r, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkCommandBuffer cmd = begin_single_time_commands(r);
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
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

    vkCmdPipelineBarrier(cmd, sourceStage, destinationStage, 0, 0, NULL, 0, NULL, 1, &barrier);
    end_single_time_commands(r, cmd);
}

static void copy_buffer_to_image(FtVulkanRenderer *r, VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
    VkCommandBuffer cmd = begin_single_time_commands(r);
    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    end_single_time_commands(r, cmd);
}

/* ========================================================================
   Device & Swapchain
   ======================================================================== */

static bool pick_device_and_queue(FtVulkanRenderer *r) {
    if (r->device != VK_NULL_HANDLE) return true;
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(r->instance, &deviceCount, NULL);
    if (deviceCount == 0) { LOGE("No Vulkan devices found"); return false; }

    VkPhysicalDevice *devices = (VkPhysicalDevice *)malloc(sizeof(VkPhysicalDevice) * deviceCount);
    vkEnumeratePhysicalDevices(r->instance, &deviceCount, devices);

    VkPhysicalDevice selected = VK_NULL_HANDLE;
    uint32_t selectedQueueFamily = 0;
    for (uint32_t i = 0; i < deviceCount; ++i) {
        uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &queueCount, NULL);
        VkQueueFamilyProperties *queues = (VkQueueFamilyProperties *)malloc(sizeof(VkQueueFamilyProperties) * queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &queueCount, queues);
        for (uint32_t q = 0; q < queueCount; ++q) {
            VkBool32 presentSupported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], q, r->surface, &presentSupported);
            if ((queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presentSupported) {
                selected = devices[i];
                selectedQueueFamily = q;
                break;
            }
        }
        free(queues);
        if (selected != VK_NULL_HANDLE) break;
    }
    free(devices);
    if (selected == VK_NULL_HANDLE) { LOGE("No suitable Vulkan device found"); return false; }

    r->physicalDevice = selected;
    r->graphicsQueueFamily = selectedQueueFamily;

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo = {};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = r->graphicsQueueFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    const char *deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo deviceCreateInfo = {};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.enabledExtensionCount = 1;
    deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions;

    VkResult result = vkCreateDevice(r->physicalDevice, &deviceCreateInfo, NULL, &r->device);
    if (result != VK_SUCCESS) { LOGE("vkCreateDevice failed: %d", result); return false; }
    // Device functions are resolved by the Vulkan loader automatically
    vkGetDeviceQueue(r->device, r->graphicsQueueFamily, 0, &r->graphicsQueue);
    return true;
}

static bool create_swapchain(FtVulkanRenderer *r) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->physicalDevice, r->surface, &caps);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(r->physicalDevice, r->surface, &formatCount, NULL);
    VkSurfaceFormatKHR *formats = (VkSurfaceFormatKHR *)malloc(sizeof(VkSurfaceFormatKHR) * formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(r->physicalDevice, r->surface, &formatCount, formats);

    VkSurfaceFormatKHR chosenFormat = formats[0];
    for (uint32_t i = 0; i < formatCount; ++i) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB) { chosenFormat = formats[i]; break; }
    }
    if (chosenFormat.format != VK_FORMAT_B8G8R8A8_SRGB) {
        for (uint32_t i = 0; i < formatCount; ++i) {
            if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) { chosenFormat = formats[i]; break; }
        }
    }
    free(formats);

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX || extent.height == UINT32_MAX) {
        extent.width = (uint32_t)r->windowWidth;
        extent.height = (uint32_t)r->windowHeight;
    }
    if (extent.width < caps.minImageExtent.width) extent.width = caps.minImageExtent.width;
    if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
    if (extent.width > caps.maxImageExtent.width) extent.width = caps.maxImageExtent.width;
    if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    if (extent.width == 0) extent.width = 1;
    if (extent.height == 0) extent.height = 1;

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR swapInfo = {};
    swapInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapInfo.surface = r->surface;
    swapInfo.minImageCount = imageCount;
    swapInfo.imageFormat = chosenFormat.format;
    swapInfo.imageColorSpace = chosenFormat.colorSpace;
    swapInfo.imageExtent = extent;
    swapInfo.imageArrayLayers = 1;
    swapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapInfo.preTransform = caps.currentTransform;
    swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapInfo.clipped = VK_TRUE;
    swapInfo.oldSwapchain = VK_NULL_HANDLE;

    VkResult result = vkCreateSwapchainKHR(r->device, &swapInfo, NULL, &r->swapchain);
    if (result != VK_SUCCESS) { LOGE("vkCreateSwapchainKHR failed: %d", result); return false; }

    vkGetSwapchainImagesKHR(r->device, r->swapchain, &imageCount, NULL);
    r->images = (VkImage *)malloc(sizeof(VkImage) * imageCount);
    vkGetSwapchainImagesKHR(r->device, r->swapchain, &imageCount, r->images);
    r->imageCount = imageCount;
    r->swapchainFormat = chosenFormat.format;
    r->swapchainExtent = extent;
    r->windowWidth = (int32_t)extent.width;
    r->windowHeight = (int32_t)extent.height;

    r->imageViews = (VkImageView *)malloc(sizeof(VkImageView) * imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = r->images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = r->swapchainFormat;
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        result = vkCreateImageView(r->device, &viewInfo, NULL, &r->imageViews[i]);
        if (result != VK_SUCCESS) { LOGE("vkCreateImageView failed: %d", result); return false; }
    }
    return true;
}

static bool create_render_pass(FtVulkanRenderer *r) {
    VkAttachmentDescription colorAttachment = {};
    colorAttachment.format = r->swapchainFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkAttachmentReference colorRef = {};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    VkResult result = vkCreateRenderPass(r->device, &renderPassInfo, NULL, &r->renderPass);
    if (result != VK_SUCCESS) { LOGE("vkCreateRenderPass failed: %d", result); return false; }
    return true;
}

static VkShaderModule create_shader_module(FtVulkanRenderer *r, const uint8_t *data, size_t size) {
    VkShaderModuleCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = size;
    info.pCode = (const uint32_t *)data;
    VkShaderModule module;
    VkResult result = vkCreateShaderModule(r->device, &info, NULL, &module);
    if (result != VK_SUCCESS) { LOGE("vkCreateShaderModule failed: %d", result); return VK_NULL_HANDLE; }
    return module;
}

/* ========================================================================
   Font Atlas
   ======================================================================== */

static bool upload_font_atlas(FtVulkanRenderer *r, uint8_t *atlas) {
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    if (!create_buffer(r, FONT_ATLAS_SIZE * FONT_ATLAS_SIZE,
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       &stagingBuffer, &stagingMemory)) {
        return false;
    }

    void *data = NULL;
    vkMapMemory(r->device, stagingMemory, 0, FONT_ATLAS_SIZE * FONT_ATLAS_SIZE, 0, &data);
    memcpy(data, atlas, FONT_ATLAS_SIZE * FONT_ATLAS_SIZE);
    vkUnmapMemory(r->device, stagingMemory);

    if (!create_image(r, FONT_ATLAS_SIZE, FONT_ATLAS_SIZE, VK_FORMAT_R8_UNORM,
                      VK_IMAGE_TILING_OPTIMAL,
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                      &r->fontImage, &r->fontImageMemory, 1)) {
        vkDestroyBuffer(r->device, stagingBuffer, NULL);
        vkFreeMemory(r->device, stagingMemory, NULL);
        return false;
    }
    transition_image_layout(r, r->fontImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copy_buffer_to_image(r, stagingBuffer, r->fontImage, FONT_ATLAS_SIZE, FONT_ATLAS_SIZE);
    transition_image_layout(r, r->fontImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(r->device, stagingBuffer, NULL);
    vkFreeMemory(r->device, stagingMemory, NULL);

    r->fontImageView = create_image_view(r, r->fontImage, VK_FORMAT_R8_UNORM, 1);
    if (r->fontImageView == VK_NULL_HANDLE) return false;

    return true;
}

static bool create_font_resources(FtVulkanRenderer *r) {
    uint8_t *atlas = (uint8_t *)malloc(FONT_ATLAS_SIZE * FONT_ATLAS_SIZE);
    if (!atlas) { LOGE("Failed to allocate font atlas"); return false; }
    if (!build_ttf_atlas(atlas)) {
        LOGE("Failed to build TTF atlas");
        free(atlas);
        return false;
    }

    if (!upload_font_atlas(r, atlas)) {
        free(atlas);
        return false;
    }
    free(atlas);

    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;

    VkResult result = vkCreateSampler(r->device, &samplerInfo, NULL, &r->fontSampler);
    if (result != VK_SUCCESS) { LOGE("vkCreateSampler failed: %d", result); return false; }

    VkDescriptorSetLayoutBinding samplerBinding = {};
    samplerBinding.binding = 0;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &samplerBinding;
    result = vkCreateDescriptorSetLayout(r->device, &layoutInfo, NULL, &r->descriptorSetLayout);
    if (result != VK_SUCCESS) { LOGE("vkCreateDescriptorSetLayout failed: %d", result); return false; }

    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    result = vkCreateDescriptorPool(r->device, &poolInfo, NULL, &r->descriptorPool);
    if (result != VK_SUCCESS) { LOGE("vkCreateDescriptorPool failed: %d", result); return false; }

    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = r->descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &r->descriptorSetLayout;
    result = vkAllocateDescriptorSets(r->device, &allocInfo, &r->descriptorSet);
    if (result != VK_SUCCESS) { LOGE("vkAllocateDescriptorSets failed: %d", result); return false; }

    VkDescriptorImageInfo imageInfo = {};
    imageInfo.sampler = r->fontSampler;
    imageInfo.imageView = r->fontImageView;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = r->descriptorSet;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(r->device, 1, &write, 0, NULL);

    return true;
}

/* ========================================================================
   Pipeline
   ======================================================================== */

static bool create_pipeline(FtVulkanRenderer *r, const uint8_t *vert_data, size_t vert_size,
                            const uint8_t *frag_data, size_t frag_size) {
    VkShaderModule vertModule = create_shader_module(r, vert_data, vert_size);
    VkShaderModule fragModule = create_shader_module(r, frag_data, frag_size);
    if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE) return false;

    VkPipelineShaderStageCreateInfo shaderStages[2] = {};
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = vertModule;
    shaderStages[0].pName = "main";
    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = fragModule;
    shaderStages[1].pName = "main";

    VkVertexInputBindingDescription binding = {};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attributes[3] = {};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[0].offset = (uint32_t)offsetof(Vertex, pos);
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[1].offset = (uint32_t)offsetof(Vertex, uv);
    attributes[2].location = 2;
    attributes[2].binding = 0;
    attributes[2].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[2].offset = (uint32_t)offsetof(Vertex, color);

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)r->swapchainExtent.width;
    viewport.height = (float)r->swapchainExtent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor = {};
    scissor.offset = {0, 0};
    scissor.extent = r->swapchainExtent;

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDynamicStateCreateInfo dynamicState = {};
    VkDynamicState dynamicStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlending = {};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(int);

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &r->descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    VkResult result = vkCreatePipelineLayout(r->device, &layoutInfo, NULL, &r->pipelineLayout);
    if (result != VK_SUCCESS) { LOGE("vkCreatePipelineLayout failed: %d", result); return false; }

    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = r->pipelineLayout;
    pipelineInfo.renderPass = r->renderPass;
    pipelineInfo.subpass = 0;

    result = vkCreateGraphicsPipelines(r->device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &r->pipeline);
    vkDestroyShaderModule(r->device, vertModule, NULL);
    vkDestroyShaderModule(r->device, fragModule, NULL);
    if (result != VK_SUCCESS) { LOGE("vkCreateGraphicsPipelines failed: %d", result); return false; }
    return true;
}

/* ========================================================================
   Framebuffers & Commands
   ======================================================================== */

static bool create_framebuffers(FtVulkanRenderer *r) {
    r->framebuffers = (VkFramebuffer *)malloc(sizeof(VkFramebuffer) * r->imageCount);
    for (uint32_t i = 0; i < r->imageCount; ++i) {
        VkImageView attachments[] = { r->imageViews[i] };
        VkFramebufferCreateInfo fbInfo = {};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = r->renderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = attachments;
        fbInfo.width = r->swapchainExtent.width;
        fbInfo.height = r->swapchainExtent.height;
        fbInfo.layers = 1;
        VkResult result = vkCreateFramebuffer(r->device, &fbInfo, NULL, &r->framebuffers[i]);
        if (result != VK_SUCCESS) { LOGE("vkCreateFramebuffer failed: %d", result); return false; }
    }
    return true;
}

static void destroy_hi_res_resources(FtVulkanRenderer *r) {
    if (r->hiResFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(r->device, r->hiResFramebuffer, NULL);
        r->hiResFramebuffer = VK_NULL_HANDLE;
    }
    if (r->hiResImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(r->device, r->hiResImageView, NULL);
        r->hiResImageView = VK_NULL_HANDLE;
    }
    if (r->hiResImage != VK_NULL_HANDLE) {
        vkDestroyImage(r->device, r->hiResImage, NULL);
        r->hiResImage = VK_NULL_HANDLE;
    }
    if (r->hiResImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(r->device, r->hiResImageMemory, NULL);
        r->hiResImageMemory = VK_NULL_HANDLE;
    }
    if (r->midResImage1 != VK_NULL_HANDLE) {
        vkDestroyImage(r->device, r->midResImage1, NULL);
        r->midResImage1 = VK_NULL_HANDLE;
    }
    if (r->midResImage1Memory != VK_NULL_HANDLE) {
        vkFreeMemory(r->device, r->midResImage1Memory, NULL);
        r->midResImage1Memory = VK_NULL_HANDLE;
    }
    if (r->midResImage2 != VK_NULL_HANDLE) {
        vkDestroyImage(r->device, r->midResImage2, NULL);
        r->midResImage2 = VK_NULL_HANDLE;
    }
    if (r->midResImage2Memory != VK_NULL_HANDLE) {
        vkFreeMemory(r->device, r->midResImage2Memory, NULL);
        r->midResImage2Memory = VK_NULL_HANDLE;
    }
}

static bool create_hi_res_resources(FtVulkanRenderer *r) {
    destroy_hi_res_resources(r);

    uint32_t w8 = r->swapchainExtent.width * 8;
    uint32_t h8 = r->swapchainExtent.height * 8;
    if (w8 < 8) w8 = 8;
    if (h8 < 8) h8 = 8;

    uint32_t w4 = r->swapchainExtent.width * 4;
    uint32_t h4 = r->swapchainExtent.height * 4;
    if (w4 < 4) w4 = 4;
    if (h4 < 4) h4 = 4;

    uint32_t w2 = r->swapchainExtent.width * 2;
    uint32_t h2 = r->swapchainExtent.height * 2;
    if (w2 < 2) w2 = 2;
    if (h2 < 2) h2 = 2;

    if (!create_image(r, w8, h8, r->swapchainFormat,
                      VK_IMAGE_TILING_OPTIMAL,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                      &r->hiResImage, &r->hiResImageMemory, 1)) {
        return false;
    }

    r->hiResImageView = create_image_view(r, r->hiResImage, r->swapchainFormat, 1);
    if (r->hiResImageView == VK_NULL_HANDLE) return false;

    VkImageView attachments[] = { r->hiResImageView };
    VkFramebufferCreateInfo fbInfo = {};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = r->renderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = attachments;
    fbInfo.width = w8;
    fbInfo.height = h8;
    fbInfo.layers = 1;
    VkResult result = vkCreateFramebuffer(r->device, &fbInfo, NULL, &r->hiResFramebuffer);
    if (result != VK_SUCCESS) { LOGE("vkCreateFramebuffer (hi-res) failed: %d", result); return false; }

    if (!create_image(r, w4, h4, r->swapchainFormat,
                      VK_IMAGE_TILING_OPTIMAL,
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                      &r->midResImage1, &r->midResImage1Memory, 1)) {
        return false;
    }

    if (!create_image(r, w2, h2, r->swapchainFormat,
                      VK_IMAGE_TILING_OPTIMAL,
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                      &r->midResImage2, &r->midResImage2Memory, 1)) {
        return false;
    }

    return true;
}

static bool create_command_pool_and_buffers(FtVulkanRenderer *r) {
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = r->graphicsQueueFamily;
    VkResult result = vkCreateCommandPool(r->device, &poolInfo, NULL, &r->commandPool);
    if (result != VK_SUCCESS) { LOGE("vkCreateCommandPool failed: %d", result); return false; }

    r->commandBuffers = (VkCommandBuffer *)malloc(sizeof(VkCommandBuffer) * r->imageCount);
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = r->commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = r->imageCount;
    result = vkAllocateCommandBuffers(r->device, &allocInfo, r->commandBuffers);
    if (result != VK_SUCCESS) { LOGE("vkAllocateCommandBuffers failed: %d", result); return false; }
    return true;
}

static bool create_sync_objects(FtVulkanRenderer *r) {
    VkSemaphoreCreateInfo semInfo = {};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkResult result = vkCreateSemaphore(r->device, &semInfo, NULL, &r->imageAvailableSemaphore);
    if (result != VK_SUCCESS) { LOGE("vkCreateSemaphore failed: %d", result); return false; }
    result = vkCreateSemaphore(r->device, &semInfo, NULL, &r->renderFinishedSemaphore);
    if (result != VK_SUCCESS) { LOGE("vkCreateSemaphore failed: %d", result); return false; }
    result = vkCreateFence(r->device, &fenceInfo, NULL, &r->inFlightFence);
    if (result != VK_SUCCESS) { LOGE("vkCreateFence failed: %d", result); return false; }
    return true;
}

/* ========================================================================
   Vertex Buffer
   ======================================================================== */

static bool create_vertex_buffer(FtVulkanRenderer *r) {
    r->vertexCapacity = 65536;
    VkDeviceSize bufferSize = r->vertexCapacity * sizeof(Vertex);
    return create_buffer(r, bufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         &r->vertexBuffer, &r->vertexBufferMemory);
}

bool ft_vk_renderer_update_vertices(FtVulkanRenderer *r, const void *vertexData, uint32_t vertexCount) {
    if (vertexCount > r->vertexCapacity) vertexCount = r->vertexCapacity;
    r->vertexCount = vertexCount;
    if (vertexCount == 0) return true;
    void *data = NULL;
    vkMapMemory(r->device, r->vertexBufferMemory, 0, r->vertexCapacity * sizeof(Vertex), 0, &data);
    memcpy(data, vertexData, vertexCount * sizeof(Vertex));
    vkUnmapMemory(r->device, r->vertexBufferMemory);
    return true;
}

/* ========================================================================
   Cleanup
   ======================================================================== */

static void cleanup_swapchain(FtVulkanRenderer *r) {
    if (r->device == VK_NULL_HANDLE) return;
    destroy_hi_res_resources(r);
    if (r->framebuffers) {
        for (uint32_t i = 0; i < r->imageCount; ++i) {
            if (r->framebuffers[i] != VK_NULL_HANDLE)
                vkDestroyFramebuffer(r->device, r->framebuffers[i], NULL);
        }
        free(r->framebuffers);
        r->framebuffers = NULL;
    }
    if (r->commandBuffers) {
        vkFreeCommandBuffers(r->device, r->commandPool, r->imageCount, r->commandBuffers);
        free(r->commandBuffers);
        r->commandBuffers = NULL;
    }
    if (r->commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(r->device, r->commandPool, NULL);
        r->commandPool = VK_NULL_HANDLE;
    }
    if (r->pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(r->device, r->pipeline, NULL);
        r->pipeline = VK_NULL_HANDLE;
    }
    if (r->pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(r->device, r->pipelineLayout, NULL);
        r->pipelineLayout = VK_NULL_HANDLE;
    }
    if (r->renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(r->device, r->renderPass, NULL);
        r->renderPass = VK_NULL_HANDLE;
    }
    if (r->imageViews) {
        for (uint32_t i = 0; i < r->imageCount; ++i) {
            if (r->imageViews[i] != VK_NULL_HANDLE)
                vkDestroyImageView(r->device, r->imageViews[i], NULL);
        }
        free(r->imageViews);
        r->imageViews = NULL;
    }
    if (r->swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(r->device, r->swapchain, NULL);
        r->swapchain = VK_NULL_HANDLE;
    }
    if (r->images) {
        free(r->images);
        r->images = NULL;
    }
    r->imageCount = 0;
}

void ft_vk_renderer_cleanup(FtVulkanRenderer *r) {
    cleanup_swapchain(r);
    if (r->vertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(r->device, r->vertexBuffer, NULL);
        r->vertexBuffer = VK_NULL_HANDLE;
    }
    if (r->vertexBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(r->device, r->vertexBufferMemory, NULL);
        r->vertexBufferMemory = VK_NULL_HANDLE;
    }
    if (r->fontSampler != VK_NULL_HANDLE) {
        vkDestroySampler(r->device, r->fontSampler, NULL);
        r->fontSampler = VK_NULL_HANDLE;
    }
    if (r->fontImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(r->device, r->fontImageView, NULL);
        r->fontImageView = VK_NULL_HANDLE;
    }
    if (r->fontImage != VK_NULL_HANDLE) {
        vkDestroyImage(r->device, r->fontImage, NULL);
        r->fontImage = VK_NULL_HANDLE;
    }
    if (r->fontImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(r->device, r->fontImageMemory, NULL);
        r->fontImageMemory = VK_NULL_HANDLE;
    }
    if (r->descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(r->device, r->descriptorPool, NULL);
        r->descriptorPool = VK_NULL_HANDLE;
    }
    if (r->descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(r->device, r->descriptorSetLayout, NULL);
        r->descriptorSetLayout = VK_NULL_HANDLE;
    }
    if (r->imageAvailableSemaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(r->device, r->imageAvailableSemaphore, NULL);
        r->imageAvailableSemaphore = VK_NULL_HANDLE;
    }
    if (r->renderFinishedSemaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(r->device, r->renderFinishedSemaphore, NULL);
        r->renderFinishedSemaphore = VK_NULL_HANDLE;
    }
    if (r->inFlightFence != VK_NULL_HANDLE) {
        vkDestroyFence(r->device, r->inFlightFence, NULL);
        r->inFlightFence = VK_NULL_HANDLE;
    }
    if (r->captureBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(r->device, r->captureBuffer, NULL);
        r->captureBuffer = VK_NULL_HANDLE;
    }
    if (r->captureBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(r->device, r->captureBufferMemory, NULL);
        r->captureBufferMemory = VK_NULL_HANDLE;
    }
    if (r->device != VK_NULL_HANDLE) {
        vkDestroyDevice(r->device, NULL);
        r->device = VK_NULL_HANDLE;
    }
    if (r->surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(r->instance, r->surface, NULL);
        r->surface = VK_NULL_HANDLE;
    }
    if (r->instance != VK_NULL_HANDLE) {
        vkDestroyInstance(r->instance, NULL);
        r->instance = VK_NULL_HANDLE;
    }
    if (r->vertShaderData) { free((void *)r->vertShaderData); r->vertShaderData = NULL; }
    if (r->fragShaderData) { free((void *)r->fragShaderData); r->fragShaderData = NULL; }
}

/* ========================================================================
   Public API
   ======================================================================== */

bool ft_vk_renderer_init(FtVulkanRenderer *r, VkInstance instance, VkSurfaceKHR surface,
                         const uint8_t *vert_data, size_t vert_size,
                         const uint8_t *frag_data, size_t frag_size) {
    memset(r, 0, sizeof(*r));
    r->instance = instance;
    r->surface = surface;
    r->vertShaderData = (const uint8_t *)malloc(vert_size);
    if (r->vertShaderData) memcpy((void *)r->vertShaderData, vert_data, vert_size);
    r->vertShaderSize = vert_size;
    r->fragShaderData = (const uint8_t *)malloc(frag_size);
    if (r->fragShaderData) memcpy((void *)r->fragShaderData, frag_data, frag_size);
    r->fragShaderSize = frag_size;

    if (!r->instance || !r->surface) return false;
    if (!pick_device_and_queue(r)) return false;
    if (!create_swapchain(r)) return false;
    if (!create_render_pass(r)) return false;
    if (!create_command_pool_and_buffers(r)) return false;
    if (!create_font_resources(r)) return false;
    if (!create_pipeline(r, vert_data, vert_size, frag_data, frag_size)) return false;
    if (!create_vertex_buffer(r)) return false;
    if (!create_framebuffers(r)) return false;
    if (!create_hi_res_resources(r)) return false;
    if (!create_sync_objects(r)) return false;
    if (!create_capture_buffer(r)) return false;

    r->ready = true;
    return true;
}

bool ft_vk_renderer_recreate_swapchain(FtVulkanRenderer *r) {
    vkDeviceWaitIdle(r->device);
    cleanup_swapchain(r);
    if (!create_swapchain(r)) return false;
    if (!create_render_pass(r)) return false;
    if (!create_command_pool_and_buffers(r)) return false;
    if (!create_pipeline(r, r->vertShaderData, r->vertShaderSize, r->fragShaderData, r->fragShaderSize)) return false;
    if (!create_framebuffers(r)) return false;
    if (!create_hi_res_resources(r)) return false;
    if (!create_capture_buffer(r)) return false;
    return true;
}

/* ========================================================================
   Display list → vertices
   ======================================================================== */

bool ft_vk_renderer_display_list(FtVulkanRenderer *r, const FtDisplayList *dl) {
    if (!dl || dl->count == 0) {
        return ft_vk_renderer_draw(r, NULL, 0);
    }

    static Vertex vertices[65536];
    uint32_t count = 0;
    uint32_t capacity = 65536;
    float screenW = (float)r->swapchainExtent.width;
    float screenH = (float)r->swapchainExtent.height;

    for (int i = 0; i < dl->count && count + 24 < capacity; i++) {
        const FtDisplayListCmd *c = &dl->cmds[i];
        switch (c->type) {
            case FT_DL_RECT:
                append_rect(vertices, &count, c->x, c->y, c->w, c->h,
                            screenW, screenH, false, c->r, c->g, c->b);
                break;
            case FT_DL_BORDER:
                append_border(vertices, &count, c->x, c->y, c->w, c->h, c->u.border.thickness,
                              screenW, screenH, false, c->r, c->g, c->b);
                break;
            case FT_DL_GLYPH:
                append_glyph(vertices, &count, c->x, c->y, c->w, c->h,
                             c->u.glyph.u0, c->u.glyph.v0, c->u.glyph.u1, c->u.glyph.v1,
                             screenW, screenH, false, c->r, c->g, c->b);
                break;
        }
    }

    return ft_vk_renderer_draw(r, vertices, count);
}

bool ft_vk_renderer_draw(FtVulkanRenderer *r, const void *vertexData, uint32_t vertexCount) {
    if (!r->ready) return false;

    ft_vk_renderer_update_vertices(r, vertexData, vertexCount);

    vkWaitForFences(r->device, 1, &r->inFlightFence, VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult result = vkAcquireNextImageKHR(r->device, r->swapchain, UINT64_MAX,
                                            r->imageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        ft_vk_renderer_recreate_swapchain(r);
        return true;
    }
    if (result != VK_SUCCESS) { LOGE("vkAcquireNextImageKHR failed: %d", result); return false; }

    /* Record command buffer for the acquired image AFTER wait, so we don't
       overwrite a command buffer that may still be in flight. */
    {
        VkDeviceSize offsets[] = {0};
        VkCommandBuffer cb = r->commandBuffers[imageIndex];
        vkResetCommandBuffer(cb, 0);

        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cb, &beginInfo);

        VkClearValue clearColor = {};
        clearColor.color.float32[0] = 0.00f;
        clearColor.color.float32[1] = 0.00f;
        clearColor.color.float32[2] = 0.00f;
        clearColor.color.float32[3] = 1.0f;

        /* Render to 8x hi-res off-screen target (64x area) */
        VkRenderPassBeginInfo renderPassInfo = {};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = r->renderPass;
        renderPassInfo.framebuffer = r->hiResFramebuffer;
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent.width = r->swapchainExtent.width * 8;
        renderPassInfo.renderArea.extent.height = r->swapchainExtent.height * 8;
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &clearColor;

        vkCmdBeginRenderPass(cb, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeline);

        VkViewport viewport = {};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = (float)(r->swapchainExtent.width * 8);
        viewport.height = (float)(r->swapchainExtent.height * 8);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cb, 0, 1, &viewport);

        VkRect2D scissor = {};
        scissor.offset = {0, 0};
        scissor.extent.width = r->swapchainExtent.width * 8;
        scissor.extent.height = r->swapchainExtent.height * 8;
        vkCmdSetScissor(cb, 0, 1, &scissor);

        vkCmdPushConstants(cb, r->pipelineLayout,
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(int), &g_subpixelMode);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                r->pipelineLayout, 0, 1, &r->descriptorSet, 0, NULL);
        vkCmdBindVertexBuffers(cb, 0, 1, &r->vertexBuffer, offsets);
        vkCmdDraw(cb, r->vertexCount, 1, 0, 0);

        vkCmdEndRenderPass(cb);

        /* Box-filter downsample: 8x → 4x → 2x → 1x (swapchain).
           Each 2x step with LINEAR averages exactly 2x2 pixels,
           chaining three stages gives a true 8x box filter. */
        VkImage swapchainImage = r->images[imageIndex];

        VkImageMemoryBarrier barriers[2] = {};
        barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].image = r->hiResImage;
        barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[0].subresourceRange.baseMipLevel = 0;
        barriers[0].subresourceRange.levelCount = 1;
        barriers[0].subresourceRange.baseArrayLayer = 0;
        barriers[0].subresourceRange.layerCount = 1;
        barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 1, &barriers[0]);

        /* Step 1: 8x → 4x */
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[0].image = r->midResImage1;
        barriers[0].srcAccessMask = 0;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 1, &barriers[0]);

        VkImageBlit blit8to4 = {};
        blit8to4.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit8to4.srcSubresource.mipLevel = 0;
        blit8to4.srcSubresource.baseArrayLayer = 0;
        blit8to4.srcSubresource.layerCount = 1;
        blit8to4.srcOffsets[0] = {0, 0, 0};
        blit8to4.srcOffsets[1] = {(int32_t)(r->swapchainExtent.width * 8), (int32_t)(r->swapchainExtent.height * 8), 1};
        blit8to4.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit8to4.dstSubresource.mipLevel = 0;
        blit8to4.dstSubresource.baseArrayLayer = 0;
        blit8to4.dstSubresource.layerCount = 1;
        blit8to4.dstOffsets[0] = {0, 0, 0};
        blit8to4.dstOffsets[1] = {(int32_t)(r->swapchainExtent.width * 4), (int32_t)(r->swapchainExtent.height * 4), 1};
        vkCmdBlitImage(cb, r->hiResImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       r->midResImage1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit8to4, VK_FILTER_LINEAR);

        /* Step 2: 4x → 2x */
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].image = r->midResImage1;
        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[1] = barriers[0];
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].image = r->midResImage2;
        barriers[1].srcAccessMask = 0;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 2, barriers);

        VkImageBlit blit4to2 = {};
        blit4to2.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit4to2.srcSubresource.mipLevel = 0;
        blit4to2.srcSubresource.baseArrayLayer = 0;
        blit4to2.srcSubresource.layerCount = 1;
        blit4to2.srcOffsets[0] = {0, 0, 0};
        blit4to2.srcOffsets[1] = {(int32_t)(r->swapchainExtent.width * 4), (int32_t)(r->swapchainExtent.height * 4), 1};
        blit4to2.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit4to2.dstSubresource.mipLevel = 0;
        blit4to2.dstSubresource.baseArrayLayer = 0;
        blit4to2.dstSubresource.layerCount = 1;
        blit4to2.dstOffsets[0] = {0, 0, 0};
        blit4to2.dstOffsets[1] = {(int32_t)(r->swapchainExtent.width * 2), (int32_t)(r->swapchainExtent.height * 2), 1};
        vkCmdBlitImage(cb, r->midResImage1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       r->midResImage2, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit4to2, VK_FILTER_LINEAR);

        /* Step 3: 2x → 1x (swapchain) */
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].image = r->midResImage2;
        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[1] = barriers[0];
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].image = swapchainImage;
        barriers[1].srcAccessMask = 0;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 2, barriers);

        VkImageBlit blit2to1 = {};
        blit2to1.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit2to1.srcSubresource.mipLevel = 0;
        blit2to1.srcSubresource.baseArrayLayer = 0;
        blit2to1.srcSubresource.layerCount = 1;
        blit2to1.srcOffsets[0] = {0, 0, 0};
        blit2to1.srcOffsets[1] = {(int32_t)(r->swapchainExtent.width * 2), (int32_t)(r->swapchainExtent.height * 2), 1};
        blit2to1.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit2to1.dstSubresource.mipLevel = 0;
        blit2to1.dstSubresource.baseArrayLayer = 0;
        blit2to1.dstSubresource.layerCount = 1;
        blit2to1.dstOffsets[0] = {0, 0, 0};
        blit2to1.dstOffsets[1] = {(int32_t)r->swapchainExtent.width, (int32_t)r->swapchainExtent.height, 1};
        vkCmdBlitImage(cb, r->midResImage2, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit2to1, VK_FILTER_LINEAR);

        barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barriers[0].image = swapchainImage;
        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[0].dstAccessMask = 0;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, NULL, 0, NULL, 1, &barriers[0]);

        if (g_captureRequested) {
            barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barriers[0].image = swapchainImage;
            barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cb,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, NULL, 0, NULL, 1, &barriers[0]);

            VkBufferImageCopy region = {};
            region.bufferOffset = 0;
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = {0, 0, 0};
            region.imageExtent = {r->swapchainExtent.width, r->swapchainExtent.height, 1};

            vkCmdCopyImageToBuffer(cb, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   r->captureBuffer, 1, &region);

            barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barriers[0].dstAccessMask = 0;
            vkCmdPipelineBarrier(cb,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                0, 0, NULL, 0, NULL, 1, &barriers[0]);
        }

        vkEndCommandBuffer(cb);
    }

    VkSemaphore waitSemaphores[] = { r->imageAvailableSemaphore };
    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSemaphore signalSemaphores[] = { r->renderFinishedSemaphore };

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &r->commandBuffers[imageIndex];
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    vkResetFences(r->device, 1, &r->inFlightFence);
    result = vkQueueSubmit(r->graphicsQueue, 1, &submitInfo, r->inFlightFence);
    if (result != VK_SUCCESS) { LOGE("vkQueueSubmit failed: %d", result); return false; }

    if (g_captureRequested) {
        /* Wait for GPU to finish the copy, then read back and analyze */
        vkWaitForFences(r->device, 1, &r->inFlightFence, VK_TRUE, UINT64_MAX);

        uint8_t *pixels = NULL;
        vkMapMemory(r->device, r->captureBufferMemory, 0, r->captureBufferSize, 0, (void **)&pixels);

        const char *bmpPath = "freetext_capture.bmp";
        save_bmp(bmpPath, pixels, r->swapchainExtent.width, r->swapchainExtent.height);
        printf("Framebuffer saved to: %s\n", bmpPath);

        analyze_subpixel_pattern(pixels, r->swapchainExtent.width, r->swapchainExtent.height);

        vkUnmapMemory(r->device, r->captureBufferMemory);
        g_captureRequested = false;
    }

    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &r->swapchain;
    presentInfo.pImageIndices = &imageIndex;

    result = vkQueuePresentKHR(r->graphicsQueue, &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        ft_vk_renderer_recreate_swapchain(r);
    } else if (result != VK_SUCCESS) {
        LOGE("vkQueuePresentKHR failed: %d", result);
    }
    return true;
}

bool ft_vk_renderer_capture_framebuffer(FtVulkanRenderer *r, const char *bmpPath) {
    (void)bmpPath; /* path is hardcoded inside draw for simplicity */
    if (!r || !r->ready) return false;
    g_captureRequested = true;
    return true;
}
