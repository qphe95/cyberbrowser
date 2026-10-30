/* gpu_vk.c - the GPU process's Vulkan backend (gpu_backend.h).
 *
 * Vulkan is loaded at run time (vulkan-1.dll, from System32), so the GPU
 * process starts without it and falls back to the CPU rasterizer.  Every
 * record of a decoded display list is one instanced quad over the pixels
 * raster_cpu.c would paint for it; gpu.frag computes the same coverage and
 * the hardware blends it.  A mask's begin copies the region under it aside;
 * its end draws mask.frag over the region, mixing what was saved with what
 * was drawn since by the mask's alpha.  Each tab has its own target image;
 * presenting copies the viewport's part of it into the swap chain of each
 * window that shows it.  Simple and synchronous: one command buffer, waited
 * for after each submit. */
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include "gpu_backend.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "gpu_decode.h"

static const uint32_t g_vert_spv[] =
#include "gpu.vert.inc"
;
static const uint32_t g_frag_spv[] =
#include "gpu.frag.inc"
;
static const uint32_t g_mask_spv[] =
#include "mask.frag.inc"
;

/* ---- the functions, loaded from vulkan-1.dll ------------------------------------- */

#define VK_FUNCS(X) \
    X(vkCreateInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice) \
    X(vkGetDeviceQueue) X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCreateCommandPool) \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkQueueSubmit) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCreateSampler) X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateDescriptorPool) \
    X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateRenderPass) \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateShaderModule) X(vkCreateGraphicsPipelines) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImage) X(vkCmdCopyImageToBuffer) \
    X(vkCmdClearColorImage) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkDeviceWaitIdle)

#define DECLARE(f) static PFN_##f f;
VK_FUNCS(DECLARE)

static PFN_vkGetInstanceProcAddr g_gipa;

/* ---- the device ---------------------------------------------------------------------- */

static VkInstance g_inst;
static VkPhysicalDevice g_pd;
static VkPhysicalDeviceMemoryProperties g_mem;
static VkDevice g_dev;
static VkQueue g_queue;
static uint32_t g_qfam;
static VkCommandPool g_pool;
static VkCommandBuffer g_cb;
static VkFence g_fence;
static VkSemaphore g_acquired;
static VkSampler g_sampler;
static VkDescriptorSetLayout g_l0, g_l1, g_l2;
static VkPipelineLayout g_draw_layout, g_mask_layout;
static VkRenderPass g_rp_clear, g_rp_load;
static VkPipeline g_draw_pipe, g_mask_pipe;
static VkDescriptorPool g_dpool;
static VkImage g_dummy_img;
static VkImageView g_dummy_view;

static const VkFormat FMT = VK_FORMAT_B8G8R8A8_UNORM;

static bool ok(VkResult r, const char *what) {
    if (r == VK_SUCCESS) return true;
    fprintf(stderr, "[gpu] %s: %d\n", what, (int)r);
    return false;
}

static int32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g_mem.memoryTypes[i].propertyFlags & want) == want) return (int32_t)i;
    return -1;
}

typedef struct Buf {
    VkBuffer buf;
    VkDeviceMemory mem;
    void *map;
    VkDeviceSize size;
} Buf;

static void buf_free(Buf *b) {
    if (b->map) vkUnmapMemory(g_dev, b->mem);
    if (b->buf) vkDestroyBuffer(g_dev, b->buf, NULL);
    if (b->mem) vkFreeMemory(g_dev, b->mem, NULL);
    memset(b, 0, sizeof(*b));
}

/* A host-visible buffer of at least size bytes (kept if big enough). */
static bool buf_need(Buf *b, VkDeviceSize size, VkBufferUsageFlags usage) {
    if (b->buf && b->size >= size) return true;
    buf_free(b);
    VkDeviceSize sz = size < 65536 ? 65536 : size + size / 2;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = sz;
    bi.usage = usage;
    if (!ok(vkCreateBuffer(g_dev, &bi, NULL, &b->buf), "vkCreateBuffer")) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_dev, b->buf, &mr);
    int32_t t = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)t;
    if (t < 0 || !ok(vkAllocateMemory(g_dev, &ai, NULL, &b->mem), "vkAllocateMemory") ||
        !ok(vkBindBufferMemory(g_dev, b->buf, b->mem, 0), "vkBindBufferMemory") ||
        !ok(vkMapMemory(g_dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map), "vkMapMemory")) {
        buf_free(b);
        return false;
    }
    b->size = sz;
    return true;
}

typedef struct Img {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    VkImageLayout layout;
    uint32_t w, h;
} Img;

static void img_free(Img *i) {
    if (i->view) vkDestroyImageView(g_dev, i->view, NULL);
    if (i->img) vkDestroyImage(g_dev, i->img, NULL);
    if (i->mem) vkFreeMemory(g_dev, i->mem, NULL);
    memset(i, 0, sizeof(*i));
}

