#ifndef FT_VULKAN_UI_H
#define FT_VULKAN_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ft_vulkan_renderer.h"
#include "ft_doc_model.h"
#include "ft_text_edit.h"
#include "ft_p2p_net.h"
#include "ft_identity.h"
#include "ft_doc_crypto.h"
#include "ft_gossip.h"
#include "ft_network_state.h"

struct FtAutocomplete;

#define FT_UI_MAX_CHATS 64

/* FtTextField / FtFormField replaced by ig_input_text (Phase 3) */

typedef enum {
    FT_STARTUP_ASK_MODE = 0,
    FT_STARTUP_ASK_NAME,
    FT_STARTUP_EDIT_PROFILE,
    FT_STARTUP_SHOW_ADDRESS,
    FT_STARTUP_CHECKING,
    FT_STARTUP_ONLINE,
    FT_STARTUP_OFFLINE
} FtStartupState;

typedef enum {
    MODAL_NONE = 0,
    MODAL_DELETE,
    MODAL_NEW_NOTE,
    MODAL_FRIEND,
    MODAL_STARTUP,
    MODAL_UPNP_WARNING,
} ModalType;

typedef struct {
    ModalType stack[4];
    int count;
} ModalManager;

static inline void modal_push(ModalManager *m, ModalType type) {
    if (type == MODAL_NONE) return;
    if (m->count > 0 && m->stack[m->count - 1] == type) return;
    if (m->count < 4) m->stack[m->count++] = type;
}

static inline void modal_pop(ModalManager *m) {
    if (m->count > 0) m->count--;
}

static inline void modal_close(ModalManager *m, ModalType type) {
    if (m->count > 0 && m->stack[m->count - 1] == type) m->count--;
}

static inline ModalType modal_top(const ModalManager *m) {
    return (m->count > 0) ? m->stack[m->count - 1] : MODAL_NONE;
}

static inline bool modal_is_open(const ModalManager *m) {
    return modal_top(m) != MODAL_NONE;
}

/* Friend modal layout (computed each frame when active, used by render + input) */
typedef struct {
    float modX, modY, modW, modH;
    float textScale, glyphH, lineGap, pad;
    float itemH, textLineH, inputH, btnH, bottomPad, gap;
    float friendListTop, friendListBottom, friendListH;
    float friendContentH, friendMaxScroll;
    float pathX, pathY, pathW;
    float inputLabelY, inputX, inputW, inputY;
    float addBtnX, addBtnY, addBtnW, addBtnH;
    float closeX, closeBtnY, closeW;
    float textLeft;
    float sbX, sbW, sbTrackY, sbTrackH, sbThumbY, sbThumbH;
    bool hasScrollbar;
    int friend_count;
} FriendModalLayout;

/* Status bar layout (computed each frame, used by render + click) */
typedef struct {
    float name_hit_x, name_hit_y, name_hit_w, name_hit_h;
    float sharing_hit_x, sharing_hit_y, sharing_hit_w, sharing_hit_h;
} StatusBarLayout;



