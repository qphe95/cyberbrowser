#include "ft_vulkan_ui.h"
#include "ft_ui_layout.h"
#include "ft_ui_display_list.h"
#include "ft_platform.h"
#include "ft_resolver.h"
#include "ft_registry.h"
#include "ft_key_manager.h"
#include "ft_autocomplete.h"
#include "ft_log_buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>

static void ui_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    va_start(args, fmt);
    ft_log_buffer_vwrite(fmt, args);
    va_end(args);
}

#ifdef _WIN32
#include <windows.h>
#endif

#define LOG_TAG "freetext-ui"
#define LOGI(...) ft_platform_log(FT_LOG_LEVEL_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) ft_platform_log(FT_LOG_LEVEL_ERROR, LOG_TAG, __VA_ARGS__)

extern FtDocument g_docs[];
extern FtTextEdit g_edits[];
extern FtDocCrypto g_doc_crypto[];
extern HWND g_hwnd;

/* Classic terminal palette */
static const float C_BG[3]        = {0.00f, 0.00f, 0.00f};
static const float C_WHITE[3]     = {0.90f, 0.90f, 0.90f};
static const float C_DIM[3]       = {0.40f, 0.40f, 0.40f};
static const float C_NEON[3]      = {0.30f, 0.95f, 1.00f}; /* cyan emphasis */
static const float C_PINK[3]      = {1.00f, 0.08f, 0.58f}; /* neon pink */
static const float C_SELECT[3]    = {0.05f, 0.30f, 0.35f};
static const float C_STATUS_BG[3] = {0.03f, 0.03f, 0.03f};
static const float C_SIDEBAR_HI[3]= {0.06f, 0.20f, 0.25f};
static const float C_ORANGE[3]     = {1.00f, 0.50f, 0.00f}; /* neon orange */
static const float C_NEON_GREEN[3] = {0.10f, 1.00f, 0.30f}; /* neon green */
static const float C_NEON_RED[3]   = {1.00f, 0.15f, 0.15f}; /* neon red */
static const float C_HOVER[3]      = {0.70f, 0.70f, 0.70f}; /* light gray for hover */

/* GLFW keycode aliases */
#define KEY_ESC     256
#define KEY_ENTER   257
#define KEY_TAB     258
#define KEY_BS      259
#define KEY_DEL     261
#define KEY_RIGHT   262
#define KEY_LEFT    263
#define KEY_DOWN    264
#define KEY_UP      265
#define KEY_HOME    268
#define KEY_END     269
#define KEY_A       65
#define KEY_C       67
#define KEY_D       68
#define KEY_F       70
#define KEY_V       86
#define KEY_X       88
#define KEY_1       49
#define KEY_8       56
#define KEY_F12     123

/* ========================================================================
 * Debug logging helpers
 * ======================================================================== */

static void ui_debug_log_internal(FtVulkanUI *ui, char logBuf[FT_UI_DEBUG_LOG_LINES][FT_UI_DEBUG_LOG_LINE_LEN],
                                    int *logCount, int *logNext, const char *fmt, va_list args) {
    (void)ui;
    char buf[FT_UI_DEBUG_LOG_LINE_LEN];
    vsnprintf(buf, sizeof(buf), fmt, args);

    if (*logCount > 0) {
        int lastIdx = (*logNext - 1 + FT_UI_DEBUG_LOG_LINES) % FT_UI_DEBUG_LOG_LINES;
        if (strcmp(logBuf[lastIdx], buf) == 0) {
            return; /* skip duplicate of last message */
        }
    }

    int idx = *logNext % FT_UI_DEBUG_LOG_LINES;
    strncpy(logBuf[idx], buf, FT_UI_DEBUG_LOG_LINE_LEN - 1);
    logBuf[idx][FT_UI_DEBUG_LOG_LINE_LEN - 1] = '\0';
    (*logNext)++;
    if (*logCount < FT_UI_DEBUG_LOG_LINES) {
        (*logCount)++;
    }
}

static void ui_debug_log(FtVulkanUI *ui, const char *fmt, ...) {
    if (!ui) return;
    va_list args;
    va_start(args, fmt);
    ui_debug_log_internal(ui, ui->debugLog, &ui->debugLogCount, &ui->debugLogNext, fmt, args);
    va_end(args);
}

static void ui_debug_log_network(FtVulkanUI *ui, const char *fmt, ...) {
    if (!ui) return;
    va_list args;
    va_start(args, fmt);
    ui_debug_log_internal(ui, ui->debugLogNetwork, &ui->debugLogNetworkCount, &ui->debugLogNetworkNext, fmt, args);
    va_end(args);
}

static void ui_debug_log_auto(FtVulkanUI *ui, const char *fmt, ...) {
    if (!ui) return;
    va_list args;
    va_start(args, fmt);
    ui_debug_log_internal(ui, ui->debugLogAuto, &ui->debugLogAutoCount, &ui->debugLogAutoNext, fmt, args);
    va_end(args);
}

static void ui_debug_log_friend(FtVulkanUI *ui, const char *fmt, ...) {
    if (!ui) return;
    va_list args;
    va_start(args, fmt);
    ui_debug_log_internal(ui, ui->debugLogFriend, &ui->debugLogFriendCount, &ui->debugLogFriendNext, fmt, args);
    va_end(args);
}

static void compute_document_layout(FtVulkanUI *ui, DocumentLayout *dl);

static uint64_t hash_editor_state(FtVulkanUI *ui, const char *label) {
    if (!ui || !ui->edit || !ui->edit->doc) return 0;
    FtDocument *doc = ui->edit->doc;
    FtTextEdit *ed = ui->edit;
    uint64_t h = 14695981039346656037ULL;
#define HASH_BYTE(b) h = (h ^ (uint8_t)(b)) * 1099511628211ULL
    for (const char *p = label; *p; p++) HASH_BYTE(*p);
    HASH_BYTE((uint8_t)(doc->len));
    HASH_BYTE((uint8_t)(doc->len >> 8));
    HASH_BYTE((uint8_t)(ed->cursor));
    HASH_BYTE((uint8_t)(ed->cursor >> 8));
    HASH_BYTE((uint8_t)(ed->sel_start));
    HASH_BYTE((uint8_t)(ed->sel_start >> 8));
    HASH_BYTE((uint8_t)(ed->sel_end));
    HASH_BYTE((uint8_t)(ed->sel_end >> 8));
    HASH_BYTE((uint8_t)ed->selecting);
    HASH_BYTE((uint8_t)ft_autocomplete_is_active(ui->autocomplete));
    HASH_BYTE((uint8_t)ft_autocomplete_get_mode(ui->autocomplete));
#undef HASH_BYTE
    return h;
}

static void ui_log_editor_state(FtVulkanUI *ui, const char *label) {
    if (!ui || !ui->edit || !ui->edit->doc) return;
    uint64_t hash = hash_editor_state(ui, label);
    if (hash == ui->debugLastStateHash) return;
    ui->debugLastStateHash = hash;

    FtDocument *doc = ui->edit->doc;
    FtTextEdit *ed = ui->edit;
    ui_debug_log(ui, "--- %s ---", label);
    ui_debug_log(ui, "doc_len=%zu cursor=%zu sel=[%zu..%zu] selecting=%d auto=%d(auto_mode=%d start=%zu)",
                 doc->len, ed->cursor, ed->sel_start, ed->sel_end,
                 (int)ed->selecting, (int)ft_autocomplete_is_active(ui->autocomplete),
                 ft_autocomplete_get_mode(ui->autocomplete),
                 ft_autocomplete_get_start_pos(ui->autocomplete));
    if (ui->renderer) {
        DocumentLayout dl;
        compute_document_layout(ui, &dl);
        float cursorX, cursorY, cursorGlyphH;
        ui_debug_log(ui, "layout: title_end=%zu contentX=%.1f contentW=%.1f charsPerLine=%d scale=%.2f",
                     dl.title_end, dl.contentX, dl.contentW, (int)dl.charsPerLine, dl.scale);
        ft_ui_layout_measure_cursor_pos(&dl, doc, ed, ui->scrollY, &cursorX, &cursorY, &cursorGlyphH);
        ui_debug_log(ui, "cursor_pos=(%.1f,%.1f)", cursorX, cursorY);
        if (doc->text && doc->len > 0) {
            size_t start = ed->cursor > 3 ? ed->cursor - 3 : 0;
            size_t end = ed->cursor + 3 < doc->len ? ed->cursor + 3 : doc->len;
            char snippet[16] = {0};
            size_t j = 0;
            for (size_t i = start; i < end && j < 15; i++) {
                unsigned char c = (unsigned char)doc->text[i];
                if (c >= 32 && c <= 126) snippet[j++] = (char)c;
                else { snippet[j++] = '?'; }
            }
            ui_debug_log(ui, "text_around_cursor=[%s]", snippet);
        }
    }
}

/* ========================================================================
 * File-scope modal state (not part of persistent FtVulkanUI)
 * ======================================================================== */

static struct {
    char eth_address[43];
    uint64_t balance_wei;
    uint64_t last_check_time;
    char first_name_buf[32];
    char last_name_buf[32];
    float addr_hit_x, addr_hit_y, addr_hit_w, addr_hit_h;
    float path_hit_x, path_hit_y, path_hit_w, path_hit_h;
    bool sharing_highlighted;
    bool addr_highlighted;
    bool path_highlighted;
    char path_field[512];
    bool balance_zero_error;
    bool network_error;
} g_startup;

static struct {
    char hash_buf[256];
    FriendModalLayout layout;
    float listScrollY;
    bool listScrollbarDragging;
    float listScrollbarDragStartY;
    float listScrollbarDragStartScrollY;
    char preview_name[FT_DISPLAY_NAME_LEN];
    char preview_hash[FT_HASH_HEX_LEN];
    bool preview_valid;
    int pathSelStart;
    int pathSelEnd;
    bool pathFocused;
    bool pathSelecting;
} g_friendModal;

static int g_deleteModal_chatToDelete;

static struct {
    float x, y, w, h;
    bool visible;
} g_deleteBtn;

static void autocomplete_on_add_participant(const char *name, void *userdata);

/* ========================================================================
 * Modal Stack Helpers (defined in ft_vulkan_ui.h as static inline)
 * ======================================================================== */

#ifdef _WIN32
static bool win32_set_clipboard_text(HWND hwnd, const char *text) {
    if (!text || !OpenClipboard(hwnd)) return false;
    EmptyClipboard();
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (wlen <= 0) { CloseClipboard(); return false; }
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, wlen * sizeof(WCHAR));
    if (!hMem) { CloseClipboard(); return false; }
    WCHAR *wstr = (WCHAR *)GlobalLock(hMem);
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wstr, wlen);
    GlobalUnlock(hMem);
    SetClipboardData(CF_UNICODETEXT, hMem);
    CloseClipboard();
    return true;
}

static char *win32_get_clipboard_text(HWND hwnd) {
    if (!OpenClipboard(hwnd)) return NULL;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (!hData) { CloseClipboard(); return NULL; }
    WCHAR *wstr = (WCHAR *)GlobalLock(hData);
    if (!wstr) { CloseClipboard(); return NULL; }
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    char *result = NULL;
    if (len > 0) {
        result = (char *)malloc(len);
        if (result) WideCharToMultiByte(CP_UTF8, 0, wstr, -1, result, len, NULL, NULL);
    }
    GlobalUnlock(hData);
    CloseClipboard();
    return result;
}
#endif

/* ========================================================================
 * On-demand derived values (Phase 4)
 * ======================================================================== */

static const char *get_sharing_address(const FtIdentity *id) {
    static char buf[256];
    if (!id || !id->first_name[0] || !id->last_name[0]) {
        buf[0] = '\0';
        return buf;
    }
    ft_sharing_hash_encode(id->display_name, id->pubkey_compressed, buf, sizeof(buf));
    return buf;
}

static void sync_active_edit_identity(FtVulkanUI *ui) {
    if (!ui->edit) return;

    /* Always sync local_display_name from identity */
    if (ui->identity) {
        strncpy(ui->edit->local_display_name, ui->identity->display_name,
                sizeof(ui->edit->local_display_name) - 1);
        ui->edit->local_display_name[sizeof(ui->edit->local_display_name) - 1] = '\0';
    }

    /* Sync protected_title_name for network notes */
    int idx = ui->activeChat;
    if (idx < 0 || idx >= ui->chatCount || !ui->isNetwork[idx]) return;

    const char *creator_hash = ui->chatCreator[idx];
    if (creator_hash[0] == '\0') return;

    if (ui->identity && strncmp(creator_hash, ui->identity->hash_hex, 32) == 0) {
        /* Own note — sync protected_title_name from identity (full name) */
        if (ui->identity->first_name[0] && ui->identity->last_name[0]) {
            snprintf(ui->edit->protected_title_name,
                     sizeof(ui->edit->protected_title_name),
                     "%s %s", ui->identity->first_name, ui->identity->last_name);
        } else if (ui->identity->first_name[0]) {
            snprintf(ui->edit->protected_title_name,
                     sizeof(ui->edit->protected_title_name),
                     "%s", ui->identity->first_name);
        } else {
            ui->edit->protected_title_name[0] = '\0';
        }
    } else if (ui->userTable) {
        /* Friend's note — sync protected_title_name from user table */
        ui->edit->protected_title_name[0] = '\0';
        for (int i = 0; i < ui->userTable->count; i++) {
            if (strncmp(ui->userTable->users[i].hash_hex, creator_hash, 32) == 0) {
                strncpy(ui->edit->protected_title_name, ui->userTable->users[i].display_name,
                        sizeof(ui->edit->protected_title_name) - 1);
                ui->edit->protected_title_name[sizeof(ui->edit->protected_title_name) - 1] = '\0';
                break;
            }
        }
    }
}

static void get_full_name(FtVulkanUI *ui, char *out, size_t out_len) {
    if (ui->identity && ui->identity->first_name[0] && ui->identity->last_name[0]) {
        snprintf(out, out_len, "%s %s", ui->identity->first_name, ui->identity->last_name);
    } else {
        const char *name = (ui->edit && ui->edit->local_display_name[0])
            ? ui->edit->local_display_name : "User";
        snprintf(out, out_len, "%s", name);
    }
}

static void get_display_name_and_hash(FtVulkanUI *ui, const char **out_name, const char **out_hash) {
    if (ui->identity && ui->identity->display_name[0]) {
        *out_name = ui->identity->display_name;
    } else if (ui->edit && ui->edit->local_display_name[0]) {
        *out_name = ui->edit->local_display_name;
    } else {
        *out_name = "User";
    }
    if (ui->identity && ui->identity->hash_hex[0]) {
        *out_hash = ui->identity->hash_hex;
    } else if (ui->edit) {
        *out_hash = ui->edit->local_hash;
    } else {
        *out_hash = "00000000000000000000000000000000";
    }
}

static void get_autocomplete_query(FtVulkanUI *ui, char *buf, size_t buf_size) {
    buf[0] = '\0';
    if (!ui->edit || !ft_autocomplete_is_active(ui->autocomplete)) return;
    FtTextEdit *ed = ui->edit;
    size_t start = ft_autocomplete_get_start_pos(ui->autocomplete);
    size_t end = ed->cursor;
    if (end <= start) return;
    size_t len = end - start;
    if (len >= buf_size) len = buf_size - 1;
    memcpy(buf, ed->doc->text + start, len);
    buf[len] = '\0';
}

/* Determine if cursor is in the title area */
static size_t get_title_end(FtDocument *doc) {
    size_t title_end = 0;
    for (size_t i = 0; i < doc->len && doc->text[i] != '\n'; i++) title_end++;
    return title_end;
}

/* Auto-trigger autocomplete for:
   - Title: after comma+space or at start of title (for friend names)
   - Body: at start of empty line (prefix assignment)
*/

/* Bridge FtTextEdit debug logging into the Autocomplete debug tab */
static void ui_text_edit_debug_log(void *ctx, const char *msg) {
    FtVulkanUI *ui = (FtVulkanUI *)ctx;
    if (!ui) return;
    ui_debug_log_auto(ui, "%s", msg);
}

static void maybe_trigger_autocomplete(FtVulkanUI *ui) {
    if (!ui->edit || ft_autocomplete_is_active(ui->autocomplete)) {
        ui_debug_log_auto(ui, "skip: no_edit=%d active=%d",
                     !ui->edit, (int)(ui ? ft_autocomplete_is_active(ui->autocomplete) : 0));
        return;
    }
    FtTextEdit *ed = ui->edit;
    FtDocument *doc = ed->doc;
    if (!doc || !doc->text) {
        ui_debug_log_auto(ui, "skip: no_doc=%d no_text=%d", !doc, !doc || !doc->text);
        return;
    }

    /* Lazy-create autocomplete module */
    if (!ui->autocomplete && ui->userTable) {
        int doc_idx = ui->activeChat;
        bool is_network = (doc_idx >= 0 && doc_idx < ui->chatCount && ui->isNetwork[doc_idx]);
        ui->autocomplete = ft_autocomplete_new(doc, ui->userTable, is_network);
        ft_autocomplete_set_on_add_participant(ui->autocomplete,
                                                autocomplete_on_add_participant, ui);
        if (ed->local_display_name) {
            ft_autocomplete_set_local_display_name(ui->autocomplete, ed->local_display_name);
        }
    }

    size_t cursor = ed->cursor;
    size_t title_end = get_title_end(doc);
    ui_debug_log_auto(ui, "cursor=%zu title_end=%zu is_network=%d", cursor, title_end, (int)ed->is_network);

    if (cursor <= title_end) {
        /* Title autocomplete: only for network notes, and only creator can edit */
        int doc_idx = ui->activeChat;
        if (doc_idx < 0 || doc_idx >= ui->chatCount) {
            ui_debug_log_auto(ui, "title skip: bad_doc_idx=%d chatCount=%d", doc_idx, ui->chatCount);
            return;
        }
        /* Local notes: no title autocomplete */
        if (!ui->isNetwork[doc_idx]) {
            ui_debug_log_auto(ui, "title skip: local note");
            return;
        }
        /* Network notes: creator-only */
        const char *local_hash = ed->local_hash;
        bool creator_match = (ui->chatCreator[doc_idx][0] == '\0') ||
                             (strncmp(ui->chatCreator[doc_idx], local_hash, 32) == 0);
        if (!creator_match) {
            ui_debug_log_auto(ui, "title skip: creator mismatch creator=%.8s local=%.8s",
                         ui->chatCreator[doc_idx], local_hash);
            return;
        }

        /* Don't trigger if cursor is inside a backtick pair */
        if (ft_autocomplete_cursor_in_backtick(doc, cursor)) {
            ui_debug_log_auto(ui, "title skip: inside backtick pair");
            return;
        }

        size_t word_start = cursor;
        while (word_start > 0 && doc->text[word_start - 1] != ' ' && doc->text[word_start - 1] != '`')
            word_start--;

        /* Always check for matches when in a valid title word */
        char query[64];
        size_t q_len = cursor - word_start;
        if (q_len >= sizeof(query)) q_len = sizeof(query) - 1;
        memcpy(query, doc->text + word_start, q_len);
        query[q_len] = '\0';
        ft_autocomplete_start(ui->autocomplete, word_start, 1);
        ft_autocomplete_update(ui->autocomplete, query);
        const FtAutocompleteResult *res = ft_autocomplete_get_results(ui->autocomplete);
        int matches = res ? res->count : 0;
        ui_debug_log_auto(ui, "title query=\"%s\" word_start=%zu matches=%d", query, word_start, matches);
        if (matches > 0) {
            ui_debug_log_auto(ui, "title TRIGGERED start=%zu", word_start);
            return;
        }
        ft_autocomplete_close(ui->autocomplete, FT_AUTO_CLOSE_ZERO_MATCHES);
        ui_debug_log_auto(ui, "title skip: no matches");
        return;
    }

    /* Body: start of empty line for prefix assignment */
    bool at_line_start = (cursor == 0) ||
                         (cursor > 0 && doc->text[cursor - 1] == '\n');
    bool line_empty = (cursor == doc->len) ||
                      (cursor < doc->len && doc->text[cursor] == '\n');

    ui_debug_log_auto(ui, "[auto] body check cursor=%zu len=%zu at_start=%d empty=%d",
                      cursor, doc->len, (int)at_line_start, (int)line_empty);
    if (at_line_start && line_empty) {
        ft_autocomplete_start(ui->autocomplete, cursor, 0);
        ui_debug_log_auto(ui, "body TRIGGERED start=%zu", cursor);
    } else {
        ui_debug_log_auto(ui, "body skip: at_line=%d line_empty=%d cursor=%zu doc_len=%zu",
                          at_line_start, line_empty, cursor, doc->len);
    }
}

/* ========================================================================
 * ig_input_text — immediate-mode single-line text input
 * ======================================================================== */

static struct {
    char *buf;
    int cap;
    int cursor;
    int sel_start, sel_end;
    bool mouse_selecting;
    float text_x;
    float text_scale;
} g_active_input;

static bool g_input_changed = false;

static void ig_input_mark_changed(void) {
    g_input_changed = true;
}

static int ig_input_len(void) {
    return (g_active_input.buf) ? (int)strlen(g_active_input.buf) : 0;
}

static int ig_input_pos_from_mouse(const char *buf, float mx, float textX, float scale) {
    float relX = mx - textX;
    int len = (int)strlen(buf);
    for (int i = 0; i < len; i++) {
        float charLeft = string_width_n(buf, i, scale);
        float charRight = string_width_n(buf, i + 1, scale);
        if (relX < charLeft + (charRight - charLeft) * 0.5f) return i;
    }
    return len;
}

static void ig_input_activate(char *buf, int cap, float textX, float scale) {
    g_active_input.buf = buf;
    g_active_input.cap = cap;
    g_active_input.cursor = (int)strlen(buf);
    g_active_input.sel_start = -1;
    g_active_input.sel_end = -1;
    g_active_input.mouse_selecting = false;
    g_active_input.text_x = textX;
    g_active_input.text_scale = scale;
}

static void ig_input_click(float mx, float textX, float scale, char *buf, int cap) {
    if (!buf) return;
    if (g_active_input.buf != buf) {
        ig_input_activate(buf, cap, textX, scale);
    }
    g_active_input.cursor = ig_input_pos_from_mouse(g_active_input.buf, mx, textX, scale);
    g_active_input.sel_start = g_active_input.cursor;
    g_active_input.sel_end = g_active_input.cursor;
    g_active_input.mouse_selecting = true;
    g_active_input.text_x = textX;
    g_active_input.text_scale = scale;
}

static void ig_input_drag(float mx) {
    if (!g_active_input.buf || !g_active_input.mouse_selecting) return;
    g_active_input.sel_end = ig_input_pos_from_mouse(g_active_input.buf, mx, g_active_input.text_x, g_active_input.text_scale);
    g_active_input.cursor = g_active_input.sel_end;
}

static void ig_input_release(void) {
    g_active_input.mouse_selecting = false;
    if (g_active_input.sel_start == g_active_input.sel_end) {
        g_active_input.sel_start = -1;
        g_active_input.sel_end = -1;
    }
}

static bool ig_input_delete_selection(void) {
    if (g_active_input.sel_start < 0 || g_active_input.sel_end < 0 || g_active_input.sel_start == g_active_input.sel_end)
        return false;
    int lo = g_active_input.sel_start;
    int hi = g_active_input.sel_end;
    if (lo > hi) { int t = lo; lo = hi; hi = t; }
    int len = ig_input_len();
    if (hi > len) hi = len;
    memmove(g_active_input.buf + lo, g_active_input.buf + hi, len - hi + 1);
    g_active_input.cursor = lo;
    g_active_input.sel_start = -1;
    g_active_input.sel_end = -1;
    ig_input_mark_changed();
    return true;
}

static void ig_input_char(char c) {
    if (!g_active_input.buf) return;
    ig_input_delete_selection();
    int len = ig_input_len();
    if (len >= g_active_input.cap - 1) return;
    int pos = g_active_input.cursor;
    memmove(g_active_input.buf + pos + 1, g_active_input.buf + pos, len - pos + 1);
    g_active_input.buf[pos] = c;
    g_active_input.cursor++;
    ig_input_mark_changed();
}

static void ig_input_backspace(void) {
    if (!g_active_input.buf) return;
    if (ig_input_delete_selection()) return;
    if (g_active_input.cursor <= 0) return;
    int pos = g_active_input.cursor;
    int len = ig_input_len();
    memmove(g_active_input.buf + pos - 1, g_active_input.buf + pos, len - pos + 1);
    g_active_input.cursor--;
    ig_input_mark_changed();
}

static void ig_input_delete(void) {
    if (!g_active_input.buf) return;
    if (ig_input_delete_selection()) return;
    int len = ig_input_len();
    if (g_active_input.cursor >= len) return;
    memmove(g_active_input.buf + g_active_input.cursor, g_active_input.buf + g_active_input.cursor + 1, len - g_active_input.cursor);
    ig_input_mark_changed();
}

static void ig_input_move_cursor(int delta, bool shift) {
    if (!g_active_input.buf) return;
    int len = ig_input_len();
    if (shift && g_active_input.sel_start < 0) {
        g_active_input.sel_start = g_active_input.cursor;
    }
    g_active_input.cursor += delta;
    if (g_active_input.cursor < 0) g_active_input.cursor = 0;
    if (g_active_input.cursor > len) g_active_input.cursor = len;
    if (shift) {
        g_active_input.sel_end = g_active_input.cursor;
    } else {
        g_active_input.sel_start = -1;
        g_active_input.sel_end = -1;
    }
}

static void ig_input_home(bool shift) {
    if (!g_active_input.buf) return;
    if (shift && g_active_input.sel_start < 0) {
        g_active_input.sel_start = g_active_input.cursor;
    }
    g_active_input.cursor = 0;
    if (shift) {
        g_active_input.sel_end = 0;
    } else {
        g_active_input.sel_start = -1;
        g_active_input.sel_end = -1;
    }
}

static void ig_input_end(bool shift) {
    if (!g_active_input.buf) return;
    int len = ig_input_len();
    if (shift && g_active_input.sel_start < 0) {
        g_active_input.sel_start = g_active_input.cursor;
    }
    g_active_input.cursor = len;
    if (shift) {
        g_active_input.sel_end = len;
    } else {
        g_active_input.sel_start = -1;
        g_active_input.sel_end = -1;
    }
}

static void ig_input_select_all(void) {
    if (!g_active_input.buf) return;
    int len = ig_input_len();
    g_active_input.sel_start = 0;
    g_active_input.sel_end = len;
    g_active_input.cursor = len;
}

static void ig_input_copy(void) {
    if (!g_active_input.buf) return;
    if (g_active_input.sel_start < 0 || g_active_input.sel_end < 0 || g_active_input.sel_start == g_active_input.sel_end) return;
    int start = g_active_input.sel_start;
    int end = g_active_input.sel_end;
    if (start > end) { int t = start; start = end; end = t; }
    int len = end - start;
    if (len <= 0) return;
    char *tmp = (char *)malloc(len + 1);
    if (!tmp) return;
    memcpy(tmp, g_active_input.buf + start, len);
    tmp[len] = '\0';
    win32_set_clipboard_text(g_hwnd, tmp);
    free(tmp);
}

static void ig_input_paste(void) {
#ifdef _WIN32
    if (!g_active_input.buf) return;
    char *text = win32_get_clipboard_text(g_hwnd);
    if (!text) return;
    ig_input_delete_selection();
    int cursor = g_active_input.cursor;
    int cap = g_active_input.cap;
    for (int i = 0; text[i] && cursor < cap - 1; i++) {
        char c = text[i];
        if (c == '\r' || c == '\n') continue;
        if (c < 32 || c > 126) continue;
        int len = ig_input_len();
        if (len >= cap - 1) break;
        memmove(g_active_input.buf + cursor + 1, g_active_input.buf + cursor, len - cursor + 1);
        g_active_input.buf[cursor] = c;
        cursor++;
        g_active_input.cursor = cursor;
        ig_input_mark_changed();
    }
    free(text);
#endif
}

static bool ig_input_handle_key(int key, int mods) {
    if (!g_active_input.buf) return false;
    bool ctrl = (mods & 0x0002);
    bool shift = (mods & 0x0001);
    switch (key) {
        case KEY_BS:    ig_input_backspace(); return true;
        case KEY_DEL:   ig_input_delete(); return true;
        case KEY_LEFT:  ig_input_move_cursor(-1, shift); return true;
        case KEY_RIGHT: ig_input_move_cursor(1, shift); return true;
        case KEY_HOME:  ig_input_home(shift); return true;
        case KEY_END:   ig_input_end(shift); return true;
        case KEY_A:
            if (ctrl) { ig_input_select_all(); return true; }
            break;
        case KEY_C:
            if (ctrl) { ig_input_copy(); return true; }
            break;
        case KEY_X:
            if (ctrl) { ig_input_copy(); ig_input_delete_selection(); return true; }
            break;
        case KEY_V:
            if (ctrl) { ig_input_paste(); return true; }
            break;
    }
    return false;
}

static bool ig_input_handle_char(unsigned int codepoint) {
    if (!g_active_input.buf) return false;
    if (codepoint < 32 || codepoint > 126) return false;
    ig_input_char((char)codepoint);
    return true;
}

static bool ig_input_active(void) {
    return g_active_input.buf != NULL;
}

static void ig_input_deactivate(void) {
    g_active_input.buf = NULL;
    g_active_input.mouse_selecting = false;
}


/* ========================================================================
 * UiDrawContext — wraps draw primitives to eliminate repetitive args
 * ======================================================================== */

typedef struct {
    Vertex *vertices;
    uint32_t *count;
    uint32_t capacity;
    float screenW;
    float screenH;
} UiDrawContext;

static void ctx_rect(UiDrawContext *ctx, float x, float y, float w, float h,
                     float r, float g, float b) {
    append_rect(ctx->vertices, ctx->count, x, y, w, h,
                ctx->screenW, ctx->screenH, false, r, g, b);
}

static void ctx_border(UiDrawContext *ctx, float x, float y, float w, float h,
                       float thickness, float r, float g, float b) {
    append_border(ctx->vertices, ctx->count, x, y, w, h, thickness,
                  ctx->screenW, ctx->screenH, false, r, g, b);
}

static void ctx_string(UiDrawContext *ctx, const char *text,
                       float x, float y, float scale,
                       float r, float g, float b, bool bold) {
    draw_string(ctx->vertices, ctx->count, ctx->capacity, text, x, y, scale,
                ctx->screenW, ctx->screenH, false, r, g, b, bold);
}

static void ctx_wrapped_text(UiDrawContext *ctx, const char *text,
                             float x, float y, float scale, float maxWidth,
                             float r, float g, float b, bool bold) {
    draw_wrapped_text(ctx->vertices, ctx->count, ctx->capacity, text, x, y, scale, maxWidth,
                      ctx->screenW, ctx->screenH, false, r, g, b, bold);
}

