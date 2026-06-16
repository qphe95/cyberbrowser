#define VK_USE_PLATFORM_METAL_EXT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <libgen.h>
#include <mach-o/dyld.h>
#include <cmath>
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

#include "vulkan_ui.h"
#include "js_quickjs.h"

#define LOG_TAG "minimalvulkan"
#define LOGI(...) platform_log(LOG_LEVEL_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) platform_log(LOG_LEVEL_ERROR, LOG_TAG, __VA_ARGS__)

static GLFWwindow *g_window = NULL;

/* Double-click tracking */
static double g_lastClickTime = 0.0;
static double g_lastClickX = 0.0;
static double g_lastClickY = 0.0;

/* ============================================================================
 * Shader loading from filesystem
 * ============================================================================ */

static char g_exec_dir[1024] = {0};

static void init_exec_dir(void) {
    if (g_exec_dir[0] != '\0') return;
    uint32_t size = sizeof(g_exec_dir);
    if (_NSGetExecutablePath(g_exec_dir, &size) != 0) {
        getcwd(g_exec_dir, sizeof(g_exec_dir));
        return;
    }
    /* dirname modifies in place, so copy first */
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", g_exec_dir);
    char *dir = dirname(tmp);
    snprintf(g_exec_dir, sizeof(g_exec_dir), "%s", dir);
}

static ShaderBlob read_file(const char *path) {
    ShaderBlob blob = {0};
    FILE *f = fopen(path, "rb");
    if (!f) {
        return blob;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return blob;
    }
    blob.data = (uint8_t *)malloc((size_t)size);
    if (!blob.data) {
        fclose(f);
        return blob;
    }
    size_t read = fread(blob.data, 1, (size_t)size, f);
    fclose(f);
    if (read != (size_t)size) {
        free(blob.data);
        blob.data = NULL;
        return blob;
    }
    blob.size = (size_t)size;
    return blob;
}

static ShaderBlob load_shader_relative(const char *name) {
    init_exec_dir();
    char path[1024];
    /* Try executable directory first */
    snprintf(path, sizeof(path), "%s/%s", g_exec_dir, name);
    ShaderBlob blob = read_file(path);
    if (blob.data) return blob;
    /* Try Resources directory (for .app bundles) */
    snprintf(path, sizeof(path), "%s/../Resources/%s", g_exec_dir, name);
    blob = read_file(path);
    if (blob.data) return blob;
    /* Try project paths */
    const char *candidates[] = {
        "app/src/main/assets",
        "../assets",
        "assets",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s/%s", g_exec_dir, candidates[i], name);
        blob = read_file(path);
        if (blob.data) return blob;
    }
    return blob;
}

/* ============================================================================
 * Platform callbacks (implemented here for macOS)
 * ============================================================================ */

void platform_show_keyboard(VulkanApp *app, bool clearText) {
    (void)app;
    (void)clearText;
    // macOS uses physical keyboard, no soft keyboard needed
}

void platform_hide_keyboard(VulkanApp *app) {
    (void)app;
    // macOS uses physical keyboard, no soft keyboard needed
}

ShaderBlob platform_load_shader(const char *path) {
    return read_file(path);
}

void platform_set_clipboard(const char *text) {
    if (g_window && text) {
        glfwSetClipboardString(g_window, text);
    }
}

const char *platform_get_clipboard(void) {
    if (!g_window) return NULL;
    return glfwGetClipboardString(g_window);
}

/* ============================================================================
 * GLFW callbacks
 * ============================================================================ */

