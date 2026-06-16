#ifndef VULKAN_UI_H
#define VULKAN_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "http_download.h"
#include <pthread.h>
#include "ui_layout.h"

#ifdef BE_PLATFORM_ANDROID
#include <android/log.h>
#include <android/native_window.h>
#include <android/asset_manager.h>
#include <jni.h>
struct android_app;
#define LOG_TAG "minimalvulkan"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#include "platform.h"
#define LOG_TAG "minimalvulkan"
#define LOGI(...) platform_log(LOG_LEVEL_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) platform_log(LOG_LEVEL_ERROR, LOG_TAG, __VA_ARGS__)
#endif

typedef struct ShaderBlob {
    uint8_t *data;
    size_t size;
} ShaderBlob;

typedef struct VulkanApp {
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
    VkSurfaceTransformFlagBitsKHR surfaceTransform;
    bool mirrorX;

    VkCommandPool commandPool;
    VkCommandBuffer *commandBuffers;

    VkSemaphore imageAvailableSemaphore;
    VkSemaphore renderFinishedSemaphore;
    VkFence inFlightFences[2];
    uint32_t currentFenceIndex;

#ifdef BE_PLATFORM_ANDROID
    AAssetManager *assetManager;
    ANativeWindow *window;
    struct android_app *androidApp;
    JavaVM *javaVm;
#endif

    bool ready;
    float densityScale;

    int32_t windowWidth;
    int32_t windowHeight;

    pthread_mutex_t uiMutex;
    pthread_t workerThread;
    bool workerRunning;
    bool inputActive;
    char urlInput[2048];
    size_t urlLen;
    char statusText[256];
    char downloadPath[512];
    float progress;
    size_t prev_downloaded_bytes;
    double prev_download_time;
    DownloadState downloadState;
    char videoTitle[256];
    char thumbnailUrl[2048];

    /* Custom filename field */
    char customFilename[256];
    bool customFilenameActive;
    bool keepVideo;
    float uiFilenameX0, uiFilenameX1, uiFilenameY0, uiFilenameY1;
    float uiFilenameTextX, uiFilenameTextY;
    float uiFilenameGlyphW, uiFilenameGlyphH;
    int uiFilenameCharsPerLine;

    float uiStartY;
    float uiGlyphW;
    float uiGlyphH;
    float uiGap;
    int uiLineCount;
    int uiKeyboardStartLine;
    int uiKeyboardLineCount;
    float keyboardHeightPx;
    float uiScrollOffsetY;   /* negative = content shifted up (keyboard / scroll) */
    float uiContentBottom;   /* bottom Y of last rendered content (unshifted) */
    bool uiScrollNeedsFocus; /* true when scroll should snap to focused field */
    float uiKeepVideoX0;
    float uiKeepVideoY0;
    float uiKeepVideoX1;
    float uiKeepVideoY1;
    float uiUrlX0;
    float uiUrlX1;
    float uiUrlY0;
    float uiUrlY1;
    float uiUrlTextX;
    float uiUrlTextY;
    float uiUrlGlyphW;
    float uiUrlGlyphH;
    int uiUrlCharsPerLine;

    /* Album art thumbnail */
    bool albumArtVisible;
    VkImage albumArtImage;
    VkDeviceMemory albumArtMemory;
    VkImageView albumArtView;
    VkSampler albumArtSampler;
    VkDescriptorSet albumArtDescriptorSet;
    uint32_t albumArtWidth;
    uint32_t albumArtHeight;
    uint32_t albumArtVertexOffset;
    uint32_t albumArtVertexCount;

    /* Shader data for pipeline recreation */
    const uint8_t *vertShaderData;
    size_t vertShaderSize;
    const uint8_t *fragShaderData;
    size_t fragShaderSize;
} VulkanApp;

typedef struct TextField {
    char buffer[2048];
    int length;
    int cursorPos;
    int selStart;
    int selEnd;
    bool isDragging;
    int dragAnchor;
} TextField;