static bool img_new(Img *i, VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    memset(i, 0, sizeof(*i));
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent.width = w;
    ci.extent.height = h;
    ci.extent.depth = 1;
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!ok(vkCreateImage(g_dev, &ci, NULL, &i->img), "vkCreateImage")) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_dev, i->img, &mr);
    int32_t t = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)t;
    if (t < 0 || !ok(vkAllocateMemory(g_dev, &ai, NULL, &i->mem), "vkAllocateMemory (image)") ||
        !ok(vkBindImageMemory(g_dev, i->img, i->mem, 0), "vkBindImageMemory")) {
        img_free(i);
        return false;
    }
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = i->img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if (!ok(vkCreateImageView(g_dev, &vi, NULL, &i->view), "vkCreateImageView")) {
        img_free(i);
        return false;
    }
    i->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    i->w = w;
    i->h = h;
    return true;
}

/* Move image i to layout `to` (all earlier work done before anything after). */
static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout *layout, VkImageLayout to) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = *layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, 1, &b);
    *layout = to;
}

static bool begin(void) {
    vkResetCommandBuffer(g_cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return ok(vkBeginCommandBuffer(g_cb, &bi), "vkBeginCommandBuffer");
}

/* End, submit (after `wait`, if any) and wait for it. */
static bool submit(VkSemaphore wait) {
    if (!ok(vkEndCommandBuffer(g_cb), "vkEndCommandBuffer")) return false;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cb;
    if (wait) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &wait;
        si.pWaitDstStageMask = &stage;
    }
    if (!ok(vkQueueSubmit(g_queue, 1, &si, g_fence), "vkQueueSubmit")) return false;
    bool done = ok(vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, 10000000000ull), "vkWaitForFences");
    vkResetFences(g_dev, 1, &g_fence);
    return done;
}

/* ---- textures --------------------------------------------------------------------------- */

struct GpuTexture {
    Img img;
    bool rgba;
};

static Buf g_staging;