static void key_callback(GLFWwindow *window, int key, int scancode, int action, int mods) {
    (void)window;
    (void)scancode;
    (void)mods;
    if (action != GLFW_PRESS && action != GLFW_REPEAT) {
        return;
    }

    bool filenameActive = (g_input.focus == FOCUS_FILENAME);
    bool urlActive = (g_input.focus == FOCUS_URL) ||
                     (g_input.focus == FOCUS_NONE && g_input.inputActive);

    if (!urlActive && !filenameActive) {
        /* Allow clipboard shortcuts even when no field is active
           (e.g. copying the download path when pathSelected is true) */
        bool isClipboardShortcut = (key == GLFW_KEY_C || key == GLFW_KEY_X || key == GLFW_KEY_A)
                                    && (mods & GLFW_MOD_SUPER);
        if (key == GLFW_KEY_ENTER || key == GLFW_KEY_V || isClipboardShortcut) {
            if (!isClipboardShortcut) {
                g_input.focus = FOCUS_URL;
                g_input.inputActive = true;
                if (g_app) g_app->inputActive = true;
                ui_set_status(g_app, "Enter URL...");
            }
        } else {
            return;
        }
        urlActive = true;
    }

    if (filenameActive) {
        switch (key) {
            case GLFW_KEY_ENTER:
                handle_submit();
                break;
            case GLFW_KEY_BACKSPACE:
                handle_filename_backspace();
                break;
            case GLFW_KEY_LEFT:
                if (mods & GLFW_MOD_SHIFT) {
                    if (!has_filename_selection()) g_input.filenameField.selStart = g_input.filenameField.cursorPos;
                    handle_filename_cursor_left();
                    g_input.filenameField.selEnd = g_input.filenameField.cursorPos;
                } else {
                    handle_filename_cursor_left();
                }
                break;
            case GLFW_KEY_RIGHT:
                if (mods & GLFW_MOD_SHIFT) {
                    if (!has_filename_selection()) g_input.filenameField.selStart = g_input.filenameField.cursorPos;
                    handle_filename_cursor_right();
                    g_input.filenameField.selEnd = g_input.filenameField.cursorPos;
                } else {
                    handle_filename_cursor_right();
                }
                break;
            case GLFW_KEY_ESCAPE:
                if (g_input.menuVisible) {
                    g_input.menuVisible = false;
                    g_input.menuHoveredItem = -1;
                } else {
                    g_input.focus = FOCUS_NONE;
                    g_input.filenameActive = false;
                    if (g_app) g_app->customFilenameActive = false;
                    clear_filename_selection();
                    g_input.pathSelected = false;
                    ui_set_status(g_app, "Idle");
                }
                break;
            case GLFW_KEY_A:
                if (mods & GLFW_MOD_SUPER) {
                    handle_filename_select_all();
                }
                break;
            case GLFW_KEY_C:
                if (mods & GLFW_MOD_SUPER) {
                    handle_filename_copy();
                }
                break;
            case GLFW_KEY_X:
                if (mods & GLFW_MOD_SUPER) {
                    handle_filename_cut();
                }
                break;
            case GLFW_KEY_V:
                if (mods & GLFW_MOD_SUPER) {
                    const char *clip = glfwGetClipboardString(window);
                    if (clip) handle_filename_paste(clip);
                }
                break;
            default:
                break;
        }
        if (g_app) {
            pthread_mutex_lock(&g_app->uiMutex);
            strncpy(g_app->customFilename, g_input.filenameField.buffer, sizeof(g_app->customFilename) - 1);
            g_app->customFilename[sizeof(g_app->customFilename) - 1] = '\0';
            pthread_mutex_unlock(&g_app->uiMutex);
        }
    } else {
        /* URL field active */
        switch (key) {
            case GLFW_KEY_ENTER:
                handle_submit();
                break;
            case GLFW_KEY_BACKSPACE:
                handle_backspace();
                break;
            case GLFW_KEY_LEFT:
                if (mods & GLFW_MOD_SHIFT) {
                    if (!has_selection()) g_input.urlField.selStart = g_input.urlField.cursorPos;
                    handle_cursor_left();
                    g_input.urlField.selEnd = g_input.urlField.cursorPos;
                } else {
                    if (has_selection()) {
                        int start = g_input.urlField.selStart < g_input.urlField.selEnd ? g_input.urlField.selStart : g_input.urlField.selEnd;
                        g_input.urlField.cursorPos = start;
                        clear_selection();
                    } else {
                        handle_cursor_left();
                    }
                }
                break;
            case GLFW_KEY_RIGHT:
                if (mods & GLFW_MOD_SHIFT) {
                    if (!has_selection()) g_input.urlField.selStart = g_input.urlField.cursorPos;
                    handle_cursor_right();
                    g_input.urlField.selEnd = g_input.urlField.cursorPos;
                } else {
                    if (has_selection()) {
                        int end = g_input.urlField.selStart < g_input.urlField.selEnd ? g_input.urlField.selEnd : g_input.urlField.selStart;
                        g_input.urlField.cursorPos = end;
                        clear_selection();
                    } else {
                        handle_cursor_right();
                    }
                }
                break;
            case GLFW_KEY_ESCAPE:
                if (g_input.menuVisible) {
                    g_input.menuVisible = false;
                    g_input.menuHoveredItem = -1;
                } else {
                    g_input.inputActive = false;
                    if (g_app) g_app->inputActive = false;
                    g_input.focus = FOCUS_NONE;
                    clear_selection();
                    g_input.pathSelected = false;
                    ui_set_status(g_app, "Idle");
                }
                break;
            case GLFW_KEY_A:
                if (mods & GLFW_MOD_SUPER) {
                    handle_select_all();
                }
                break;
            case GLFW_KEY_C:
                if (mods & GLFW_MOD_SUPER) {
                    handle_copy();
                }
                break;
            case GLFW_KEY_X:
                if (mods & GLFW_MOD_SUPER) {
                    handle_cut();
                }
                break;
            case GLFW_KEY_V:
                if (mods & GLFW_MOD_SUPER) {
                    const char *clip = glfwGetClipboardString(window);
                    if (clip) {
                        if (!g_input.inputActive) {
                            g_input.inputActive = true;
                            if (g_app) g_app->inputActive = true;
                            ui_set_status(g_app, "Enter URL...");
                        }
                        handle_paste(clip);
                    }
                }
                break;
            default:
                break;
        }
        if (g_app) {
            pthread_mutex_lock(&g_app->uiMutex);
            strncpy(g_app->urlInput, g_input.urlField.buffer, sizeof(g_app->urlInput) - 1);
            g_app->urlInput[sizeof(g_app->urlInput) - 1] = '\0';
            g_app->urlLen = g_input.urlField.length;
            pthread_mutex_unlock(&g_app->uiMutex);
        }
    }
}