typedef enum InputFocus {
    FOCUS_NONE = 0,
    FOCUS_URL = 1,
    FOCUS_FILENAME = 2
} InputFocus;

typedef struct NativeInputState {
    bool initialized;

    /* URL field */
    TextField urlField;
    bool inputActive;

    /* Filename field */
    TextField filenameField;
    bool filenameActive;

    /* Active focus tracking */
    InputFocus focus;

    bool keyboardVisible;
    float urlX0, urlY0, urlX1, urlY1;
    float filenameX0, filenameY0, filenameX1, filenameY1;

    /* Download path selection */
    bool pathSelected;
    float pathX0, pathY0, pathX1, pathY1;

    /* Context menu (Vulkan-rendered) */
    bool menuVisible;
    float menuX, menuY;          /* click position in framebuffer coords */
    int menuHoveredItem;         /* -1 = none, 0=Cut 1=Copy 2=Paste 3=SelectAll */
    float menuX0, menuY0, menuX1, menuY1;  /* computed rendered bounds */

    /* Keep video checkbox hover */
    bool keepVideoHovered;
} NativeInputState;

extern VulkanApp *g_app;
extern NativeInputState g_input;

/* Test mode: set to non-NULL to render a single line of test text */
extern const char *g_testText;

/* File logging */
void file_log(const char *fmt, ...);

/* Shader helpers */
void free_shader(ShaderBlob *blob);

/* Vulkan helpers */
uint32_t find_memory_type(VulkanApp *app, uint32_t typeFilter, VkMemoryPropertyFlags properties);
bool create_buffer(VulkanApp *app, VkDeviceSize size, VkBufferUsageFlags usage,
                   VkMemoryPropertyFlags properties, VkBuffer *buffer, VkDeviceMemory *bufferMemory);
VkCommandBuffer begin_single_time_commands(VulkanApp *app);
void end_single_time_commands(VulkanApp *app, VkCommandBuffer commandBuffer);
bool create_image(VulkanApp *app, uint32_t width, uint32_t height, VkFormat format,
                  VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties,
                  VkImage *image, VkDeviceMemory *imageMemory);
VkImageView create_image_view(VulkanApp *app, VkImage image, VkFormat format);
void transition_image_layout(VulkanApp *app, VkImage image, VkImageLayout oldLayout,
                             VkImageLayout newLayout);
void copy_buffer_to_image(VulkanApp *app, VkBuffer buffer, VkImage image,
                          uint32_t width, uint32_t height);

/* UI state */
void ui_set_status(VulkanApp *app, const char *status);
void ui_set_progress(VulkanApp *app, float progress);
void ui_snapshot(VulkanApp *app, char *urlOut, size_t urlLen,
                 char *statusOut, size_t statusLen, float *progressOut);

/* Vertex buffer update */
void update_text_vertices(VulkanApp *app);
void update_test_text_vertices(VulkanApp *app, const char *text);

/* Worker thread */
void *worker_thread(void *arg);
void start_worker(VulkanApp *app);

/* Input handling (URL field wrappers) */
void handle_character_input(char c);
void handle_backspace(void);
void handle_cursor_left(void);
void handle_cursor_right(void);
void handle_submit(void);
void update_input_bounds(void);
bool is_inside_url_box(float x, float y);
bool is_inside_filename_box(float x, float y);
bool is_inside_keep_video_checkbox(float x, float y);
void show_soft_keyboard(void);
void hide_soft_keyboard(void);

/* Scroll / keyboard focus */
void ui_clamp_scroll_offset(void);
void ui_scroll_to_focus(InputFocus focus);
void ui_apply_scroll_delta(float deltaY);
float ui_get_visible_bottom(void);