/* Measure how many visual lines draw_wrapped_text would use for this text. */
static int measure_wrapped_lines(const char *text, float scale, float maxWidth) {
    if (!text || !text[0]) return 0;
    float cx = 0.0f;
    float spaceW = g_glyphRegular[0].advance * scale;
    int lines = 1;
    const char *p = text;
    while (*p) {
        const char *wordEnd = p;
        while (*wordEnd && *wordEnd != ' ' && *wordEnd != '\n') wordEnd++;
        int wordLen = (int)(wordEnd - p);
        float wordW = string_width_n(p, wordLen, scale);
        if (cx > 0.0f && cx + wordW > maxWidth) {
            lines++;
            cx = 0.0f;
        }
        cx += wordW;
        if (*wordEnd == ' ') {
            cx += spaceW;
            p = wordEnd + 1;
        } else if (*wordEnd == '\n') {
            lines++;
            cx = 0.0f;
            p = wordEnd + 1;
        } else {
            p = wordEnd;
        }
    }
    return lines;
}

/* ========================================================================
 * Shared modal rendering primitives
 * ======================================================================== */

static void render_modal_dim(UiDrawContext *ctx, float screenW, float screenH) {
    ctx_rect(ctx, 0, 0, screenW, screenH, 0.0f, 0.0f, 0.0f);
}

static void render_modal_frame(UiDrawContext *ctx, float x, float y, float w, float h,
                                float densityScale, const float *borderColor) {
    ctx_rect(ctx, x, y, w, h, 0.00f, 0.00f, 0.00f);
    ctx_border(ctx, x, y, w, h, 2.0f * densityScale, borderColor[0], borderColor[1], borderColor[2]);
}

static void render_modal_button(UiDrawContext *ctx, float x, float y, float w, float h,
                                 float glyphH, float textScale, const char *label,
                                 const float *color, float borderThickness) {
    ctx_border(ctx, x, y, w, h, borderThickness, color[0], color[1], color[2]);
    float tw = string_width(label, textScale);
    ctx_string(ctx, label, x + (w - tw) * 0.5f, y + (h - glyphH) * 0.5f,
               textScale, color[0], color[1], color[2], false);
}

static bool ig_input_text(const char *label, char *buf, int cap,
                            float x, float y, float w, float h,
                            float textX, float textY,
                            float scale, float glyphH, float cursor_h,
                            UiDrawContext *ctx) {
    (void)label;
    bool is_active = (g_active_input.buf == buf);

    /* Background */
    ctx_rect(ctx, x, y, w, h, 0.0f, 0.0f, 0.0f);
    /* Border */
    const float *borderColor = is_active ? C_NEON : C_DIM;
    ctx_border(ctx, x, y, w, h, 1.0f, borderColor[0], borderColor[1], borderColor[2]);

    /* Selection highlight */
    if (is_active && g_active_input.sel_start >= 0 && g_active_input.sel_end >= 0 &&
        g_active_input.sel_start != g_active_input.sel_end) {
        int lo = g_active_input.sel_start;
        int hi = g_active_input.sel_end;
        if (lo > hi) { int t = lo; lo = hi; hi = t; }
        int len = (int)strlen(buf);
        if (hi > len) hi = len;
        if (lo < hi) {
            float sx1 = textX + string_width_n(buf, lo, scale);
            float sx2 = textX + string_width_n(buf, hi, scale);
            ctx_rect(ctx, sx1, textY - 2.0f, sx2 - sx1, glyphH + 4.0f, 0.15f, 0.50f, 0.60f);
        }
    }

    /* Text */
    ctx_string(ctx, buf, textX, textY, scale, C_WHITE[0], C_WHITE[1], C_WHITE[2], false);

    /* Cursor */
    if (is_active) {
        int cursor = g_active_input.cursor;
        int len = (int)strlen(buf);
        if (cursor > len) cursor = len;
        float cx = textX + string_width_n(buf, cursor, scale);
        float ch = (cursor_h > 0.0f) ? cursor_h : (h - 8.0f);
        float cy = (cursor_h > 0.0f) ? (textY - 2.0f) : (y + 4.0f);
        ctx_rect(ctx, cx, cy, 2.0f, ch, C_NEON[0], C_NEON[1], C_NEON[2]);
    }

    return (g_input_changed && is_active);
}



bool ft_vulkan_ui_init(FtVulkanUI *ui, FtVulkanRenderer *renderer, FtDocument *doc, FtTextEdit *edit, FtNet *net) {
    memset(ui, 0, sizeof(*ui));
    ui->renderer = renderer;
    ui->doc = doc;
    ui->edit = edit;
    ui->net = net;
    ui->densityScale = 1.0f;
    ui->scrollY = 0.0f;
    ui->cursorBlink = 1.0f; /* cursor always visible */
    ui->sidebarWidth = 180.0f * ui->densityScale;
    snprintf(ui->statusText, sizeof(ui->statusText), "Ready");
    /* Seed each debug tab with an initial line so users can verify tabs work */
    ui_debug_log(ui, "Editor log ready");
    ui_debug_log_auto(ui, "Autocomplete log ready — type to trigger");
    ui_debug_log_network(ui, "Network log ready — press Ctrl+Shift+D to dump state");
    return true;
}

static void update_cursor_blink(FtVulkanUI *ui) {
    (void)ui;
    /* No blinking — cursor stays fully visible */
}


/* ========================================================================
 * Shared layout helpers for sidebar, right panel, and document chrome
 * ======================================================================== */

static bool compute_show_right_panel(FtVulkanUI *ui) {
    return ft_vulkan_ui_startup_online(ui) && (ui->chatCount == 0 || ui->isNetwork[ui->activeChat]);
}

/* ========================================================================
 * Debug panel helpers
 * ======================================================================== */

typedef struct {
    float panelY, panelW, panelH;
    float dbgScale, dbgLineH;
    float tabBarY, tabBarH;
    float textY;
    int maxVisibleLines;
    int startLine, visibleLines;
    int totalLines;
    float maxScroll;
    float *scrollY;
    bool *scrollbarDragging;
    float *scrollbarDragStartY;
    float *scrollbarDragStartScrollY;
    char (*logBuf)[FT_UI_DEBUG_LOG_LINE_LEN];
} DebugPanelLayout;

static bool get_debug_panel_layout(FtVulkanUI *ui, DebugPanelLayout *out) {
    if (!ui->showDebugPanel) return false;
    float scale = 1.0f * ui->densityScale;
    float dbgScale = scale * 0.7f;
    float dbgLineH = g_fontLineHeight * dbgScale + 2.0f;
    float tabBarH = dbgLineH + 4.0f;
    float screenH = (float)ui->renderer->swapchainExtent.height;
    float statusBarH = 21.5f * ui->densityScale;
    float availableH = screenH - statusBarH;
    float panelH = availableH * 0.5f;
    if (panelH < tabBarH + dbgLineH * 3.0f + 8.0f) {
        panelH = tabBarH + dbgLineH * 3.0f + 8.0f;
    }
    float panelY = screenH - statusBarH - panelH;
    float panelW = (float)ui->renderer->swapchainExtent.width;
    if (compute_show_right_panel(ui)) panelW -= ui->sidebarWidth;
    if (panelY < 0) panelY = 0;
    if (panelH > availableH) panelH = availableH;

    int maxVisibleLines = (int)((panelH - tabBarH - 8.0f) / dbgLineH);
    if (maxVisibleLines < 3) maxVisibleLines = 3;

    int totalLines;
    float *scrollY;
    bool *scrollbarDragging;
    float *scrollbarDragStartY;
    float *scrollbarDragStartScrollY;
    char (*logBuf)[FT_UI_DEBUG_LOG_LINE_LEN];
    switch (ui->debugPanelActiveTab) {
        case 1: /* Autocomplete */
            totalLines = ui->debugLogAutoCount;
            scrollY = &ui->debugPanelScrollYAuto;
            scrollbarDragging = &ui->debugPanelScrollbarDraggingAuto;
            scrollbarDragStartY = &ui->debugPanelScrollbarDragStartYAuto;
            scrollbarDragStartScrollY = &ui->debugPanelScrollbarDragStartScrollYAuto;
            logBuf = ui->debugLogAuto;
            break;
        case 2: /* Network */
            totalLines = ui->debugLogNetworkCount;
            scrollY = &ui->debugPanelScrollYNetwork;
            scrollbarDragging = &ui->debugPanelScrollbarDraggingNetwork;
            scrollbarDragStartY = &ui->debugPanelScrollbarDragStartYNetwork;
            scrollbarDragStartScrollY = &ui->debugPanelScrollbarDragStartScrollYNetwork;
            logBuf = ui->debugLogNetwork;
            break;
        case 3: /* Friends */
            totalLines = ui->debugLogFriendCount;
            scrollY = &ui->debugPanelScrollYFriend;
            scrollbarDragging = &ui->debugPanelScrollbarDraggingFriend;
            scrollbarDragStartY = &ui->debugPanelScrollbarDragStartYFriend;
            scrollbarDragStartScrollY = &ui->debugPanelScrollbarDragStartScrollYFriend;
            logBuf = ui->debugLogFriend;
            break;
        default: /* Editor */
            totalLines = ui->debugLogCount;
            scrollY = &ui->debugPanelScrollY;
            scrollbarDragging = &ui->debugPanelScrollbarDragging;
            scrollbarDragStartY = &ui->debugPanelScrollbarDragStartY;
            scrollbarDragStartScrollY = &ui->debugPanelScrollbarDragStartScrollY;
            logBuf = ui->debugLog;
            break;
    }
    float maxScroll = (totalLines > maxVisibleLines) ? (totalLines - maxVisibleLines) * dbgLineH : 0.0f;
    if (*scrollY < 0) *scrollY = 0;
    if (*scrollY > maxScroll) *scrollY = maxScroll;

    int startLine = (int)(*scrollY / dbgLineH);
    if (startLine < 0) startLine = 0;
    if (startLine > totalLines - maxVisibleLines) startLine = totalLines - maxVisibleLines;
    if (startLine < 0) startLine = 0;
    int visibleLines = (totalLines - startLine < maxVisibleLines) ? (totalLines - startLine) : maxVisibleLines;
    float textY = panelY + tabBarH;

    out->panelY = panelY;
    out->panelW = panelW;
    out->panelH = panelH;
    out->dbgScale = dbgScale;
    out->dbgLineH = dbgLineH;
    out->tabBarY = panelY;
    out->tabBarH = tabBarH;
    out->textY = textY;
    out->maxVisibleLines = maxVisibleLines;
    out->startLine = startLine;
    out->visibleLines = visibleLines;
    out->totalLines = totalLines;
    out->maxScroll = maxScroll;
    out->scrollY = scrollY;
    out->scrollbarDragging = scrollbarDragging;
    out->scrollbarDragStartY = scrollbarDragStartY;
    out->scrollbarDragStartScrollY = scrollbarDragStartScrollY;
    out->logBuf = logBuf;
    return true;
}

static int debug_col_from_x(const char *text, float x, float scale) {
    if (x <= 0) return 0;
    int len = (int)strlen(text);
    for (int i = 0; i < len; i++) {
        float w = string_width_n(text, i + 1, scale);
        if (w > x) {
            float w_prev = string_width_n(text, i, scale);
            if (x - w_prev < w - x) return i;
            return i + 1;
        }
    }
    return len;
}

static bool debug_hit_test(FtVulkanUI *ui, double mx, double my,
                           int *outLine, int *outCol) {
    DebugPanelLayout dl;
    if (!get_debug_panel_layout(ui, &dl)) return false;
    if (mx < 0 || mx >= dl.panelW || my < dl.textY) return false;
    int relLine = (int)((my - dl.textY) / dl.dbgLineH);
    if (relLine < 0 || relLine >= dl.visibleLines) return false;
    int line = dl.startLine + relLine;
    if (line < 0 || line >= dl.totalLines) return false;
    int col = debug_col_from_x(dl.logBuf[line % FT_UI_DEBUG_LOG_LINES], (float)mx - 8.0f, dl.dbgScale);
    *outLine = line;
    *outCol = col;
    return true;
}

static void debug_normalize_selection(FtVulkanUI *ui) {
    if (ui->debugSelStartLine > ui->debugSelEndLine ||
        (ui->debugSelStartLine == ui->debugSelEndLine && ui->debugSelStartCol > ui->debugSelEndCol)) {
        int t;
        t = ui->debugSelStartLine; ui->debugSelStartLine = ui->debugSelEndLine; ui->debugSelEndLine = t;
        t = ui->debugSelStartCol; ui->debugSelStartCol = ui->debugSelEndCol; ui->debugSelEndCol = t;
    }
}

static void debug_clear_selection(FtVulkanUI *ui) {
    ui->debugHasSelection = false;
    ui->debugSelStartLine = ui->debugSelStartCol = 0;
    ui->debugSelEndLine = ui->debugSelEndCol = 0;
    ui->debugDragStartLine = ui->debugDragStartCol = 0;
}

typedef struct {
    float itemH, gap, itemTotalH, btnH;
    float sidebarBottomPad, sidebarTopBtnH;
    float sidebarContentH, sidebarVisibleH, sidebarMaxScroll;
    bool hasScrollbar;
    float sbW;
} SidebarLayout;

static void compute_sidebar_layout(FtVulkanUI *ui, SidebarLayout *out) {
    float scale = 1.0f * ui->densityScale;
    float glyphH = g_fontLineHeight * scale;
    float screenH = (float)ui->renderer->swapchainExtent.height;
    float statusBarH = 21.5f * ui->densityScale;
    float margin = 20.0f * ui->densityScale;
    out->gap = 6.0f * ui->densityScale;
    out->itemH = glyphH * 2.0f + 12.0f;
    out->itemTotalH = out->itemH + out->gap;
    out->btnH = glyphH * 1.5f + 16.0f;
    out->sidebarBottomPad = 12.0f * ui->densityScale;
    out->sidebarTopBtnH = out->btnH + out->gap;
    out->sidebarContentH = ui->chatCount * out->itemTotalH + 8.0f + margin;
    out->sidebarVisibleH = screenH - statusBarH - out->gap - out->sidebarTopBtnH - out->sidebarBottomPad;
    out->sidebarMaxScroll = out->sidebarContentH - out->sidebarVisibleH;
    if (out->sidebarMaxScroll < 0) out->sidebarMaxScroll = 0;
    out->hasScrollbar = (out->sidebarMaxScroll > 0);
    out->sbW = 4.0f * ui->densityScale;
}

static void update_chat_order(FtVulkanUI *ui) {
    /* Build array of (created_at, index) pairs and sort by created_at descending.
       If timestamps are equal, preserve original order (stable sort). */
    struct Pair { uint64_t created_at; int idx; };
    Pair pairs[FT_UI_MAX_CHATS];
    for (int i = 0; i < ui->chatCount; i++) {
        pairs[i].idx = i;
        FtDocument *doc = ui->chatDocs[i];
        pairs[i].created_at = doc ? doc->created_at : 0;
    }
    /* Insertion sort (stable, n <= 64) — descending */
    for (int i = 1; i < ui->chatCount; i++) {
        Pair key = pairs[i];
        int j = i - 1;
        while (j >= 0 && pairs[j].created_at < key.created_at) {
            pairs[j + 1] = pairs[j];
            j--;
        }
        pairs[j + 1] = key;
    }
    for (int i = 0; i < ui->chatCount; i++) {
        ui->chatOrder[i] = pairs[i].idx;
    }
}

typedef struct {
    float friendItemH, friendBtnH, friendGap, friendTopBtnH;
    float friendBottomPad, friendListTop, friendListVisibleH;
    float friendContentH, friendMaxScroll;
    bool hasScrollbar;
    float sbW;
} RightPanelLayout;

static void compute_right_panel_layout(FtVulkanUI *ui, RightPanelLayout *out) {
    float scale = 1.0f * ui->densityScale;
    float glyphH = g_fontLineHeight * scale;
    float screenH = (float)ui->renderer->swapchainExtent.height;
    float statusBarH = 21.5f * ui->densityScale;
    float margin = 20.0f * ui->densityScale;
    out->friendItemH = glyphH + 10.0f;
    out->friendBtnH = glyphH * 1.5f + 16.0f;
    out->friendGap = 6.0f * ui->densityScale;
    out->friendTopBtnH = out->friendBtnH + out->friendGap;
    out->friendBottomPad = 12.0f * ui->densityScale;
    out->friendListTop = out->friendGap + out->friendTopBtnH;
    out->friendListVisibleH = screenH - statusBarH - out->friendListTop - out->friendBottomPad;
    int friend_count = ft_friend_count(ui->userTable);
    out->friendContentH = friend_count * out->friendItemH + margin;
    out->friendMaxScroll = out->friendContentH - out->friendListVisibleH;
    if (out->friendMaxScroll < 0) out->friendMaxScroll = 0;
    out->hasScrollbar = (out->friendMaxScroll > 0);
    out->sbW = 4.0f * ui->densityScale;
}
/* ========================================================================
 * Layout engine wrappers (delegates to ft_ui_layout.cpp)
 * ======================================================================== */

static void compute_document_layout(FtVulkanUI *ui, DocumentLayout *dl) {
    bool showRightPanel = compute_show_right_panel(ui);
    float screenW = (float)ui->renderer->swapchainExtent.width;
    float screenH = (float)ui->renderer->swapchainExtent.height;
    ft_ui_layout_compute_document_layout(dl, ui->densityScale, ui->sidebarWidth, showRightPanel, ui->doc, screenW, screenH);
}

static int build_document_layout_lines(FtVulkanUI *ui, DocLayoutLine *lines, int max_lines) {
    DocumentLayout dl;
    compute_document_layout(ui, &dl);
    return ft_ui_layout_build_lines(&dl, ui->doc, ui->edit, lines, max_lines);
}

static size_t pos_from_click(FtVulkanUI *ui, double mx, double my, bool *out_on_prefix) {
    DocumentLayout dl;
    compute_document_layout(ui, &dl);
    return ft_ui_layout_pos_from_click(&dl, ui->doc, ui->edit, (float)mx, (float)my, ui->scrollY, out_on_prefix);
}

static float measure_document_height(FtVulkanUI *ui) {
    DocumentLayout dl;
    compute_document_layout(ui, &dl);
    return ft_ui_layout_measure_height(&dl, ui->doc, ui->edit, ui->edit ? ui->edit->is_network : false);
}

static float measure_y_at_pos(FtVulkanUI *ui, size_t targetPos, float *out_lineH) {
    DocumentLayout dl;
    compute_document_layout(ui, &dl);
    return ft_ui_layout_measure_y_at_pos(&dl, ui->doc, ui->edit, targetPos, out_lineH);
}

static void measure_cursor_screen_pos(FtVulkanUI *ui, float *out_x, float *out_y, float *out_glyphH) {
    DocumentLayout dl;
    compute_document_layout(ui, &dl);
    ft_ui_layout_measure_cursor_pos(&dl, ui->doc, ui->edit, ui->scrollY, out_x, out_y, out_glyphH);
}

static void ui_edit_move_line_visual(FtVulkanUI *ui, int dir) {
    DocumentLayout dl;
    compute_document_layout(ui, &dl);
    ft_ui_layout_move_line_visual(&dl, ui->doc, ui->edit, dir);
}

static void scroll_region_compute(ScrollRegion *sr, float densityScale) {
    ft_ui_scroll_region_compute(sr, densityScale);
}
static void scroll_region_clamp(ScrollRegion *sr) {
    ft_ui_scroll_region_clamp(sr);
}
static void scroll_region_wheel(ScrollRegion *sr, float wheelDelta, float densityScale) {
    ft_ui_scroll_region_wheel(sr, wheelDelta, densityScale);
}
static void scroll_region_page(ScrollRegion *sr, int direction) {
    ft_ui_scroll_region_page(sr, direction);
}
static void scroll_region_drag(ScrollRegion *sr, float mouseDeltaY, float dragStartScrollY) {
    ft_ui_scroll_region_drag(sr, mouseDeltaY, dragStartScrollY);
}
static bool scroll_region_hit_thumb(const ScrollRegion *sr, float y) {
    return ft_ui_scroll_region_hit_thumb(sr, y);
}




/* Compute and cache autocomplete dropdown geometry.
   Call whenever the dropdown needs to be rendered or hit-tested. */
static void compute_autocomplete_layout(FtVulkanUI *ui) {
    ui->autocompleteLayout.valid = false;
    if (!ft_autocomplete_is_active(ui->autocomplete) || !ui->edit) return;

    float cursorX, cursorY, cursorGlyphH;
    measure_cursor_screen_pos(ui, &cursorX, &cursorY, &cursorGlyphH);

    float scale = 1.0f * ui->densityScale;
    float glyphH = g_fontLineHeight * scale;
    float margin = 20.0f * ui->densityScale;
    float screenW = (float)ui->renderer->swapchainExtent.width;
    float screenH = (float)ui->renderer->swapchainExtent.height;
    float statusBarH = 21.5f * ui->densityScale;

    float itemH = glyphH + 8.0f;
    float dropW = 180.0f * ui->densityScale;

    char query[64];
    get_autocomplete_query(ui, query, sizeof(query));
    ft_autocomplete_update(ui->autocomplete, query);
    const FtAutocompleteResult *res = ft_autocomplete_get_results(ui->autocomplete);
    int total_users = res ? res->count : 0;
    if (total_users == 0) {
        ui_debug_log_auto(ui, "layout_close: zero matches for query='%s'", query);
        ft_autocomplete_close(ui->autocomplete, FT_AUTO_CLOSE_ZERO_MATCHES);
        return;
    }

    float dropH = total_users * itemH + 8.0f;
    float dropX = cursorX;
    float dropY = cursorY + cursorGlyphH;

    if (dropX + dropW > screenW) dropX = screenW - dropW - margin;
    if (dropY + dropH > screenH - statusBarH) dropY = cursorY - dropH - glyphH;
    if (dropY < margin) dropY = margin;

    ui->autocompleteLayout.dropX = dropX;
    ui->autocompleteLayout.dropY = dropY;
    ui->autocompleteLayout.dropW = dropW;
    ui->autocompleteLayout.dropH = dropH;
    ui->autocompleteLayout.itemH = itemH;
    ui->autocompleteLayout.valid = true;
}


static void get_scrollbar_metrics(FtVulkanUI *ui, float *out_trackY, float *out_trackH,
                                   float *out_thumbY, float *out_thumbH, float *out_maxScroll) {
    float margin = 20.0f * ui->densityScale;
    float screenH = (float)ui->renderer->swapchainExtent.height;
    float statusBarH = 21.5f * ui->densityScale;
    float visibleH = screenH - statusBarH - margin;
    float totalH = measure_document_height(ui);

    ScrollRegion sr = {
        &ui->scrollY, totalH, visibleH,
        margin, visibleH, 0, 0, 0, false
    };
    ft_ui_scroll_region_compute(&sr, ui->densityScale);

    if (out_trackY) *out_trackY = sr.trackY;
    if (out_trackH) *out_trackH = sr.trackH;
    if (out_thumbY) *out_thumbY = sr.thumbY;
    if (out_thumbH) *out_thumbH = sr.thumbH;
    if (out_maxScroll) *out_maxScroll = sr.maxScroll;
}

static bool is_over_scrollbar(FtVulkanUI *ui, double x, double y) {
    float margin = 20.0f * ui->densityScale;
    float screenW = (float)ui->renderer->swapchainExtent.width;
    float screenH = (float)ui->renderer->swapchainExtent.height;
    float statusBarH = 21.5f * ui->densityScale;
    float visibleH = screenH - statusBarH - margin;
    float totalH = measure_document_height(ui);

    ScrollRegion sr = {
        &ui->scrollY, totalH, visibleH,
        margin, visibleH, 0, 0, 0, false
    };
    ft_ui_scroll_region_compute(&sr, ui->densityScale);
    if (!sr.hasScrollbar) return false;

    bool showRightPanel = compute_show_right_panel(ui);
    float rightPanelW = ui->sidebarWidth;
    float scrollbarW = 8.0f * ui->densityScale;
    float scrollbarX = screenW - scrollbarW - (showRightPanel ? rightPanelW : 0.0f);
    return (x >= scrollbarX && x < scrollbarX + scrollbarW && y >= margin && y < margin + visibleH);
}

static void ensure_cursor_visible(FtVulkanUI *ui) {
    if (!ui->edit || !ui->renderer) return;

    float cursorLineH;
    float cursorY = measure_y_at_pos(ui, ui->edit->cursor, &cursorLineH);

    float margin = 20.0f * ui->densityScale;
    float screenH = (float)ui->renderer->swapchainExtent.height;
    float statusBarH = 21.5f * ui->densityScale;
    float visibleH = screenH - statusBarH - margin;
    float totalH = measure_document_height(ui);

    ScrollRegion sr = {
        &ui->scrollY, totalH, visibleH,
        margin, visibleH, 0, 0, 0, false
    };
    ft_ui_scroll_region_compute(&sr, ui->densityScale);

    /* Cursor above viewport: scroll up */
    if (cursorY < ui->scrollY) {
        ui->scrollY = cursorY;
    }
    /* Cursor below viewport: scroll down so cursor appears near bottom */
    else if (cursorY + cursorLineH > ui->scrollY + visibleH) {
        ui->scrollY = cursorY + cursorLineH - visibleH + cursorLineH;
    }

    ft_ui_scroll_region_clamp(&sr);
}

static void ui_update_blocks(FtVulkanUI *ui);
static void ui_touch_current_block(FtVulkanUI *ui);

/* ========================================================================
 * Startup Flow: Ethereum Onboarding Modal
 * ======================================================================== */

void ft_vulkan_ui_startup_init(FtVulkanUI *ui, FtIdentity *id) {
    memset(&g_startup, 0, sizeof(g_startup));
    ui->state = FT_STARTUP_OFFLINE;
    ui->networking_enabled = false;
    g_startup.balance_zero_error = false;
    g_startup.network_error = false;
    snprintf(g_startup.path_field, sizeof(g_startup.path_field), "%s/keys/local.identity", ui->dataDir);
    if (id && id->hash_hex[0]) {
        ft_identity_eth_address(id, g_startup.eth_address);
        strncpy(g_startup.first_name_buf, id->first_name, sizeof(g_startup.first_name_buf) - 1);
        g_startup.first_name_buf[sizeof(g_startup.first_name_buf) - 1] = '\0';
        strncpy(g_startup.last_name_buf, id->last_name, sizeof(g_startup.last_name_buf) - 1);
        g_startup.last_name_buf[sizeof(g_startup.last_name_buf) - 1] = '\0';
    } else {
        g_startup.eth_address[0] = '\0';
        g_startup.first_name_buf[0] = '\0';
        g_startup.last_name_buf[0] = '\0';
    }
}

bool ft_vulkan_ui_startup_done(FtVulkanUI *ui) {
    return ui->state == FT_STARTUP_ONLINE || ui->state == FT_STARTUP_OFFLINE;
}

bool ft_vulkan_ui_startup_online(FtVulkanUI *ui) {
    return ui->state == FT_STARTUP_ONLINE;
}

/* Poll balance. Called from main loop. Returns true if state changed. */
bool ft_vulkan_ui_startup_tick(FtVulkanUI *ui) {
    /* Balance check only runs when user explicitly chose Create Network Note */
    if (ui->state != FT_STARTUP_CHECKING) {
        return false;
    }

    /* Throttle balance checks to avoid flooding RPC endpoints */
    uint64_t now = (uint64_t)time(NULL);
    if (g_startup.last_check_time > 0 && now - g_startup.last_check_time < 5) {
        return false;
    }

    /* Kick off async balance check if not already in flight */
    if (ui->networkState && ui->networkState->balance_status == FT_NET_STATUS_IDLE
        && g_startup.eth_address[0]) {
        ui_log("[startup_tick] kicking off balance check for %s\n", g_startup.eth_address);
        g_startup.last_check_time = now;
        FtResolver *resolver = ft_registry_resolver();
        ft_resolver_get_balance(resolver, g_startup.eth_address, ui->networkState);
        return true; /* trigger redraw to show "Checking..." */
    }

    if (!ui->networkState) return false;

    /* Still pending — UI keeps showing "Checking..." */
    if (ui->networkState->balance_status == FT_NET_STATUS_PENDING) {
        return false;
    }

    /* Result arrived */
    if (ui->networkState->balance_status == FT_NET_STATUS_SUCCESS) {
        uint64_t bal = ui->networkState->balance_wei;
        g_startup.balance_wei = bal;
        ui_log("[startup_tick] balance result: %llu wei (%.6f ETH) for %s\n",
                (unsigned long long)bal, (double)bal / 1e18, g_startup.eth_address);
        if (bal > 0) {
            g_startup.balance_zero_error = false;
            g_startup.network_error = false;
            if (g_startup.first_name_buf[0] && g_startup.last_name_buf[0]) {
                ui->state = FT_STARTUP_ONLINE;
                ui->networking_enabled = true;
            } else {
                ui->state = FT_STARTUP_ASK_NAME;
                ig_input_activate(g_startup.first_name_buf, sizeof(g_startup.first_name_buf), 0.0f, 0.0f);
            }
        } else {
            g_startup.balance_zero_error = true;
            g_startup.network_error = false;
        }
        /* Reset so user can retry */
        ui->networkState->balance_status = FT_NET_STATUS_IDLE;
        return true;
    }

    /* Error */
    if (ui->networkState->balance_status == FT_NET_STATUS_ERROR) {
        ui_log("[startup_tick] balance check ERROR for %s\n", g_startup.eth_address);
        g_startup.network_error = true;
        g_startup.balance_zero_error = false;
        ui->networkState->balance_status = FT_NET_STATUS_IDLE;
        return true;
    }

    return false;
}

typedef struct {
    float modX, modY, modW, modH;
    float backX, backY, backW, backH;
    float contX, contY, contW, contH;
    bool showContinue;
    /* Shared text metrics */
    float textScale, glyphH, lineGap;
    /* Name input fields */
    float labelW, inputW, inputH;
    float firstX, firstY, firstTextX, firstTextY;
    float lastX, lastY, lastTextX, lastTextY;
    /* EDIT_PROFILE / SHOW_ADDRESS hit boxes & text metrics */
    float pathHitX, pathHitY, pathHitW, pathHitH;
    float pathTextX, pathTextY, pathTextW, pathRenderH;
    float addrHitX, addrHitY, addrHitW, addrHitH;
    float addrTextX, addrTextY, addrTextW, addrRenderH;
    /* ASK_MODE buttons */
    float askSkipX, askSkipY, askYesX, askYesY;
    /* SHOW_ADDRESS / CHECKING buttons */
    float checkBtnW, checkBtnH, checkX, checkY;
    float skipBtnX, skipBtnY;
} StartupModalLayout;