static void char_callback(GLFWwindow *window, unsigned int codepoint) {
    (void)window;
    if (codepoint >= 32 && codepoint < 127) {
        if (g_input.focus == FOCUS_FILENAME) {
            handle_filename_character((char)codepoint);
            if (g_app) {
                pthread_mutex_lock(&g_app->uiMutex);
                strncpy(g_app->customFilename, g_input.filenameField.buffer, sizeof(g_app->customFilename) - 1);
                g_app->customFilename[sizeof(g_app->customFilename) - 1] = '\0';
                pthread_mutex_unlock(&g_app->uiMutex);
            }
        } else {
            if (!g_input.inputActive) {
                g_input.inputActive = true;
                if (g_app) g_app->inputActive = true;
                ui_set_status(g_app, "Enter URL...");
                g_input.focus = FOCUS_URL;
            }
            handle_character_input((char)codepoint);
            if (g_app) {
                pthread_mutex_lock(&g_app->uiMutex);
                strncpy(g_app->urlInput, g_input.urlField.buffer, sizeof(g_app->urlInput) - 1);
                g_app->urlInput[sizeof(g_app->urlInput) - 1] = '\0';
                g_app->urlLen = g_input.urlField.length;
                pthread_mutex_unlock(&g_app->uiMutex);
            }
        }
    }
}