typedef struct FtVulkanUI {
    FtVulkanRenderer *renderer;
    FtDocument *doc;
    FtTextEdit *edit;
    FtNet *net;

    float densityScale;
    float scrollY;
    float cursorBlink;
    double lastCursorUpdate;

    /* Remote cursor sync throttling */
    double lastCursorSendTime;
    size_t lastCursorSentPos;

    char statusText[256];

    /* Mouse state */
    bool mouseDown;
    double mouseX;
    double mouseY;
    size_t dragStartPos;

    /* Sidebar / chat switching */
    float sidebarWidth;
    int activeChat;
    int chatCount;
    char chatNames[FT_UI_MAX_CHATS][32];
    FtDocument *chatDocs[FT_UI_MAX_CHATS];
    FtTextEdit *chatEdits[FT_UI_MAX_CHATS];
    float sidebarScrollY;
    float rightPanelScrollY;
    /* Display order: maps display position -> array index, sorted by created_at */
    int chatOrder[FT_UI_MAX_CHATS];

    /* Autocomplete module (extracted in Refactor 6) */
    struct FtAutocomplete *autocomplete;

    /* Cached autocomplete dropdown geometry (recomputed each frame when active) */
    struct {
        float dropX, dropY, dropW, dropH, itemH;
        bool valid;
    } autocompleteLayout;

    /* Scrollbar state */
    struct {
        bool dragging;
        float dragStartY;
        float dragStartScrollY;
    } scrollbar;
    bool sidebarScrollbarDragging;
    float sidebarScrollbarDragStartY;
    float sidebarScrollbarDragStartScrollY;
    bool rightPanelScrollbarDragging;
    float rightPanelScrollbarDragStartY;
    float rightPanelScrollbarDragStartScrollY;

    /* Document encryption contexts (parallel to chatDocs) */
    FtDocCrypto *chatCrypto[FT_UI_MAX_CHATS];
    FtUserTable *userTable;
    FtIdentity *identity;
    FtGossip *gossip;
    FtNetworkState *networkState; /* async network operations write here */

    /* Startup / networking state (persisted beyond modal) */
    FtStartupState state;
    bool networking_enabled;

    /* Per-chat network flag: true if this note is synced over P2P */
    bool isNetwork[FT_UI_MAX_CHATS];

    /* Per-chat creator: hex identity hash of who created the network note */
    char chatCreator[FT_UI_MAX_CHATS][33];

    /* Status bar layout cache */
    StatusBarLayout statusBarLayout;

    /* Modal stack — only the top modal receives input and is rendered */
    ModalManager modalManager;

    /* Flags for one-shot modal notifications */
    bool upnp_warning_shown;

    /* Peer cache path for friend persistence */
    char peerCachePath[512];

    /* Data directory (relative to binary, e.g. "freetext_data") */
    char dataDir[512];

    /* Debug panel toggle */
    bool showDebugPanel;

    /* Circular debug log for editor/block state */
#define FT_UI_DEBUG_LOG_LINES 64
#define FT_UI_DEBUG_LOG_LINE_LEN 256
    char debugLog[FT_UI_DEBUG_LOG_LINES][FT_UI_DEBUG_LOG_LINE_LEN];
    int debugLogCount;
    int debugLogNext;
    float debugPanelScrollY;
    bool debugPanelScrollbarDragging;
    float debugPanelScrollbarDragStartY;
    float debugPanelScrollbarDragStartScrollY;

    /* Circular debug log for network/state */
    char debugLogNetwork[FT_UI_DEBUG_LOG_LINES][FT_UI_DEBUG_LOG_LINE_LEN];
    int debugLogNetworkCount;
    int debugLogNetworkNext;
    float debugPanelScrollYNetwork;
    bool debugPanelScrollbarDraggingNetwork;
    float debugPanelScrollbarDragStartYNetwork;
    float debugPanelScrollbarDragStartScrollYNetwork;

    /* Circular debug log for autocomplete state */
    char debugLogAuto[FT_UI_DEBUG_LOG_LINES][FT_UI_DEBUG_LOG_LINE_LEN];
    int debugLogAutoCount;
    int debugLogAutoNext;
    float debugPanelScrollYAuto;
    bool debugPanelScrollbarDraggingAuto;
    float debugPanelScrollbarDragStartYAuto;
    float debugPanelScrollbarDragStartScrollYAuto;

    /* Circular debug log for friend state changes */
    char debugLogFriend[FT_UI_DEBUG_LOG_LINES][FT_UI_DEBUG_LOG_LINE_LEN];
    int debugLogFriendCount;
    int debugLogFriendNext;
    float debugPanelScrollYFriend;
    bool debugPanelScrollbarDraggingFriend;
    float debugPanelScrollbarDragStartYFriend;
    float debugPanelScrollbarDragStartScrollYFriend;

    /* Debug panel tabs: 0=editor, 1=autocomplete, 2=network, 3=friends */
    int debugPanelActiveTab;

    /* Debug panel selection */
    bool debugSelecting;
    bool debugHasSelection;
    int debugSelStartLine;
    int debugSelStartCol;
    int debugSelEndLine;
    int debugSelEndCol;
    int debugDragStartLine;
    int debugDragStartCol;
    uint64_t debugLastStateHash;


} FtVulkanUI;

bool ft_vulkan_ui_init(FtVulkanUI *ui, FtVulkanRenderer *renderer, FtDocument *doc, FtTextEdit *edit, FtNet *net);
void ft_vulkan_ui_render(FtVulkanUI *ui);
void ft_vulkan_ui_handle_key(FtVulkanUI *ui, int key, int mods);
void ft_vulkan_ui_handle_char(FtVulkanUI *ui, unsigned int codepoint);
void ft_vulkan_ui_copy(FtVulkanUI *ui);
void ft_vulkan_ui_cut(FtVulkanUI *ui);
void ft_vulkan_ui_paste(FtVulkanUI *ui);
void ft_vulkan_ui_handle_scroll(FtVulkanUI *ui, double xoffset, double yoffset);
void ft_vulkan_ui_handle_mouse_click(FtVulkanUI *ui, double x, double y);
void ft_vulkan_ui_handle_mouse_press(FtVulkanUI *ui, double x, double y);
void ft_vulkan_ui_handle_mouse_move(FtVulkanUI *ui, double x, double y);
void ft_vulkan_ui_handle_mouse_release(FtVulkanUI *ui, double x, double y);
void ft_vulkan_ui_set_status(FtVulkanUI *ui, const char *status);
void ft_vulkan_ui_switch_chat(FtVulkanUI *ui, int chatIndex);
bool ft_vulkan_ui_add_chat(FtVulkanUI *ui, const char *display_name, const char *local_hash, bool is_network);
/* Add a network chat for a document received from a friend via gossip.
   doc_id: the 32-char hex doc id from the sender
   creator_hash_hex: the sender's identity hash (32 chars)
   Returns the chat index on success, -1 on failure. */
int ft_vulkan_ui_add_network_chat_from_gossip(FtVulkanUI *ui, const char *doc_id, const char *creator_hash_hex);
void ft_vulkan_ui_remove_chat(FtVulkanUI *ui, int chatIndex);
void ft_vulkan_ui_load_chats(FtVulkanUI *ui, const char *local_hash);

/* Scan the active document for *adds / *removes commands and execute them.
   Call after text changes (e.g., on Enter or paste). */
void ft_vulkan_ui_process_commands(FtVulkanUI *ui);

/* Initialize startup flow. Computes ETH address from identity. */
void ft_vulkan_ui_startup_init(FtVulkanUI *ui, FtIdentity *id);

/* Returns true when the startup flow is complete (online or offline chosen). */
bool ft_vulkan_ui_startup_done(FtVulkanUI *ui);

/* Check if user chose online mode (networking will be enabled). */
bool ft_vulkan_ui_startup_online(FtVulkanUI *ui);

/* Poll balance when in SHOW_ADDRESS or CHECKING state. Call from main loop.
   Returns true if state changed (e.g. balance detected → ONLINE). */
bool ft_vulkan_ui_startup_tick(FtVulkanUI *ui);

#ifdef __cplusplus
}
#endif

#endif