static void compute_startup_layout(const FtVulkanUI *ui, StartupModalLayout *out) {
    float screenW = (float)ui->renderer->swapchainExtent.width;
    float screenH = (float)ui->renderer->swapchainExtent.height;
    float scale = 1.0f * ui->densityScale;

    out->modW = 520.0f * ui->densityScale;
    out->modH = 320.0f * ui->densityScale;
    if (ui->state == FT_STARTUP_EDIT_PROFILE) {
        out->modW = 780.0f * ui->densityScale;
        out->modH = 460.0f * ui->densityScale;
    }
    out->modX = (screenW - out->modW) * 0.5f;
    out->modY = (screenH - out->modH) * 0.5f;

    out->backW = 120.0f * ui->densityScale;
    out->backH = 36.0f * ui->densityScale;
    out->backY = out->modY + out->modH - out->backH - 28.0f * ui->densityScale;

    out->showContinue = (ui->state == FT_STARTUP_ASK_NAME);
    out->backX = out->showContinue ? (out->modX + out->modW * 0.25f - out->backW * 0.5f)
                                   : (out->modX + out->modW * 0.5f - out->backW * 0.5f);

    out->contW = out->backW;
    out->contH = out->backH;
    out->contX = out->modX + out->modW * 0.75f - out->contW * 0.5f;
    out->contY = out->backY;

    out->textScale = scale * 0.85f;
    out->glyphH = g_fontLineHeight * scale;
    out->lineGap = out->glyphH * 1.4f;

    float modX = out->modX, modY = out->modY;
    float modW = out->modW, modH = out->modH;
    float lineGap = out->lineGap;
    float textScale = out->textScale;

    out->labelW = 100.0f * ui->densityScale;
    out->inputW = modW - 80.0f * ui->densityScale;
    out->inputH = out->glyphH + 12.0f;

    /* Name input fields (ASK_NAME / EDIT_PROFILE) */
    out->firstY = modY + 24.0f * ui->densityScale + lineGap * 1.5f;
    out->firstX = modX + 20.0f * ui->densityScale + out->labelW;
    out->firstTextX = out->firstX + 8.0f;
    out->firstTextY = out->firstY + 6.0f;

    out->lastY = out->firstY + out->inputH + lineGap;
    out->lastX = out->firstX;
    out->lastTextX = out->lastX + 8.0f;
    out->lastTextY = out->lastY + 6.0f;

    /* EDIT_PROFILE hit boxes & text metrics */
    if (ui->state == FT_STARTUP_EDIT_PROFILE) {
        float tmpY = out->lastY + out->inputH + lineGap;
        tmpY += lineGap * 0.8f; /* after "Identity:" label */
        out->pathTextX = modX + 20.0f * ui->densityScale;
        out->pathTextW = string_width(g_startup.path_field, textScale);
        out->pathRenderH = textScale * g_fontLineHeight;
        out->pathTextY = tmpY;
        out->pathHitX = out->pathTextX - 4.0f * ui->densityScale;
        out->pathHitY = out->pathTextY - g_fontAscent * textScale;
        out->pathHitW = out->pathTextW + 8.0f * ui->densityScale;
        out->pathHitH = out->pathRenderH + 8.0f * ui->densityScale;

        tmpY += lineGap * 1.2f;
        tmpY += lineGap * 0.8f; /* after "ETH Address:" label */
        out->addrTextX = modX + 20.0f * ui->densityScale;
        out->addrTextW = string_width(g_startup.eth_address, textScale);
        out->addrRenderH = textScale * g_fontLineHeight;
        out->addrTextY = tmpY;
        out->addrHitX = out->addrTextX - 4.0f * ui->densityScale;
        out->addrHitY = out->addrTextY - g_fontAscent * textScale;
        out->addrHitW = out->addrTextW + 8.0f * ui->densityScale;
        out->addrHitH = out->addrRenderH + 8.0f * ui->densityScale;
    } else if (ui->state == FT_STARTUP_SHOW_ADDRESS ||
               ui->state == FT_STARTUP_CHECKING) {
        float tmpY = modY + 24.0f * ui->densityScale;
        tmpY += lineGap * 1.5f;
        tmpY += lineGap; /* after "Your Ethereum address (Base L2):" */
        out->addrTextX = modX + 20.0f * ui->densityScale;
        out->addrTextW = string_width(g_startup.eth_address, textScale);
        out->addrRenderH = textScale * g_fontLineHeight;
        out->addrTextY = tmpY;
        out->addrHitX = out->addrTextX - 4.0f * ui->densityScale;
        out->addrHitY = out->addrTextY - g_fontAscent * textScale;
        out->addrHitW = out->addrTextW + 8.0f * ui->densityScale;
        out->addrHitH = out->addrRenderH + 8.0f * ui->densityScale;
    }

    /* ASK_MODE buttons */
    out->askSkipX = modX + modW * 0.25f - out->backW * 0.5f;
    out->askSkipY = out->backY;
    out->askYesX = modX + modW * 0.75f - out->backW * 0.5f;
    out->askYesY = out->backY;

    /* SHOW_ADDRESS / CHECKING buttons */
    out->checkBtnW = 160.0f * ui->densityScale;
    out->checkBtnH = out->backH;
    out->checkX = modX + modW * 0.7f - out->checkBtnW * 0.5f;
    out->checkY = modY + modH - out->checkBtnH - 28.0f * ui->densityScale;
    out->skipBtnX = modX + modW * 0.3f - out->checkBtnW * 0.5f;
    out->skipBtnY = out->checkY;
}

/* ── UPNP Warning Modal Layout ── */
typedef struct {
    float modX, modY, modW, modH;
    float textScale;
    float line1Y, line2Y, line3Y, line4Y, line5Y;
    float okW, okH, okX, okY;
} UpnpWarningModalLayout;

static void compute_upnp_warning_modal_layout(const FtVulkanUI *ui, UpnpWarningModalLayout *out) {
    float screenW = (float)ui->renderer->swapchainExtent.width;
    float screenH = (float)ui->renderer->swapchainExtent.height;
    float scale = 1.0f * ui->densityScale;
    float glyphH = g_fontLineHeight * scale;

    out->modW = 560.0f * ui->densityScale;
    out->modH = 260.0f * ui->densityScale;
    out->modX = (screenW - out->modW) * 0.5f;
    out->modY = (screenH - out->modH) * 0.5f;

    out->textScale = scale;
    float top = out->modY + 24.0f * ui->densityScale;
    float lineGap = glyphH + 8.0f * ui->densityScale;
    out->line1Y = top;
    out->line2Y = top + lineGap;
    out->line3Y = top + lineGap * 2.0f;
    out->line4Y = top + lineGap * 3.0f;
    out->line5Y = top + lineGap * 4.0f;

    out->okW = 80.0f * ui->densityScale;
    out->okH = 32.0f * ui->densityScale;
    out->okX = out->modX + (out->modW - out->okW) * 0.5f;
    out->okY = out->modY + out->modH - out->okH - 20.0f * ui->densityScale;
}

/* ── Delete Modal Layout ── */
typedef struct {
    float modX, modY, modW, modH;
    float msgScale, msgX, msgY;
    float warnScale, warnX, warnY;
    float yesW, yesH, yesX, yesY;
    float noW, noH, noX, noY;
    bool is_network;
} DeleteModalLayout;

static void compute_delete_modal_layout(const FtVulkanUI *ui, DeleteModalLayout *out, bool is_network) {
    float screenW = (float)ui->renderer->swapchainExtent.width;
    float screenH = (float)ui->renderer->swapchainExtent.height;
    float scale = 1.0f * ui->densityScale;
    float glyphH = g_fontLineHeight * scale;

    out->is_network = is_network;
    out->modW = 360.0f * ui->densityScale;
    out->modH = is_network ? 220.0f * ui->densityScale : 140.0f * ui->densityScale;
    out->modX = (screenW - out->modW) * 0.5f;
    out->modY = (screenH - out->modH) * 0.5f;

    const char *msg = "Delete this note?";
    out->msgScale = scale;
    out->msgY = out->modY + 24.0f * ui->densityScale;
    out->msgX = out->modX + (out->modW - string_width(msg, out->msgScale)) * 0.5f;

    if (is_network) {
        const char *warn = "This will remove the note ONLY from your";
        const char *warn2 = "machine and NOT other people you have";
        const char *warn3 = "shared it with. Do you want to continue?";
        out->warnScale = scale * 0.85f;
        float warnH = g_fontLineHeight * out->warnScale;
        float warnBlockH = warnH * 3.0f + 8.0f * ui->densityScale;
        out->warnY = out->msgY + glyphH + 12.0f * ui->densityScale;
        out->warnX = out->modX + (out->modW - string_width(warn, out->warnScale)) * 0.5f;
        (void)warnBlockH;
    }

    out->yesW = 80.0f * ui->densityScale;
    out->yesH = 32.0f * ui->densityScale;
    out->yesX = out->modX + out->modW * 0.25f - out->yesW * 0.5f;
    out->yesY = out->modY + out->modH - out->yesH - 20.0f * ui->densityScale;

    out->noW = 80.0f * ui->densityScale;
    out->noH = 32.0f * ui->densityScale;
    out->noX = out->modX + out->modW * 0.75f - out->noW * 0.5f;
    out->noY = out->yesY;
}

/* ── New Note Modal Layout ── */
typedef struct {
    float modX, modY, modW, modH;
    float textScale;
    float titleX, titleY;
    float btnW, btnH, btnY;
    float localX, netX;
} NewNoteModalLayout;

static void compute_new_note_modal_layout(const FtVulkanUI *ui, NewNoteModalLayout *out) {
    float screenW = (float)ui->renderer->swapchainExtent.width;
    float screenH = (float)ui->renderer->swapchainExtent.height;
    float scale = 1.0f * ui->densityScale;

    out->modW = 360.0f * ui->densityScale;
    out->modH = 180.0f * ui->densityScale;
    out->modX = (screenW - out->modW) * 0.5f;
    out->modY = (screenH - out->modH) * 0.5f;

    out->textScale = scale * 0.9f;
    const char *title = "Create New Note";
    out->titleY = out->modY + 24.0f * ui->densityScale;
    out->titleX = out->modX + (out->modW - string_width(title, out->textScale)) * 0.5f;

    out->btnW = 140.0f * ui->densityScale;
    out->btnH = 36.0f * ui->densityScale;
    out->btnY = out->modY + out->modH - out->btnH - 28.0f * ui->densityScale;
    out->localX = out->modX + out->modW * 0.25f - out->btnW * 0.5f;
    out->netX = out->modX + out->modW * 0.75f - out->btnW * 0.5f;
}

/* ========================================================================
 * Startup Page Vtable
 * ======================================================================== */

typedef struct {
    void (*render)(FtVulkanUI *ui, UiDrawContext *ctx,
                   const StartupModalLayout *layout, float scale, float glyphH);
    bool (*click)(FtVulkanUI *ui, const StartupModalLayout *layout, double mx, double my);
    bool (*key)(FtVulkanUI *ui, int key, int mods);
} StartupPage;

/* ------------------------------------------------------------------------
 * Shared helpers
 * ------------------------------------------------------------------------ */

static void startup_name_fields_render(FtVulkanUI *ui, UiDrawContext *ctx,
                                        const StartupModalLayout *layout, float scale, float glyphH) {
    float textScale = scale * 0.85f;
    ctx_string(ctx, "First:", layout->modX + 20.0f * ui->densityScale, layout->firstY + 6.0f,
               textScale, C_WHITE[0], C_WHITE[1], C_WHITE[2], false);
    ig_input_text("First", g_startup.first_name_buf, sizeof(g_startup.first_name_buf),
                  layout->firstX, layout->firstY,
                  layout->inputW - layout->labelW, layout->inputH,
                  layout->firstTextX, layout->firstTextY,
                  textScale, glyphH, 0.0f, ctx);
    ctx_string(ctx, "Last:", layout->modX + 20.0f * ui->densityScale, layout->lastY + 6.0f,
               textScale, C_WHITE[0], C_WHITE[1], C_WHITE[2], false);
    ig_input_text("Last", g_startup.last_name_buf, sizeof(g_startup.last_name_buf),
                  layout->lastX, layout->lastY,
                  layout->inputW - layout->labelW, layout->inputH,
                  layout->lastTextX, layout->lastTextY,
                  textScale, glyphH, 0.0f, ctx);
}

static bool startup_name_fields_click(FtVulkanUI *ui, const StartupModalLayout *layout, double mx, double my) {
    float ts = layout->textScale;
    if (mx >= layout->firstX && mx < layout->firstX + layout->inputW - layout->labelW &&
        my >= layout->firstY && my < layout->firstY + layout->inputH) {
        ig_input_click((float)mx, layout->firstTextX, ts,
                       g_startup.first_name_buf, sizeof(g_startup.first_name_buf));
        return true;
    }
    if (mx >= layout->lastX && mx < layout->lastX + layout->inputW - layout->labelW &&
        my >= layout->lastY && my < layout->lastY + layout->inputH) {
        ig_input_click((float)mx, layout->lastTextX, ts,
                       g_startup.last_name_buf, sizeof(g_startup.last_name_buf));
        return true;
    }
    return false;
}

static bool startup_name_fields_key(FtVulkanUI *ui, int key, int mods) {
    if (key == KEY_TAB) {
        float ts = 1.0f * ui->densityScale * 0.85f;
        float textX = g_active_input.text_x; /* both fields share same x offset */
        if (g_active_input.buf == g_startup.first_name_buf) {
            ig_input_activate(g_startup.last_name_buf, sizeof(g_startup.last_name_buf), textX, ts);
        } else {
            ig_input_activate(g_startup.first_name_buf, sizeof(g_startup.first_name_buf), textX, ts);
        }
        return true;
    }
    if (ig_input_handle_key(key, mods)) {
        return true;
    }
    if (key < 32 || key > 126) return true;
    return false; /* let printable chars fall through to handle_char */
}

static void title_replace_name(FtDocument *doc, const char *old_name, const char *new_name);
static void title_remove_name(FtDocument *doc, const char *name, const char *protected_name);