static void cursor_pos_callback(GLFWwindow *window, double x, double y) {
    (void)window;
    if (!g_app) return;
    int winW, winH;
    glfwGetWindowSize(g_window, &winW, &winH);
    if (g_app->windowWidth <= 0 || g_app->windowHeight <= 0) return;
    float swapW = (float)g_app->swapchainExtent.width;
    float swapH = (float)g_app->swapchainExtent.height;
    float scaledX = (float)x * (swapW / (float)winW);
    float scaledY = (float)y * (swapH / (float)winH);

    /* Context menu hover tracking */
    if (g_input.menuVisible) {
        float mx0 = g_input.menuX0;
        float my0 = g_input.menuY0;
        float mx1 = g_input.menuX1;
        float my1 = g_input.menuY1;
        if (scaledX >= mx0 && scaledX <= mx1 && scaledY >= my0 && scaledY <= my1) {
            float glyphH = FONT_GLYPH_H * 2.0f * g_app->densityScale;
            float densityScale = g_app->densityScale;
            float padY = 6.0f * densityScale;
            float itemH = glyphH + padY * 2.0f;
            float sepH = 4.0f * densityScale;
            float dy = scaledY - my0;
            int item = -1;
            if (dy < 2.0f * itemH) {
                item = (int)(dy / itemH);
            } else if (dy >= 2.0f * itemH + sepH) {
                item = 2 + (int)((dy - 2.0f * itemH - sepH) / itemH);
            }
            if (item < 0 || item > 3) item = -1;
            g_input.menuHoveredItem = item;
        } else {
            g_input.menuHoveredItem = -1;
        }
        return;
    }

    /* Keep video checkbox hover tracking */
    if (g_app) {
        pthread_mutex_lock(&g_app->uiMutex);
        float cx0 = g_app->uiKeepVideoX0;
        float cy0 = g_app->uiKeepVideoY0;
        float cx1 = g_app->uiKeepVideoX1;
        float cy1 = g_app->uiKeepVideoY1;
        pthread_mutex_unlock(&g_app->uiMutex);
        if (scaledX >= cx0 && scaledX <= cx1 && scaledY >= cy0 && scaledY <= cy1) {
            g_input.keepVideoHovered = true;
        } else {
            g_input.keepVideoHovered = false;
        }
    }

    if (g_input.focus == FOCUS_FILENAME && g_input.filenameField.isDragging) {
        pthread_mutex_lock(&g_app->uiMutex);
        float fnTextX = g_app->uiFilenameTextX;
        float fnTextY = g_app->uiFilenameTextY;
        float glyphW = g_app->uiFilenameGlyphW;
        float glyphH = g_app->uiFilenameGlyphH;
        int charsPerLine = g_app->uiFilenameCharsPerLine;
        pthread_mutex_unlock(&g_app->uiMutex);
        tf_update_selection_drag(&g_input.filenameField, scaledX, scaledY,
                                 fnTextX, fnTextY, glyphW, glyphH, charsPerLine);
        return;
    }

    if (g_input.urlField.isDragging) {
        pthread_mutex_lock(&g_app->uiMutex);
        float urlTextX = g_app->uiUrlTextX;
        float urlTextY = g_app->uiUrlTextY;
        float glyphW = g_app->uiUrlGlyphW;
        float glyphH = g_app->uiUrlGlyphH;
        int charsPerLine = g_app->uiUrlCharsPerLine;
        pthread_mutex_unlock(&g_app->uiMutex);
        tf_update_selection_drag(&g_input.urlField, scaledX, scaledY,
                                 urlTextX, urlTextY, glyphW, glyphH, charsPerLine);
    }
}