/* Selection / clipboard (URL field wrappers) */
void handle_select_all(void);
void handle_copy(void);
void handle_cut(void);
void handle_paste(const char *text);
void sync_input_to_app(void);
void clear_selection(void);
bool has_selection(void);
void delete_selection(void);

/* Filename field editing (wrappers with .m4a enforcement) */
bool has_filename_selection(void);
void clear_filename_selection(void);
void delete_filename_selection(void);
void handle_filename_select_all(void);
void handle_filename_copy(void);
void handle_filename_cut(void);
void handle_filename_paste(const char *text);
void handle_filename_character(char c);
void handle_filename_backspace(void);
void handle_filename_cursor_left(void);
void handle_filename_cursor_right(void);

/* Generic TextField editing */
void tf_insert_char(TextField *tf, char c);
void tf_backspace(TextField *tf);
void tf_cursor_left(TextField *tf);
void tf_cursor_right(TextField *tf);
void tf_select_all(TextField *tf);
void tf_copy(TextField *tf);
void tf_cut(TextField *tf);
void tf_paste(TextField *tf, const char *text);
bool tf_has_selection(TextField *tf);
void tf_clear_selection(TextField *tf);
void tf_delete_selection(TextField *tf);
int tf_char_pos_from_mouse(TextField *tf, float mouseX, float mouseY,
                           float textX, float textY,
                           float glyphW, float glyphH, int charsPerLine);
void tf_update_selection_drag(TextField *tf, float mouseX, float mouseY,
                              float textX, float textY,
                              float glyphW, float glyphH, int charsPerLine);

/* Media extension enforcement (.m4a or .mp4) */
void enforce_media_extension(TextField *tf);

/* Mouse helpers */
int char_pos_from_mouse(float mouseX, float mouseY, float urlTextX, float urlTextY,
                        float glyphW, float glyphH, int charsPerLine);
void set_cursor_from_mouse(float mouseX, float mouseY, float urlTextX, float urlTextY,
                           float glyphW, float glyphH, int charsPerLine);
void update_selection_drag(float mouseX, float mouseY, float urlTextX, float urlTextY,
                           float glyphW, float glyphH, int charsPerLine);

/* Platform callbacks (implemented in platform main file) */
void platform_show_keyboard(VulkanApp *app, bool clearText);
void platform_hide_keyboard(VulkanApp *app);
ShaderBlob platform_load_shader(const char *path);
void platform_set_clipboard(const char *text);
const char *platform_get_clipboard(void);

/* Vulkan UI init */
bool vulkan_ui_init(VulkanApp *app, VkInstance instance, VkSurfaceKHR surface,
                    const uint8_t *vert_data, size_t vert_size,
                    const uint8_t *frag_data, size_t frag_size);

/* Vulkan setup */
bool create_swapchain(VulkanApp *app);
bool create_render_pass(VulkanApp *app);
VkShaderModule create_shader_module(VulkanApp *app, const uint8_t *data, size_t size);
bool create_font_resources(VulkanApp *app, const uint8_t *atlas_data);
bool create_vertex_buffer(VulkanApp *app);
bool create_pipeline(VulkanApp *app, const uint8_t *vert_data, size_t vert_size,
                     const uint8_t *frag_data, size_t frag_size);
bool create_framebuffers(VulkanApp *app);
bool create_command_pool_and_buffers(VulkanApp *app);
void record_command_buffers(VulkanApp *app);
bool create_sync_objects(VulkanApp *app);
void cleanup_swapchain(VulkanApp *app);
void cleanup_device(VulkanApp *app);
bool init_vulkan(VulkanApp *app);
bool recreate_swapchain(VulkanApp *app);
void draw_frame(VulkanApp *app);
bool capture_swapchain_image(VulkanApp *app, uint8_t **outPixels, uint32_t *outW, uint32_t *outH);
bool pick_device_and_queue(VulkanApp *app);

#ifdef __cplusplus
}
#endif

#endif /* VULKAN_UI_H */