static void startup_save_names(FtVulkanUI *ui) {
    if (g_startup.first_name_buf[0] && g_startup.last_name_buf[0] && ui->identity) {
        char old_display_name[FT_DISPLAY_NAME_LEN];
        strncpy(old_display_name, ui->identity->display_name, sizeof(old_display_name) - 1);
        old_display_name[sizeof(old_display_name) - 1] = '\0';

        strncpy(ui->identity->first_name, g_startup.first_name_buf, sizeof(ui->identity->first_name) - 1);
        ui->identity->first_name[sizeof(ui->identity->first_name) - 1] = '\0';
        strncpy(ui->identity->last_name, g_startup.last_name_buf, sizeof(ui->identity->last_name) - 1);
        ui->identity->last_name[sizeof(ui->identity->last_name) - 1] = '\0';
        snprintf(ui->identity->display_name, sizeof(ui->identity->display_name), "%s %s",
                 ui->identity->first_name, ui->identity->last_name);
        ft_identity_save(ui->identity, g_startup.path_field, NULL);

        if (strcmp(old_display_name, ui->identity->display_name) != 0) {
            /* Broadcast name update to all active peers */
            if (ui->gossip && ui->net) {
                for (size_t i = 0; i < ui->net->peer_count; i++) {
                    if (ui->net->peers[i].active) {
                        ft_gossip_send_name_update(ui->gossip, &ui->net->peers[i].addr, ui->identity->display_name);
                    }
                }
            }
            /* Update titles in all network notes we created */
            for (int c = 0; c < ui->chatCount; c++) {
                if (ui->isNetwork[c] && strncmp(ui->chatCreator[c], ui->identity->hash_hex, 32) == 0) {
                    title_replace_name(ui->chatDocs[c], old_display_name, ui->identity->display_name);
                    if (ui->chatEdits[c]) {
                        snprintf(ui->chatEdits[c]->protected_title_name,
                                 sizeof(ui->chatEdits[c]->protected_title_name),
                                 "%s %s", ui->identity->first_name, ui->identity->last_name);
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------
 * Shared address-page rendering / handling
 * ------------------------------------------------------------------------ */

static void startup_address_page_render(FtVulkanUI *ui, UiDrawContext *ctx,
                                         const StartupModalLayout *layout, float scale, float glyphH,
                                         bool checking) {
    float textScale = scale * 0.85f;
    float lineY = layout->modY + 24.0f * ui->densityScale;
    float lineGap = layout->lineGap;

    ctx_string(ctx, "Deposit ETH to Enable Networking",
                layout->modX + 20.0f * ui->densityScale, lineY,
                textScale, C_NEON[0], C_NEON[1], C_NEON[2], false);
    lineY += lineGap * 1.5f;
    ctx_string(ctx, "Your Ethereum address (Base L2):",
                layout->modX + 20.0f * ui->densityScale, lineY,
                textScale, C_WHITE[0], C_WHITE[1], C_WHITE[2], false);
    lineY += lineGap;

    if (g_startup.addr_highlighted) {
        ctx_rect(ctx,
                    layout->addrTextX - 2.0f * ui->densityScale,
                    layout->addrTextY - 2.0f * ui->densityScale,
                    layout->addrTextW + 4.0f * ui->densityScale,
                    layout->addrRenderH + 4.0f * ui->densityScale,
                    0.15f, 0.55f, 0.55f);
    }
    ctx_string(ctx, g_startup.eth_address,
                layout->addrTextX, layout->addrTextY,
                textScale, C_NEON[0], C_NEON[1], C_NEON[2], false);
    ctx_rect(ctx,
                layout->addrTextX, layout->addrTextY + layout->addrRenderH,
                layout->addrTextW, 1.5f * ui->densityScale, C_NEON[0], C_NEON[1], C_NEON[2]);
    lineY += lineGap * 1.5f;

    if (checking) {
        char buf[128];
        const char *spinner = "|/-\\";
        int frame = (int)(GetTickCount() / 250) % 4;
        snprintf(buf, sizeof(buf), "Checking balance... %c (last: %llu wei)",
                 spinner[frame], (unsigned long long)g_startup.balance_wei);
        ctx_string(ctx, buf,
                    layout->modX + 20.0f * ui->densityScale, lineY,
                    textScale, C_WHITE[0], C_WHITE[1], C_WHITE[2], false);
    } else {
        if (g_startup.balance_wei > 0) {
            char balBuf[128];
            double eth = (double)g_startup.balance_wei / 1e18;
            snprintf(balBuf, sizeof(balBuf), "Balance: %.6f ETH (ready)", eth);
            ctx_string(ctx, balBuf,
                        layout->modX + 20.0f * ui->densityScale, lineY,
                        textScale, C_NEON[0], C_NEON[1], C_NEON[2], false);
            lineY += lineGap;
            ctx_string(ctx, "Click Create Network Note to start chatting.",
                        layout->modX + 20.0f * ui->densityScale, lineY,
                        textScale, C_WHITE[0], C_WHITE[1], C_WHITE[2], false);
        } else {
            ctx_string(ctx, "Send any amount of ETH to the address above.",
                        layout->modX + 20.0f * ui->densityScale, lineY,
                        textScale, C_WHITE[0], C_WHITE[1], C_WHITE[2], false);
            lineY += lineGap;
            ctx_string(ctx, "Click Create Network Note once deposited.",
                        layout->modX + 20.0f * ui->densityScale, lineY,
                        textScale, C_WHITE[0], C_WHITE[1], C_WHITE[2], false);
        }
    }

    if (g_startup.network_error) {
        lineY += lineGap;
        ctx_string(ctx, "Network error: could not connect to registry.",
                    layout->modX + 20.0f * ui->densityScale, lineY,
                    textScale, C_PINK[0], C_PINK[1], C_PINK[2], false);
    } else if (g_startup.balance_zero_error) {
        lineY += lineGap;
        ctx_string(ctx, "Error: Balance is zero. Please deposit ETH.",
                    layout->modX + 20.0f * ui->densityScale, lineY,
                    textScale, C_PINK[0], C_PINK[1], C_PINK[2], false);
    }

    render_modal_button(ctx, layout->checkX, layout->checkY, layout->checkBtnW, layout->checkBtnH,
                        glyphH, textScale, "Create Network Note", C_NEON,
                        1.5f * ui->densityScale);
    render_modal_button(ctx, layout->skipBtnX, layout->skipBtnY, layout->checkBtnW, layout->checkBtnH,
                        glyphH, textScale, "Create Local Note", C_PINK,
                        1.5f * ui->densityScale);
}

static bool startup_address_page_click(FtVulkanUI *ui, const StartupModalLayout *layout, double mx, double my) {
    /* Click on Ethereum address = copy to clipboard */
    if (mx >= layout->addrHitX && mx < layout->addrHitX + layout->addrHitW &&
        my >= layout->addrHitY && my < layout->addrHitY + layout->addrHitH) {
        win32_set_clipboard_text(g_hwnd, g_startup.eth_address);
        g_startup.addr_highlighted = true;
        ft_vulkan_ui_set_status(ui, "Address copied to clipboard");
        return true;
    }
    /* Skip (left) - Create Local Note */
    if (mx >= layout->skipBtnX && mx < layout->skipBtnX + layout->checkBtnW &&
        my >= layout->skipBtnY && my < layout->skipBtnY + layout->checkBtnH) {
        modal_close(&ui->modalManager, MODAL_STARTUP);
        ui->state = FT_STARTUP_OFFLINE;
        ui->networking_enabled = false;
        const char *display_name, *local_hash;
        get_display_name_and_hash(ui, &display_name, &local_hash);
        ft_vulkan_ui_add_chat(ui, display_name, local_hash, false);
        return true;
    }
    /* Create Network Note (right) */
    if (mx >= layout->checkX && mx < layout->checkX + layout->checkBtnW &&
        my >= layout->checkY && my < layout->checkY + layout->checkBtnH) {
        ui->state = FT_STARTUP_CHECKING;
        g_startup.last_check_time = 0;
        g_startup.balance_zero_error = false;
        g_startup.network_error = false;
        return true;
    }
    return false;
}

static bool startup_nonname_key(FtVulkanUI *ui, int key, int mods) {
    (void)mods;
    if (key == KEY_ESC) {
        modal_close(&ui->modalManager, MODAL_STARTUP);
        ui->state = FT_STARTUP_OFFLINE;
        ui->networking_enabled = false;
        return true;
    }
    return true; /* block text editing */
}

/* ------------------------------------------------------------------------
 * Per-state page implementations
 * ------------------------------------------------------------------------ */

static void page_ask_mode_render(FtVulkanUI *ui, UiDrawContext *ctx,
                                  const StartupModalLayout *layout, float scale, float glyphH) {
    float textScale = scale * 0.85f;
    float lineY = layout->modY + 24.0f * ui->densityScale;
    float lineGap = layout->lineGap;

    ctx_string(ctx, "FreeText P2P Networking",
                layout->modX + 20.0f * ui->densityScale, lineY,
                textScale, C_NEON[0], C_NEON[1], C_NEON[2], false);
    lineY += lineGap * 1.5f;
    ctx_string(ctx, "To chat with others, you need a small",
                layout->modX + 20.0f * ui->densityScale, lineY,
                textScale, C_WHITE[0], C_WHITE[1], C_WHITE[2], false);
    lineY += lineGap;
    ctx_string(ctx, "amount of ETH on Base L2 (~$0.01).",
                layout->modX + 20.0f * ui->densityScale, lineY,
                textScale, C_WHITE[0], C_WHITE[1], C_WHITE[2], false);
    lineY += lineGap * 1.5f;
    ctx_string(ctx, "You can also skip this and use FreeText",
                layout->modX + 20.0f * ui->densityScale, lineY,
                textScale, C_WHITE[0], C_WHITE[1], C_WHITE[2], false);
    lineY += lineGap;
    ctx_string(ctx, "as a local note-taking app.",
                layout->modX + 20.0f * ui->densityScale, lineY,
                textScale, C_WHITE[0], C_WHITE[1], C_WHITE[2], false);

    render_modal_button(ctx, layout->askSkipX, layout->askSkipY, layout->backW, layout->backH,
                        glyphH, textScale, "Create Local Note", C_PINK,
                        1.5f * ui->densityScale);
    render_modal_button(ctx, layout->askYesX, layout->askYesY, layout->backW, layout->backH,
                        glyphH, textScale, "Enable Networking", C_NEON,
                        1.5f * ui->densityScale);
}

static bool page_ask_mode_click(FtVulkanUI *ui, const StartupModalLayout *layout, double mx, double my) {
    /* Skip (left) */
    if (mx >= layout->askSkipX && mx < layout->askSkipX + layout->backW &&
        my >= layout->askSkipY && my < layout->askSkipY + layout->backH) {
        modal_close(&ui->modalManager, MODAL_STARTUP);
        ui->state = FT_STARTUP_OFFLINE;
        ui->networking_enabled = false;
        const char *display_name, *local_hash;
        get_display_name_and_hash(ui, &display_name, &local_hash);
        ft_vulkan_ui_add_chat(ui, display_name, local_hash, false);
        return true;
    }
    /* Enable Networking (right) */
    if (mx >= layout->askYesX && mx < layout->askYesX + layout->backW &&
        my >= layout->askYesY && my < layout->askYesY + layout->backH) {
        ui->state = FT_STARTUP_SHOW_ADDRESS;
        g_startup.last_check_time = 0;
        g_startup.balance_zero_error = false;
        g_startup.network_error = false;
        return true;
    }
    return false;
}

static bool page_ask_mode_key(FtVulkanUI *ui, int key, int mods) {
    return startup_nonname_key(ui, key, mods);
}

static void page_ask_name_render(FtVulkanUI *ui, UiDrawContext *ctx,
                                  const StartupModalLayout *layout, float scale, float glyphH) {
    float textScale = scale * 0.85f;
    float lineY = layout->modY + 24.0f * ui->densityScale;

    ctx_string(ctx, "Enter Your Name",
                layout->modX + 20.0f * ui->densityScale, lineY,
                textScale, C_NEON[0], C_NEON[1], C_NEON[2], false);
    startup_name_fields_render(ui, ctx, layout, scale, glyphH);
    render_modal_button(ctx, layout->backX, layout->backY, layout->backW, layout->backH,
                        glyphH, textScale, "Back", C_PINK,
                        1.5f * ui->densityScale);
    render_modal_button(ctx, layout->contX, layout->contY, layout->contW, layout->contH,
                        glyphH, textScale, "Continue", C_NEON,
                        1.5f * ui->densityScale);
}

static bool page_ask_name_click(FtVulkanUI *ui, const StartupModalLayout *layout, double mx, double my) {
    if (startup_name_fields_click(ui, layout, mx, my)) return true;

    /* Back button */
    if (mx >= layout->backX && mx < layout->backX + layout->backW &&
        my >= layout->backY && my < layout->backY + layout->backH) {
        ui->state = FT_STARTUP_ASK_MODE;
        return true;
    }

    /* Continue button */
    if (mx >= layout->contX && mx < layout->contX + layout->contW &&
        my >= layout->contY && my < layout->contY + layout->contH) {
        if (!g_startup.first_name_buf[0] || !g_startup.last_name_buf[0])
            return true; /* require both names */
        startup_save_names(ui);
        ui->state = FT_STARTUP_ONLINE;
        ui->networking_enabled = true;
        modal_close(&ui->modalManager, MODAL_STARTUP);
        const char *display_name, *local_hash;
        get_display_name_and_hash(ui, &display_name, &local_hash);
        ft_vulkan_ui_add_chat(ui, display_name, local_hash, true);
        return true;
    }

    return false;
}

static bool page_ask_name_key(FtVulkanUI *ui, int key, int mods) {
    if (key == KEY_ESC) {
        ui->state = FT_STARTUP_ASK_MODE;
        return true;
    }
    if (key == KEY_ENTER) {
        if (!g_startup.first_name_buf[0] || !g_startup.last_name_buf[0])
            return true; /* require both names */
        startup_save_names(ui);
        /* If balance already checked and > 0, go straight online */
        if (g_startup.balance_wei > 0) {
            ui->state = FT_STARTUP_ONLINE;
            ui->networking_enabled = true;
            modal_close(&ui->modalManager, MODAL_STARTUP);
            const char *display_name, *local_hash;
            get_display_name_and_hash(ui, &display_name, &local_hash);
            ft_vulkan_ui_add_chat(ui, display_name, local_hash, true);
        } else {
            ui->state = FT_STARTUP_SHOW_ADDRESS;
            g_startup.last_check_time = 0;
        }
        return true;
    }
    return startup_name_fields_key(ui, key, mods);
}

static void page_edit_profile_render(FtVulkanUI *ui, UiDrawContext *ctx,
                                      const StartupModalLayout *layout, float scale, float glyphH) {
    float textScale = scale * 0.85f;
    float lineY = layout->modY + 24.0f * ui->densityScale;

    ctx_string(ctx, "Enter Your Name",
                layout->modX + 20.0f * ui->densityScale, lineY,
                textScale, C_NEON[0], C_NEON[1], C_NEON[2], false);
    startup_name_fields_render(ui, ctx, layout, scale, glyphH);

    /* Identity keyfile path — neon blue clickable text (not an input box) */
    ctx_string(ctx, "Identity:",
                layout->modX + 20.0f * ui->densityScale, layout->pathTextY - layout->lineGap * 0.8f,
                textScale, C_WHITE[0], C_WHITE[1], C_WHITE[2], false);
    if (g_startup.path_highlighted) {
        ctx_rect(ctx,
                    layout->pathTextX - 2.0f * ui->densityScale,
                    layout->pathTextY - 2.0f * ui->densityScale,
                    layout->pathTextW + 4.0f * ui->densityScale,
                    layout->pathRenderH + 4.0f * ui->densityScale,
                    0.15f, 0.55f, 0.55f);
    }
    ctx_string(ctx, g_startup.path_field,
                layout->pathTextX, layout->pathTextY,
                textScale, C_NEON[0], C_NEON[1], C_NEON[2], false);
    ctx_rect(ctx,
                layout->pathTextX, layout->pathTextY + layout->pathRenderH,
                layout->pathTextW, 1.5f * ui->densityScale, C_NEON[0], C_NEON[1], C_NEON[2]);

    /* ETH Address (click-to-copy, highlightable) */
    ctx_string(ctx, "ETH Address:",
                layout->modX + 20.0f * ui->densityScale, layout->addrTextY - layout->lineGap * 0.8f,
                textScale, C_WHITE[0], C_WHITE[1], C_WHITE[2], false);
    if (g_startup.addr_highlighted) {
        ctx_rect(ctx,
                    layout->addrTextX - 2.0f * ui->densityScale,
                    layout->addrTextY - 2.0f * ui->densityScale,
                    layout->addrTextW + 4.0f * ui->densityScale,
                    layout->addrRenderH + 4.0f * ui->densityScale,
                    0.15f, 0.55f, 0.55f);
    }
    ctx_string(ctx, g_startup.eth_address,
                layout->addrTextX, layout->addrTextY,
                textScale, C_NEON[0], C_NEON[1], C_NEON[2], false);
    ctx_rect(ctx,
                layout->addrTextX, layout->addrTextY + layout->addrRenderH,
                layout->addrTextW, 1.5f * ui->densityScale, C_NEON[0], C_NEON[1], C_NEON[2]);

    /* Balance */
    if (g_startup.eth_address[0]) {
        char balBuf[128];
        double eth = (double)g_startup.balance_wei / 1e18;
        snprintf(balBuf, sizeof(balBuf), "Balance: %.6f ETH", eth);
        ctx_string(ctx, balBuf,
                    layout->modX + 20.0f * ui->densityScale,
                    layout->addrTextY + layout->lineGap * 1.2f,
                    textScale, C_WHITE[0], C_WHITE[1], C_WHITE[2], false);
    }

    render_modal_button(ctx, layout->backX, layout->backY, layout->backW, layout->backH,
                        glyphH, textScale, "Close", C_PINK,
                        1.5f * ui->densityScale);
}

static bool page_edit_profile_click(FtVulkanUI *ui, const StartupModalLayout *layout, double mx, double my) {
    if (startup_name_fields_click(ui, layout, mx, my)) return true;

    /* Path field click-to-copy */
    if (mx >= layout->pathHitX && mx < layout->pathHitX + layout->pathHitW &&
        my >= layout->pathHitY && my < layout->pathHitY + layout->pathHitH) {
        win32_set_clipboard_text(g_hwnd, g_startup.path_field);
        g_startup.path_highlighted = true;
        ft_vulkan_ui_set_status(ui, "Identity path copied to clipboard");
        return true;
    }
    /* ETH address click-to-copy */
    if (mx >= layout->addrHitX && mx < layout->addrHitX + layout->addrHitW &&
        my >= layout->addrHitY && my < layout->addrHitY + layout->addrHitH) {
        win32_set_clipboard_text(g_hwnd, g_startup.eth_address);
        g_startup.addr_highlighted = true;
        ft_vulkan_ui_set_status(ui, "Address copied to clipboard");
        return true;
    }
    /* Close button */
    if (mx >= layout->backX && mx < layout->backX + layout->backW &&
        my >= layout->backY && my < layout->backY + layout->backH) {
        startup_save_names(ui);
        modal_close(&ui->modalManager, MODAL_STARTUP);
        ui->state = ui->networking_enabled ? FT_STARTUP_ONLINE : FT_STARTUP_OFFLINE;
        return true;
    }
    return false;
}

static bool page_edit_profile_key(FtVulkanUI *ui, int key, int mods) {
    if (key == KEY_ESC) {
        modal_close(&ui->modalManager, MODAL_STARTUP);
        ui->state = ui->networking_enabled ? FT_STARTUP_ONLINE : FT_STARTUP_OFFLINE;
        return true;
    }
    if (key == KEY_ENTER) {
        startup_save_names(ui);
        modal_close(&ui->modalManager, MODAL_STARTUP);
        ui->state = ui->networking_enabled ? FT_STARTUP_ONLINE : FT_STARTUP_OFFLINE;
        return true;
    }
    return startup_name_fields_key(ui, key, mods);
}

static void page_show_address_render(FtVulkanUI *ui, UiDrawContext *ctx,
                                      const StartupModalLayout *layout, float scale, float glyphH) {
    startup_address_page_render(ui, ctx, layout, scale, glyphH, false);
}

static bool page_show_address_click(FtVulkanUI *ui, const StartupModalLayout *layout, double mx, double my) {
    return startup_address_page_click(ui, layout, mx, my);
}

static bool page_show_address_key(FtVulkanUI *ui, int key, int mods) {
    return startup_nonname_key(ui, key, mods);
}

static void page_checking_render(FtVulkanUI *ui, UiDrawContext *ctx,
                                  const StartupModalLayout *layout, float scale, float glyphH) {
    startup_address_page_render(ui, ctx, layout, scale, glyphH, true);
}

static bool page_checking_click(FtVulkanUI *ui, const StartupModalLayout *layout, double mx, double my) {
    return startup_address_page_click(ui, layout, mx, my);
}

static bool page_checking_key(FtVulkanUI *ui, int key, int mods) {
    return startup_nonname_key(ui, key, mods);
}

/* ------------------------------------------------------------------------
 * Singleton instances and lookup
 * ------------------------------------------------------------------------ */

static const StartupPage page_ask_mode = {
    page_ask_mode_render, page_ask_mode_click, page_ask_mode_key
};

static const StartupPage page_ask_name = {
    page_ask_name_render, page_ask_name_click, page_ask_name_key
};

static const StartupPage page_edit_profile = {
    page_edit_profile_render, page_edit_profile_click, page_edit_profile_key
};

static const StartupPage page_show_address = {
    page_show_address_render, page_show_address_click, page_show_address_key
};

static const StartupPage page_checking = {
    page_checking_render, page_checking_click, page_checking_key
};

static const StartupPage *startup_page_for_state(FtStartupState state) {
    switch (state) {
        case FT_STARTUP_ASK_MODE:       return &page_ask_mode;
        case FT_STARTUP_ASK_NAME:       return &page_ask_name;
        case FT_STARTUP_EDIT_PROFILE:   return &page_edit_profile;
        case FT_STARTUP_SHOW_ADDRESS:   return &page_show_address;
        case FT_STARTUP_CHECKING:       return &page_checking;
        default:                        return NULL;
    }
}

static void render_startup_modal(FtVulkanUI *ui, Vertex *vertices, uint32_t *count, uint32_t capacity,
                                  float screenW, float screenH, float scale, float glyphH) {
    UiDrawContext ctx = {vertices, count, capacity, screenW, screenH};
    StartupModalLayout layout;
    compute_startup_layout(ui, &layout);
    float modX = layout.modX, modY = layout.modY, modW = layout.modW, modH = layout.modH;

    ctx_rect(&ctx, modX, modY, modW, modH, 0.00f, 0.00f, 0.00f);
    ctx_border(&ctx, modX, modY, modW, modH, 2.0f * ui->densityScale, C_NEON[0], C_NEON[1], C_NEON[2]);

    const StartupPage *page = startup_page_for_state(ui->state);
    if (page && page->render) page->render(ui, &ctx, &layout, scale, glyphH);
}

/* Handle mouse clicks on startup modal buttons.
   Returns true if a button was clicked and state changed. */
static bool handle_startup_click(FtVulkanUI *ui, double mx, double my) {
    if (ft_vulkan_ui_startup_done(ui)) return false;
    if (g_startup.addr_highlighted || g_startup.path_highlighted) {
        g_startup.addr_highlighted = false;
        g_startup.path_highlighted = false;
    }
    StartupModalLayout layout;
    compute_startup_layout(ui, &layout);
    const StartupPage *page = startup_page_for_state(ui->state);
    if (page && page->click) return page->click(ui, &layout, mx, my);
    return false;
}

/* ========================================================================
 * New Note Choice Modal
 * ======================================================================== */

static void render_new_note_modal(FtVulkanUI *ui, Vertex *vertices, uint32_t *count, uint32_t capacity,
                                   float screenW, float screenH, float scale, float glyphH) {
    UiDrawContext ctx = {vertices, count, capacity, screenW, screenH};
    NewNoteModalLayout nl;
    compute_new_note_modal_layout(ui, &nl);

    render_modal_dim(&ctx, screenW, screenH);
    render_modal_frame(&ctx, nl.modX, nl.modY, nl.modW, nl.modH,
                       ui->densityScale, C_NEON);

    ctx_string(&ctx, "Create New Note", nl.titleX, nl.titleY,
                nl.textScale, C_NEON[0], C_NEON[1], C_NEON[2], false);

    render_modal_button(&ctx, nl.localX, nl.btnY, nl.btnW, nl.btnH,
                        glyphH, nl.textScale, "Local Note", C_NEON,
                        1.5f * ui->densityScale);
    render_modal_button(&ctx, nl.netX, nl.btnY, nl.btnW, nl.btnH,
                        glyphH, nl.textScale, "Network Note", C_PINK,
                        1.5f * ui->densityScale);
}

static void handle_new_note_modal_click(FtVulkanUI *ui, double mx, double my) {
    NewNoteModalLayout nl;
    compute_new_note_modal_layout(ui, &nl);

    const char *display_name, *local_hash;
    get_display_name_and_hash(ui, &display_name, &local_hash);

    /* Local Note button */
    if (mx >= nl.localX && mx < nl.localX + nl.btnW && my >= nl.btnY && my < nl.btnY + nl.btnH) {
        modal_close(&ui->modalManager, MODAL_NEW_NOTE);
        ft_vulkan_ui_add_chat(ui, display_name, local_hash, false);
        return;
    }

    /* Network Note button */
    if (mx >= nl.netX && mx < nl.netX + nl.btnW && my >= nl.btnY && my < nl.btnY + nl.btnH) {
        modal_close(&ui->modalManager, MODAL_NEW_NOTE);
        if (ft_vulkan_ui_startup_online(ui)) {
            ft_vulkan_ui_add_chat(ui, display_name, local_hash, true);
        } else {
            modal_push(&ui->modalManager, MODAL_STARTUP);
            if (ui->identity && ui->identity->display_name[0]) {
                /* User has a name — check balance immediately */
                ui->state = FT_STARTUP_CHECKING;
                g_startup.last_check_time = 0;
                g_startup.balance_zero_error = false;
            g_startup.network_error = false;
            } else {
                /* New user — need name before we can check balance */
                ui->state = FT_STARTUP_ASK_MODE;
            }
        }
        return;
    }

    /* Click outside modal = cancel */
    modal_close(&ui->modalManager, MODAL_NEW_NOTE);
}

/* ========================================================================
 * Friend List Modal
 * ======================================================================== */

static int friend_modal_pos_from_x(const char *text, int len, float relX, float scale) {
    float x = 0.0f;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        float cw = g_fontAdvance * scale;
        if (c >= FONT_ASCII_START && c <= 127) {
            cw = g_glyphRegular[(int)c - FONT_ASCII_START].advance * scale;
        }
        if (relX < x + cw * 0.5f) return i;
        x += cw;
    }
    return len;
}

/* Multi-line text helpers for the friend modal input box */
#define MODAL_INPUT_MAX_LINES 2

static int modal_wrap_lines(const char *text, int len, float maxWidth, float scale,
                            int *lineStarts, int *lineEnds, int maxLines) {
    int lineCount = 0;
    int pos = 0;
    while (pos < len && lineCount < maxLines) {
        lineStarts[lineCount] = pos;
        int end = pos;
        float w = 0.0f;
        while (end < len) {
            unsigned char c = (unsigned char)text[end];
            float cw = g_fontAdvance * scale;
            if (c >= FONT_ASCII_START && c <= 127) {
                cw = g_glyphRegular[(int)c - FONT_ASCII_START].advance * scale;
            }
            if (w + cw > maxWidth && w > 0.0f) break;
            w += cw;
            end++;
        }
        if (end == pos && pos < len) end++; /* at least 1 char per line */
        lineEnds[lineCount] = end;
        pos = end;
        lineCount++;
    }
    return lineCount;
}

static int modal_pos_from_xy(const char *text, int len, float relX, float relY,
                             float scale, float lineH, float maxWidth) {
    int lineStarts[MODAL_INPUT_MAX_LINES];
    int lineEnds[MODAL_INPUT_MAX_LINES];
    int lineCount = modal_wrap_lines(text, len, maxWidth, scale, lineStarts, lineEnds, MODAL_INPUT_MAX_LINES);

    int lineIdx = (int)(relY / lineH);
    if (lineIdx < 0) lineIdx = 0;
    if (lineIdx >= lineCount) {
        if (lineCount > 0) return lineEnds[lineCount - 1];
        return 0;
    }
    int lineStart = lineStarts[lineIdx];
    int lineLen = lineEnds[lineIdx] - lineStart;
    int pos = friend_modal_pos_from_x(text + lineStart, lineLen, relX, scale);
    return lineStart + pos;
}

static void modal_cursor_xy(const char *text, int len, int cursorPos,
                            float scale, float lineH, float maxWidth,
                            int *outLine, float *outX) {
    int lineStarts[MODAL_INPUT_MAX_LINES];
    int lineEnds[MODAL_INPUT_MAX_LINES];
    int lineCount = modal_wrap_lines(text, len, maxWidth, scale, lineStarts, lineEnds, MODAL_INPUT_MAX_LINES);

    for (int i = 0; i < lineCount; i++) {
        if (cursorPos >= lineStarts[i] && cursorPos <= lineEnds[i]) {
            *outLine = i;
            *outX = string_width_n(text + lineStarts[i], cursorPos - lineStarts[i], scale);
            return;
        }
    }
    /* Cursor past end of last line */
    if (lineCount > 0) {
        *outLine = lineCount - 1;
        *outX = string_width_n(text + lineStarts[lineCount - 1],
                               lineEnds[lineCount - 1] - lineStarts[lineCount - 1], scale);
    } else {
        *outLine = 0;
        *outX = 0.0f;
    }
}

static void update_friend_modal_preview(FtVulkanUI *ui) {
    if (((int)strlen(g_friendModal.hash_buf)) <= 0) {
        g_friendModal.preview_valid = false;
        g_friendModal.preview_name[0] = '\0';
        g_friendModal.preview_hash[0] = '\0';
        return;
    }
    char hash_hex[FT_HASH_HEX_LEN];
    char display_name[FT_DISPLAY_NAME_LEN];
    char pubkey_hex[FT_PUBKEY_HEX_LEN];
    if (ft_sharing_hash_decode(g_friendModal.hash_buf, hash_hex, display_name, sizeof(display_name), pubkey_hex, sizeof(pubkey_hex)) == 0) {
        g_friendModal.preview_valid = true;
        strncpy(g_friendModal.preview_name, display_name, sizeof(g_friendModal.preview_name) - 1);
        g_friendModal.preview_name[sizeof(g_friendModal.preview_name) - 1] = '\0';
        strncpy(g_friendModal.preview_hash, hash_hex, sizeof(g_friendModal.preview_hash) - 1);
        g_friendModal.preview_hash[sizeof(g_friendModal.preview_hash) - 1] = '\0';
    } else {
        g_friendModal.preview_valid = false;
        g_friendModal.preview_name[0] = '\0';
        g_friendModal.preview_hash[0] = '\0';
    }
}

static void save_user_table(FtVulkanUI *ui) {
    if (ui->userTable && ui->peerCachePath[0]) {
        ft_user_table_save(ui->userTable, ui->peerCachePath);
    }
}

/* ── Friend Modal Layout ── */
static void compute_friend_modal_layout(FtVulkanUI *ui, FriendModalLayout *out) {
    float screenW = (float)ui->renderer->swapchainExtent.width;
    float screenH = (float)ui->renderer->swapchainExtent.height;
    float scale = 1.0f * ui->densityScale;
    float glyphH = g_fontLineHeight * scale;

    out->modW = 960.0f * ui->densityScale;
    out->modH = 520.0f * ui->densityScale;
    out->modX = (screenW - out->modW) * 0.5f;
    out->modY = (screenH - out->modH) * 0.5f;

    out->textScale = scale * 0.9f;
    out->glyphH = glyphH;
    out->lineGap = glyphH * 1.3f;
    out->pad = 20.0f * ui->densityScale;

    out->itemH = glyphH * 2.0f + 10.0f;
    out->textLineH = g_fontLineHeight * out->textScale + 4.0f;
    out->inputH = out->textLineH * MODAL_INPUT_MAX_LINES + 8.0f;
    out->btnH = 28.0f * ui->densityScale;
    out->bottomPad = 16.0f * ui->densityScale;
    out->gap = 12.0f * ui->densityScale;

    float modX = out->modX, modY = out->modY, modW = out->modW, modH = out->modH;
    float lineGap = out->lineGap;
    float pad = out->pad;
    float btnH = out->btnH;
    float bottomPad = out->bottomPad;
    float gap = out->gap;

    float lineY = modY + 16.0f * ui->densityScale;
    lineY += lineGap * 1.3f; /* title */
    lineY += lineGap;        /* label */
    out->pathY = lineY;
    lineY += lineGap;        /* path */

    out->pathX = modX + pad;
    out->pathW = string_width(ui->peerCachePath, out->textScale);

    int friend_count = 0;
    FtUser **friends = ui->userTable ? ft_friend_list(ui->userTable, &friend_count) : NULL;
    out->friend_count = friend_count;
    (void)friends;

    bool showPreview = ((int)strlen(g_friendModal.hash_buf)) > 0;
    float bottomContentH = bottomPad + btnH + gap;
    if (showPreview) {
        bottomContentH += lineGap * 2 + gap * 0.5f;
    }
    bottomContentH += out->inputH + lineGap + gap;

    out->friendListTop = lineY + gap * 0.5f;
    out->friendListBottom = modY + modH - bottomContentH;
    out->friendListH = out->friendListBottom - out->friendListTop;
    if (out->friendListH < out->itemH) out->friendListH = out->itemH;

    out->friendContentH = friend_count * out->itemH;
    out->friendMaxScroll = out->friendContentH - out->friendListH;
    if (out->friendMaxScroll < 0) out->friendMaxScroll = 0;
    if (g_friendModal.listScrollY < 0) g_friendModal.listScrollY = 0;
    if (g_friendModal.listScrollY > out->friendMaxScroll) g_friendModal.listScrollY = out->friendMaxScroll;

    /* Scrollbar metrics */
    out->hasScrollbar = (out->friendContentH > out->friendListH && out->friendMaxScroll > 0);
    out->sbW = 4.0f * ui->densityScale;
    out->sbX = modX + modW - out->sbW - 4.0f * ui->densityScale;
    out->sbTrackY = out->friendListTop;
    out->sbTrackH = out->friendListH;
    out->sbThumbH = out->sbTrackH * (out->friendListH / out->friendContentH);
    if (out->sbThumbH < 20.0f * ui->densityScale) out->sbThumbH = 20.0f * ui->densityScale;
    out->sbThumbY = out->sbTrackY + (g_friendModal.listScrollY / out->friendMaxScroll) * (out->sbTrackH - out->sbThumbH);

    /* Bottom section */
    out->inputLabelY = out->friendListBottom + gap * 0.5f;
    out->inputX = modX + pad;
    out->addBtnW = 70.0f * ui->densityScale;
    out->addBtnH = btnH;
    out->addBtnX = modX + modW - pad - out->addBtnW;
    out->inputW = out->addBtnX - out->inputX - gap;
    out->inputY = out->inputLabelY + lineGap;
    out->textLeft = out->inputX + 8.0f * ui->densityScale;
    out->addBtnY = out->inputY + (out->inputH - out->addBtnH) * 0.5f;

    /* Close button */
    out->closeW = 100.0f * ui->densityScale;
    out->closeX = modX + modW * 0.5f - out->closeW * 0.5f;
    out->closeBtnY = modY + modH - bottomPad - btnH;
}

static void render_friend_modal(FtVulkanUI *ui, Vertex *vertices, uint32_t *count, uint32_t capacity,
                                 float screenW, float screenH, float scale, float glyphH) {
    UiDrawContext ctx = {vertices, count, capacity, screenW, screenH};
    FriendModalLayout fl;
    compute_friend_modal_layout(ui, &fl);
    float modX = fl.modX, modY = fl.modY, modW = fl.modW, modH = fl.modH;
    float textScale = fl.textScale;
    float lineGap = fl.lineGap;
    float pad = fl.pad;
    float itemH = fl.itemH;
    float btnH = fl.btnH;
    float gap = fl.gap;
    float bottomPad = fl.bottomPad;

    /* Dim overlay */
    ctx_rect(&ctx, 0, 0, screenW, screenH,
                0.0f, 0.0f, 0.0f);

    /* Modal background - pure black */
    ctx_rect(&ctx, modX, modY, modW, modH, 0.00f, 0.00f, 0.00f);

    int friend_count = fl.friend_count;
    FtUser **friends = ui->userTable ? ft_friend_list(ui->userTable, &friend_count) : NULL;

    /* Draw friend list items with clipping */
    if (friend_count > 0) {
        for (int i = 0; i < friend_count; i++) {
            float itemY = fl.friendListTop + i * fl.itemH - g_friendModal.listScrollY;
            /* Clip to list region */
            if (itemY + fl.itemH < fl.friendListTop || itemY > fl.friendListBottom) continue;

            FtUser *u = friends[i];

            /* Name */
            ctx_string(&ctx,
                        u->display_name[0] ? u->display_name : u->hash_hex,
                        modX + pad, itemY,
                        textScale, C_WHITE[0], C_WHITE[1], C_WHITE[2], false);

            /* Address (dimmed) */
            float hashY = itemY + glyphH + 2.0f;
            ctx_string(&ctx, u->hash_hex,
                        modX + pad, hashY,
                        textScale, 0.5f, 0.5f, 0.5f, false);

            /* Remove button */
            float remX = modX + modW - pad - 70.0f * ui->densityScale;
            float remW = 60.0f * ui->densityScale;
            float remH = 22.0f * ui->densityScale;
            float remY = itemY + (fl.itemH - remH) * 0.5f;

            /* Status: online, offline, or needs reciprocal friendship */
            bool has_active_peer = false;
            if (ui->net) {
                FtPeer *p = ft_net_find_peer_by_hash(ui->net, u->hash_hex);
                has_active_peer = (p && p->active);
            }
            bool is_reciprocal = u->friend_state == FT_FRIEND_RECIPROCAL;
            const char *status;
            float statusR, statusG, statusB;
            if (!is_reciprocal) {
                /* One-way friendship: always show the lowest relationship level,
                   even if we can temporarily reach their peer. */
                status = "Needs Reciprocal Friendship";
                statusR = C_NEON_RED[0]; statusG = C_NEON_RED[1]; statusB = C_NEON_RED[2];
            } else if (has_active_peer) {
                status = "Online Friend";
                statusR = C_NEON_GREEN[0]; statusG = C_NEON_GREEN[1]; statusB = C_NEON_GREEN[2];
            } else {
                status = "Offline Friend";
                statusR = C_ORANGE[0]; statusG = C_ORANGE[1]; statusB = C_ORANGE[2];
            }
            float statusW = string_width(status, textScale);
            float statusX = remX - gap - statusW;
            ctx_string(&ctx, status,
                        statusX, itemY + (fl.itemH - glyphH) * 0.5f,
                        textScale, statusR, statusG, statusB, false);
            ctx_border(&ctx, remX, remY, remW, remH,
                          1.0f * ui->densityScale,
                          C_PINK[0], C_PINK[1], C_PINK[2]);
            float remTW = string_width("Remove", textScale);
            ctx_string(&ctx, "Remove",
                        remX + (remW - remTW) * 0.5f, remY + (remH - glyphH) * 0.5f,
                        textScale, C_PINK[0], C_PINK[1], C_PINK[2], false);
        }

        /* Scrollbar */
        if (fl.hasScrollbar) {
            ctx_rect(&ctx, fl.sbX, fl.sbTrackY, fl.sbW, fl.sbTrackH, 0.08f, 0.08f, 0.08f);
            ctx_rect(&ctx, fl.sbX, fl.sbThumbY, fl.sbW, fl.sbThumbH, 0.35f, 0.35f, 0.35f);
        }
    }

    /* Clip masks: hide friend items that overflow the scroll region */
    float maskInset = 4.0f * ui->densityScale;
    ctx_rect(&ctx, modX + maskInset, modY, modW - maskInset * 2.0f, fl.friendListTop - modY, 0.00f, 0.00f, 0.00f);
    ctx_rect(&ctx, modX + maskInset, fl.friendListBottom, modW - maskInset * 2.0f, modY + modH - fl.friendListBottom, 0.00f, 0.00f, 0.00f);

    /* Modal border (drawn after masks so it stays visible) */
    ctx_border(&ctx, modX, modY, modW, modH,
                  2.0f * ui->densityScale,
                  C_NEON[0], C_NEON[1], C_NEON[2]);

    /* Title */
    float titleY = modY + 16.0f * ui->densityScale;
    ctx_string(&ctx, "Edit Friends",
                modX + (modW - string_width("Edit Friends", textScale)) * 0.5f, titleY,
                textScale, C_NEON[0], C_NEON[1], C_NEON[2], false);

    /* Explanatory label */
    float pathLabelY = titleY + lineGap * 1.3f;
    ctx_string(&ctx, "Friend list stored at:",
                modX + pad, pathLabelY,
                textScale, 0.5f, 0.5f, 0.5f, false);

    /* Peer cache file path (selectable) */
    if (ui->peerCachePath[0]) {
        const char *path = ui->peerCachePath;
        int pathLen = (int)strlen(path);

        /* Selection highlight */
        bool pathHasSel = g_friendModal.pathSelStart >= 0 && g_friendModal.pathSelEnd >= 0 &&
                          g_friendModal.pathSelStart != g_friendModal.pathSelEnd;
        if (pathHasSel) {
            int pLo = g_friendModal.pathSelStart;
            int pHi = g_friendModal.pathSelEnd;
            if (pLo > pHi) { int t = pLo; pLo = pHi; pHi = t; }
            if (pLo < pathLen && pHi > 0) {
                if (pLo < 0) pLo = 0;
                if (pHi > pathLen) pHi = pathLen;
                float sx1 = fl.pathX + string_width_n(path, pLo, textScale);
                float sx2 = fl.pathX + string_width_n(path, pHi, textScale);
                ctx_rect(&ctx, sx1, fl.pathY - 2.0f, sx2 - sx1, glyphH + 4.0f, 0.15f, 0.50f, 0.60f);
            }
        }

        /* Cursor when focused but no selection */
        if (g_friendModal.pathFocused && !pathHasSel && g_friendModal.pathSelStart >= 0) {
            float cx = fl.pathX + string_width_n(path, g_friendModal.pathSelStart, textScale);
            ctx_rect(&ctx, cx, fl.pathY - 1.0f, 1.5f * ui->densityScale, glyphH + 2.0f, C_NEON[0], C_NEON[1], C_NEON[2]);
        }

        float pathR = g_friendModal.pathFocused ? 1.0f : 0.5f;
        float pathG = g_friendModal.pathFocused ? 1.0f : 0.5f;
        float pathB = g_friendModal.pathFocused ? 1.0f : 0.5f;
        ctx_string(&ctx, path,
                    fl.pathX, fl.pathY,
                    textScale, pathR, pathG, pathB, false);
    }

    /* Bottom section: label, input, add button, preview, close */
    /* Label */
    ctx_string(&ctx, "Enter Ethereum address or sharing code:",
                fl.inputX, fl.inputLabelY,
                textScale, C_WHITE[0], C_WHITE[1], C_WHITE[2], false);

    ig_input_text("Hash", g_friendModal.hash_buf, sizeof(g_friendModal.hash_buf),
                  fl.inputX, fl.inputY, fl.inputW, fl.inputH,
                  fl.inputX + 8.0f, fl.inputY + 6.0f,
                  textScale, glyphH, glyphH + 4.0f, &ctx);

    render_modal_button(&ctx, fl.addBtnX, fl.addBtnY, fl.addBtnW, fl.addBtnH,
                        glyphH, textScale, "Add", C_PINK,
                        1.5f * ui->densityScale);

    /* Parsed friend preview */
    if (((int)strlen(g_friendModal.hash_buf)) > 0) {
        float previewY = fl.inputY + fl.inputH + gap * 0.5f;
        char previewLine[128];
        const char *previewName = g_friendModal.preview_valid
            ? g_friendModal.preview_name
            : "(invalid link)";
        const char *previewHash = g_friendModal.preview_valid
            ? g_friendModal.preview_hash
            : "(invalid link)";

        snprintf(previewLine, sizeof(previewLine), "Name: %s", previewName);
        ctx_string(&ctx, previewLine,
                    fl.inputX, previewY,
                    textScale, C_WHITE[0], C_WHITE[1], C_WHITE[2], false);
        previewY += lineGap;

        snprintf(previewLine, sizeof(previewLine), "Address: %s", previewHash);
        ctx_string(&ctx, previewLine,
                    fl.inputX, previewY,
                    textScale, C_WHITE[0], C_WHITE[1], C_WHITE[2], false);
    }

    render_modal_button(&ctx, fl.closeX, fl.closeBtnY, fl.closeW, btnH,
                        glyphH, textScale, "Close", C_PINK,
                        1.5f * ui->densityScale);
}


static void handle_friend_modal_click(FtVulkanUI *ui, double mx, double my) {
    FriendModalLayout fl;
    compute_friend_modal_layout(ui, &fl);
    float modX = fl.modX, modY = fl.modY, modW = fl.modW, modH = fl.modH;
    float textScale = fl.textScale;
    float glyphH = fl.glyphH;

    /* Path click / selection start */
    const char *path = ui->peerCachePath;
    int pathLen = (int)strlen(path);
    if (pathLen > 0 && mx >= fl.pathX && mx < fl.pathX + fl.pathW && my >= fl.pathY && my < fl.pathY + glyphH) {
        /* Triple-click detection: if already focused, select all */
        if (g_friendModal.pathFocused) {
            g_friendModal.pathSelStart = 0;
            g_friendModal.pathSelEnd = pathLen;
        } else {
            g_friendModal.pathFocused = true;
            ig_input_deactivate();
            g_friendModal.pathSelecting = true;
            float relX = (float)mx - fl.pathX;
            int pos = friend_modal_pos_from_x(path, pathLen, relX, textScale);
            g_friendModal.pathSelStart = pos;
            g_friendModal.pathSelEnd = pos;
        }
        return;
    }

    int friend_count = fl.friend_count;
    FtUser **friends = ui->userTable ? ft_friend_list(ui->userTable, &friend_count) : NULL;

    /* Scrollbar click */
    if (fl.hasScrollbar) {
        if (mx >= fl.sbX && mx < fl.sbX + fl.sbW && my >= fl.sbTrackY && my < fl.sbTrackY + fl.sbTrackH) {
            ScrollRegion sr = {
                &g_friendModal.listScrollY, fl.friendContentH, fl.friendListH,
                fl.friendListTop, fl.friendListH, fl.friendMaxScroll,
                fl.sbThumbY, fl.sbThumbH, fl.hasScrollbar
            };
            if (ft_ui_scroll_region_hit_thumb(&sr, (float)my)) {
                g_friendModal.listScrollbarDragging = true;
                g_friendModal.listScrollbarDragStartY = (float)my;
                g_friendModal.listScrollbarDragStartScrollY = g_friendModal.listScrollY;
            } else {
                ft_ui_scroll_region_page(&sr, (my < fl.sbThumbY) ? -1 : 1);
            }
            return;
        }
    }

    /* Check Remove buttons (friend list) */
    if (friends && friend_count > 0) {
        for (int i = 0; i < friend_count; i++) {
            float itemY = fl.friendListTop + i * fl.itemH - g_friendModal.listScrollY;
            if (itemY + fl.itemH < fl.friendListTop || itemY > fl.friendListBottom) continue;
            float remX = modX + modW - fl.pad - 70.0f * ui->densityScale;
            float remW = 60.0f * ui->densityScale;
            float remH = 22.0f * ui->densityScale;
            float remY = itemY + (fl.itemH - remH) * 0.5f;
            if (mx >= remX && mx < remX + remW && my >= remY && my < remY + remH) {
                /* Notify the friend that we removed them, if we have a peer for them */
                bool notified = false;
                if (ui->gossip && ui->net) {
                    for (size_t p = 0; p < ui->net->peer_count; p++) {
                        if (strncmp(ui->net->peers[p].hash_hex, friends[i]->hash_hex, 32) == 0) {
                            ui_log("[ui_friend_remove] sending friend_removed to %s at %s:%d\n",
                                    friends[i]->hash_hex,
                                    inet_ntoa(ui->net->peers[p].addr.sin_addr),
                                    ntohs(ui->net->peers[p].addr.sin_port));
                            ft_gossip_send_friend_removed(ui->gossip, &ui->net->peers[p].addr);
                            notified = true;
                        }
                    }
                }
                if (!notified) {
                    ui_log("[ui_friend_remove] NO peer found for %s, friend_removed notification not sent\n",
                            friends[i]->hash_hex);
                }
                /* Remove friend from all network notes where we are the creator */
                if (ui->identity) {
                    const char *friend_name = friends[i]->display_name;
                    const char *friend_hash = friends[i]->hash_hex;
                    for (int c = 0; c < ui->chatCount; c++) {
                        if (!ui->isNetwork[c]) continue;
                        if (ui->chatCreator[c][0] == '\0' ||
                            strncmp(ui->chatCreator[c], ui->identity->hash_hex, 32) != 0) {
                            continue;  /* only remove from notes we created */
                        }
                        /* Remove from document crypto member list */
                        if (ui->gossip && ui->gossip->key_manager) {
                            ft_key_manager_remove_member(ui->gossip->key_manager, c, friend_hash);
                        } else if (ui->chatCrypto[c]) {
                            ft_doc_crypto_remove_member(ui->chatCrypto[c], friend_hash);
                        }
                        /* Remove backtick-wrapped name from title */
                        if (friend_name[0] != '\0') {
                            title_remove_name(ui->chatDocs[c], friend_name, NULL);
                        }
                        /* Persist updated member list */
                        if (ui->chatDocs[c] && ui->chatCrypto[c]) {
                            ft_doc_save_meta(ui->chatDocs[c], ui->chatCrypto[c]);
                        }
                    }
                }
                ft_friend_remove(ui->userTable, friends[i]->hash_hex);
                save_user_table(ui);
                return;
            }
        }
    }

    /* Check input box */
    if (mx >= fl.inputX && mx < fl.inputX + fl.inputW && my >= fl.inputY && my < fl.inputY + fl.inputH) {
        g_friendModal.pathFocused = false;
        g_friendModal.pathSelStart = -1;
        g_friendModal.pathSelEnd = -1;
        ig_input_click((float)mx, fl.textLeft, textScale,
                       g_friendModal.hash_buf, sizeof(g_friendModal.hash_buf));
        return;
    } else {
        ig_input_release();
    }

    /* Check Add button */
    if (mx >= fl.addBtnX && mx < fl.addBtnX + fl.addBtnW && my >= fl.addBtnY && my < fl.addBtnY + fl.addBtnH) {
        if (((int)strlen(g_friendModal.hash_buf)) > 0 && ui->userTable && ui->identity) {
            char hash_hex[FT_HASH_HEX_LEN];
            char display_name[FT_DISPLAY_NAME_LEN];
            char pubkey_hex[FT_PUBKEY_HEX_LEN];
            if (ft_sharing_hash_decode(g_friendModal.hash_buf, hash_hex, display_name, sizeof(display_name), pubkey_hex, sizeof(pubkey_hex)) == 0) {
                if (strcmp(hash_hex, ui->identity->hash_hex) != 0) {
                    ft_friend_add(ui->userTable, hash_hex, pubkey_hex, display_name);
                    ui_log("[ui_friend_add] %s added. Waiting for reciprocity...\n", display_name[0] ? display_name : hash_hex);
                }
            } else if (strcmp(g_friendModal.hash_buf, ui->identity->hash_hex) != 0) {
                ft_friend_add(ui->userTable, g_friendModal.hash_buf, NULL, NULL);
                ui_log("[ui_friend_add] %s added. Waiting for reciprocity...\n", g_friendModal.hash_buf);
            }
            g_friendModal.hash_buf[0] = '\0';
            ig_input_deactivate();
            g_friendModal.preview_valid = false;
            g_friendModal.preview_name[0] = '\0';
            g_friendModal.preview_hash[0] = '\0';
            save_user_table(ui);
        }
        return;
    }

    /* Close button */
    if (mx >= fl.closeX && mx < fl.closeX + fl.closeW && my >= fl.closeBtnY && my < fl.closeBtnY + fl.btnH) {
        modal_close(&ui->modalManager, MODAL_FRIEND);
        g_friendModal.hash_buf[0] = '\0';
        ig_input_deactivate();
        g_friendModal.preview_valid = false;
        g_friendModal.preview_name[0] = '\0';
        g_friendModal.preview_hash[0] = '\0';
        g_friendModal.pathSelStart = -1;
        g_friendModal.pathSelEnd = -1;
        g_friendModal.pathFocused = false;
        return;
    }

    /* Click outside modal = close */
    if (mx < modX || mx >= modX + modW || my < modY || my >= modY + modH) {
        modal_close(&ui->modalManager, MODAL_FRIEND);
        g_friendModal.hash_buf[0] = '\0';
        ig_input_deactivate();
        g_friendModal.preview_valid = false;
        g_friendModal.preview_name[0] = '\0';
        g_friendModal.preview_hash[0] = '\0';
        g_friendModal.pathSelStart = -1;
        g_friendModal.pathSelEnd = -1;
        g_friendModal.pathFocused = false;
    }
}


void ft_vulkan_ui_render(FtVulkanUI *ui) {
    if (!ui->renderer || !ui->renderer->ready) return;

    sync_active_edit_identity(ui);

    /* Drain global log buffer (network/gossip/p2p layers) into network debug tab */
    {
        char drained[16][FT_LOG_BUF_LINE_LEN];
        int n = ft_log_buffer_drain(drained, 16);
        for (int i = 0; i < n; i++) {
            ui_debug_log_network(ui, "%s", drained[i]);
        }
    }

    /* Drain friend-state log buffer into friends debug tab */
    {
        char drained[16][FT_LOG_BUF_LINE_LEN];
        int n = ft_log_buffer_friend_drain(drained, 16);
        for (int i = 0; i < n; i++) {
            ui_debug_log_friend(ui, "%s", drained[i]);
        }
    }

    ui_update_blocks(ui);
    update_cursor_blink(ui);
    /* Immediate-mode: render every frame */

    float scale = 1.0f * ui->densityScale;
    float glyphW = g_fontAdvance * scale;
    float glyphH = g_fontLineHeight * scale;
    float lineH = glyphH + 8.0f;
    float screenW = (float)ui->renderer->swapchainExtent.width;
    float screenH = (float)ui->renderer->swapchainExtent.height;
    float margin = 20.0f * ui->densityScale;
    float statusBarH = 21.5f * ui->densityScale;
    float sidebarW = ui->sidebarWidth;

    static Vertex vertices[65536];
    uint32_t count = 0;
    uint32_t capacity = 65536;
    UiDrawContext ctx = {vertices, &count, capacity, screenW, screenH};

    /* Background fill */
    ctx_rect(&ctx, 0, 0, screenW, screenH, C_BG[0], C_BG[1], C_BG[2]);

    /* ── Sidebar ── (black background, neon outlines, with spacing & scroll) */
    SidebarLayout sl;
    compute_sidebar_layout(ui, &sl);
    if (ui->sidebarScrollY < 0) ui->sidebarScrollY = 0;
    if (ui->sidebarScrollY > sl.sidebarMaxScroll) ui->sidebarScrollY = sl.sidebarMaxScroll;

    /* Compute content width accounting for scrollbar */
    float sidebarContentW = sidebarW - 8.0f;
    if (sl.hasScrollbar) {
        sidebarContentW -= (sl.sbW + 4.0f);
    }

    update_chat_order(ui);
    for (int display_i = 0; display_i < ui->chatCount && count + 6 < capacity; display_i++) {
        int i = ui->chatOrder[display_i];
        float iy = sl.gap + sl.sidebarTopBtnH + display_i * sl.itemTotalH - ui->sidebarScrollY;
        /* Clip items completely outside visible area */
        if (iy + sl.itemH < sl.gap + sl.sidebarTopBtnH || iy > screenH - statusBarH) continue;
        bool isActive = (i == ui->activeChat);
        float borderThickness = isActive ? 2.0f : 1.0f;
        const float *borderColor = isActive ? C_NEON : C_SELECT;
        ctx_border(&ctx, 4.0f, iy, sidebarContentW, sl.itemH,
                      borderThickness,
                      borderColor[0], borderColor[1], borderColor[2]);
        float nameScale = 1.0f;
        float textX = 12.0f;
        float textY = iy + 6.0f;
        float textMaxW = sidebarW - textX - 4.0f;
        if (sl.hasScrollbar) {
            textMaxW -= (sl.sbW + 6.0f);
        }

        /* Get live title from document */
        const char *displayName = ui->chatNames[i];
        char titleBuf[128];
        FtDocument *chatDoc = ui->chatDocs[i];
        if (chatDoc && chatDoc->text && chatDoc->len > 0) {
            size_t tlen = 0;
            for (size_t j = 0; j < chatDoc->len && tlen < sizeof(titleBuf) - 1; j++) {
                if (chatDoc->text[j] == '\n') break;
                if (chatDoc->text[j] == '`') continue;
                titleBuf[tlen++] = chatDoc->text[j];
            }
            titleBuf[tlen] = '\0';
            if (tlen > 0) displayName = titleBuf;
        }

        /* Wrap up to two lines; truncate with ellipsis if it overflows */
        int textMaxLines = 2;
        int linesNeeded = measure_wrapped_lines(displayName, nameScale, textMaxW);
        if (linesNeeded <= textMaxLines) {
            ctx_wrapped_text(&ctx, displayName, textX, textY,
                              nameScale, textMaxW,
                              isActive ? C_NEON[0] : C_WHITE[0],
                              isActive ? C_NEON[1] : C_WHITE[1],
                              isActive ? C_NEON[2] : C_WHITE[2],
                              false);
        } else {
            size_t best_pos = 0;
            const char *p = displayName;
            while (*p) {
                const char *wordEnd = p;
                while (*wordEnd && *wordEnd != ' ' && *wordEnd != '\n') wordEnd++;
                size_t pos = (size_t)(wordEnd - displayName);

                char candidate[128];
                size_t copyLen = pos;
                if (copyLen > sizeof(candidate) - 4) copyLen = sizeof(candidate) - 4;
                strncpy(candidate, displayName, copyLen);
                candidate[copyLen] = '\0';
                strcat(candidate, "...");

                int cand_lines = measure_wrapped_lines(candidate, nameScale, textMaxW);
                if (cand_lines <= textMaxLines) {
                    best_pos = pos;
                }

                if (*wordEnd == ' ' || *wordEnd == '\n') {
                    p = wordEnd + 1;
                } else {
                    p = wordEnd;
                }
            }

            if (best_pos > 0) {
                char ellip[128];
                size_t copyLen = best_pos;
                if (copyLen > sizeof(ellip) - 4) copyLen = sizeof(ellip) - 4;
                strncpy(ellip, displayName, copyLen);
                ellip[copyLen] = '\0';
                strcat(ellip, "...");
                ctx_wrapped_text(&ctx, ellip, textX, textY,
                                  nameScale, textMaxW,
                                  isActive ? C_NEON[0] : C_WHITE[0],
                                  isActive ? C_NEON[1] : C_WHITE[1],
                                  isActive ? C_NEON[2] : C_WHITE[2],
                                  false);
            } else {
                float ellipsisW = string_width("...", nameScale);
                if (ellipsisW <= textMaxW) {
                    ctx_string(&ctx, "...", textX, textY,
                                nameScale,
                                isActive ? C_NEON[0] : C_WHITE[0],
                                isActive ? C_NEON[1] : C_WHITE[1],
                                isActive ? C_NEON[2] : C_WHITE[2],
                                false);
                }
            }
        }

    }

    /* ── New Note button (fixed at top of sidebar, drawn AFTER items so it covers scrolled items) ── */
    if (ui->chatCount < FT_UI_MAX_CHATS && count + 30 < capacity) {
        float btnY = sl.gap;
        ctx_rect(&ctx, 4.0f, btnY, sidebarContentW, sl.btnH, C_BG[0], C_BG[1], C_BG[2]);
        ctx_border(&ctx, 4.0f, btnY, sidebarContentW, sl.btnH, 2.0f, C_PINK[0], C_PINK[1], C_PINK[2]);
        float textW = string_width("+ New Note", scale);
        float textX = 4.0f + (sidebarContentW - textW) * 0.5f;
        float textY = btnY + (sl.btnH - glyphH) * 0.5f;
        ctx_string(&ctx, "+ New Note",
                    textX, textY,
                    scale,
                    C_PINK[0], C_PINK[1], C_PINK[2], false);
    }

    /* Sidebar scrollbar */
    if (sl.hasScrollbar && count + 12 < capacity) {
        ScrollRegion sr = {
            &ui->sidebarScrollY, sl.sidebarContentH, sl.sidebarVisibleH,
            sl.gap + sl.sidebarTopBtnH, sl.sidebarVisibleH, 0, 0, 0, false
        };
        ft_ui_scroll_region_compute(&sr, ui->densityScale);
        float sbX = sidebarW - sl.sbW - 2.0f;
        ctx_rect(&ctx, sbX, sr.trackY, sl.sbW, sr.trackH, 0.00f, 0.00f, 0.00f);
        ctx_rect(&ctx, sbX, sr.thumbY, sl.sbW, sr.thumbH, 0.15f, 0.50f, 0.60f);
    }

    float contentX = sidebarW + margin;
    float scrollbarW = 8.0f * ui->densityScale;
    bool showRightPanel = compute_show_right_panel(ui);
    float rightPanelW = ui->sidebarWidth;
    float contentW = screenW - sidebarW - margin - scrollbarW - (showRightPanel ? rightPanelW : 0.0f);
    if (contentW < 120.0f) contentW = 120.0f;

    /* Document text rendering — title is first line, body is rest */
    FtDocument *doc = ui->doc;
    FtTextEdit *ed = ui->edit;
    if (doc && doc->text) {
        DocumentLayout dl;
        compute_document_layout(ui, &dl);

        FtDisplayList displayList;
        ft_display_list_init(&displayList);
        ft_display_list_build_document(&displayList, &dl, doc, ed, ui->scrollY, ui->cursorBlink);

        int dlCount = displayList.count;
        for (int i = 0; i < displayList.count && count + 24 < capacity; i++) {
            const FtDisplayListCmd *c = &displayList.cmds[i];
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
                case FT_DL_REMOTE_CURSOR:
                    append_rect(vertices, &count, c->x, c->y, c->w, c->h,
                                screenW, screenH, false, c->r, c->g, c->b);
                    /* Invert text under remote cursor to black */
                    if (doc && c->u.remote_cursor.text_len > 0 &&
                        c->u.remote_cursor.text_start + c->u.remote_cursor.text_len <= doc->len) {
                        draw_string_n(vertices, &count, capacity,
                                      doc->text + c->u.remote_cursor.text_start,
                                      (int)c->u.remote_cursor.text_len,
                                      c->x, c->y, c->u.remote_cursor.text_scale,
                                      screenW, screenH, false,
                                      0.0f, 0.0f, 0.0f, false);
                    }
                    if (c->u.remote_cursor.name && c->u.remote_cursor.name[0]) {
                        float labelScale = dl.scale; /* same size as body text */
                        float labelW = string_width(c->u.remote_cursor.name, labelScale);
                        float labelH = g_fontLineHeight * labelScale;
                        float padX = 6.0f * ui->densityScale;
                        float padY = 3.0f * ui->densityScale;
                        float boxW = labelW + padX * 2.0f;
                        float boxH = labelH + padY * 2.0f;
                        float arrowH = 5.0f * ui->densityScale;
                        float arrowW = 8.0f * ui->densityScale;
                        float gap = 3.0f * ui->densityScale;
                        float itemH = boxH + arrowH + gap;
                        float stackOffset = c->u.remote_cursor.stack_index * itemH;

                        float arrowTipX = c->x + c->w * 0.5f;
                        float arrowTipY = c->y + c->h + gap + stackOffset;
                        float arrowBaseY = arrowTipY + arrowH;
                        float arrowHalfW = arrowW * 0.5f;

                        /* Box starts so arrow sits on far left, tooltip flows right */
                        float boxX = arrowTipX - arrowHalfW;
                        float boxY = arrowBaseY;

                        /* Clamp to editor content bounds */
                        float minX = dl.contentX;
                        float maxX = dl.contentX + dl.contentW - boxW;
                        if (boxX < minX) boxX = minX;
                        if (boxX > maxX) boxX = maxX;
                        float maxY = dl.screenH - dl.statusBarH - boxH - arrowH;
                        if (boxY > maxY) boxY = maxY;

                        float arrowBaseLeftX = boxX;
                        float arrowBaseRightX = boxX + arrowW;
                        /* Keep arrow tip inside arrow base so it always points to cursor */
                        if (arrowTipX < arrowBaseLeftX || arrowTipX > arrowBaseRightX) {
                            arrowBaseLeftX = arrowTipX - arrowHalfW;
                            arrowBaseRightX = arrowTipX + arrowHalfW;
                        }

                        /* Neon blue box background */
                        append_rect(vertices, &count, boxX, boxY, boxW, boxH,
                                    screenW, screenH, false, C_NEON[0], C_NEON[1], C_NEON[2]);
                        /* Upward arrow */
                        append_triangle(vertices, &count,
                                        arrowBaseLeftX, arrowBaseY,
                                        arrowBaseRightX, arrowBaseY,
                                        arrowTipX, arrowTipY,
                                        screenW, screenH, false, C_NEON[0], C_NEON[1], C_NEON[2]);
                        /* Name text in black */
                        draw_string(vertices, &count, capacity,
                                    c->u.remote_cursor.name,
                                    boxX + padX, boxY + padY, labelScale,
                                    screenW, screenH, false,
                                    0.0f, 0.0f, 0.0f, false);
                    }
                    break;
            }
        }
        ft_display_list_free(&displayList);
        ui_debug_log_auto(ui, "[render] doc_lines=%d net=%d", dlCount, ed ? (int)ed->is_network : 0);
    }

    /* ── Delete button (top-right of editor) ── */
    {
        g_deleteBtn.visible = (ui->chatCount > 0 && ui->activeChat >= 0 && ui->activeChat < ui->chatCount);
        if (g_deleteBtn.visible && count + 6 < capacity) {
            float delScale = 1.2f * ui->densityScale;
            const char *delLabel = "X";
            float delW = string_width(delLabel, delScale);
            float delH = g_fontLineHeight * delScale;
            float delRightMargin = 20.0f * ui->densityScale;
            float delTopMargin = 2.0f * ui->densityScale;
            float delHitPad = 6.0f * ui->densityScale;
            float delX = contentX + contentW - delW - delRightMargin;
            float delY = margin - ui->scrollY + delTopMargin;
            /* Don't let it overlap the scrollbar */
            float scrollbarW = 8.0f * ui->densityScale;
            float maxDelX = screenW - scrollbarW - delW - delRightMargin - (showRightPanel ? rightPanelW : 0.0f);
            if (delX > maxDelX) delX = maxDelX;
            /* Only show when scrolled near top */
            if (ui->scrollY < delH + delTopMargin + delHitPad * 2.0f) {
                g_deleteBtn.x = delX - delHitPad;
                g_deleteBtn.y = delY - delHitPad;
                g_deleteBtn.w = delW + delHitPad * 2.0f;
                g_deleteBtn.h = delH + delHitPad * 2.0f;
                ctx_rect(&ctx, g_deleteBtn.x, g_deleteBtn.y, g_deleteBtn.w, g_deleteBtn.h,
                         C_BG[0], C_BG[1], C_BG[2]);
                ctx_string(&ctx, delLabel, delX, delY, delScale,
                           C_PINK[0], C_PINK[1], C_PINK[2], false);
            } else {
                g_deleteBtn.visible = false;
            }
        }
    }

    /* ── Scrollbar ── */
    {
        float totalH = measure_document_height(ui);
        float visibleH = screenH - statusBarH - margin;
        ScrollRegion sr = {
            &ui->scrollY, totalH, visibleH,
            margin, visibleH, 0, 0, 0, false
        };
        ft_ui_scroll_region_compute(&sr, ui->densityScale);
        if (sr.hasScrollbar && count + 12 < capacity) {
            float scrollbarW = 8.0f * ui->densityScale;
            float scrollbarX = screenW - scrollbarW - (showRightPanel ? rightPanelW : 0.0f);
            /* Track */
            ctx_rect(&ctx, scrollbarX, sr.trackY, scrollbarW, sr.trackH, 0.00f, 0.00f, 0.00f);
            /* Thumb */
            ctx_rect(&ctx, scrollbarX, sr.thumbY, scrollbarW, sr.thumbH, 0.15f, 0.50f, 0.60f);
        }
    }

    /* Status bar (hidden for local notes when startup is complete) */
    bool showStatusBar = true;
    if (ft_vulkan_ui_startup_done(ui) && ui->chatCount > 0 && !ui->isNetwork[ui->activeChat]) {
        showStatusBar = false;
    }
    if (showStatusBar) {
        float statusY = screenH - statusBarH;
        ctx_rect(&ctx, 0, statusY, screenW, statusBarH,
                    C_STATUS_BG[0], C_STATUS_BG[1], C_STATUS_BG[2]);

        float statusTextScale = scale;
        float statusGlyphH = g_fontLineHeight * statusTextScale;
        float textBaselineY = statusY + (statusBarH - statusGlyphH) * 0.5f;

        /* Left side: User's full name (clickable to edit) */
        char fullName[128];
        get_full_name(ui, fullName, sizeof(fullName));
        float nameLabelW = string_width("Name: ", statusTextScale);
        float nameW = string_width(fullName, statusTextScale);
        ctx_string(&ctx, "Name: ", margin, textBaselineY,
                    statusTextScale,
                    C_DIM[0], C_DIM[1], C_DIM[2], false);
        ctx_string(&ctx, fullName,
                    margin + nameLabelW, textBaselineY,
                    statusTextScale,
                    C_NEON[0], C_NEON[1], C_NEON[2], false);
        /* Underline to indicate clickability */
        ctx_rect(&ctx, margin + nameLabelW, textBaselineY + statusGlyphH,
                    nameW, 1.0f * ui->densityScale, C_NEON[0], C_NEON[1], C_NEON[2]);
        /* Hit box for name click (covers both label and name) */
        ui->statusBarLayout.name_hit_x = margin;
        ui->statusBarLayout.name_hit_y = textBaselineY;
        ui->statusBarLayout.name_hit_w = nameLabelW + nameW;
        ui->statusBarLayout.name_hit_h = statusGlyphH;

        /* Center: Network status */
        const char *netStatusText = NULL;
        const float *netStatusColor = C_DIM;
        bool is_connecting = (ui->networkState &&
            (ui->networkState->balance_status == FT_NET_STATUS_PENDING ||
             ui->networkState->register_status == FT_NET_STATUS_PENDING));
        if (is_connecting) {
            netStatusText = "Connecting...";
            netStatusColor = C_ORANGE;
        } else if (ft_vulkan_ui_startup_online(ui)) {
            netStatusText = "Connected";
            netStatusColor = C_NEON;
        }
        const char *sharing_address = get_sharing_address(ui->identity);
        if (netStatusText) {
            float netW = string_width(netStatusText, statusTextScale);
            float nameEndX = margin + nameLabelW + nameW;
            float shareStartX = sharing_address[0] ?
                (screenW - margin - string_width("Friend Link: ", statusTextScale)
                 - string_width(sharing_address, statusTextScale)) :
                (screenW - margin);
            float netX = (nameEndX + shareStartX) * 0.5f - netW * 0.5f;
            if (netX < nameEndX + 10.0f) netX = nameEndX + 10.0f;
            ctx_string(&ctx, netStatusText,
                        netX, textBaselineY,
                        statusTextScale,
                        netStatusColor[0], netStatusColor[1], netStatusColor[2], false);
        }

        /* Right side: Friend Link (clickable) */
        if (sharing_address[0]) {
            const char *linkLabel = "Friend Link: ";
            float shareLabelW = string_width(linkLabel, statusTextScale);
            float shareAddrW = string_width(sharing_address, statusTextScale);
            float totalShareW = shareLabelW + shareAddrW;
            float shareX = screenW - margin - totalShareW;
            ctx_string(&ctx, linkLabel, shareX, textBaselineY,
                        statusTextScale,
                        C_DIM[0], C_DIM[1], C_DIM[2], false);
            /* Highlight background when selected */
            if (g_startup.sharing_highlighted) {
                ctx_rect(&ctx,
                            shareX + shareLabelW - 2.0f * ui->densityScale,
                            textBaselineY - 2.0f * ui->densityScale,
                            shareAddrW + 4.0f * ui->densityScale,
                            statusGlyphH + 4.0f * ui->densityScale,
                            0.15f, 0.55f, 0.55f);
            }
            ctx_string(&ctx, sharing_address,
                        shareX + shareLabelW, textBaselineY,
                        statusTextScale,
                        C_NEON[0], C_NEON[1], C_NEON[2], false);
            /* Underline to indicate clickability */
            ctx_rect(&ctx, shareX + shareLabelW, textBaselineY + statusGlyphH,
                        shareAddrW, 1.0f * ui->densityScale, C_NEON[0], C_NEON[1], C_NEON[2]);
            /* Hit box for click handling */
            ui->statusBarLayout.sharing_hit_x = shareX + shareLabelW;
            ui->statusBarLayout.sharing_hit_y = textBaselineY;
            ui->statusBarLayout.sharing_hit_w = shareAddrW;
            ui->statusBarLayout.sharing_hit_h = statusGlyphH;
        } else {
            ui->statusBarLayout.sharing_hit_w = 0;
        }
    } else {
        ui->statusBarLayout.name_hit_w = 0;
        ui->statusBarLayout.sharing_hit_w = 0;
    }

    /* ── Right Friends Panel ── */
    if (showRightPanel && count + 200 < capacity) {
        RightPanelLayout rpl;
        compute_right_panel_layout(ui, &rpl);
        int friend_count = 0;
        FtUser **friends = ui->userTable ? ft_friend_list(ui->userTable, &friend_count) : NULL;

        ScrollRegion sr = {
            &ui->rightPanelScrollY, rpl.friendContentH, rpl.friendListVisibleH,
            rpl.friendListTop, rpl.friendListVisibleH, 0, 0, 0, false
        };
        ft_ui_scroll_region_compute(&sr, ui->densityScale);

        /* Background */
        ctx_rect(&ctx, screenW - rightPanelW, 0, rightPanelW, screenH - statusBarH, C_BG[0], C_BG[1], C_BG[2]);

        /* Friend list */
        for (int i = 0; i < friend_count && count + 6 < capacity; i++) {
            float fy = rpl.friendListTop + i * rpl.friendItemH - ui->rightPanelScrollY;
            if (fy + rpl.friendItemH < rpl.friendListTop || fy > rpl.friendListTop + rpl.friendListVisibleH) continue;
            bool is_online = false;
            if (ui->net) {
                FtPeer *p = ft_net_find_peer_by_hash(ui->net, friends[i]->hash_hex);
                is_online = (p && p->active);
            }
            const float *fcolor;
            if (friends[i]->friend_state != FT_FRIEND_RECIPROCAL) {
                fcolor = C_NEON_RED;                   /* one-way: needs reciprocal */
            } else if (is_online) {
                fcolor = C_NEON_GREEN;                 /* online and reciprocal */
            } else {
                fcolor = C_ORANGE;                     /* offline but reciprocal */
            }
            const char *name = friends[i]->display_name[0] ? friends[i]->display_name : friends[i]->hash_hex;
            float nameX = screenW - rightPanelW + 12.0f;
            float maxNameW = rightPanelW - 24.0f - (rpl.hasScrollbar ? (rpl.sbW + 4.0f) : 0.0f);
            if (string_width(name, scale) > maxNameW) {
                int maxChars = (int)strlen(name);
                float ellipsisW = string_width("...", scale);
                while (maxChars > 0 && string_width_n(name, maxChars, scale) + ellipsisW > maxNameW) {
                    maxChars--;
                }
                char truncated[64];
                if (maxChars > 0) {
                    strncpy(truncated, name, maxChars);
                    truncated[maxChars] = '\0';
                    strcat(truncated, "...");
                } else {
                    strcpy(truncated, "...");
                }
                ctx_string(&ctx, truncated, nameX, fy, scale, fcolor[0], fcolor[1], fcolor[2], false);
            } else {
                ctx_string(&ctx, name, nameX, fy, scale, fcolor[0], fcolor[1], fcolor[2], false);
            }
        }

        /* Fixed Edit Friends button (at top, drawn AFTER friends so it covers scrolled friends) */
        float addBtnY = rpl.friendGap;
        float addBtnX = screenW - rightPanelW + 4.0f;
        float addBtnW = rightPanelW - 8.0f - (rpl.hasScrollbar ? (rpl.sbW + 4.0f) : 0.0f);
        ctx_rect(&ctx, addBtnX, addBtnY, addBtnW, rpl.friendBtnH, C_BG[0], C_BG[1], C_BG[2]);
        ctx_border(&ctx, addBtnX, addBtnY, addBtnW, rpl.friendBtnH, 2.0f, C_PINK[0], C_PINK[1], C_PINK[2]);
        float addTextW = string_width("+ Edit Friends", scale);
        float addTextX = addBtnX + (addBtnW - addTextW) * 0.5f;
        float addTextY = addBtnY + (rpl.friendBtnH - glyphH) * 0.5f;
        ctx_string(&ctx, "+ Edit Friends",
                    addTextX, addTextY,
                    scale,
                    C_PINK[0], C_PINK[1], C_PINK[2], false);

        /* Right panel scrollbar */
        if (rpl.hasScrollbar && count + 12 < capacity) {
            float sbX = screenW - rpl.sbW - 2.0f;
            ctx_rect(&ctx, sbX, sr.trackY, rpl.sbW, sr.trackH, 0.00f, 0.00f, 0.00f);
            ctx_rect(&ctx, sbX, sr.thumbY, rpl.sbW, sr.thumbH, 0.15f, 0.50f, 0.60f);
        }
    }

    /* ── Autocomplete dropdown ── */
    if (ui->autocomplete && ft_autocomplete_is_active(ui->autocomplete) && count + 30 < capacity) {
        compute_autocomplete_layout(ui);
        if (ui->autocompleteLayout.valid) {
            float dropX = ui->autocompleteLayout.dropX;
            float dropY = ui->autocompleteLayout.dropY;
            float dropW = ui->autocompleteLayout.dropW;
            float dropH = ui->autocompleteLayout.dropH;
            float itemH = ui->autocompleteLayout.itemH;

            const FtAutocompleteResult *res = ft_autocomplete_get_results(ui->autocomplete);
            int total_users = res ? res->count : 0;
            int selected = ft_autocomplete_get_selected_index(ui->autocomplete);
            if (total_users > 0) {
                /* Solid black background + neon border */
                ctx_rect(&ctx, dropX, dropY, dropW, dropH, 0.0f, 0.0f, 0.0f);
                ctx_border(&ctx, dropX, dropY, dropW, dropH, 1.0f, C_NEON[0], C_NEON[1], C_NEON[2]);

                /* Items */
                for (int u = 0; u < total_users && count + 6 < capacity; u++) {
                    const char *uname = res->names[u];
                    float uy = dropY + 4.0f + u * itemH;
                    if (u == selected && count + 12 < capacity) {
                        ctx_border(&ctx, dropX + 2.0f, uy, dropW - 4.0f, itemH,
                                      1.0f,
                                      C_NEON[0], C_NEON[1], C_NEON[2]);
                    }
                    ctx_string(&ctx, uname, dropX + 8.0f, uy + 2.0f,
                                scale * 0.8f,
                                (u == selected) ? C_NEON[0] : C_WHITE[0],
                                (u == selected) ? C_NEON[1] : C_WHITE[1],
                                (u == selected) ? C_NEON[2] : C_WHITE[2],
                                false);
                }
            }
        }
    }

    /* ── Modal rendering ── */
    switch (modal_top(&ui->modalManager)) {
        case MODAL_DELETE:
            if (count + 60 < capacity) {
                int del_idx = g_deleteModal_chatToDelete;
                bool is_net = (del_idx >= 0 && del_idx < ui->chatCount && ui->isNetwork[del_idx]);
                DeleteModalLayout dl;
                compute_delete_modal_layout(ui, &dl, is_net);

                render_modal_dim(&ctx, screenW, screenH);
                render_modal_frame(&ctx, dl.modX, dl.modY, dl.modW, dl.modH,
                                   ui->densityScale, C_PINK);

                ctx_string(&ctx, "Delete this note?", dl.msgX, dl.msgY,
                            dl.msgScale,
                            C_WHITE[0], C_WHITE[1], C_WHITE[2], false);

                if (dl.is_network) {
                    float warnLineH = g_fontLineHeight * dl.warnScale;
                    ctx_string(&ctx, "This will remove the note ONLY from your",
                               dl.warnX, dl.warnY,
                               dl.warnScale, C_ORANGE[0], C_ORANGE[1], C_ORANGE[2], false);
                    ctx_string(&ctx, "machine and NOT other people you have",
                               dl.warnX, dl.warnY + warnLineH,
                               dl.warnScale, C_ORANGE[0], C_ORANGE[1], C_ORANGE[2], false);
                    ctx_string(&ctx, "shared it with. Do you want to continue?",
                               dl.warnX, dl.warnY + warnLineH * 2.0f,
                               dl.warnScale, C_ORANGE[0], C_ORANGE[1], C_ORANGE[2], false);
                }

                render_modal_button(&ctx, dl.yesX, dl.yesY, dl.yesW, dl.yesH,
                                    glyphH, dl.msgScale, "Yes", C_PINK, 1.5f);
                render_modal_button(&ctx, dl.noX, dl.noY, dl.noW, dl.noH,
                                    glyphH, dl.msgScale, "No", C_NEON, 1.5f);
            }
            break;
        case MODAL_NEW_NOTE:
            if (count + 60 < capacity) {
                render_new_note_modal(ui, vertices, &count, capacity, screenW, screenH, scale, glyphH);
            }
            break;
        case MODAL_FRIEND:
            if (count + 200 < capacity) {
                compute_friend_modal_layout(ui, &g_friendModal.layout);
                render_friend_modal(ui, vertices, &count, capacity, screenW, screenH, scale, glyphH);
            }
            break;
        case MODAL_STARTUP:
            if (count + 60 < capacity) {
                render_modal_dim(&ctx, screenW, screenH);
                render_startup_modal(ui, vertices, &count, capacity, screenW, screenH, scale, glyphH);
            }
            break;
        case MODAL_UPNP_WARNING:
            if (count + 60 < capacity) {
                UpnpWarningModalLayout ul;
                compute_upnp_warning_modal_layout(ui, &ul);

                render_modal_dim(&ctx, screenW, screenH);
                render_modal_frame(&ctx, ul.modX, ul.modY, ul.modW, ul.modH,
                                   ui->densityScale, C_ORANGE);

                ctx_string(&ctx, "Connection with UPnP failed.", ul.modX + 24.0f * ui->densityScale, ul.line1Y,
                           ul.textScale, C_WHITE[0], C_WHITE[1], C_WHITE[2], false);
                ctx_string(&ctx, "Falling back to api.ipify.org for public IP.", ul.modX + 24.0f * ui->densityScale, ul.line2Y,
                           ul.textScale, C_WHITE[0], C_WHITE[1], C_WHITE[2], false);
                ctx_string(&ctx, "This may be untrustworthy.", ul.modX + 24.0f * ui->densityScale, ul.line3Y,
                           ul.textScale, C_ORANGE[0], C_ORANGE[1], C_ORANGE[2], false);
                ctx_string(&ctx, "Enable UPnP on your router for better privacy.", ul.modX + 24.0f * ui->densityScale, ul.line4Y,
                           ul.textScale, C_DIM[0], C_DIM[1], C_DIM[2], false);

                render_modal_button(&ctx, ul.okX, ul.okY, ul.okW, ul.okH,
                                    glyphH, ul.textScale, "OK", C_ORANGE, 1.5f);
            }
            break;
        default:
            break;
    }

    /* ── Debug Panel ── */
    if (ui->showDebugPanel && count + 200 < capacity) {
        DebugPanelLayout dl;
        if (get_debug_panel_layout(ui, &dl)) {
            /* Background */
            ctx_rect(&ctx, 0, dl.panelY, dl.panelW, dl.panelH, 0.05f, 0.05f, 0.05f);
            ctx_border(&ctx, 0, dl.panelY, dl.panelW, dl.panelH, 1.0f, C_NEON[0], C_NEON[1], C_NEON[2]);

            /* Tabs */
            const char *tabLabels[4] = {"Editor", "Auto", "Network", "Friends"};
            float tabX = 8.0f;
            float mx = (float)ui->mouseX;
            float my = (float)ui->mouseY;
            for (int t = 0; t < 4; t++) {
                float tabW = string_width(tabLabels[t], dl.dbgScale) + 16.0f;
                bool active = (ui->debugPanelActiveTab == t);
                float tabY = dl.tabBarY + 2.0f;
                float tabH = dl.tabBarH - 4.0f;
                bool hovered = (mx >= tabX && mx < tabX + tabW && my >= tabY && my < tabY + tabH);
                /* Tab background: brighter on hover, dim when inactive */
                float bg = active ? 0.20f : (hovered ? 0.12f : 0.05f);
                ctx_rect(&ctx, tabX, tabY, tabW, tabH, bg, bg, bg);
                /* Tab border: neon when active/hovered */
                float br = active ? C_NEON[0] : (hovered ? C_HOVER[0] : C_DIM[0]);
                float bg_ = active ? C_NEON[1] : (hovered ? C_HOVER[1] : C_DIM[1]);
                float bb = active ? C_NEON[2] : (hovered ? C_HOVER[2] : C_DIM[2]);
                ctx_border(&ctx, tabX, tabY, tabW, tabH, 1.0f, br, bg_, bb);
                /* Active tab "connects" to content by drawing over the panel top border */
                if (active) {
                    ctx_rect(&ctx, tabX + 1.0f, tabY + tabH - 1.0f, tabW - 2.0f, 2.0f, bg, bg, bg);
                }
                /* Tab text */
                ctx_string(&ctx, tabLabels[t], tabX + 8.0f, tabY + 2.0f, dl.dbgScale, br, bg_, bb, false);
                tabX += tabW + 4.0f;
            }

            /* Log lines */
            for (int i = dl.startLine; i < dl.totalLines && count + 6 < capacity; i++) {
                int idx = i % FT_UI_DEBUG_LOG_LINES;
                float ly = dl.textY + (i - dl.startLine) * dl.dbgLineH;
                if (ly + g_fontLineHeight * dl.dbgScale > dl.panelY + dl.panelH - 4.0f) break;

                /* Selection background */
                if (ui->debugHasSelection && i >= ui->debugSelStartLine && i <= ui->debugSelEndLine) {
                    const char *text = dl.logBuf[idx];
                    int len = (int)strlen(text);
                    int selLo = 0, selHi = len;
                    if (i == ui->debugSelStartLine) selLo = ui->debugSelStartCol;
                    if (i == ui->debugSelEndLine) selHi = ui->debugSelEndCol;
                    if (selLo < 0) selLo = 0;
                    if (selHi > len) selHi = len;
                    if (selLo < selHi) {
                        float sx = 8.0f + string_width_n(text, selLo, dl.dbgScale);
                        float sw = string_width_n(text + selLo, selHi - selLo, dl.dbgScale);
                        ctx_rect(&ctx, sx, ly, sw, g_fontLineHeight * dl.dbgScale,
                                 C_SELECT[0], C_SELECT[1], C_SELECT[2]);
                    }
                }

                ctx_string(&ctx, dl.logBuf[idx], 8.0f, ly, dl.dbgScale,
                           C_DIM[0], C_DIM[1], C_DIM[2], false);
            }

            /* Debug panel scrollbar */
            float contentH = dl.totalLines * dl.dbgLineH;
            float visibleH = dl.maxVisibleLines * dl.dbgLineH;
            if (contentH > visibleH && count + 12 < capacity) {
                float sbW = 8.0f * ui->densityScale;
                float sbX = dl.panelW - sbW - 2.0f;
                ScrollRegion sr = {
                    dl.scrollY, contentH, visibleH,
                    dl.textY, visibleH, 0, 0, 0, false
                };
                ft_ui_scroll_region_compute(&sr, ui->densityScale);
                ctx_rect(&ctx, sbX, sr.trackY, sbW, sr.trackH, 0.00f, 0.00f, 0.00f);
                ctx_rect(&ctx, sbX, sr.thumbY, sbW, sr.thumbH, 0.15f, 0.50f, 0.60f);
            }
        }
    }

    /* Send cursor position for network notes */
    if (ed && ed->is_network && ui->gossip && ui->activeChat >= 0) {
        size_t pos = ed->cursor;
#ifdef _WIN32
        DWORD now_ms = GetTickCount();
        double now = (double)now_ms;
#else
        double now = 0;
#endif
        if (pos != ui->lastCursorSentPos || now - ui->lastCursorSendTime > 500.0) {
            int doc_idx = ui->activeChat;
            ft_gossip_send_cursor(ui->gossip, doc_idx, pos);
            ui->lastCursorSentPos = pos;
            ui->lastCursorSendTime = now;
        }
    }

    ft_vk_renderer_draw(ui->renderer, vertices, count);
}

static bool title_has_name(FtDocument *doc, const char *name) {
    size_t title_end = get_title_end(doc);
    size_t name_len = strlen(name);
    const char *p = doc->text;
    const char *end = doc->text + title_end;
    while (p < end) {
        if (*p == '`') {
            const char *name_start = p + 1;
            const char *name_end = name_start;
            while (name_end < end && *name_end != '`') name_end++;
            if (name_end >= end) break;
            size_t token_len = (size_t)(name_end - name_start);
            if (token_len == name_len && strncmp(name_start, name, name_len) == 0)
                return true;
            p = name_end + 1;
        } else {
            p++;
        }
    }
    return false;
}

static void title_add_name(FtDocument *doc, const char *name) {
    if (title_has_name(doc, name)) return;
    size_t title_end = get_title_end(doc);
    char wrapped[128];
    size_t name_len = strlen(name);
    if (name_len + 2 >= sizeof(wrapped)) return;
    wrapped[0] = '`';
    memcpy(wrapped + 1, name, name_len);
    wrapped[name_len + 1] = '`';
    wrapped[name_len + 2] = '\0';
    ft_doc_insert_raw(doc, title_end, wrapped, name_len + 2);
}

static void title_remove_name(FtDocument *doc, const char *name, const char *protected_name) {
    if (protected_name && protected_name[0] != '\0' && strcmp(name, protected_name) == 0) return;
    size_t title_end = get_title_end(doc);
    size_t name_len = strlen(name);
    const char *p = doc->text;
    const char *end = doc->text + title_end;
    while (p < end) {
        if (*p == '`') {
            const char *name_start = p + 1;
            const char *name_end = name_start;
            while (name_end < end && *name_end != '`') name_end++;
            if (name_end >= end) break;
            size_t token_len = (size_t)(name_end - name_start);
            if (token_len == name_len && strncmp(name_start, name, name_len) == 0) {
                size_t remove_start = p - doc->text;
                size_t remove_end = (name_end + 1) - doc->text;
                ft_doc_delete_raw(doc, remove_start, remove_end - remove_start);
                return;
            }
            p = name_end + 1;
        } else {
            p++;
        }
    }
}

static void title_replace_name(FtDocument *doc, const char *old_name, const char *new_name) {
    if (!old_name || !new_name || strcmp(old_name, new_name) == 0) return;
    size_t title_end = get_title_end(doc);
    size_t old_len = strlen(old_name);
    size_t new_len = strlen(new_name);
    const char *p = doc->text;
    const char *end = doc->text + title_end;
    while (p < end) {
        if (*p == '`') {
            const char *name_start = p + 1;
            const char *name_end = name_start;
            while (name_end < end && *name_end != '`') name_end++;
            if (name_end >= end) break;
            size_t token_len = (size_t)(name_end - name_start);
            if (token_len == old_len && strncmp(name_start, old_name, old_len) == 0) {
                size_t replace_start = p - doc->text;
                ft_doc_delete_raw(doc, replace_start, (name_end + 1) - p);
                char wrapped[128];
                if (new_len + 2 < sizeof(wrapped)) {
                    wrapped[0] = '`';
                    memcpy(wrapped + 1, new_name, new_len);
                    wrapped[new_len + 1] = '`';
                    ft_doc_insert_raw(doc, replace_start, wrapped, new_len + 2);
                }
                return;
            }
            p = name_end + 1;
        } else {
            p++;
        }
    }
}

/* ========================================================================
 * Edit-state invariant checks (Refactor 3)
 * ======================================================================== */
#ifdef NDEBUG
#define check_edit_invariants(ed) ((void)0)
#else
static void check_edit_invariants(const FtTextEdit *ed) {
    if (!ed || !ed->doc) return;
    if (ed->cursor > ed->doc->len) {
        LOGE("Invariant violation: cursor=%zu > doc->len=%zu", ed->cursor, ed->doc->len);
        abort();
    }
    if (ed->sel_start > ed->doc->len) {
        LOGE("Invariant violation: sel_start=%zu > doc->len=%zu", ed->sel_start, ed->doc->len);
        abort();
    }
    if (ed->sel_end > ed->doc->len) {
        LOGE("Invariant violation: sel_end=%zu > doc->len=%zu", ed->sel_end, ed->doc->len);
        abort();
    }
}
#endif

/* ========================================================================
 * Document transaction helper (Refactor 2)
 * Tracks length changes across multi-step document mutations so the
 * cursor can be mechanically adjusted instead of hand-updated.
 * ======================================================================== */
typedef struct {
    FtDocument *doc;
    size_t len_before;
} DocTxn;

static void doc_txn_start(DocTxn *txn, FtDocument *doc) {
    txn->doc = doc;
    txn->len_before = doc->len;
}

static int doc_txn_delta(const DocTxn *txn) {
    return (int)txn->doc->len - (int)txn->len_before;
}

/* Callback wrapper for key distribution when autocomplete adds a participant */
static void autocomplete_on_add_participant(const char *name, void *userdata) {
    FtVulkanUI *ui = (FtVulkanUI *)userdata;
    int doc_idx = ui->activeChat;
    if (doc_idx < 0 || doc_idx >= ui->chatCount) return;
    if (!ui->isNetwork[doc_idx]) return;

    /* Only creator can add members */
    bool is_creator = true;
    if (ui->chatCreator[doc_idx][0] != '\0' &&
        strncmp(ui->chatCreator[doc_idx], ui->identity->hash_hex, 32) != 0) {
        is_creator = false;
    }
    if (!is_creator) {
        ui_log("[distribute_key_for_name] ignored: not creator\n");
        return;
    }

    FtDocCrypto *dc = ui->chatCrypto[doc_idx];
    if (!dc || !ui->userTable || !ui->gossip) {
        ui_log("[distribute_key_for_name] missing dc=%p userTable=%p gossip=%p\n",
                (void*)dc, (void*)ui->userTable, (void*)ui->gossip);
        return;
    }

    FtUser *u = NULL;
    for (int i = 0; i < ui->userTable->count; i++) {
        if (strcmp(ui->userTable->users[i].display_name, name) == 0) {
            u = &ui->userTable->users[i];
            break;
        }
    }
    if (!u) {
        ui_log("[distribute_key_for_name] user '%s' not found in userTable\n", name);
        return;
    }

    ui_log("[distribute_key_for_name] found user %s hash=%s pubkey=%s\n",
            u->display_name, u->hash_hex, u->pubkey_hex[0] ? u->pubkey_hex : "(none)");

    uint8_t pubkey_bytes[33] = {0};
    if (u->pubkey_hex[0] != '\0') {
        ft_hex_to_bytes(u->pubkey_hex, pubkey_bytes, 33);
    }
    if (ui->gossip && ui->gossip->key_manager) {
        ft_key_manager_add_member(ui->gossip->key_manager, doc_idx, u->hash_hex, pubkey_bytes);
    } else {
        ft_doc_crypto_add_member(dc, u->hash_hex, pubkey_bytes);
    }
    int dist_rc = ft_gossip_distribute_key(ui->gossip, doc_idx, u->hash_hex);
    ui_log("[distribute_key_for_name] distribute_key rc=%d\n", dist_rc);

    /* Broadcast updated member list so all peers learn the new member's pubkey */
    for (size_t p = 0; p < ui->gossip->net->peer_count; p++) {
        if (ui->gossip->net->peers[p].active && ui->gossip->net->peers[p].verified) {
            ft_gossip_send_member_list_enc(ui->gossip, &ui->gossip->net->peers[p].addr, doc_idx);
        }
    }

    /* Remove from gossip removed list if present */
    for (int r = 0; r < ui->gossip->removed_count; r++) {
        if (strncmp(ui->gossip->removed_hashes[r], u->hash_hex, 32) == 0) {
            for (int k = r; k < ui->gossip->removed_count - 1; k++) {
                memcpy(ui->gossip->removed_hashes[k], ui->gossip->removed_hashes[k + 1], 33);
            }
            ui->gossip->removed_count--;
            break;
        }
    }
}

static bool handle_startup_key(FtVulkanUI *ui, int key, int mods) {
    if (modal_top(&ui->modalManager) != MODAL_STARTUP) return false;
    const StartupPage *page = startup_page_for_state(ui->state);
    if (page && page->key) return page->key(ui, key, mods);
    return true; /* block keys for unknown states */
}

static bool handle_new_note_modal_key(FtVulkanUI *ui, int key, int mods) {
    (void)mods;
    if (modal_top(&ui->modalManager) != MODAL_NEW_NOTE) return false;
    if (key == KEY_ESC) {
        modal_close(&ui->modalManager, MODAL_NEW_NOTE);
    }
    return true; /* block all other keys while modal is open */
}

static bool handle_delete_modal_key(FtVulkanUI *ui, int key, int mods) {
    (void)mods;
    if (modal_top(&ui->modalManager) != MODAL_DELETE) return false;
    if (key == KEY_ESC) {
        modal_close(&ui->modalManager, MODAL_DELETE);
        return true;
    }
    if (key == KEY_ENTER) {
        int idx = g_deleteModal_chatToDelete;
        modal_close(&ui->modalManager, MODAL_DELETE);
        ft_vulkan_ui_remove_chat(ui, idx);
        return true;
    }
    return true; /* block other keys while modal is open */
}

/* ========================================================================
 * Debug state dump
 * ======================================================================== */
static void ft_vulkan_ui_dump_state(FtVulkanUI *ui) {
    ui_debug_log_network(ui, "========== FREETEXT STATE DUMP ==========");

    /* Local identity */
    if (ui->identity) {
        ui_debug_log_network(ui, "--- Identity ---");
        ui_debug_log_network(ui, "  name:   %s", ui->identity->display_name);
        ui_debug_log_network(ui, "  hash:   %s", ui->identity->hash_hex);
    } else {
        ui_debug_log_network(ui, "--- Identity: NULL ---");
    }

    /* User table */
    ui_debug_log_network(ui, "--- User Table (%d entries) ---", ui->userTable ? ui->userTable->count : 0);
    if (ui->userTable) {
        for (int i = 0; i < ui->userTable->count; i++) {
            FtUser *u = &ui->userTable->users[i];
            ui_debug_log_network(ui, "  [%d] hash=%s name=\"%s\" state=%s",
                                 i, u->hash_hex, u->display_name,
                                 ft_friend_state_name(u->friend_state));
        }
    }

    /* Net peer table */
    ui_debug_log_network(ui, "--- Net Peers (%zu entries) ---", ui->net ? ui->net->peer_count : 0);
    if (ui->net) {
        for (size_t i = 0; i < ui->net->peer_count; i++) {
            FtPeer *p = &ui->net->peers[i];
            ui_debug_log_network(ui, "  [%zu] %s:%d hash=%.8s active=%d verified=%d",
                                 i, inet_ntoa(p->addr.sin_addr), ntohs(p->addr.sin_port),
                                 p->hash_hex, (int)p->active, (int)p->verified);
        }
    }

    /* Network state */
    if (ui->networkState) {
        ui_debug_log_network(ui, "--- Network State ---");
        ui_debug_log_network(ui, "  resolve_status: %d (0=idle 1=pending 2=success 3=error)",
                             (int)ui->networkState->resolve_status);
        ui_debug_log_network(ui, "  resolved: public=%u:%u local=%u:%u",
                             ui->networkState->resolved_ip, ui->networkState->resolved_port,
                             ui->networkState->resolved_local_ip, ui->networkState->resolved_local_port);
        ui_debug_log_network(ui, "  resolve_hash: %s", ui->networkState->resolve_hash);
        ui_debug_log_network(ui, "  register_status: %d", (int)ui->networkState->register_status);
        ui_debug_log_network(ui, "  balance_status: %d wei=%llu",
                             (int)ui->networkState->balance_status,
                             (unsigned long long)ui->networkState->balance_wei);
    }

    /* Gossip state */
    if (ui->gossip) {
        ui_debug_log_network(ui, "--- Gossip ---");
        ui_debug_log_network(ui, "  pending_resolve: %s", ui->gossip->has_pending_resolve ? "yes" : "no");
        if (ui->gossip->has_pending_resolve) {
            ui_debug_log_network(ui, "    hash: %s", ui->gossip->pending_resolve_hash);
            ui_debug_log_network(ui, "    eth:  %s", ui->gossip->pending_resolve_eth);
        }
        int pending_challenges = 0;
        for (int i = 0; i < FT_MAX_CONCURRENT_CHALLENGES; i++) {
            if (ui->gossip->challenges[i].pending) pending_challenges++;
        }
        ui_debug_log_network(ui, "  identity_challenges: %d pending", pending_challenges);
        for (int i = 0; i < FT_MAX_CONCURRENT_CHALLENGES; i++) {
            if (ui->gossip->challenges[i].pending) {
                ui_debug_log_network(ui, "    [%d] hash=%s is_local=%d public_tried=%d",
                                     i, ui->gossip->challenges[i].hash_hex,
                                     (int)ui->gossip->challenges[i].is_local,
                                     (int)ui->gossip->challenges[i].public_tried);
            }
        }
        ui_debug_log_network(ui, "  hole_punch: remaining=%d is_local=%d",
                             (int)ui->gossip->hole_punch.packets_remaining,
                             (int)ui->gossip->hole_punch.is_local);
    }

    ui_debug_log_network(ui, "========== END DUMP ==========");
}

static bool handle_friend_modal_key(FtVulkanUI *ui, int key, int mods) {
    if (modal_top(&ui->modalManager) != MODAL_FRIEND) return false;
    if ((mods & 0x0002) && (mods & 0x0001) && key == KEY_D) {
        ft_vulkan_ui_dump_state(ui);
        return true;
    }
    if (key == KEY_ESC) {
        modal_close(&ui->modalManager, MODAL_FRIEND);
        g_friendModal.hash_buf[0] = '\0';
        ig_input_deactivate();
        g_friendModal.preview_valid = false;
        g_friendModal.preview_name[0] = '\0';
        g_friendModal.preview_hash[0] = '\0';
        g_friendModal.pathSelStart = -1;
        g_friendModal.pathSelEnd = -1;
        g_friendModal.pathFocused = false;
        return true;
    }
    if (key == KEY_ENTER) {
        if (((int)strlen(g_friendModal.hash_buf)) > 0 && ui->userTable && ui->identity) {
            char hash_hex[FT_HASH_HEX_LEN];
            char display_name[FT_DISPLAY_NAME_LEN];
            char pubkey_hex[FT_PUBKEY_HEX_LEN];
            if (ft_sharing_hash_decode(g_friendModal.hash_buf, hash_hex, display_name, sizeof(display_name), pubkey_hex, sizeof(pubkey_hex)) == 0) {
                if (strcmp(hash_hex, ui->identity->hash_hex) != 0) {
                    ft_friend_add(ui->userTable, hash_hex, pubkey_hex, display_name);
                    ui_log("[ui_friend_add] %s added. Waiting for reciprocity...\n", display_name[0] ? display_name : hash_hex);
                }
            } else if (strcmp(g_friendModal.hash_buf, ui->identity->hash_hex) != 0) {
                ft_friend_add(ui->userTable, g_friendModal.hash_buf, NULL, NULL);
                ui_log("[ui_friend_add] %s added. Waiting for reciprocity...\n", g_friendModal.hash_buf);
            }
            g_friendModal.hash_buf[0] = '\0';
            ig_input_deactivate();
            g_friendModal.preview_valid = false;
            g_friendModal.preview_name[0] = '\0';
            g_friendModal.preview_hash[0] = '\0';
            save_user_table(ui);
        }
        return true;
    }
    if (!g_friendModal.pathFocused && ig_input_handle_key(key, mods)) {
        if (key == KEY_BS || key == KEY_DEL || key == KEY_V || key == KEY_X) {
            update_friend_modal_preview(ui);
        }
        return true;
    }
    if (key == KEY_A && (mods & 0x0002) && g_friendModal.pathFocused) {
        int pathLen = (int)strlen(ui->peerCachePath);
        g_friendModal.pathSelStart = 0;
        g_friendModal.pathSelEnd = pathLen;
        return true;
    }
    return true; /* block other keys while modal is open */
}

static bool handle_global_shortcut_key(FtVulkanUI *ui, int key, int mods) {
    /* Dump state to stderr with Ctrl+Shift+D */
    if ((mods & 0x0002) && (mods & 0x0001) && key == KEY_D) {
        ft_vulkan_ui_dump_state(ui);
        return true;
    }
    /* Toggle debug panel with F12 */
    if (key == KEY_F12) {
        ui->showDebugPanel = !ui->showDebugPanel;
        return true;
    }
    /* Toggle friend modal with Ctrl+Shift+F */
    if ((mods & 0x0002) && (mods & 0x0001) && key == KEY_F) {
        if (modal_top(&ui->modalManager) == MODAL_FRIEND) {
            modal_close(&ui->modalManager, MODAL_FRIEND);
            g_friendModal.hash_buf[0] = '\0';
            ig_input_deactivate();
            g_friendModal.preview_valid = false;
            g_friendModal.preview_name[0] = '\0';
            g_friendModal.preview_hash[0] = '\0';
        } else {
            if (ui->edit) {
                ui->edit->selecting = false;
                ui->edit->sel_start = ui->edit->sel_end = ui->edit->cursor;
            }
            modal_push(&ui->modalManager, MODAL_FRIEND);
        }
        return true;
    }
    /* Ctrl+Number keys 1-8 switch chats */
    if ((mods & 0x0002) && key >= KEY_1 && key <= KEY_8) {
        int idx = key - KEY_1;
        if (idx < ui->chatCount) {
            ft_vulkan_ui_switch_chat(ui, idx);
            return true;
        }
    }
    return false;
}

static bool handle_autocomplete_key(FtVulkanUI *ui, int key, int mods) {
    (void)mods;
    if (!ui->autocomplete) return false;
    FtAutocomplete *ac = ui->autocomplete;
    if (!ft_autocomplete_is_active(ac)) return false;
    FtTextEdit *ed = ui->edit;
    const FtAutocompleteResult *res = ft_autocomplete_get_results(ac);
    int total = res ? res->count : 0;
    int sel = ft_autocomplete_get_selected_index(ac);
    switch (key) {
        case KEY_UP:
            if (sel <= 0) {
                ft_autocomplete_close(ac, FT_AUTO_CLOSE_KEY_UP_TOP);
                return false; /* fall through to normal cursor movement */
            }
            ft_autocomplete_move_selection(ac, -1);
            return true;
        case KEY_DOWN:
            ft_autocomplete_move_selection(ac, 1);
            return true;
        case KEY_ENTER:
        case KEY_TAB:
            if (total == 0) {
                /* Allow local body mode to create a new participant block from the typed query. */
                if (!ft_autocomplete_select(ac, -1, ed)) {
                    ft_autocomplete_close(ac, FT_AUTO_CLOSE_KEY_ENTER_EMPTY);
                    return false; /* let normal Enter/TAB handling proceed */
                }
            } else {
                ft_autocomplete_select(ac, ft_autocomplete_get_selected_index(ac), ed);
            }
            ensure_cursor_visible(ui);
            ft_vulkan_ui_process_commands(ui);
            return true;
        case KEY_ESC:
            ft_autocomplete_close(ac, FT_AUTO_CLOSE_KEY_ESC);
            return true;
        case KEY_LEFT:
        case KEY_RIGHT:
        case KEY_DEL:
            ft_autocomplete_close(ac, FT_AUTO_CLOSE_KEY_LR_DEL);
            return false; /* fall through to normal key handling */
        case KEY_BS:
            if (ed->cursor == ft_autocomplete_get_start_pos(ac)) {
                ft_autocomplete_close(ac, FT_AUTO_CLOSE_KEY_BS_AT_START);
            }
            return false; /* fall through to normal backspace */
        default:
            return true; /* ignore other keys while autocomplete is active */
    }
}

/* ========================================================================
 * Centralized Modal Input Dispatch (Refactor F)
 * ======================================================================== */

static bool modal_handle_key(FtVulkanUI *ui, int key, int mods) {
    (void)mods;
    switch (modal_top(&ui->modalManager)) {
        case MODAL_STARTUP:    return handle_startup_key(ui, key, mods);
        case MODAL_NEW_NOTE:   return handle_new_note_modal_key(ui, key, mods);
        case MODAL_DELETE:     return handle_delete_modal_key(ui, key, mods);
        case MODAL_FRIEND:     return handle_friend_modal_key(ui, key, mods);
        case MODAL_UPNP_WARNING:
            if (key == KEY_ENTER || key == KEY_ESC) {
                modal_pop(&ui->modalManager);
                return true;
            }
            return true;
        default: return false;
    }
}

static bool modal_handle_char(FtVulkanUI *ui, unsigned int codepoint) {
    ModalType top = modal_top(&ui->modalManager);

    /* Block chars for startup (non-name states), delete, and new note */
    if (top == MODAL_STARTUP &&
        ui->state != FT_STARTUP_ASK_NAME &&
        ui->state != FT_STARTUP_EDIT_PROFILE) {
        return true;
    }
    if (top == MODAL_DELETE || top == MODAL_NEW_NOTE) {
        return true;
    }

    /* Startup name input */
    if (top == MODAL_STARTUP &&
        (ui->state == FT_STARTUP_ASK_NAME ||
         ui->state == FT_STARTUP_EDIT_PROFILE)) {
        ig_input_handle_char(codepoint);
        return true;
    }

    /* Friend modal input */
    if (top == MODAL_FRIEND) {
        ig_input_handle_char(codepoint);
        update_friend_modal_preview(ui);
        return true;
    }

    return false;
}

static bool modal_handle_scroll(FtVulkanUI *ui, double xoffset, double yoffset) {
    (void)xoffset;
    ModalType top = modal_top(&ui->modalManager);

    /* Block scroll for startup, new note, delete, upnp warning */
    if (top == MODAL_STARTUP || top == MODAL_NEW_NOTE || top == MODAL_DELETE || top == MODAL_UPNP_WARNING) {
        return true;
    }

    /* Friend modal scroll */
    if (top == MODAL_FRIEND) {
        FriendModalLayout fl;
        compute_friend_modal_layout(ui, &fl);
        ScrollRegion sr = {
            &g_friendModal.listScrollY, fl.friendContentH, fl.friendListH,
            fl.friendListTop, fl.friendListH, 0, 0, 0, false
        };
        ft_ui_scroll_region_compute(&sr, ui->densityScale);
        ft_ui_scroll_region_wheel(&sr, (float)yoffset, ui->densityScale);
        return true;
    }

    return false;
}

static bool modal_handle_mouse_click(FtVulkanUI *ui, double x, double y) {
    (void)x; (void)y;
    return modal_is_open(&ui->modalManager);
}

static bool modal_handle_mouse_press(FtVulkanUI *ui, double x, double y) {
    switch (modal_top(&ui->modalManager)) {
        case MODAL_STARTUP:
            handle_startup_click(ui, x, y);
            return true;
        case MODAL_NEW_NOTE:
            handle_new_note_modal_click(ui, x, y);
            return true;
        case MODAL_DELETE: {
            int del_idx = g_deleteModal_chatToDelete;
            bool is_net = (del_idx >= 0 && del_idx < ui->chatCount && ui->isNetwork[del_idx]);
            DeleteModalLayout dl;
            compute_delete_modal_layout(ui, &dl, is_net);
            if (x >= dl.yesX && x < dl.yesX + dl.yesW && y >= dl.yesY && y < dl.yesY + dl.yesH) {
                int idx = g_deleteModal_chatToDelete;
                modal_pop(&ui->modalManager);
                ft_vulkan_ui_remove_chat(ui, idx);
                return true;
            }
            if (x >= dl.noX && x < dl.noX + dl.noW && y >= dl.noY && y < dl.noY + dl.noH) {
                modal_pop(&ui->modalManager);
                return true;
            }
            /* Click outside modal = cancel */
            modal_pop(&ui->modalManager);
            return true;
        }
        case MODAL_FRIEND:
            handle_friend_modal_click(ui, x, y);
            return true;
        case MODAL_UPNP_WARNING: {
            UpnpWarningModalLayout ul;
            compute_upnp_warning_modal_layout(ui, &ul);
            if (x >= ul.okX && x < ul.okX + ul.okW && y >= ul.okY && y < ul.okY + ul.okH) {
                modal_pop(&ui->modalManager);
                return true;
            }
            /* Click outside modal = dismiss */
            modal_pop(&ui->modalManager);
            return true;
        }
        default:
            return false;
    }
}

static bool modal_handle_mouse_move(FtVulkanUI *ui, double x, double y) {
    ModalType top = modal_top(&ui->modalManager);

    /* Startup text field drag */
    if (top == MODAL_STARTUP &&
        (ui->state == FT_STARTUP_ASK_NAME ||
         ui->state == FT_STARTUP_EDIT_PROFILE)) {
        if (g_active_input.mouse_selecting &&
            (g_active_input.buf == g_startup.first_name_buf ||
             g_active_input.buf == g_startup.last_name_buf)) {
            ig_input_drag((float)x);
            return true;
        }
    }

    /* Friend modal drag selection / scrollbar */
    if (top == MODAL_FRIEND && ui->mouseDown) {
        const FriendModalLayout *fl = &g_friendModal.layout;

        /* Path drag selection */
        if (g_friendModal.pathSelecting) {
            const char *path = ui->peerCachePath;
            int pathLen = (int)strlen(path);
            float relX = (float)x - fl->pathX;
            int pos = friend_modal_pos_from_x(path, pathLen, relX, fl->textScale);
            if (pos < 0) pos = 0;
            if (pos > pathLen) pos = pathLen;
            g_friendModal.pathSelEnd = pos;
            return true;
        }

        /* Friend list scrollbar drag */
        if (g_friendModal.listScrollbarDragging) {
            ScrollRegion sr = {
                &g_friendModal.listScrollY, fl->friendContentH, fl->friendListH,
                fl->friendListTop, fl->friendListH, fl->friendMaxScroll,
                fl->sbThumbY, fl->sbThumbH, fl->hasScrollbar
            };
            float deltaY = (float)(y - g_friendModal.listScrollbarDragStartY);
            ft_ui_scroll_region_drag(&sr, deltaY, g_friendModal.listScrollbarDragStartScrollY);
            return true;
        }

        /* Input box drag selection */
        if (g_active_input.mouse_selecting && g_active_input.buf == g_friendModal.hash_buf) {
            ig_input_drag((float)x);
            return true;
        }
    }

    /* Block background interaction for any open modal */
    if (modal_is_open(&ui->modalManager)) return true;

    return false;
}

static bool modal_handle_mouse_release(FtVulkanUI *ui, double x, double y) {
    (void)x; (void)y;
    ModalType top = modal_top(&ui->modalManager);

    /* Startup text field release */
    if (top == MODAL_STARTUP &&
        (ui->state == FT_STARTUP_ASK_NAME ||
         ui->state == FT_STARTUP_EDIT_PROFILE)) {
        ig_input_release();
    }

    /* Block background release for startup and new note */
    if (top == MODAL_STARTUP || top == MODAL_NEW_NOTE) {
        return true;
    }

    /* Friend modal release */
    if (top == MODAL_FRIEND) {
        ig_input_release();
        g_friendModal.pathSelecting = false;
        g_friendModal.listScrollbarDragging = false;
    }

    return false;
}

/* Returns true if the current network note should be read-only because
   the app is still connecting to the network. */
static bool is_network_note_frozen(const FtVulkanUI *ui) {
    if (!ui->networkState) return false;
    int doc_idx = ui->activeChat;
    if (doc_idx < 0 || doc_idx >= ui->chatCount || !ui->isNetwork[doc_idx]) return false;
    return (ui->networkState->balance_status == FT_NET_STATUS_PENDING ||
            ui->networkState->register_status == FT_NET_STATUS_PENDING);
}

void ft_vulkan_ui_handle_key(FtVulkanUI *ui, int key, int mods) {
    if (modal_handle_key(ui, key, mods)) return;
    if (!ui->edit) return;
    if (handle_global_shortcut_key(ui, key, mods)) return;
    if (handle_autocomplete_key(ui, key, mods)) return;

    FtTextEdit *ed = ui->edit;
    bool frozen = is_network_note_frozen(ui);
    bool text_mutated = false;
    size_t old_len = ed->doc ? ed->doc->len : 0;
    switch (key) {
        case KEY_LEFT:
            ft_edit_move_cursor(ed, -1);
            break;
        case KEY_RIGHT:
            ft_edit_move_cursor(ed, 1);
            break;
        case KEY_UP:
            if (!frozen) {
                ui_edit_move_line_visual(ui, -1);
            }
            break;
        case KEY_DOWN:
            if (!frozen) {
                size_t old_cursor = ed->cursor;
                ui_edit_move_line_visual(ui, 1);
                ui_debug_log_auto(ui, "[down] old=%zu new=%zu len=%zu net=%d",
                                  old_cursor, ed->cursor, ed->doc->len, (int)ed->is_network);
                if (ed->cursor == ed->doc->len && old_cursor == ed->doc->len) {
                    /* At end of document: insert Enter and move to new line */
                    ft_edit_enter(ed);
                    text_mutated = (ed->doc && ed->doc->len != old_len);
                    if (text_mutated) {
                        maybe_trigger_autocomplete(ui);
                        ft_vulkan_ui_process_commands(ui);
                    }
                } else if (ed->cursor_on_virtual_line && ed->is_network) {
                    maybe_trigger_autocomplete(ui);
                }
            }
            break;
        case KEY_ENTER:
            if (!frozen) {
                ft_edit_enter(ed);
                text_mutated = (ed->doc && ed->doc->len != old_len);
                if (text_mutated) {
                    maybe_trigger_autocomplete(ui);
                    ft_vulkan_ui_process_commands(ui);
                }
            }
            break;
        case KEY_BS:
            if (!frozen) {
                if (ed->selecting && ed->sel_start != ed->sel_end) {
                    ft_edit_delete_selection(ed);
                } else {
                    ft_edit_delete(ed, -1);
                }
                text_mutated = (ed->doc && ed->doc->len != old_len);
            }
            break;
        case KEY_DEL:
            if (!frozen) {
                if (ed->selecting && ed->sel_start != ed->sel_end) {
                    ft_edit_delete_selection(ed);
                } else {
                    ft_edit_delete(ed, 1);
                }
                text_mutated = (ed->doc && ed->doc->len != old_len);
            }
            break;
        case KEY_A:
            if (mods & 2) ft_edit_select_all(ed);
            break;
        case KEY_C:
            if (mods & 2) ft_vulkan_ui_copy(ui);
            break;
        case KEY_X:
            if (!frozen && (mods & 2)) { ft_vulkan_ui_cut(ui); text_mutated = (ed->doc && ed->doc->len != old_len); }
            break;
        case KEY_V:
            if (!frozen && (mods & 2)) { ft_vulkan_ui_paste(ui); text_mutated = (ed->doc && ed->doc->len != old_len); }
            break;
    }
    if (text_mutated) {
        ui_touch_current_block(ui);
        check_edit_invariants(ed);
    }
    ensure_cursor_visible(ui);
}

static void ui_update_blocks(FtVulkanUI *ui) {
    if (!ui->doc || !ui->edit) return;
    ft_doc_parse_blocks(ui->doc, ui->edit->local_display_name, ui->edit->local_hash, ui->userTable);
    ui_log_editor_state(ui, "ui_update_blocks");
}

static void ui_touch_current_block(FtVulkanUI *ui) {
    if (!ui->doc || !ui->edit) return;

    /* Network notes use position-based CRDT sync — no block touch needed.
       Local notes still touch blocks for metadata. */
    if (!ui->isNetwork[ui->activeChat]) {
        ft_doc_touch_block(ui->doc, ui->edit->cursor, (uint64_t)time(NULL));
    }
}

void ft_vulkan_ui_handle_char(FtVulkanUI *ui, unsigned int codepoint) {
    if (modal_handle_char(ui, codepoint)) return;

    if (!ui->edit) return;
    if (codepoint >= 32 && codepoint < 127) {
        if (codepoint == '`') return; /* Backtick is system reserved */
        if (is_network_note_frozen(ui)) return;

        char c = (char)codepoint;
        FtTextEdit *ed = ui->edit;

        /* Replace selected text first */
        if (ed->selecting && ed->sel_start != ed->sel_end) {
            ft_edit_delete_selection(ed);
        }

        size_t old_len = ed->doc ? ed->doc->len : 0;
        if (ui->autocomplete && ft_autocomplete_is_active(ui->autocomplete)) {
            ft_edit_insert(ed, &c, 1);
            if (ed->doc && ed->doc->len != old_len) {
                ui_debug_log(ui, "[handle_char] auto_active c='%c' old_len=%zu new_len=%zu",
                             c, old_len, ed->doc->len);
                ui_touch_current_block(ui);
                check_edit_invariants(ed);
                ui_log_editor_state(ui, "handle_char(auto)");
            }
            ensure_cursor_visible(ui);
            return;
        }

        /* Trigger autocomplete before inserting so the first typed character
           on an empty body line becomes part of the autocomplete query. */
        maybe_trigger_autocomplete(ui);

        if (ui->autocomplete && ft_autocomplete_is_active(ui->autocomplete)) {
            ft_edit_insert(ed, &c, 1);
            if (ed->doc && ed->doc->len != old_len) {
                ui_debug_log(ui, "[handle_char] auto_triggered c='%c' old_len=%zu new_len=%zu",
                             c, old_len, ed->doc->len);
                ui_touch_current_block(ui);
                check_edit_invariants(ed);
                ui_log_editor_state(ui, "handle_char(trigger)");
            }
            ensure_cursor_visible(ui);
            return;
        }

        ft_edit_insert(ed, &c, 1);
        if (ed->doc && ed->doc->len != old_len) {
            ui_debug_log(ui, "[handle_char] normal c='%c' old_len=%zu new_len=%zu",
                         c, old_len, ed->doc->len);
            ui_touch_current_block(ui);
            check_edit_invariants(ed);
            ui_log_editor_state(ui, "handle_char(normal)");
        }
        ensure_cursor_visible(ui);
        ft_vulkan_ui_process_commands(ui);
    }
}

void ft_vulkan_ui_copy(FtVulkanUI *ui) {
    /* Debug panel selection */
    if (ui->debugHasSelection) {
        int startLine = ui->debugSelStartLine;
        int startCol = ui->debugSelStartCol;
        int endLine = ui->debugSelEndLine;
        int endCol = ui->debugSelEndCol;
        char (*logBuf)[FT_UI_DEBUG_LOG_LINE_LEN] = ui->debugLog;
        switch (ui->debugPanelActiveTab) {
            case 1: logBuf = ui->debugLogAuto; break;
            case 2: logBuf = ui->debugLogNetwork; break;
            case 3: logBuf = ui->debugLogFriend; break;
            default: logBuf = ui->debugLog; break;
        }
        size_t totalLen = 0;
        for (int i = startLine; i <= endLine; i++) {
            const char *text = logBuf[i % FT_UI_DEBUG_LOG_LINES];
            int len = (int)strlen(text);
            int lo = 0, hi = len;
            if (i == startLine) lo = startCol;
            if (i == endLine) hi = endCol;
            if (lo < 0) lo = 0;
            if (hi > len) hi = len;
            if (hi > lo) totalLen += (size_t)(hi - lo);
            if (i < endLine) totalLen += 1; /* newline between lines */
        }
        char *buf = (char *)malloc(totalLen + 1);
        if (!buf) return;
        size_t off = 0;
        for (int i = startLine; i <= endLine; i++) {
            const char *text = logBuf[i % FT_UI_DEBUG_LOG_LINES];
            int len = (int)strlen(text);
            int lo = 0, hi = len;
            if (i == startLine) lo = startCol;
            if (i == endLine) hi = endCol;
            if (lo < 0) lo = 0;
            if (hi > len) hi = len;
            if (hi > lo) {
                memcpy(buf + off, text + lo, (size_t)(hi - lo));
                off += (size_t)(hi - lo);
            }
            if (i < endLine) buf[off++] = '\n';
        }
        buf[off] = '\0';
#ifdef _WIN32
        win32_set_clipboard_text(g_hwnd, buf);
#endif
        free(buf);
        return;
    }

    /* Friend modal path selection */
    if (modal_top(&ui->modalManager) == MODAL_FRIEND &&
        g_friendModal.pathSelStart >= 0 && g_friendModal.pathSelEnd >= 0 &&
        g_friendModal.pathSelStart != g_friendModal.pathSelEnd) {
        int start = g_friendModal.pathSelStart;
        int end = g_friendModal.pathSelEnd;
        if (start > end) { int t = start; start = end; end = t; }
        int len = end - start;
        char *buf = (char *)malloc(len + 1);
        if (!buf) return;
        memcpy(buf, ui->peerCachePath + start, len);
        buf[len] = '\0';
#ifdef _WIN32
        win32_set_clipboard_text(g_hwnd, buf);
#endif
        free(buf);
        return;
    }

    /* Friend modal input selection */
    if (modal_top(&ui->modalManager) == MODAL_FRIEND &&
        g_active_input.sel_start >= 0 && g_active_input.sel_end >= 0 &&
        g_active_input.sel_start != g_active_input.sel_end) {
        ig_input_copy();
        return;
    }

    if (!ui->edit || !ui->edit->selecting || ui->edit->sel_start == ui->edit->sel_end) return;
    FtTextEdit *ed = ui->edit;
    size_t start = ed->sel_start < ed->sel_end ? ed->sel_start : ed->sel_end;
    size_t end   = ed->sel_start < ed->sel_end ? ed->sel_end : ed->sel_start;
    size_t len = end - start;
    char *buf = (char *)malloc(len + 1);
    if (!buf) return;
    memcpy(buf, ed->doc->text + start, len);
    buf[len] = '\0';
#ifdef _WIN32
    win32_set_clipboard_text(g_hwnd, buf);
#endif
    free(buf);
}

void ft_vulkan_ui_cut(FtVulkanUI *ui) {
    /* Friend modal cut */
    if (modal_top(&ui->modalManager) == MODAL_FRIEND &&
        g_active_input.sel_start >= 0 && g_active_input.sel_end >= 0 &&
        g_active_input.sel_start != g_active_input.sel_end) {
        ig_input_copy();
        ig_input_delete_selection();
        update_friend_modal_preview(ui);
        return;
    }

    ft_vulkan_ui_copy(ui);
    if (ui->edit && ui->edit->selecting && ui->edit->sel_start != ui->edit->sel_end) {
        ft_edit_delete_selection(ui->edit);
        maybe_trigger_autocomplete(ui);
        ensure_cursor_visible(ui);
    }
}

void ft_vulkan_ui_paste(FtVulkanUI *ui) {
    if (!ui->edit) return;
#ifdef _WIN32
    char *text = win32_get_clipboard_text(g_hwnd);
    if (!text) return;

    if (ui->edit->selecting && ui->edit->sel_start != ui->edit->sel_end) {
        ft_edit_delete_selection(ui->edit);
    }

    size_t old_len = ui->edit->doc ? ui->edit->doc->len : 0;
    size_t tlen = strlen(text);
    for (size_t i = 0; i < tlen; i++) {
        char c = text[i];
        if (c == '\r') continue;
        if (c == '`') continue; /* Filter out reserved backtick character */
        if (c == '\n') {
            ft_edit_enter(ui->edit);
        } else if ((unsigned char)c >= 32 && (unsigned char)c < 127) {
            ft_edit_insert(ui->edit, &c, 1);
        }
    }
    free(text);
    ft_vulkan_ui_process_commands(ui);
    if (ui->edit->doc && ui->edit->doc->len != old_len) {
        ui_touch_current_block(ui);
        check_edit_invariants(ui->edit);
    }
    ensure_cursor_visible(ui);
#endif
}

void ft_vulkan_ui_handle_scroll(FtVulkanUI *ui, double xoffset, double yoffset) {
    (void)xoffset;
    if (modal_handle_scroll(ui, xoffset, yoffset)) return;

    float margin = 20.0f * ui->densityScale;
    float screenH = (float)ui->renderer->swapchainExtent.height;
    float statusBarH = 21.5f * ui->densityScale;

    /* Scroll sidebar if mouse is over it */
    if (ui->mouseX < ui->sidebarWidth) {
        SidebarLayout sl;
        compute_sidebar_layout(ui, &sl);
        ScrollRegion sr = {
            &ui->sidebarScrollY, sl.sidebarContentH, sl.sidebarVisibleH,
            sl.gap, sl.sidebarVisibleH, 0, 0, 0, false
        };
        ft_ui_scroll_region_compute(&sr, ui->densityScale);
        ft_ui_scroll_region_wheel(&sr, (float)yoffset, ui->densityScale);
        return;
    }

    /* Scroll right panel if mouse is over it */
    {
        float scale = 1.0f * ui->densityScale;
        float screenW = (float)ui->renderer->swapchainExtent.width;
        bool showRightPanel = compute_show_right_panel(ui);
        float rightPanelW = ui->sidebarWidth;
        if (showRightPanel && ui->mouseX > screenW - rightPanelW) {
            RightPanelLayout rpl;
            compute_right_panel_layout(ui, &rpl);

            ScrollRegion sr = {
                &ui->rightPanelScrollY, rpl.friendContentH, rpl.friendListVisibleH,
                rpl.friendListTop, rpl.friendListVisibleH, 0, 0, 0, false
            };
            ft_ui_scroll_region_compute(&sr, ui->densityScale);
            ft_ui_scroll_region_wheel(&sr, (float)yoffset, ui->densityScale);
            return;
        }
    }

    /* Scroll debug panel if mouse is over it */
    if (ui->showDebugPanel) {
        DebugPanelLayout dl;
        if (get_debug_panel_layout(ui, &dl)) {
            if (ui->mouseY >= dl.panelY && ui->mouseY < dl.panelY + dl.panelH && ui->mouseX >= 0 && ui->mouseX < dl.panelW) {
                float contentH = dl.totalLines * dl.dbgLineH;
                float visibleH = dl.maxVisibleLines * dl.dbgLineH;
                ScrollRegion sr = {
                    dl.scrollY, contentH, visibleH,
                    dl.textY, visibleH, 0, 0, 0, false
                };
                ft_ui_scroll_region_compute(&sr, ui->densityScale);
                ft_ui_scroll_region_wheel(&sr, (float)yoffset, ui->densityScale);
                return;
            }
        }
    }

    /* Otherwise scroll document */
    float visibleH = screenH - statusBarH - margin;
    float totalH = measure_document_height(ui);
    ScrollRegion sr = {
        &ui->scrollY, totalH, visibleH,
        margin, visibleH, 0, 0, 0, false
    };
    ft_ui_scroll_region_compute(&sr, ui->densityScale);
    ft_ui_scroll_region_wheel(&sr, (float)yoffset, ui->densityScale);
}

void ft_vulkan_ui_handle_mouse_click(FtVulkanUI *ui, double x, double y) {
    if (modal_handle_mouse_click(ui, x, y)) return;
    if (x < ui->sidebarWidth && ui->chatCount > 0) {
        SidebarLayout sl;
        compute_sidebar_layout(ui, &sl);
        int display_idx = (int)((y - sl.gap - sl.sidebarTopBtnH + ui->sidebarScrollY) / sl.itemTotalH);
        if (display_idx >= 0 && display_idx < ui->chatCount) {
            float itemY = sl.gap + sl.sidebarTopBtnH + display_idx * sl.itemTotalH - ui->sidebarScrollY;
            if (y >= itemY && y < itemY + sl.itemH) {
                ft_vulkan_ui_switch_chat(ui, ui->chatOrder[display_idx]);
            }
        }
    }
}

void ft_vulkan_ui_handle_mouse_press(FtVulkanUI *ui, double x, double y) {
    ui->mouseDown = true;
    ui->mouseX = x;
    ui->mouseY = y;

    if (modal_handle_mouse_press(ui, x, y)) return;

    /* Clear Friend Link highlight on any non-link click */
    if (g_startup.sharing_highlighted) {
        g_startup.sharing_highlighted = false;
    }

    /* Status bar: Friend Link click-to-select-and-copy */
    if (ui->statusBarLayout.sharing_hit_w > 0) {
        if (x >= ui->statusBarLayout.sharing_hit_x && x < ui->statusBarLayout.sharing_hit_x + ui->statusBarLayout.sharing_hit_w &&
            y >= ui->statusBarLayout.sharing_hit_y && y < ui->statusBarLayout.sharing_hit_y + ui->statusBarLayout.sharing_hit_h) {
            g_startup.sharing_highlighted = true;
            if (ui->edit) {
                ui->edit->selecting = false;
                ui->edit->sel_start = ui->edit->sel_end = ui->edit->cursor;
            }
            win32_set_clipboard_text(g_hwnd, get_sharing_address(ui->identity));
            ft_vulkan_ui_set_status(ui, "Friend Link copied to clipboard");
            return;
        }
    }

    /* Status bar: name click to edit */
    if (ui->statusBarLayout.name_hit_w > 0) {
        if (x >= ui->statusBarLayout.name_hit_x && x < ui->statusBarLayout.name_hit_x + ui->statusBarLayout.name_hit_w &&
            y >= ui->statusBarLayout.name_hit_y && y < ui->statusBarLayout.name_hit_y + ui->statusBarLayout.name_hit_h) {
            modal_push(&ui->modalManager, MODAL_STARTUP);
            ui->state = FT_STARTUP_EDIT_PROFILE;
            if (ui->identity) {
                strncpy(g_startup.first_name_buf, ui->identity->first_name, sizeof(g_startup.first_name_buf) - 1);
                g_startup.first_name_buf[sizeof(g_startup.first_name_buf) - 1] = '\0';
                strncpy(g_startup.last_name_buf, ui->identity->last_name, sizeof(g_startup.last_name_buf) - 1);
                g_startup.last_name_buf[sizeof(g_startup.last_name_buf) - 1] = '\0';
            }
            return;
        }
    }

    /* Autocomplete click-to-select */
    if (ui->autocomplete && ft_autocomplete_is_active(ui->autocomplete)) {
        compute_autocomplete_layout(ui);
        if (ui->autocompleteLayout.valid) {
            float dropX = ui->autocompleteLayout.dropX;
            float dropY = ui->autocompleteLayout.dropY;
            float dropW = ui->autocompleteLayout.dropW;
            float dropH = ui->autocompleteLayout.dropH;
            float itemH = ui->autocompleteLayout.itemH;

            const FtAutocompleteResult *res = ft_autocomplete_get_results(ui->autocomplete);
            int total_users = res ? res->count : 0;
            if (total_users > 0) {
                if (x >= dropX && x < dropX + dropW && y >= dropY && y < dropY + dropH) {
                    int clicked = (int)((y - dropY - 4.0f) / itemH);
                    if (clicked >= 0 && clicked < total_users) {
                        ft_autocomplete_select(ui->autocomplete, clicked, ui->edit);
                        ensure_cursor_visible(ui);
                        ft_vulkan_ui_process_commands(ui);
                        return;
                    }
                }
            }
        }
        ui_debug_log_auto(ui, "mouse_close: click outside dropdown or invalid layout");
        ft_autocomplete_close(ui->autocomplete, FT_AUTO_CLOSE_MOUSE_CLICK);
    }

    /* Document scrollbar click */
    if (is_over_scrollbar(ui, x, y)) {
        float margin = 20.0f * ui->densityScale;
        float screenH = (float)ui->renderer->swapchainExtent.height;
        float statusBarH = 21.5f * ui->densityScale;
        float visibleH = screenH - statusBarH - margin;
        float totalH = measure_document_height(ui);
        ScrollRegion sr = {
            &ui->scrollY, totalH, visibleH,
            margin, visibleH, 0, 0, 0, false
        };
        ft_ui_scroll_region_compute(&sr, ui->densityScale);
        if (ft_ui_scroll_region_hit_thumb(&sr, (float)y)) {
            ui->scrollbar.dragging = true;
            ui->scrollbar.dragStartY = (float)y;
            ui->scrollbar.dragStartScrollY = ui->scrollY;
        } else {
            ft_ui_scroll_region_page(&sr, (y < sr.thumbY) ? -1 : 1);
        }
        return;
    }

    /* Sidebar scrollbar click */
    {
        SidebarLayout sl;
        compute_sidebar_layout(ui, &sl);
        float sidebarW = ui->sidebarWidth;

        ScrollRegion sr = {
            &ui->sidebarScrollY, sl.sidebarContentH, sl.sidebarVisibleH,
            sl.gap, sl.sidebarVisibleH, 0, 0, 0, false
        };
        ft_ui_scroll_region_compute(&sr, ui->densityScale);
        if (sl.hasScrollbar) {
            float sbX = sidebarW - sl.sbW - 2.0f;
            if (x >= sbX && x < sbX + sl.sbW && y >= sl.gap && y < sl.gap + sl.sidebarVisibleH) {
                if (ft_ui_scroll_region_hit_thumb(&sr, (float)y)) {
                    ui->sidebarScrollbarDragging = true;
                    ui->sidebarScrollbarDragStartY = (float)y;
                    ui->sidebarScrollbarDragStartScrollY = ui->sidebarScrollY;
                } else {
                    ft_ui_scroll_region_page(&sr, (y < sr.thumbY) ? -1 : 1);
                }
                return;
            }
        }
    }

    /* Compute debug panel top Y so sidebar/right panel don't swallow clicks inside it */
    float debugPanelY = (float)(ui->renderer->swapchainExtent.height);
    if (ui->showDebugPanel) {
        DebugPanelLayout dl;
        if (get_debug_panel_layout(ui, &dl)) {
            debugPanelY = dl.panelY;
        }
    }

    /* Right panel scrollbar click */
    {
        float scale = 1.0f * ui->densityScale;
        float screenW = (float)ui->renderer->swapchainExtent.width;
        bool showRightPanel = compute_show_right_panel(ui);
        float rightPanelW = ui->sidebarWidth;
        if (showRightPanel) {
            RightPanelLayout rpl;
            compute_right_panel_layout(ui, &rpl);

            ScrollRegion sr = {
                &ui->rightPanelScrollY, rpl.friendContentH, rpl.friendListVisibleH,
                rpl.friendListTop, rpl.friendListVisibleH, 0, 0, 0, false
            };
            ft_ui_scroll_region_compute(&sr, ui->densityScale);
            if (rpl.hasScrollbar) {
                float sbX = screenW - rpl.sbW - 2.0f;
                if (x >= sbX && x < sbX + rpl.sbW && y >= rpl.friendListTop && y < rpl.friendListTop + rpl.friendListVisibleH) {
                    if (ft_ui_scroll_region_hit_thumb(&sr, (float)y)) {
                        ui->rightPanelScrollbarDragging = true;
                        ui->rightPanelScrollbarDragStartY = (float)y;
                        ui->rightPanelScrollbarDragStartScrollY = ui->rightPanelScrollY;
                    } else {
                        ft_ui_scroll_region_page(&sr, (y < sr.thumbY) ? -1 : 1);
                    }
                    return;
                }
            }
        }
    }

    /* Sidebar click */
    if (x < ui->sidebarWidth && y < debugPanelY) {
        float scale = 1.0f * ui->densityScale;
        float glyphH = g_fontLineHeight * scale;
        float margin = 20.0f * ui->densityScale;
        float itemH = glyphH * 2.0f + 12.0f;
        float gap = 6.0f * ui->densityScale;
        float itemTotalH = itemH + gap;
        float btnH = glyphH * 1.5f + 16.0f;
        float sidebarTopBtnH = btnH + gap;

        /* New Note button click (fixed at top) -- check BEFORE chat items so it
           takes priority over any note scrolled underneath it */
        if (ui->chatCount < FT_UI_MAX_CHATS) {
            float btnY = gap;
            if (y >= btnY && y < btnY + btnH) {
                if (ui->edit) {
                    ui->edit->selecting = false;
                    ui->edit->sel_start = ui->edit->sel_end = ui->edit->cursor;
                }
                modal_push(&ui->modalManager, MODAL_NEW_NOTE);
                return;
            }
        }

        int display_idx = (int)((y - gap - sidebarTopBtnH + ui->sidebarScrollY) / itemTotalH);

        /* Chat item click */
        if (display_idx >= 0 && display_idx < ui->chatCount) {
            float itemY = gap + sidebarTopBtnH + display_idx * itemTotalH - ui->sidebarScrollY;
            if (y >= itemY && y < itemY + itemH) {
                ft_vulkan_ui_switch_chat(ui, ui->chatOrder[display_idx]);
                return;
            }
        }
        return;
    }

    /* Right panel click */
    {
        float screenW = (float)ui->renderer->swapchainExtent.width;
        bool showRightPanel = compute_show_right_panel(ui);
        float rightPanelW = ui->sidebarWidth;
        if (showRightPanel && x > screenW - rightPanelW && y < debugPanelY) {
            RightPanelLayout rpl;
            compute_right_panel_layout(ui, &rpl);
            float addBtnY = rpl.friendGap;
            if (y >= addBtnY && y < addBtnY + rpl.friendBtnH) {
                if (ui->edit) {
                    ui->edit->selecting = false;
                    ui->edit->sel_start = ui->edit->sel_end = ui->edit->cursor;
                }
                modal_push(&ui->modalManager, MODAL_FRIEND);
                g_friendModal.pathSelStart = -1;
                g_friendModal.pathSelEnd = -1;
                g_friendModal.pathFocused = false;
                g_friendModal.pathSelecting = false;
            }
            return;
        }
    }

    /* Delete button click */
    if (g_deleteBtn.visible &&
        x >= g_deleteBtn.x && x < g_deleteBtn.x + g_deleteBtn.w &&
        y >= g_deleteBtn.y && y < g_deleteBtn.y + g_deleteBtn.h) {
        if (ui->edit) {
            ui->edit->selecting = false;
            ui->edit->sel_start = ui->edit->sel_end = ui->edit->cursor;
        }
        g_deleteModal_chatToDelete = ui->activeChat;
        modal_push(&ui->modalManager, MODAL_DELETE);
        return;
    }

    /* Debug panel click (tabs + scrollbar + text selection) */
    if (ui->showDebugPanel) {
        DebugPanelLayout dl;
        if (get_debug_panel_layout(ui, &dl)) {
            /* Tab click */
            if (y >= dl.tabBarY && y < dl.tabBarY + dl.tabBarH) {
                const char *tabLabels[4] = {"Editor", "Auto", "Network", "Friends"};
                float tabX = 8.0f;
                for (int t = 0; t < 4; t++) {
                    float tabW = string_width(tabLabels[t], dl.dbgScale) + 16.0f;
                    if (x >= tabX && x < tabX + tabW) {
                        if (ui->debugPanelActiveTab != t) {
                            ui->debugPanelActiveTab = t;
                            debug_clear_selection(ui);
                        }
                        return;
                    }
                    tabX += tabW + 4.0f;
                }
            }

            /* Scrollbar click */
            float contentH = dl.totalLines * dl.dbgLineH;
            float visibleH = dl.maxVisibleLines * dl.dbgLineH;
            if (contentH > visibleH) {
                float sbW = 8.0f * ui->densityScale;
                float sbX = dl.panelW - sbW - 2.0f;
                ScrollRegion sr = {
                    dl.scrollY, contentH, visibleH,
                    dl.textY, visibleH, 0, 0, 0, false
                };
                ft_ui_scroll_region_compute(&sr, ui->densityScale);
                if (ft_ui_scroll_region_hit_thumb(&sr, (float)y) && x >= sbX && x < sbX + sbW) {
                    *dl.scrollbarDragging = true;
                    *dl.scrollbarDragStartY = (float)y;
                    *dl.scrollbarDragStartScrollY = *dl.scrollY;
                    return;
                }
            }
        }
    }

    /* Debug panel text selection click */
    {
        int line, col;
        if (debug_hit_test(ui, x, y, &line, &col)) {
            debug_clear_selection(ui);
            ui->debugSelecting = true;
            ui->debugHasSelection = false;
            ui->debugDragStartLine = line;
            ui->debugDragStartCol = col;
            ui->debugSelStartLine = line;
            ui->debugSelStartCol = col;
            ui->debugSelEndLine = line;
            ui->debugSelEndCol = col;
            if (ui->edit) {
                ui->edit->selecting = false;
                ui->edit->sel_start = ui->edit->sel_end = ui->edit->cursor;
            }
            return;
        } else {
            debug_clear_selection(ui);
        }
    }

    /* Document area click */
    if (ui->edit) {
        bool on_prefix = false;
        size_t pos = pos_from_click(ui, x, y, &on_prefix);
        if (on_prefix) {
            size_t pstart, pend;
            if (ft_edit_get_any_prefix_bounds(ui->edit, pos, &pstart, &pend)) {
                /* Click on already-selected prefix → deselect */
                if (ui->edit->sel_start == pstart && ui->edit->sel_end == pend) {
                    ft_edit_set_cursor(ui->edit, pos);
                    ui->edit->selecting = false;
                    ui->dragStartPos = pos;
                } else {
                    ui->edit->sel_start = pstart;
                    ui->edit->sel_end = pend;
                    ui->edit->selecting = true;
                    ft_edit_update_cursor_intent(ui->edit, pend);
                    ui->dragStartPos = pstart;
                }
            } else {
                ft_edit_set_cursor(ui->edit, pos);
                ui->edit->selecting = false;
                ui->dragStartPos = pos;
            }
        } else {
            /* Title name atomic click: select entire backtick pair */
            FtTextEdit *ed = ui->edit;
            bool deselected = false;
            if (ed && ed->doc) {
                size_t title_end = 0;
                for (size_t i = 0; i < ed->doc->len && ed->doc->text[i] != '\n'; i++) title_end++;
                if (pos <= title_end) {
                    size_t ticks[32];
                    int tick_count = 0;
                    for (size_t i = 0; i < title_end && tick_count < 32; i++) {
                        if (ed->doc->text[i] == '`') ticks[tick_count++] = i;
                    }
                    if (tick_count >= 2 && tick_count % 2 == 0) {
                        for (int i = 0; i < tick_count; i += 2) {
                            if (pos >= ticks[i] && pos <= ticks[i + 1]) {
                                size_t name_start = ticks[i];
                                size_t name_end = ticks[i + 1] + 1;
                                /* Click on already-selected name → deselect */
                                if (ed->sel_start == name_start && ed->sel_end == name_end) {
                                    ft_edit_set_cursor(ed, pos);
                                    ed->selecting = false;
                                    ui->dragStartPos = pos;
                                } else {
                                    ed->sel_start = name_start;
                                    ed->sel_end = name_end;
                                    ed->selecting = true;
                                    ft_edit_update_cursor_intent(ed, name_end);
                                    ui->dragStartPos = name_start;
                                }
                                deselected = true;
                                break;
                            }
                        }
                    }
                }
            }
            if (!deselected) {
                ft_edit_set_cursor(ui->edit, pos);
                ui->edit->selecting = false;
                ui->dragStartPos = pos;
            }
        }

    }
}

void ft_vulkan_ui_handle_mouse_move(FtVulkanUI *ui, double x, double y) {
    ui->mouseX = x;
    ui->mouseY = y;

    if (modal_handle_mouse_move(ui, x, y)) return;
    if (!ui->mouseDown) return;

    /* Document scrollbar drag */
    if (ui->scrollbar.dragging) {
        float margin = 20.0f * ui->densityScale;
        float screenH = (float)ui->renderer->swapchainExtent.height;
        float statusBarH = 21.5f * ui->densityScale;
        float visibleH = screenH - statusBarH - margin;
        float totalH = measure_document_height(ui);
        ScrollRegion sr = {
            &ui->scrollY, totalH, visibleH,
            margin, visibleH, 0, 0, 0, false
        };
        ft_ui_scroll_region_compute(&sr, ui->densityScale);
        float deltaY = (float)(y - ui->scrollbar.dragStartY);
        ft_ui_scroll_region_drag(&sr, deltaY, ui->scrollbar.dragStartScrollY);
        return;
    }

    /* Sidebar scrollbar drag */
    if (ui->sidebarScrollbarDragging) {
        SidebarLayout sl;
        compute_sidebar_layout(ui, &sl);
        ScrollRegion sr = {
            &ui->sidebarScrollY, sl.sidebarContentH, sl.sidebarVisibleH,
            sl.gap, sl.sidebarVisibleH, 0, 0, 0, false
        };
        ft_ui_scroll_region_compute(&sr, ui->densityScale);
        float deltaY = (float)(y - ui->sidebarScrollbarDragStartY);
        ft_ui_scroll_region_drag(&sr, deltaY, ui->sidebarScrollbarDragStartScrollY);
        return;
    }

    /* Right panel scrollbar drag */
    if (ui->rightPanelScrollbarDragging) {
        float screenW = (float)ui->renderer->swapchainExtent.width;
        bool showRightPanel = compute_show_right_panel(ui);
        float rightPanelW = ui->sidebarWidth;
        if (showRightPanel && x > screenW - rightPanelW) {
            RightPanelLayout rpl;
            compute_right_panel_layout(ui, &rpl);

            ScrollRegion sr = {
                &ui->rightPanelScrollY, rpl.friendContentH, rpl.friendListVisibleH,
                rpl.friendListTop, rpl.friendListVisibleH, 0, 0, 0, false
            };
            ft_ui_scroll_region_compute(&sr, ui->densityScale);
            float deltaY = (float)(y - ui->rightPanelScrollbarDragStartY);
            ft_ui_scroll_region_drag(&sr, deltaY, ui->rightPanelScrollbarDragStartScrollY);
            return;
        }
    }

    /* Debug panel scrollbar drag */
    if (ui->debugPanelScrollbarDragging || ui->debugPanelScrollbarDraggingNetwork || ui->debugPanelScrollbarDraggingFriend) {
        DebugPanelLayout dl;
        if (get_debug_panel_layout(ui, &dl)) {
            float contentH = dl.totalLines * dl.dbgLineH;
            float visibleH = dl.maxVisibleLines * dl.dbgLineH;
            ScrollRegion sr = {
                dl.scrollY, contentH, visibleH,
                0, visibleH, 0, 0, 0, false
            };
            ft_ui_scroll_region_compute(&sr, ui->densityScale);
            float deltaY = (float)(y - *dl.scrollbarDragStartY);
            ft_ui_scroll_region_drag(&sr, deltaY, *dl.scrollbarDragStartScrollY);
            return;
        }
    }

    /* Debug panel drag */
    if (ui->debugSelecting) {
        int line, col;
        if (debug_hit_test(ui, x, y, &line, &col)) {
            ui->debugSelEndLine = line;
            ui->debugSelEndCol = col;
            ui->debugSelStartLine = ui->debugDragStartLine;
            ui->debugSelStartCol = ui->debugDragStartCol;
            debug_normalize_selection(ui);
            ui->debugHasSelection = (ui->debugSelStartLine != ui->debugSelEndLine ||
                                     ui->debugSelStartCol != ui->debugSelEndCol);
        }
        return;
    }

    if (!ui->edit || !ui->mouseDown) return;
    float screenW = (float)ui->renderer->swapchainExtent.width;
    bool showRightPanel = compute_show_right_panel(ui);
    float rightPanelW = ui->sidebarWidth;
    if (x < ui->sidebarWidth) return; /* Don't select over sidebar */
    if (showRightPanel && x > screenW - rightPanelW) return; /* Don't select over right panel */

    size_t pos = pos_from_click(ui, x, y, NULL);
    ui->edit->sel_end = pos;
    ft_edit_update_cursor_intent(ui->edit, pos);
    if (pos != ui->dragStartPos) {
        ui->edit->selecting = true;
    }
    ui->edit->sel_start = ui->dragStartPos;
    ft_edit_normalize_selection(ui->edit);
}

void ft_vulkan_ui_handle_mouse_release(FtVulkanUI *ui, double x, double y) {
    (void)x;
    (void)y;

    if (modal_handle_mouse_release(ui, x, y)) return;
    ui->mouseDown = false;
    ui->scrollbar.dragging = false;
    ui->sidebarScrollbarDragging = false;
    ui->rightPanelScrollbarDragging = false;
    ui->debugPanelScrollbarDragging = false;
    ui->debugPanelScrollbarDraggingNetwork = false;
    ui->debugPanelScrollbarDraggingFriend = false;

    if (ui->debugSelecting) {
        ui->debugSelecting = false;
        if (ui->debugSelStartLine == ui->debugSelEndLine &&
            ui->debugSelStartCol == ui->debugSelEndCol) {
            ui->debugHasSelection = false;
        }
    }
}

void ft_vulkan_ui_set_status(FtVulkanUI *ui, const char *status) {
    snprintf(ui->statusText, sizeof(ui->statusText), "%s", status);
}

void ft_vulkan_ui_switch_chat(FtVulkanUI *ui, int chatIndex) {
    if (chatIndex < 0 || chatIndex >= ui->chatCount) return;
    ui->activeChat = chatIndex;
    ui->doc = ui->chatDocs[chatIndex];
    ui->edit = ui->chatEdits[chatIndex];
    ui->lastCursorSendTime = 0;
    ui->lastCursorSentPos = 0;
    ui_debug_log(ui, "[switch_chat] idx=%d name=%s", chatIndex, ui->chatNames[chatIndex]);
    if (ui->edit) {
        if (ui->isNetwork[chatIndex]) {
            const char *creator_hash = ui->chatCreator[chatIndex];
            if (creator_hash[0] != '\0' && ui->identity) {
                if (strncmp(creator_hash, ui->identity->hash_hex, 32) == 0) {
                    if (ui->identity->first_name[0] && ui->identity->last_name[0]) {
                        snprintf(ui->edit->protected_title_name, sizeof(ui->edit->protected_title_name),
                                 "%s %s", ui->identity->first_name, ui->identity->last_name);
                    } else if (ui->identity->first_name[0]) {
                        snprintf(ui->edit->protected_title_name, sizeof(ui->edit->protected_title_name),
                                 "%s", ui->identity->first_name);
                    } else if (ui->edit->local_display_name[0]) {
                        strncpy(ui->edit->protected_title_name, ui->edit->local_display_name,
                                sizeof(ui->edit->protected_title_name) - 1);
                        ui->edit->protected_title_name[sizeof(ui->edit->protected_title_name) - 1] = '\0';
                    } else {
                        ui->edit->protected_title_name[0] = '\0';
                    }
                } else if (ui->userTable) {
                    ui->edit->protected_title_name[0] = '\0';
                    for (int i = 0; i < ui->userTable->count; i++) {
                        if (strncmp(ui->userTable->users[i].hash_hex, creator_hash, 32) == 0) {
                            strncpy(ui->edit->protected_title_name, ui->userTable->users[i].display_name,
                                    sizeof(ui->edit->protected_title_name) - 1);
                            ui->edit->protected_title_name[sizeof(ui->edit->protected_title_name) - 1] = '\0';
                            break;
                        }
                    }
                } else {
                    ui->edit->protected_title_name[0] = '\0';
                }
            } else {
                ui->edit->protected_title_name[0] = '\0';
            }
        } else {
            ui->edit->protected_title_name[0] = '\0';
        }
    }
    sync_active_edit_identity(ui);
    if (ui->autocomplete) {
        ft_autocomplete_close(ui->autocomplete, FT_AUTO_CLOSE_SWITCH_CHAT);
    }
    ensure_cursor_visible(ui);

    /* Re-parse blocks so title/body boundaries and ownership are fresh */
    if (ui->doc && ui->edit) {
        ft_doc_parse_blocks(ui->doc, ui->edit->local_display_name, ui->edit->local_hash, ui->userTable);
    }

    /* Update autocomplete document/network for new chat */
    if (ui->autocomplete && ui->edit && ui->edit->doc) {
        ft_autocomplete_set_document(ui->autocomplete, ui->edit->doc);
        int doc_idx = ui->activeChat;
        bool is_network = (doc_idx >= 0 && doc_idx < ui->chatCount && ui->isNetwork[doc_idx]);
        ft_autocomplete_set_network(ui->autocomplete, is_network);
        if (ui->edit->local_display_name) {
            ft_autocomplete_set_local_display_name(ui->autocomplete, ui->edit->local_display_name);
        }
    }

    /* Auto-trigger autocomplete if cursor is at start of empty body line */
    if (ui->edit && ui->edit->doc && ui->autocomplete) {
        FtDocument *doc = ui->edit->doc;
        size_t cursor = ui->edit->cursor;
        size_t title_end = 0;
        for (size_t i = 0; i < doc->len && doc->text[i] != '\n'; i++) title_end++;
        if (cursor > title_end) {
            bool at_line_start = (cursor == 0) ||
                                 (cursor > 0 && doc->text[cursor - 1] == '\n');
            bool line_empty = (cursor == doc->len) ||
                              (cursor < doc->len && doc->text[cursor] == '\n');
            if (at_line_start && line_empty) {
                ft_autocomplete_start(ui->autocomplete, cursor, 0);
            }
        }
    }
}

/* ========================================================================
 * Document + Edit initialization for a new chat (Refactor 5)
 * ======================================================================== */
static void doc_init_for_chat(FtVulkanUI *ui, FtDocument *doc, FtTextEdit *edit, FtDocCrypto *dc,
                               int idx, const char *dataDir,
                               const char *display_name, const char *local_hash,
                               bool is_network, FtUserTable *userTable) {
    ft_doc_init(doc, NULL);
    doc->is_network = is_network;
    if (is_network) {
        ft_doc_generate_id(doc);
    }
    ui_log("[doc_init_for_chat] idx=%d doc_id=%s is_network=%d\n", idx, doc->doc_id, (int)is_network);
    char path[512];
    snprintf(path, sizeof(path), "%s/note_%d.txt", dataDir, idx);
    strncpy(doc->filepath, path, sizeof(doc->filepath) - 1);

    bool title_readonly = false;
    if (is_network && local_hash) {
        ft_doc_set_author(doc, local_hash);
    }
    ft_edit_init(edit, doc, local_hash, display_name, is_network, title_readonly);
    edit->debug_log = ui_text_edit_debug_log;
    edit->debug_log_ctx = ui;
    ft_doc_crypto_init(dc);

    /* For network notes, add only the creator's name to the title.
       Do NOT insert a trailing newline here — the creator needs to be able
       to add more people to the title first. */
    if (is_network) {
        if (display_name && display_name[0]) {
            title_add_name(doc, display_name);
        }
    }

    /* Cursor must be at end AFTER all title names are inserted */
    ft_edit_set_cursor(edit, is_network ? get_title_end(doc) : doc->len);
    check_edit_invariants(edit);

    /* Parse blocks so autocomplete and editing work immediately */
    ft_doc_parse_blocks(doc, display_name, local_hash, userTable);
}

static void ui_register_chat(FtVulkanUI *ui, int idx, FtDocument *doc, FtTextEdit *edit,
                              FtDocCrypto *dc, bool is_network, const char *creator_hash) {
    strncpy(ui->chatNames[idx], "...", sizeof(ui->chatNames[idx]) - 1);
    ui->chatDocs[idx] = doc;
    ui->chatEdits[idx] = edit;
    ui->chatCrypto[idx] = dc;
    ui->isNetwork[idx] = is_network;
    if (is_network && creator_hash) {
        strncpy(ui->chatCreator[idx], creator_hash, 32);
        ui->chatCreator[idx][32] = '\0';
    } else {
        ui->chatCreator[idx][0] = '\0';
    }
    ui->chatCount++;
}

bool ft_vulkan_ui_add_chat(FtVulkanUI *ui, const char *display_name, const char *local_hash, bool is_network) {
    if (ui->chatCount >= FT_UI_MAX_CHATS) return false;

    int idx = ui->chatCount;
    FtDocument *doc = &g_docs[idx];
    FtTextEdit *edit = &g_edits[idx];
    FtDocCrypto *dc = &g_doc_crypto[idx];

    doc_init_for_chat(ui, doc, edit, dc, idx, ui->dataDir,
                      display_name, local_hash, is_network, ui->userTable);
    ui_register_chat(ui, idx, doc, edit, dc, is_network, local_hash);

    ft_vulkan_ui_switch_chat(ui, idx);
    return true;
}

int ft_vulkan_ui_add_network_chat_from_gossip(FtVulkanUI *ui, const char *doc_id, const char *creator_hash_hex) {
    if (ui->chatCount >= FT_UI_MAX_CHATS) return -1;
    if (!ui->identity) return -1;

    int idx = ui->chatCount;
    FtDocument *doc = &g_docs[idx];
    FtTextEdit *edit = &g_edits[idx];
    FtDocCrypto *dc = &g_doc_crypto[idx];

    ft_doc_init(doc, NULL);
    doc->is_network = true;
    strncpy(doc->doc_id, doc_id, FT_DOC_ID_HEX_LEN - 1);
    doc->doc_id[FT_DOC_ID_HEX_LEN - 1] = '\0';
    if (creator_hash_hex) {
        ft_doc_set_author(doc, creator_hash_hex);
    }
    ui_log("[add_network_chat_from_gossip] idx=%d doc_id=%s creator=%s\n",
            idx, doc->doc_id, creator_hash_hex ? creator_hash_hex : "unknown");

    char path[512];
    snprintf(path, sizeof(path), "%s/note_%d.txt", ui->dataDir, idx);
    strncpy(doc->filepath, path, sizeof(doc->filepath) - 1);
    doc->filepath[sizeof(doc->filepath) - 1] = '\0';

    bool title_readonly = true; /* Receiver is never the creator */
    ft_edit_init(edit, doc, ui->identity->hash_hex, ui->identity->display_name, true, title_readonly);
    edit->debug_log = ui_text_edit_debug_log;
    edit->debug_log_ctx = ui;
    /* Don't generate a document key — the creator will send it via key offer.
       Zero the crypto state so has_key remains false until unwrap. */
    memset(dc, 0, sizeof(*dc));

    ft_edit_set_cursor(edit, doc->len);

    ui_register_chat(ui, idx, doc, edit, dc, true, creator_hash_hex);

    /* Parse blocks so title rendering works correctly once content arrives */
    ft_doc_parse_blocks(doc, ui->identity->display_name, ui->identity->hash_hex, ui->userTable);

    return idx;
}

/* ========================================================================
 * Load existing notes from data directory on startup
 * ======================================================================== */
void ft_vulkan_ui_load_chats(FtVulkanUI *ui, const char *local_hash) {
    if (!ui || !ui->dataDir[0]) return;

    for (int idx = 0; idx < FT_UI_MAX_CHATS; idx++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/note_%d.txt", ui->dataDir, idx);

        /* Check if note file exists */
        FILE *f = fopen(path, "rb");
        if (!f) continue; /* Skip missing notes — gaps are OK */
        fclose(f);

        int mem_idx = ui->chatCount;
        FtDocument *doc = &g_docs[mem_idx];
        FtTextEdit *edit = &g_edits[mem_idx];
        FtDocCrypto *dc = &g_doc_crypto[mem_idx];

        ft_doc_init(doc, path);
        ft_doc_crypto_init(dc);
        if (ft_doc_load(doc) != 0) {
            /* Load failed — init as empty */
            ft_doc_init(doc, path);
        } else {
            /* Load metadata (including member hashes) */
            ft_doc_load_meta(doc, dc);
        }

        /* Skip permanently deleted notes */
        if (ui->gossip && doc->doc_id[0] &&
            ft_gossip_is_doc_deleted(ui->gossip, doc->doc_id)) {
            ui_log("[load_chats] note_%d doc_id=%s is deleted, removing stale files\n",
                    idx, doc->doc_id);
            remove(path);
            char meta_path[576];
            ft_doc_meta_path(doc, meta_path, sizeof(meta_path));
            remove(meta_path);
            ft_doc_free(doc);
            continue;
        }

        /* Determine if this was a network note from loaded meta */
        bool is_network = doc->is_network;

        bool title_readonly = false;
        if (is_network && doc->creator_hash[0] != '\0' &&
            strncmp(doc->creator_hash, local_hash, 32) != 0) {
            title_readonly = true;
        }
        ft_edit_init(edit, doc, local_hash, ui->identity ? ui->identity->display_name : "User", is_network, title_readonly);
        edit->debug_log = ui_text_edit_debug_log;
        edit->debug_log_ctx = ui;

        const char *creator = doc->creator_hash[0] ? doc->creator_hash : local_hash;
        ui_register_chat(ui, mem_idx, doc, edit, dc, is_network, creator);
    }

    ui_log("[load_chats] loaded %d notes\n", ui->chatCount);
    for (int i = 0; i < ui->chatCount; i++) {
        ui_log("[load_chats] note[%d] doc_id=%s filepath=%s is_network=%d\n",
                i, ui->chatDocs[i]->doc_id, ui->chatDocs[i]->filepath, ui->isNetwork[i]);
    }
    if (ui->chatCount > 0) {
        update_chat_order(ui);
        ft_vulkan_ui_switch_chat(ui, ui->chatOrder[0]);
    }
}

void ft_vulkan_ui_process_commands(FtVulkanUI *ui) {
    (void)ui;
    /* *adds and *remove commands have been removed. */
}

void ft_vulkan_ui_remove_chat(FtVulkanUI *ui, int chatIndex) {
    if (chatIndex < 0 || chatIndex >= ui->chatCount) return;
    /* Allow deleting the last note; the app handles chatCount==0 gracefully */

    /* Save and free the document */
    ft_doc_save(ui->chatDocs[chatIndex], ui->chatCrypto[chatIndex]);

    /* Mark as deleted so it will never resync */
    if (ui->gossip && ui->chatDocs[chatIndex]->doc_id[0]) {
        ft_gossip_mark_doc_deleted(ui->gossip, ui->chatDocs[chatIndex]->doc_id);
    }

    /* Delete the note files */
    if (ui->chatDocs[chatIndex]->filepath[0]) {
        remove(ui->chatDocs[chatIndex]->filepath);
        char meta_path[576];
        ft_doc_meta_path(ui->chatDocs[chatIndex], meta_path, sizeof(meta_path));
        remove(meta_path);
    }

    ft_doc_free(ui->chatDocs[chatIndex]);

    int move_count = ui->chatCount - chatIndex - 1;
    if (move_count > 0) {
        memmove(&ui->chatNames[chatIndex], &ui->chatNames[chatIndex + 1],
                move_count * sizeof(ui->chatNames[0]));
        memmove(&g_docs[chatIndex], &g_docs[chatIndex + 1],
                move_count * sizeof(g_docs[0]));
        memmove(&g_edits[chatIndex], &g_edits[chatIndex + 1],
                move_count * sizeof(g_edits[0]));
        memmove(&g_doc_crypto[chatIndex], &g_doc_crypto[chatIndex + 1],
                move_count * sizeof(g_doc_crypto[0]));
        memmove(&ui->isNetwork[chatIndex], &ui->isNetwork[chatIndex + 1],
                move_count * sizeof(ui->isNetwork[0]));
        memmove(&ui->chatCreator[chatIndex], &ui->chatCreator[chatIndex + 1],
                move_count * sizeof(ui->chatCreator[0]));
    }

    /* Rebuild UI pointers and fixup doc pointers after memmove */
    for (int i = 0; i < ui->chatCount - 1; i++) {
        ui->chatDocs[i] = &g_docs[i];
        ui->chatEdits[i] = &g_edits[i];
        g_edits[i].doc = &g_docs[i];
    }

    /* Clear the last slot */
    int last = ui->chatCount - 1;
    ui->chatNames[last][0] = '\0';
    ui->chatDocs[last] = NULL;
    ui->chatEdits[last] = NULL;
    ui->chatCrypto[last] = NULL;
    ui->isNetwork[last] = false;
    ui->chatCreator[last][0] = '\0';
    memset(&g_docs[last], 0, sizeof(g_docs[last]));
    memset(&g_edits[last], 0, sizeof(g_edits[last]));
    memset(&g_doc_crypto[last], 0, sizeof(g_doc_crypto[last]));

    ui->chatCount--;

    /* Adjust active chat */
    if (ui->activeChat >= ui->chatCount) {
        ui->activeChat = ui->chatCount - 1;
    }
    if (ui->chatCount > 0) {
        ft_vulkan_ui_switch_chat(ui, ui->activeChat);
    } else {
        ui->doc = NULL;
        ui->edit = NULL;
    }
}