static void mouse_button_callback(GLFWwindow *window, int button, int action, int mods) {
    (void)mods;
    if (button != GLFW_MOUSE_BUTTON_LEFT && button != GLFW_MOUSE_BUTTON_RIGHT) return;

    double x, y;
    glfwGetCursorPos(window, &x, &y);
    int winW, winH;
    glfwGetWindowSize(window, &winW, &winH);
    if (!g_app || g_app->windowWidth <= 0 || g_app->windowHeight <= 0) return;

    float swapW = (float)g_app->swapchainExtent.width;
    float swapH = (float)g_app->swapchainExtent.height;
    float scaledX = (float)x * (swapW / (float)winW);
    float scaledY = (float)y * (swapH / (float)winH);

    if (action == GLFW_PRESS) {
        update_input_bounds();
        bool inUrlBox = is_inside_url_box(scaledX, scaledY);
        bool inFilenameBox = is_inside_filename_box(scaledX, scaledY);
        bool inPath = (scaledX >= g_input.pathX0 && scaledX <= g_input.pathX1 &&
                       scaledY >= g_input.pathY0 && scaledY <= g_input.pathY1);
        LOGI("mouse_press: scaled=(%.1f,%.1f) urlBox=(%.1f,%.1f,%.1f,%.1f) path=(%.1f,%.1f,%.1f,%.1f) inUrl=%d inPath=%d",
             scaledX, scaledY,
             g_input.urlX0, g_input.urlY0, g_input.urlX1, g_input.urlY1,
             g_input.pathX0, g_input.pathY0, g_input.pathX1, g_input.pathY1,
             inUrlBox ? 1 : 0, inPath ? 1 : 0);

        if (button == GLFW_MOUSE_BUTTON_RIGHT) {
            if (inPath) {
                if (g_input.inputActive) {
                    g_input.inputActive = false;
                    g_app->inputActive = false;
                    clear_selection();
                    ui_set_status(g_app, "Idle");
                }
                g_input.focus = FOCUS_NONE;
                g_input.pathSelected = true;
                g_input.menuVisible = true;
                g_input.menuX = scaledX;
                g_input.menuY = scaledY;
                g_input.menuHoveredItem = -1;
                return;
            }
            if (inFilenameBox) {
                g_input.focus = FOCUS_FILENAME;
                g_input.filenameActive = true;
                if (g_app) g_app->customFilenameActive = true;
                g_input.pathSelected = false;
                pthread_mutex_lock(&g_app->uiMutex);
                float fnTextX = g_app->uiFilenameTextX;
                float glyphW = g_app->uiFilenameGlyphW;
                pthread_mutex_unlock(&g_app->uiMutex);
                int pos = (int)((scaledX - fnTextX) / glyphW + 0.5f);
                if (pos < 0) pos = 0;
                if (pos > g_input.filenameField.length) pos = g_input.filenameField.length;
                if (has_filename_selection()) {
                    int selStart = g_input.filenameField.selStart < g_input.filenameField.selEnd ?
                                   g_input.filenameField.selStart : g_input.filenameField.selEnd;
                    int selEnd = g_input.filenameField.selStart < g_input.filenameField.selEnd ?
                                 g_input.filenameField.selEnd : g_input.filenameField.selStart;
                    if (pos < selStart || pos > selEnd) {
                        clear_filename_selection();
                        g_input.filenameField.cursorPos = pos;
                    }
                } else {
                    g_input.filenameField.cursorPos = pos;
                }
                g_input.menuVisible = true;
                g_input.menuX = scaledX;
                g_input.menuY = scaledY;
                g_input.menuHoveredItem = -1;
                return;
            }
            if (inUrlBox) {
                if (!g_input.inputActive) {
                    g_input.inputActive = true;
                    g_app->inputActive = true;
                    ui_set_status(g_app, "Enter URL...");
                }
                g_input.focus = FOCUS_URL;
                g_input.pathSelected = false;
                pthread_mutex_lock(&g_app->uiMutex);
                float urlTextX = g_app->uiUrlTextX;
                float urlTextY = g_app->uiUrlTextY;
                float glyphW = g_app->uiUrlGlyphW;
                float glyphH = g_app->uiUrlGlyphH;
                int charsPerLine = g_app->uiUrlCharsPerLine;
                pthread_mutex_unlock(&g_app->uiMutex);
                int pos = char_pos_from_mouse(scaledX, scaledY, urlTextX, urlTextY,
                                              glyphW, glyphH, charsPerLine);
                if (has_selection()) {
                    int selStart = g_input.urlField.selStart < g_input.urlField.selEnd ? g_input.urlField.selStart : g_input.urlField.selEnd;
                    int selEnd = g_input.urlField.selStart < g_input.urlField.selEnd ? g_input.urlField.selEnd : g_input.urlField.selStart;
                    if (pos < selStart || pos > selEnd) {
                        clear_selection();
                        g_input.urlField.cursorPos = pos;
                    }
                } else {
                    g_input.urlField.cursorPos = pos;
                }
                g_input.menuVisible = true;
                g_input.menuX = scaledX;
                g_input.menuY = scaledY;
                g_input.menuHoveredItem = -1;
            }
            return;
        }

        /* LEFT CLICK */
        /* If menu is visible, check for item click or dismiss */
        if (g_input.menuVisible) {
            float mx0 = g_input.menuX0;
            float my0 = g_input.menuY0;
            float mx1 = g_input.menuX1;
            float my1 = g_input.menuY1;
            bool inMenu = (scaledX >= mx0 && scaledX <= mx1 &&
                           scaledY >= my0 && scaledY <= my1);
            if (inMenu && g_input.menuHoveredItem >= 0) {
                if (g_input.focus == FOCUS_FILENAME) {
                    switch (g_input.menuHoveredItem) {
                        case 0: handle_filename_cut(); break;
                        case 1: handle_filename_copy(); break;
                        case 2: {
                            const char *clip = platform_get_clipboard();
                            if (clip) handle_filename_paste(clip);
                            break;
                        }
                        case 3: handle_filename_select_all(); break;
                    }
                } else {
                    switch (g_input.menuHoveredItem) {
                        case 0: handle_cut(); break;
                        case 1: handle_copy(); break;
                        case 2: {
                            const char *clip = platform_get_clipboard();
                            if (clip) handle_paste(clip);
                            break;
                        }
                        case 3: handle_select_all(); break;
                    }
                }
                sync_input_to_app();
            }
            g_input.menuVisible = false;
            g_input.menuHoveredItem = -1;
            if (inMenu) {
                return;
            }
            /* fall through to normal left-click handling */
        }

        /* Check keep video checkbox click */
        if (g_app) {
            pthread_mutex_lock(&g_app->uiMutex);
            float cx0 = g_app->uiKeepVideoX0;
            float cy0 = g_app->uiKeepVideoY0;
            float cx1 = g_app->uiKeepVideoX1;
            float cy1 = g_app->uiKeepVideoY1;
            pthread_mutex_unlock(&g_app->uiMutex);
            if (scaledX >= cx0 && scaledX <= cx1 && scaledY >= cy0 && scaledY <= cy1) {
                g_app->keepVideo = !g_app->keepVideo;
                g_input.keepVideoHovered = false;
                /* Update filename extension to match */
                enforce_media_extension(&g_input.filenameField);
                if (g_app) {
                    pthread_mutex_lock(&g_app->uiMutex);
                    strncpy(g_app->customFilename, g_input.filenameField.buffer, sizeof(g_app->customFilename) - 1);
                    g_app->customFilename[sizeof(g_app->customFilename) - 1] = '\0';
                    pthread_mutex_unlock(&g_app->uiMutex);
                }
                return;
            }
        }

        if (inFilenameBox) {
            g_input.focus = FOCUS_FILENAME;
            g_input.filenameActive = true;
            if (g_app) g_app->customFilenameActive = true;
            g_input.pathSelected = false;
            pthread_mutex_lock(&g_app->uiMutex);
            float fnTextX = g_app->uiFilenameTextX;
            float glyphW = g_app->uiFilenameGlyphW;
            pthread_mutex_unlock(&g_app->uiMutex);
            int pos = (int)((scaledX - fnTextX) / glyphW + 0.5f);
            if (pos < 0) pos = 0;
            if (pos > g_input.filenameField.length) pos = g_input.filenameField.length;
            double now = glfwGetTime();
            bool isDoubleClick = (now - g_lastClickTime < 0.3) &&
                                 (std::fabs(x - g_lastClickX) < 10.0) &&
                                 (std::fabs(y - g_lastClickY) < 10.0);
            g_lastClickTime = now;
            g_lastClickX = x;
            g_lastClickY = y;
            if (isDoubleClick) {
                handle_filename_select_all();
                g_input.filenameField.isDragging = false;
            } else if (mods & GLFW_MOD_SHIFT) {
                if (!has_filename_selection()) {
                    g_input.filenameField.selStart = g_input.filenameField.cursorPos;
                }
                g_input.filenameField.selEnd = pos;
                g_input.filenameField.cursorPos = pos;
            } else {
                clear_filename_selection();
                g_input.filenameField.cursorPos = pos;
                g_input.filenameField.isDragging = true;
                g_input.filenameField.dragAnchor = pos;
            }
        } else if (inUrlBox) {
            if (!g_input.inputActive) {
                g_input.inputActive = true;
                g_app->inputActive = true;
                ui_set_status(g_app, "Enter URL...");
            }
            g_input.focus = FOCUS_URL;
            g_input.pathSelected = false;
            pthread_mutex_lock(&g_app->uiMutex);
            float urlTextX = g_app->uiUrlTextX;
            float urlTextY = g_app->uiUrlTextY;
            float glyphW = g_app->uiUrlGlyphW;
            float glyphH = g_app->uiUrlGlyphH;
            int charsPerLine = g_app->uiUrlCharsPerLine;
            pthread_mutex_unlock(&g_app->uiMutex);
            int pos = char_pos_from_mouse(scaledX, scaledY, urlTextX, urlTextY,
                                          glyphW, glyphH, charsPerLine);
            double now = glfwGetTime();
            bool isDoubleClick = (now - g_lastClickTime < 0.3) &&
                                 (std::fabs(x - g_lastClickX) < 10.0) &&
                                 (std::fabs(y - g_lastClickY) < 10.0);
            g_lastClickTime = now;
            g_lastClickX = x;
            g_lastClickY = y;
            if (isDoubleClick) {
                handle_select_all();
                g_input.urlField.isDragging = false;
            } else if (mods & GLFW_MOD_SHIFT) {
                if (!has_selection()) {
                    g_input.urlField.selStart = g_input.urlField.cursorPos;
                }
                g_input.urlField.selEnd = pos;
                g_input.urlField.cursorPos = pos;
            } else {
                clear_selection();
                g_input.urlField.cursorPos = pos;
                g_input.urlField.isDragging = true;
                g_input.urlField.dragAnchor = pos;
            }
        } else {
            if (g_input.inputActive) {
                g_input.inputActive = false;
                g_app->inputActive = false;
                ui_set_status(g_app, "Idle");
            }
            if (g_input.filenameActive) {
                g_input.filenameActive = false;
                if (g_app) g_app->customFilenameActive = false;
            }
            g_input.focus = FOCUS_NONE;
            if (inPath) {
                g_input.pathSelected = true;
            } else {
                g_input.pathSelected = false;
            }
        }
    } else if (action == GLFW_RELEASE) {
        g_input.urlField.isDragging = false;
        g_input.filenameField.isDragging = false;
    }
}