static GpuTexture *vk_texture_new(bool rgba, uint32_t w, uint32_t h) {
    GpuTexture *t = (GpuTexture *)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->rgba = rgba;
    if (!img_new(&t->img, rgba ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8_UNORM, w, h,
                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        free(t);
        return NULL;
    }
    return t;
}

static void vk_texture_update(GpuTexture *t, uint32_t y0, uint32_t rows, const uint8_t *pixels) {
    if (!t || rows == 0 || y0 >= t->img.h) return;
    if (rows > t->img.h - y0) rows = t->img.h - y0;
    VkDeviceSize bytes = (VkDeviceSize)t->img.w * rows * (t->rgba ? 4 : 1);
    if (!buf_need(&g_staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) || !begin()) return;
    memcpy(g_staging.map, pixels, (size_t)bytes);
    barrier(g_cb, t->img.img, &t->img.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageOffset.y = (int32_t)y0;
    c.imageExtent.width = t->img.w;
    c.imageExtent.height = rows;
    c.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(g_cb, g_staging.buf, t->img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(g_cb, t->img.img, &t->img.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    submit(VK_NULL_HANDLE);
}

static void vk_texture_free(GpuTexture *t) {
    if (!t) return;
    vkDeviceWaitIdle(g_dev);
    img_free(&t->img);
    free(t);
}

/* ---- targets ------------------------------------------------------------------------------ */

struct GpuTarget {
    Img img;                        /* the tab's content */
    VkFramebuffer fb;
    Img current;                    /* a copy of img, for a mask's end */
    Img saved[GPU_MAX_DEPTH];       /* what was under each open mask, made when first needed */
};

static void vk_target_free(GpuTarget *t) {
    if (!t) return;
    vkDeviceWaitIdle(g_dev);
    if (t->fb) vkDestroyFramebuffer(g_dev, t->fb, NULL);
    img_free(&t->img);
    img_free(&t->current);
    for (int i = 0; i < GPU_MAX_DEPTH; i++) img_free(&t->saved[i]);
    free(t);
}

static const VkImageUsageFlags TARGET_USE = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

static GpuTarget *target_new(uint32_t w, uint32_t h) {
    GpuTarget *t = (GpuTarget *)calloc(1, sizeof(*t));
    if (!t) return NULL;
    if (!img_new(&t->img, FMT, w, h, TARGET_USE) || !img_new(&t->current, FMT, w, h, TARGET_USE)) {
        vk_target_free(t);
        return NULL;
    }
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fi.renderPass = g_rp_load;
    fi.attachmentCount = 1;
    fi.pAttachments = &t->img.view;
    fi.width = w;
    fi.height = h;
    fi.layers = 1;
    if (!ok(vkCreateFramebuffer(g_dev, &fi, NULL, &t->fb), "vkCreateFramebuffer")) {
        vk_target_free(t);
        return NULL;
    }
    return t;
}

/* ---- the records ---------------------------------------------------------------------------- */

typedef struct Item {
    float rect[4];
    float color[4];
    float uv[4];
    int32_t quad[4];
    int32_t info[4];
    float extra[4];
} Item;

typedef struct Mask {
    float box[4], img[4], rx[4], ry[4], border[4], ex_rx[4], ex_ry[4];
    float stop_pos[8], stop_alpha[8];
    float misc[4];
    int32_t kind[4];
} Mask;

/* What one step of the frame does. */
typedef enum { OP_DRAW, OP_SAVE, OP_BLEND } OpKind;
typedef struct Op {
    OpKind kind;
    uint32_t first, count;          /* OP_DRAW: items; OP_BLEND: the blend's item */
    GpuTexture *tex;                /* OP_DRAW, OP_BLEND: the texture (NULL: the stand-in) */
    int level;                      /* OP_SAVE, OP_BLEND */
    int32_t box[4];                 /* OP_SAVE, OP_BLEND: x0, y0, x1, y1 */
} Op;

static Buf g_items, g_masks;
static Op *g_ops;
static uint32_t g_ops_cap;

static int32_t clampi(double v, int32_t lo, int32_t hi) {
    return v < lo ? lo : v > hi ? hi : (int32_t)v;
}

static bool op_push(uint32_t *n, Op op) {
    if (*n == g_ops_cap) {
        uint32_t cap = g_ops_cap ? g_ops_cap * 2 : 1024;
        Op *o = (Op *)realloc(g_ops, cap * sizeof(Op));
        if (!o) return false;
        g_ops = o;
        g_ops_cap = cap;
    }
    g_ops[(*n)++] = op;
    return true;
}

static VkDescriptorSet tex_set(GpuTexture *t) {
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = g_dpool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g_l1;
    VkDescriptorSet s = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(g_dev, &ai, &s) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkDescriptorImageInfo ii = { g_sampler, t ? t->img.view : g_dummy_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = s;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(g_dev, 1, &w, 0, NULL);
    return s;
}

static bool vk_render(GpuTarget **tp, uint32_t w, uint32_t h, const DisplayList *dl, GpuTexture *const *atlases,
                      GpuTexture *const *images, const RasterResources *res) {
    (void)res;
    GpuTarget *t = *tp;
    if (!t || t->img.w != w || t->img.h != h) {
        vk_target_free(t);
        t = *tp = target_new(w, h);
        if (!t) return false;
    }
    /* The records: one item each, and the steps that draw them. */
    uint32_t nitems = 0, nops = 0;
    if (!buf_need(&g_items, (VkDeviceSize)(dl->count + 1) * sizeof(Item), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !buf_need(&g_masks, (VkDeviceSize)(dl->mask_count + 1) * sizeof(Mask), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
        return false;
    Mask *mk = (Mask *)g_masks.map;
    for (int i = 0; i < dl->mask_count; i++) {
        const DisplayMask *m = &dl->masks[i];
        Mask *o = &mk[i];
        memset(o, 0, sizeof(*o));
        o->box[0] = m->x; o->box[1] = m->y; o->box[2] = m->w; o->box[3] = m->h;
        o->img[0] = m->ix; o->img[1] = m->iy; o->img[2] = m->iw; o->img[3] = m->ih;
        for (int k = 0; k < 4; k++) {
            o->rx[k] = m->rx[k]; o->ry[k] = m->ry[k]; o->border[k] = m->border[k];
            o->ex_rx[k] = m->ex_rx[k]; o->ex_ry[k] = m->ex_ry[k];
        }
        for (int k = 0; k < 8; k++) { o->stop_pos[k] = m->stop_pos[k]; o->stop_alpha[k] = m->stop_alpha[k]; }
        o->misc[0] = m->angle_deg; o->misc[1] = m->blur; o->misc[2] = m->repeat ? 1.0f : 0.0f;
        o->misc[3] = (float)m->nstops;
        o->kind[0] = m->kind;
        o->kind[1] = m->kind == 1 && m->image_handle >= 0 && m->image_handle < GPU_MAX_IMAGES &&
                     images[m->image_handle] ? 1 : 0;
    }
    Item *it = (Item *)g_items.map;
    int level = 0;
    int32_t W = (int32_t)w, H = (int32_t)h;
    for (int i = 0; i < dl->count; i++) {
        const DisplayListCmd *c = &dl->cmds[i];
        if (c->type == DL_MASK_BEGIN) {
            const DisplayMask *m = &dl->masks[c->u.mask_index];
            Op op = { OP_SAVE, 0, 0, NULL, level,
                      { clampi(floor(m->x), 0, W), clampi(floor(m->y), 0, H), clampi(ceil(m->x + m->w), 0, W),
                        clampi(ceil(m->y + m->h), 0, H) } };
            if (level >= GPU_MAX_DEPTH || !op_push(&nops, op)) return false;
            level++;
            continue;
        }
        if (c->type == DL_MASK_END) {
            if (level == 0) continue;
            level--;
            /* Its begin's op, the latest save at this level. */
            const Op *save = NULL;
            for (uint32_t k = nops; k-- > 0;)
                if (g_ops[k].kind == OP_SAVE && g_ops[k].level == level) { save = &g_ops[k]; break; }
            if (!save) continue;
            int32_t box[4] = { save->box[0], save->box[1], save->box[2], save->box[3] };
            Item *o = &it[nitems];
            memset(o, 0, sizeof(*o));
            memcpy(o->quad, box, sizeof(box));
            /* The mask is the one the begin named: find it the same way. */
            int mi = -1, depth = 0;
            for (int k = i - 1; k >= 0; k--) {
                if (dl->cmds[k].type == DL_MASK_END) depth++;
                else if (dl->cmds[k].type == DL_MASK_BEGIN && depth-- == 0) { mi = dl->cmds[k].u.mask_index; break; }
            }
            if (mi < 0) continue;
            o->info[1] = mi;
            o->info[3] = level;
            const DisplayMask *m = &dl->masks[mi];
            GpuTexture *mt = m->kind == 1 && m->image_handle >= 0 && m->image_handle < GPU_MAX_IMAGES
                                 ? images[m->image_handle] : NULL;
            Op op = { OP_BLEND, nitems, 1, mt, level, { box[0], box[1], box[2], box[3] } };
            if (!op_push(&nops, op)) return false;
            nitems++;
            continue;
        }
        Item *o = &it[nitems];
        memset(o, 0, sizeof(*o));
        o->rect[0] = c->x; o->rect[1] = c->y; o->rect[2] = c->w; o->rect[3] = c->h;
        o->color[0] = c->r; o->color[1] = c->g; o->color[2] = c->b; o->color[3] = c->a;
        o->info[0] = (int32_t)c->type;
        GpuTexture *tex = NULL;
        double x0 = floor(c->x), y0 = floor(c->y), x1, y1;
        switch (c->type) {
        case DL_RECT:
        case DL_BORDER:
            x1 = floor(c->x + c->w) + 1;
            y1 = floor(c->y + c->h) + 1;
            if (c->type == DL_BORDER) {
                float th = c->u.border.thickness > 0 ? c->u.border.thickness : 1.0f;
                o->info[2] = (int32_t)ceilf(th);
                if (o->info[2] > 4096) o->info[2] = 4096;
            }
            break;
        case DL_GLYPH:
            if (c->w <= 0 || c->h <= 0) continue;
            x1 = ceil(c->x + c->w);
            y1 = ceil(c->y + c->h);
            tex = atlases[c->u.glyph.font_slot];
            o->uv[0] = c->u.glyph.u0; o->uv[1] = c->u.glyph.v0; o->uv[2] = c->u.glyph.u1; o->uv[3] = c->u.glyph.v1;
            break;
        case DL_IMAGE:
            x1 = floor(c->x + c->w);
            y1 = floor(c->y + c->h);
            tex = c->u.image.image_handle >= 0 && c->u.image.image_handle < GPU_MAX_IMAGES
                      ? images[c->u.image.image_handle] : NULL;
            break;
        case DL_SHADOW:
            x1 = ceil(c->x + c->w);
            y1 = ceil(c->y + c->h);
            o->info[1] = c->u.mask_index;
            break;
        default:
            continue;
        }
        if ((c->type == DL_GLYPH || c->type == DL_IMAGE) && !tex) continue;
        o->quad[0] = clampi(x0, 0, W); o->quad[1] = clampi(y0, 0, H);
        o->quad[2] = clampi(x1, 0, W); o->quad[3] = clampi(y1, 0, H);
        if (o->quad[2] <= o->quad[0] || o->quad[3] <= o->quad[1]) continue;
        /* Into the current run if it draws with the same texture. */
        Op *last = nops ? &g_ops[nops - 1] : NULL;
        if (last && last->kind == OP_DRAW && last->tex == tex && last->first + last->count == nitems) {
            last->count++;
        } else {
            Op op = { OP_DRAW, nitems, 1, tex, 0, { 0, 0, 0, 0 } };
            if (!op_push(&nops, op)) return false;
        }
        nitems++;
    }

    /* Record and run them. */
    vkResetDescriptorPool(g_dev, g_dpool, 0);
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = g_dpool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g_l0;
    VkDescriptorSet s0;
    if (vkAllocateDescriptorSets(g_dev, &ai, &s0) != VK_SUCCESS) return false;
    VkDescriptorBufferInfo bis[2] = { { g_items.buf, 0, VK_WHOLE_SIZE }, { g_masks.buf, 0, VK_WHOLE_SIZE } };
    VkWriteDescriptorSet wr[2];
    for (int k = 0; k < 2; k++) {
        VkWriteDescriptorSet x = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        x.dstSet = s0;
        x.dstBinding = (uint32_t)k;
        x.descriptorCount = 1;
        x.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        x.pBufferInfo = &bis[k];
        wr[k] = x;
    }
    vkUpdateDescriptorSets(g_dev, 2, wr, 0, NULL);

    if (!begin()) return false;
    VkClearValue white = { { { 1.0f, 1.0f, 1.0f, 1.0f } } };
    VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rb.framebuffer = t->fb;
    rb.renderArea.extent.width = w;
    rb.renderArea.extent.height = h;
    rb.clearValueCount = 1;
    rb.pClearValues = &white;
    barrier(g_cb, t->img.img, &t->img.layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    rb.renderPass = g_rp_clear;
    vkCmdBeginRenderPass(g_cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    rb.renderPass = g_rp_load;
    VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { w, h } };
    float size[2] = { (float)w, (float)h };
    bool in_pass = true;
    VkPipeline bound = VK_NULL_HANDLE;
    for (uint32_t k = 0; k < nops; k++) {
        Op *op = &g_ops[k];
        if (op->kind == OP_SAVE || op->kind == OP_BLEND) {
            if (in_pass) vkCmdEndRenderPass(g_cb);
            in_pass = false;
            bound = VK_NULL_HANDLE;
            int32_t bw = op->box[2] - op->box[0], bh = op->box[3] - op->box[1];
            Img *dst = op->kind == OP_SAVE ? &t->saved[op->level] : &t->current;
            if (op->kind == OP_SAVE && !dst->img && !img_new(dst, FMT, w, h, TARGET_USE)) return false;
            if (bw > 0 && bh > 0) {
                VkImageLayout tl = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                barrier(g_cb, t->img.img, &tl, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                barrier(g_cb, dst->img, &dst->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkImageCopy cp;
                memset(&cp, 0, sizeof(cp));
                cp.srcSubresource.aspectMask = cp.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                cp.srcSubresource.layerCount = cp.dstSubresource.layerCount = 1;
                cp.srcOffset.x = cp.dstOffset.x = op->box[0];
                cp.srcOffset.y = cp.dstOffset.y = op->box[1];
                cp.extent.width = (uint32_t)bw;
                cp.extent.height = (uint32_t)bh;
                cp.extent.depth = 1;
                vkCmdCopyImage(g_cb, t->img.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->img,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
                barrier(g_cb, dst->img, &dst->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                barrier(g_cb, t->img.img, &tl, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            }
            if (op->kind == OP_SAVE || bw <= 0 || bh <= 0) continue;
            /* The blend: saved and current mixed by the mask. */
            Img *saved = &t->saved[op->level];
            if (!saved->img) continue;
            ai.pSetLayouts = &g_l2;
            VkDescriptorSet s2;
            if (vkAllocateDescriptorSets(g_dev, &ai, &s2) != VK_SUCCESS) return false;
            VkDescriptorImageInfo ii[2] = { { g_sampler, saved->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                            { g_sampler, t->current.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
            VkWriteDescriptorSet w2 = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w2.dstSet = s2;
            w2.descriptorCount = 2;
            w2.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w2.pImageInfo = ii;
            vkUpdateDescriptorSets(g_dev, 1, &w2, 0, NULL);
            VkDescriptorSet s1 = tex_set(op->tex);
            if (!s1) return false;
            vkCmdBeginRenderPass(g_cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
            in_pass = true;
            vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_mask_pipe);
            bound = g_mask_pipe;
            vkCmdSetViewport(g_cb, 0, 1, &vp);
            vkCmdSetScissor(g_cb, 0, 1, &sc);
            VkDescriptorSet sets[3] = { s0, s1, s2 };
            vkCmdBindDescriptorSets(g_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_mask_layout, 0, 3, sets, 0, NULL);
            vkCmdPushConstants(g_cb, g_mask_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(size), size);
            vkCmdDraw(g_cb, 6, 1, 0, op->first);
            continue;
        }
        if (!in_pass) {
            vkCmdBeginRenderPass(g_cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
            in_pass = true;
        }
        if (bound != g_draw_pipe) {
            vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_draw_pipe);
            bound = g_draw_pipe;
            vkCmdSetViewport(g_cb, 0, 1, &vp);
            vkCmdSetScissor(g_cb, 0, 1, &sc);
            vkCmdPushConstants(g_cb, g_draw_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(size), size);
        }
        VkDescriptorSet s1 = tex_set(op->tex);
        if (!s1) return false;
        VkDescriptorSet sets[2] = { s0, s1 };
        vkCmdBindDescriptorSets(g_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_draw_layout, 0, 2, sets, 0, NULL);
        vkCmdDraw(g_cb, 6, op->count, 0, op->first);
    }
    if (in_pass) vkCmdEndRenderPass(g_cb);
    return submit(VK_NULL_HANDLE);
}

/* ---- presenting and reading back ------------------------------------------------------------- */

/* A window's swap chain: one per UI window that shows a tab. */
typedef struct Chain {
    HWND hwnd;
    VkSurfaceKHR surface;
    VkSwapchainKHR chain;
    VkImage img[8];
    uint32_t n, w, h;
} Chain;

#define MAX_CHAINS 16
static Chain g_chains[MAX_CHAINS];

static void chain_free(Chain *c) {
    vkDeviceWaitIdle(g_dev);
    if (c->chain) vkDestroySwapchainKHR(g_dev, c->chain, NULL);
    c->chain = VK_NULL_HANDLE;
    c->w = c->h = 0;
}

static void vk_window_gone(HWND hwnd) {
    for (int i = 0; i < MAX_CHAINS; i++) {
        Chain *c = &g_chains[i];
        if (!c->hwnd || c->hwnd != hwnd) continue;
        chain_free(c);
        if (c->surface) vkDestroySurfaceKHR(g_inst, c->surface, NULL);
        memset(c, 0, sizeof(*c));
    }
}

/* hwnd's chain, made (or remade at its window's size) if need be. */
static Chain *chain_make(HWND hwnd) {
    Chain *c = NULL;
    for (int i = 0; i < MAX_CHAINS && !c; i++)
        if (g_chains[i].hwnd == hwnd) c = &g_chains[i];
    if (!c) {
        for (int i = 0; i < MAX_CHAINS && !c; i++)
            if (!g_chains[i].hwnd) c = &g_chains[i];
        if (!c) {
            fprintf(stderr, "[gpu] too many windows\n");
            return NULL;
        }
        VkWin32SurfaceCreateInfoKHR si = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
        si.hinstance = GetModuleHandleW(NULL);
        si.hwnd = hwnd;
        if (!ok(vkCreateWin32SurfaceKHR(g_inst, &si, NULL, &c->surface), "vkCreateWin32SurfaceKHR")) return NULL;
        c->hwnd = hwnd;
        VkBool32 can = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_pd, g_qfam, c->surface, &can);
        if (!can) {
            fprintf(stderr, "[gpu] the queue can't present to that window\n");
            vk_window_gone(hwnd);
            return NULL;
        }
    }
    VkSurfaceCapabilitiesKHR caps;
    if (!ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_pd, c->surface, &caps), "surface capabilities")) return NULL;
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return NULL;      /* minimized */
    if (c->chain && caps.currentExtent.width == c->w && caps.currentExtent.height == c->h) return c;
    VkSwapchainKHR old = c->chain;
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = c->surface;
    ci.minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount) ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = FMT;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = caps.currentExtent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    vkDeviceWaitIdle(g_dev);
    VkResult r = vkCreateSwapchainKHR(g_dev, &ci, NULL, &c->chain);
    if (old) vkDestroySwapchainKHR(g_dev, old, NULL);
    if (!ok(r, "vkCreateSwapchainKHR")) {
        c->chain = VK_NULL_HANDLE;
        c->w = c->h = 0;
        return NULL;
    }
    c->n = 8;
    vkGetSwapchainImagesKHR(g_dev, c->chain, &c->n, c->img);
    c->w = caps.currentExtent.width;
    c->h = caps.currentExtent.height;
    return c;
}

static bool vk_present(HWND hwnd, uint32_t w, uint32_t h, GpuTarget *t, int32_t src_y) {
    (void)w;
    (void)h;
    for (int attempt = 0; attempt < 2; attempt++) {
        Chain *c = chain_make(hwnd);
        if (!c) return false;
        uint32_t idx = 0;
        VkResult r = vkAcquireNextImageKHR(g_dev, c->chain, 2000000000ull, g_acquired, VK_NULL_HANDLE, &idx);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { chain_free(c); continue; }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return ok(r, "vkAcquireNextImageKHR");
        if (!begin()) return false;
        VkImageLayout cl = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier(g_cb, c->img[idx], &cl, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue white = { { 1.0f, 1.0f, 1.0f, 1.0f } };
        VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(g_cb, c->img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &all);
        /* Rows [src_y, src_y + window height) of the target; what lies
         * outside it stays white. */
        int64_t top = src_y < 0 ? 0 : src_y, bottom = (int64_t)src_y + c->h;
        if (bottom > t->img.h) bottom = t->img.h;
        uint32_t cw = t->img.w < c->w ? t->img.w : c->w;
        if (bottom > top && cw) {
            barrier(g_cb, t->img.img, &t->img.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkImageCopy cp;
            memset(&cp, 0, sizeof(cp));
            cp.srcSubresource.aspectMask = cp.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            cp.srcSubresource.layerCount = cp.dstSubresource.layerCount = 1;
            cp.srcOffset.y = (int32_t)top;
            cp.dstOffset.y = (int32_t)(top - src_y);
            cp.extent.width = cw;
            cp.extent.height = (uint32_t)(bottom - top);
            cp.extent.depth = 1;
            vkCmdCopyImage(g_cb, t->img.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->img[idx],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        }
        barrier(g_cb, c->img[idx], &cl, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        if (!submit(g_acquired)) return false;
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.swapchainCount = 1;
        pi.pSwapchains = &c->chain;
        pi.pImageIndices = &idx;
        r = vkQueuePresentKHR(g_queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { chain_free(c); continue; }
        return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
    }
    return false;
}

static Buf g_readback;

static bool vk_readback(GpuTarget *t, uint8_t *out, size_t cap, uint32_t *w, uint32_t *h) {
    VkDeviceSize bytes = (VkDeviceSize)t->img.w * t->img.h * 4;
    if (bytes > cap) return false;
    if (!buf_need(&g_readback, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT) || !begin()) return false;
    barrier(g_cb, t->img.img, &t->img.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = t->img.w;
    c.imageExtent.height = t->img.h;
    c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(g_cb, t->img.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_readback.buf, 1, &c);
    if (!submit(VK_NULL_HANDLE)) return false;
    memcpy(out, g_readback.map, (size_t)bytes);
    for (size_t i = 3; i < (size_t)bytes; i += 4) out[i] = 255;
    *w = t->img.w;
    *h = t->img.h;
    return true;
}

/* ---- setting up ---------------------------------------------------------------------------- */

static VkShaderModule shader(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    ok(vkCreateShaderModule(g_dev, &ci, NULL, &m), "vkCreateShaderModule");
    return m;
}

static VkRenderPass render_pass(bool clear) {
    VkAttachmentDescription a;
    memset(&a, 0, sizeof(a));
    a.format = FMT;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp;
    memset(&sp, 0, sizeof(sp));
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &ref;
    VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ci.attachmentCount = 1;
    ci.pAttachments = &a;
    ci.subpassCount = 1;
    ci.pSubpasses = &sp;
    VkRenderPass rp = VK_NULL_HANDLE;
    ok(vkCreateRenderPass(g_dev, &ci, NULL, &rp), "vkCreateRenderPass");
    return rp;
}

static VkPipeline pipeline(VkPipelineLayout layout, VkShaderModule vs, VkShaderModule fs, bool blend) {
    VkPipelineShaderStageCreateInfo st[2];
    memset(st, 0, sizeof(st));
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = fs;
    st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba;
    memset(&ba, 0, sizeof(ba));
    ba.colorWriteMask = 0xf;
    if (blend) {
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &ds;
    ci.layout = layout;
    ci.renderPass = g_rp_load;
    VkPipeline p = VK_NULL_HANDLE;
    ok(vkCreateGraphicsPipelines(g_dev, VK_NULL_HANDLE, 1, &ci, NULL, &p), "vkCreateGraphicsPipelines");
    return p;
}

static VkDescriptorSetLayout set_layout(VkDescriptorType type, uint32_t n, VkShaderStageFlags stages) {
    VkDescriptorSetLayoutBinding b[2];
    memset(b, 0, sizeof(b));
    for (uint32_t i = 0; i < n; i++) {
        b[i].binding = i;
        b[i].descriptorType = type;
        b[i].descriptorCount = 1;
        b[i].stageFlags = stages;
    }
    VkDescriptorSetLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    ci.bindingCount = n;
    ci.pBindings = b;
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    ok(vkCreateDescriptorSetLayout(g_dev, &ci, NULL, &l), "vkCreateDescriptorSetLayout");
    return l;
}

static VkPipelineLayout pipeline_layout(const VkDescriptorSetLayout *sets, uint32_t n) {
    VkPushConstantRange pr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 2 * sizeof(float) };
    VkPipelineLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    ci.setLayoutCount = n;
    ci.pSetLayouts = sets;
    ci.pushConstantRangeCount = 1;
    ci.pPushConstantRanges = &pr;
    VkPipelineLayout l = VK_NULL_HANDLE;
    ok(vkCreatePipelineLayout(g_dev, &ci, NULL, &l), "vkCreatePipelineLayout");
    return l;
}

static bool load(void) {
    HMODULE dll = LoadLibraryW(L"vulkan-1.dll");
    if (!dll) {
        fprintf(stderr, "[gpu] no vulkan-1.dll\n");
        return false;
    }
    g_gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(dll, "vkGetInstanceProcAddr");
    if (!g_gipa) return false;
    vkCreateInstance = (PFN_vkCreateInstance)g_gipa(NULL, "vkCreateInstance");
    if (!vkCreateInstance) return false;
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "cb_gpu";
    app.apiVersion = VK_API_VERSION_1_1;
    const char *exts[2] = { "VK_KHR_surface", "VK_KHR_win32_surface" };
    VkInstanceCreateInfo ii = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ii.pApplicationInfo = &app;
    ii.enabledExtensionCount = 2;
    ii.ppEnabledExtensionNames = exts;
    if (!ok(vkCreateInstance(&ii, NULL, &g_inst), "vkCreateInstance")) return false;
#define LOAD(f) f = (PFN_##f)g_gipa(g_inst, #f); if (!f) { fprintf(stderr, "[gpu] no %s\n", #f); return false; }
    VK_FUNCS(LOAD)
#undef LOAD
    return true;
}

static bool device(void) {
    uint32_t n = 8;
    VkPhysicalDevice pds[8];
    if (vkEnumeratePhysicalDevices(g_inst, &n, pds) < 0 || n == 0) return false;
    /* A discrete GPU first, with a graphics queue. */
    int best = -1, best_score = -1;
    uint32_t best_q = 0;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pds[i], &p);
        uint32_t nq = 16;
        VkQueueFamilyProperties q[16];
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, q);
        for (uint32_t k = 0; k < nq; k++) {
            if (!(q[k].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2
                      : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
            if (score > best_score) { best = (int)i; best_score = score; best_q = k; }
            break;
        }
    }
    if (best < 0) return false;
    g_pd = pds[best];
    g_qfam = best_q;
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(g_pd, &p);
    fprintf(stderr, "[gpu] %s\n", p.deviceName);
    vkGetPhysicalDeviceMemoryProperties(g_pd, &g_mem);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = g_qfam;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    const char *dexts[1] = { "VK_KHR_swapchain" };
    VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1;
    di.ppEnabledExtensionNames = dexts;
    if (!ok(vkCreateDevice(g_pd, &di, NULL, &g_dev), "vkCreateDevice")) return false;
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);
    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = g_qfam;
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandBufferCount = 1;
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (!ok(vkCreateCommandPool(g_dev, &pi, NULL, &g_pool), "vkCreateCommandPool")) return false;
    ai.commandPool = g_pool;
    return ok(vkAllocateCommandBuffers(g_dev, &ai, &g_cb), "vkAllocateCommandBuffers") &&
           ok(vkCreateFence(g_dev, &fi, NULL, &g_fence), "vkCreateFence") &&
           ok(vkCreateSemaphore(g_dev, &si, NULL, &g_acquired), "vkCreateSemaphore");
}

static bool pipelines(void) {
    VkSamplerCreateInfo sc = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sc.magFilter = sc.minFilter = VK_FILTER_NEAREST;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!ok(vkCreateSampler(g_dev, &sc, NULL, &g_sampler), "vkCreateSampler")) return false;
    g_l0 = set_layout(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    g_l1 = set_layout(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    g_l2 = set_layout(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, VK_SHADER_STAGE_FRAGMENT_BIT);
    VkDescriptorSetLayout draw_sets[2] = { g_l0, g_l1 }, mask_sets[3] = { g_l0, g_l1, g_l2 };
    g_draw_layout = pipeline_layout(draw_sets, 2);
    g_mask_layout = pipeline_layout(mask_sets, 3);
    g_rp_clear = render_pass(true);
    g_rp_load = render_pass(false);
    VkShaderModule vs = shader(g_vert_spv, sizeof(g_vert_spv)), fs = shader(g_frag_spv, sizeof(g_frag_spv)),
                   ms = shader(g_mask_spv, sizeof(g_mask_spv));
    if (!g_l0 || !g_l1 || !g_l2 || !g_draw_layout || !g_mask_layout || !g_rp_clear || !g_rp_load || !vs || !fs || !ms)
        return false;
    g_draw_pipe = pipeline(g_draw_layout, vs, fs, true);
    g_mask_pipe = pipeline(g_mask_layout, vs, ms, false);
    VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64 },
                                      { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u << 17 } };
    VkDescriptorPoolCreateInfo dp = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dp.maxSets = 1u << 17;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = sizes;
    if (!g_draw_pipe || !g_mask_pipe || !ok(vkCreateDescriptorPool(g_dev, &dp, NULL, &g_dpool), "descriptor pool"))
        return false;
    /* The stand-in texture for records that sample none. */
    Img dummy;
    if (!img_new(&dummy, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !begin())
        return false;
    barrier(g_cb, dummy.img, &dummy.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue zero = { { 0, 0, 0, 0 } };
    VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(g_cb, dummy.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &all);
    barrier(g_cb, dummy.img, &dummy.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!submit(VK_NULL_HANDLE)) return false;
    g_dummy_img = dummy.img;
    g_dummy_view = dummy.view;
    return true;
}

static const GpuBackend g_vk = {
    "Vulkan", vk_texture_new, vk_texture_update, vk_texture_free, vk_render, vk_target_free, vk_present, vk_readback,
    vk_window_gone,
};

const GpuBackend *gpu_vk_backend(void) {
    if (!load() || !device() || !pipelines()) {
        fprintf(stderr, "[gpu] Vulkan can't be used here\n");
        return NULL;
    }
    return &g_vk;
}