static volatile bool g_needs_resize = false;

static void handle_resize(int width, int height) {
    if (!g_app || !g_app->ready) return;
    if (width <= 0 || height <= 0) return;
    if (g_app->windowWidth == width && g_app->windowHeight == height) return;

    g_app->windowWidth = width;
    g_app->windowHeight = height;
    recreate_swapchain(g_app);
    draw_frame(g_app);
}

static void framebuffer_size_callback(GLFWwindow *window, int width, int height) {
    (void)window;
    LOGI("framebuffer_size_callback: %dx%d", width, height);
    g_needs_resize = true;
    /* Handle resize immediately so we redraw during live resize on macOS */
    handle_resize(width, height);
}

static void window_refresh_callback(GLFWwindow *window) {
    (void)window;
    if (!g_app || !g_app->ready) return;
    int fbW, fbH;
    glfwGetFramebufferSize(window, &fbW, &fbH);
    LOGI("window_refresh_callback: fb=%dx%d", fbW, fbH);
    handle_resize(fbW, fbH);
}

/* ============================================================================
 * Vulkan instance creation
 * ============================================================================ */

static bool init_instance(VulkanApp *app) {
    if (app->instance != VK_NULL_HANDLE) {
        return true;
    }

    uint32_t glfwExtensionCount = 0;
    const char **glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    if (!glfwExtensions) {
        LOGE("glfwGetRequiredInstanceExtensions failed");
        return false;
    }

    // Add portability enumeration for MoltenVK
    const char *extensions[16];
    uint32_t extensionCount = 0;
    for (uint32_t i = 0; i < glfwExtensionCount; i++) {
        extensions[extensionCount++] = glfwExtensions[i];
    }
    extensions[extensionCount++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;

    VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "BGMDWLDR",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "minimal",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_0
    };

    VkInstanceCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
        .enabledExtensionCount = extensionCount,
        .ppEnabledExtensionNames = extensions,
        .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
    };

    VkResult result = vkCreateInstance(&createInfo, NULL, &app->instance);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateInstance failed: %d", result);
        return false;
    }
    return true;
}

/* ============================================================================
 * Main entry point
 * ============================================================================ */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return 1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    g_window = glfwCreateWindow(1280, 800, "BGMDWLDR", NULL, NULL);
    if (!g_window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }
    glfwSetWindowSizeLimits(g_window, 640, 400, GLFW_DONT_CARE, GLFW_DONT_CARE);

    glfwSetKeyCallback(g_window, key_callback);
    glfwSetCharCallback(g_window, char_callback);
    glfwSetMouseButtonCallback(g_window, mouse_button_callback);
    glfwSetCursorPosCallback(g_window, cursor_pos_callback);
    glfwSetFramebufferSizeCallback(g_window, framebuffer_size_callback);
    glfwSetWindowRefreshCallback(g_window, window_refresh_callback);

    VulkanApp vk = {0};
    g_app = &vk;
    pthread_mutex_init(&vk.uiMutex, NULL);
    download_state_init(&vk.downloadState);
    vk.urlInput[0] = '\0';
    vk.urlLen = 0;
    vk.progress = 0.0f;
    vk.workerRunning = false;
    vk.inputActive = false;
    vk.keepVideo = false;
    vk.keyboardHeightPx = 0.0f;
    float xscale, yscale;
    glfwGetWindowContentScale(g_window, &xscale, &yscale);
    vk.densityScale = xscale;
    if (vk.densityScale < 1.0f) vk.densityScale = 1.0f;
    snprintf(vk.statusText, sizeof(vk.statusText), "Idle");

    int winW, winH;
    glfwGetFramebufferSize(g_window, &winW, &winH);
    vk.windowWidth = winW;
    vk.windowHeight = winH;

    if (!init_instance(&vk)) {
        LOGE("init_instance failed");
        glfwDestroyWindow(g_window);
        glfwTerminate();
        return 1;
    }

    VkResult result = glfwCreateWindowSurface(vk.instance, g_window, NULL, &vk.surface);
    if (result != VK_SUCCESS) {
        LOGE("glfwCreateWindowSurface failed: %d", result);
        cleanup_device(&vk);
        glfwDestroyWindow(g_window);
        glfwTerminate();
        return 1;
    }

    // Load shaders relative to executable location
    ShaderBlob vert = load_shader_relative("triangle.vert.spv");
    ShaderBlob frag = load_shader_relative("triangle.frag.spv");

    if (!vert.data || !frag.data) {
        LOGE("Failed to load shaders");
        free_shader(&vert);
        free_shader(&frag);
        cleanup_device(&vk);
        glfwDestroyWindow(g_window);
        glfwTerminate();
        return 1;
    }

    if (!vulkan_ui_init(&vk, vk.instance, vk.surface,
                        vert.data, vert.size,
                        frag.data, frag.size)) {
        LOGE("vulkan_ui_init failed");
        free_shader(&vert);
        free_shader(&frag);
        cleanup_device(&vk);
        glfwDestroyWindow(g_window);
        glfwTerminate();
        return 1;
    }
    free_shader(&vert);
    free_shader(&frag);

    while (!glfwWindowShouldClose(g_window)) {
        glfwPollEvents();
        if (g_needs_resize && vk.ready) {
            g_needs_resize = false;
            int fbW, fbH;
            glfwGetFramebufferSize(g_window, &fbW, &fbH);
            int winW, winH;
            glfwGetWindowSize(g_window, &winW, &winH);
            float xs, ys;
            glfwGetWindowContentScale(g_window, &xs, &ys);
            LOGI("resize: glfwFb=%dx%d glfwWin=%dx%d scale=%.2fx%.2f vkWin=%dx%d",
                 fbW, fbH, winW, winH, xs, ys, vk.windowWidth, vk.windowHeight);
            if (fbW > 0 && fbH > 0 && (vk.windowWidth != fbW || vk.windowHeight != fbH)) {
                /* Feed GLFW's framebuffer size into create_swapchain so it
                   can override caps.currentExtent when MoltenVK lags. */
                vk.windowWidth = fbW;
                vk.windowHeight = fbH;
                recreate_swapchain(&vk);
            }
        }
        if (vk.ready && !glfwGetWindowAttrib(g_window, GLFW_ICONIFIED)) {
            draw_frame(&vk);
        }
    }

    vkDeviceWaitIdle(vk.device);
    pthread_mutex_destroy(&vk.uiMutex);
    g_app = NULL;
    cleanup_device(&vk);
    glfwDestroyWindow(g_window);
    glfwTerminate();
    return 0;
}
