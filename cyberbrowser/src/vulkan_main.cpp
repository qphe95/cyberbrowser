#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windowsx.h>
#else
#include <unistd.h>
#include <sys/select.h>
#include <errno.h>
#endif
#include <signal.h>
#include <time.h>
#include <exception>

#include "ft_vulkan_renderer.h"
#include "ft_vulkan_ui.h"
#include "ft_crypto.h"
#include "ft_doc_model.h"
#include "ft_identity.h"
#include "ft_text_edit.h"
#include "ft_p2p_net.h"
#include "ft_gossip.h"
#include "ft_registry.h"
#include "ft_key_manager.h"
#include "ft_dns_bootstrap.h"
#include "ft_font_system.h"
#include "ft_async_http.h"
#include "ft_network_state.h"
#include "ft_upnp.h"
#include "ft_log_buffer.h"

#ifdef _WIN32
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

/* Write crash log using raw Win32 APIs — no CRT calls after crash.
   Returns true if log was written successfully. */
static BOOL write_crash_log_raw(EXCEPTION_POINTERS *ep)
{
    HANDLE hFile = INVALID_HANDLE_VALUE;
    char crash_path[MAX_PATH] = {0};
    if (GetModuleFileNameA(NULL, crash_path, MAX_PATH - 20) > 0) {
        char *last_slash = strrchr(crash_path, '\\');
        if (last_slash) strcpy(last_slash + 1, "crash_log.txt");
        else strcat(crash_path, ".crash_log.txt");
        hFile = CreateFileA(crash_path, GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (hFile == INVALID_HANDLE_VALUE) return FALSE;

    char buf[4096];
    int len = 0;
    DWORD written = 0;

    SYSTEMTIME st;
    GetLocalTime(&st);
    len += snprintf(buf + len, sizeof(buf) - len,
        "=== FreeText Crash Log ===\n"
        "Time: %04d-%02d-%02d %02d:%02d:%02d\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    if (ep && ep->ExceptionRecord) {
        len += snprintf(buf + len, sizeof(buf) - len,
            "Exception code: 0x%08X\n"
            "Fault address: 0x%p\n",
            ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress);
        if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            ep->ExceptionRecord->NumberParameters >= 2) {
            len += snprintf(buf + len, sizeof(buf) - len,
                "Access violation: %s address 0x%p\n",
                ep->ExceptionRecord->ExceptionInformation[0] ? "write to" : "read from",
                (void *)ep->ExceptionRecord->ExceptionInformation[1]);
        }
    }
    len += snprintf(buf + len, sizeof(buf) - len, "\nStack trace:\n");

    HANDLE process = GetCurrentProcess();
    SymInitialize(process, NULL, TRUE);

    void *stack[64];
    WORD frames = CaptureStackBackTrace(0, 64, stack, NULL);

    /* Use static buffer for symbol info to avoid heap allocation after crash */
    static char symbol_buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *symbol = (SYMBOL_INFO *)symbol_buf;
    symbol->MaxNameLen = 255;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

    for (WORD i = 0; i < frames && len < (int)sizeof(buf) - 256; i++) {
        DWORD64 disp = 0;
        if (SymFromAddr(process, (DWORD64)stack[i], &disp, symbol)) {
            len += snprintf(buf + len, sizeof(buf) - len,
                "  %d: 0x%llX %s+0x%llX\n", i,
                (unsigned long long)stack[i], symbol->Name, (unsigned long long)disp);
        } else {
            len += snprintf(buf + len, sizeof(buf) - len,
                "  %d: 0x%llX (no symbol)\n", i, (unsigned long long)stack[i]);
        }
    }
    SymCleanup(process);

    len += snprintf(buf + len, sizeof(buf) - len, "\n=== End Crash Log ===\n");
    WriteFile(hFile, buf, len, &written, NULL);
    FlushFileBuffers(hFile);
    CloseHandle(hFile);
    return TRUE;
}

/* Fatal exception handler — writes log and terminates WITHOUT calling
   ExitProcess (which crashes in WinHTTP callback threads). */
static volatile LONG s_crash_handled = 0;

static LONG WINAPI fatal_exception_handler(EXCEPTION_POINTERS *ep)
{
    /* Only handle the first crash — ignore nested/recursive exceptions */
    if (InterlockedCompareExchange(&s_crash_handled, 1, 0) != 0) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
        code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_STACK_OVERFLOW ||
        code == EXCEPTION_PRIV_INSTRUCTION) {
        write_crash_log_raw(ep);
        /* Use TerminateProcess instead of ExitProcess to avoid CRT cleanup
           that crashes in WinHTTP callback threads. */
        TerminateProcess(GetCurrentProcess(), 1);
        /* Never reached, but just in case: */
        return EXCEPTION_EXECUTE_HANDLER;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void crash_signal_handler(int sig)
{
    (void)sig;
    write_crash_log_raw(NULL);
    TerminateProcess(GetCurrentProcess(), 1);
}

static void crash_terminate_handler(void)
{
    write_crash_log_raw(NULL);
    TerminateProcess(GetCurrentProcess(), 1);
}
#endif

static void main_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr,  fmt, args);
    va_end(args);
    va_start(args, fmt);
    ft_log_buffer_vwrite(fmt, args);
    va_end(args);
}

static volatile int g_running = 1;
static FtVulkanUI *g_ui = NULL;
HWND g_hwnd = NULL;
static int g_window_focused = 0;
static int g_ignore_next_click = 0;

#define MAX_CHATS 64

FtDocument g_docs[MAX_CHATS];
FtTextEdit g_edits[MAX_CHATS];
FtDocCrypto g_doc_crypto[MAX_CHATS];

/* Callback for network-induced document changes: resolve cursor intent */
static void on_doc_changed(FtDocument *doc, FtEditOrigin origin,
                             size_t pos, size_t old_len, size_t new_len, void *ctx) {
    (void)doc;
    (void)pos;
    (void)old_len;
    (void)new_len;
    /* Local edits already keep cursor in sync via ft_text_edit.
       Only resolve cursor for remote changes that may shift text. */
    if (origin == FT_EDIT_LOCAL) return;
    int doc_idx = (int)(intptr_t)ctx;
    if (doc_idx < 0 || doc_idx >= MAX_CHATS) return;
    ft_edit_resolve_cursor(&g_edits[doc_idx]);
}

/* Public IP discovery + periodic on-chain re-registration state */
static uint32_t g_public_ip = 0;
static time_t   g_last_register_time = 0;
static bool     g_ip_discovery_pending = false;
static char     g_upnp_control_url[512] = {0};
static FtUpnpAsyncCtx g_upnp_async_ctx;

/* Determine the local IP address the OS would use for the default route. */
static uint32_t get_default_local_ip(void)
{
#ifdef _WIN32
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) return 0;
    struct sockaddr_in remote;
    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_addr.s_addr = inet_addr("8.8.8.8");
    remote.sin_port = htons(53);
    if (connect(sock, (struct sockaddr *)&remote, sizeof(remote)) == 0) {
        struct sockaddr_in local;
        int len = sizeof(local);
        if (getsockname(sock, (struct sockaddr *)&local, &len) == 0) {
            closesocket(sock);
            return local.sin_addr.s_addr;
        }
    }
    closesocket(sock);
#endif
    return 0;
}

typedef struct {
    FtIdentity *id;
    FtNetworkState *net_state;
    uint16_t port;
} IpRegisterCtx;

static void on_public_ip_discovered(FtAsyncHttpRequest *req, void *ctx,
                                    int status, const char *response)
{
    (void)req;
    g_ip_discovery_pending = false;
    if (status == 200 && response) {
        char ip_str[32];
        strncpy(ip_str, response, sizeof(ip_str) - 1);
        ip_str[sizeof(ip_str) - 1] = '\0';
        /* trim trailing whitespace */
        char *end = ip_str + strlen(ip_str) - 1;
        while (end > ip_str && (*end == '\n' || *end == '\r' || *end == ' ')) {
            *end = '\0';
            end--;
        }
        uint32_t ip = inet_addr(ip_str);
        if (ip != INADDR_NONE && ip != 0) {
            g_public_ip = ip;
            IpRegisterCtx *rctx = (IpRegisterCtx *)ctx;
            if (rctx && rctx->id && rctx->net_state) {
                uint32_t local_ip = get_default_local_ip();
                uint32_t local_ip_host = 0;
                if (local_ip != 0) {
                    local_ip_host = ntohl(local_ip);
                }
                FtResolver *resolver = ft_registry_resolver();
                ft_resolver_register(resolver, rctx->id, ntohl(g_public_ip), rctx->port,
                                     local_ip_host, rctx->port, rctx->net_state);
                g_last_register_time = time(NULL);
                main_log( "Discovered public IP: %s, registering on-chain (public+local)\n", ip_str);
            }
        } else {
            main_log( "Public IP discovery returned invalid IP: %s\n", ip_str);
        }
    } else {
        main_log( "Public IP discovery failed (status=%d)\n", status);
    }
}

/* Queue the UPnP warning modal in the UI. Called from the main loop thread. */
static void show_upnp_fallback_warning(FtVulkanUI *ui, const char *detailed_error)
{
    (void)detailed_error;
    if (!ui->upnp_warning_shown) {
        ui->upnp_warning_shown = true;
        modal_push(&ui->modalManager, MODAL_UPNP_WARNING);
    }
}

/* Start HTTP fallback for public IP discovery.
   Called from main loop when UPnP async fails or is skipped. */
static void start_http_fallback_discovery(FtIdentity *id, FtNetworkState *ns, uint16_t port)
{
    static IpRegisterCtx ctx;
    ctx.id = id;
    ctx.net_state = ns;
    ctx.port = port;
    if (ft_async_http_get("https://api.ipify.org", on_public_ip_discovered, &ctx) == 0) {
        g_ip_discovery_pending = true;
    } else {
        main_log( "Failed to start public IP discovery\n");
        ns->register_status = FT_NET_STATUS_ERROR;
    }
}

/* Try UPnP first (self-reliant, asks our own router).
   If UPnP is disabled or fails, fall back to external HTTP service.
   UPnP discovery is now a state machine using non-blocking UDP + async HTTP.
   No threads — everything is polled on the main loop. */
static void trigger_public_ip_discovery(FtIdentity *id, FtNetworkState *ns, uint16_t port)
{
    if (g_ip_discovery_pending) return;

    /* Mark registration as in-progress so the status bar shows "Connecting" */
    ns->register_status = FT_NET_STATUS_PENDING;

    /* If we already have a cached UPnP control URL (e.g. 24h re-registration),
       skip SSDP/discovery and go straight to SOAP GetExternalIPAddress. */
    if (g_upnp_control_url[0] != '\0') {
        uint32_t local_ip = get_default_local_ip();
        if (ft_upnp_async_start_cached(&g_upnp_async_ctx, g_upnp_control_url,
                                       port, local_ip) == 0) {
            g_ip_discovery_pending = true;
            main_log( "UPnP re-registration started (async)\n");
            return;
        }
        /* Cached start failed — clear and fall through to full discovery */
        g_upnp_control_url[0] = '\0';
    }

    /* Start full async UPnP discovery (SSDP + device description + SOAP) */
    uint32_t local_ip = get_default_local_ip();
    if (ft_upnp_async_start(&g_upnp_async_ctx, port, local_ip) == 0) {
        g_ip_discovery_pending = true;
        main_log( "UPnP discovery started (async state machine)\n");
    } else {
        main_log( "UPnP async start failed, falling back to HTTP\n");
        start_http_fallback_discovery(id, ns, port);
    }
}

static void on_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

static const char *get_home_dir(void) {
#ifdef _WIN32
    const char *home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
    if (!home) {
        const char *homedrive = getenv("HOMEDRIVE");
        const char *homepath = getenv("HOMEPATH");
        if (homedrive && homepath) {
            static char buf[MAX_PATH];
            snprintf(buf, sizeof(buf), "%s%s", homedrive, homepath);
            return buf;
        }
    }
    return home ? home : ".";
#else
    const char *home = getenv("HOME");
    return home ? home : ".";
#endif
}

static void get_binary_dir(char *out, size_t out_size) {
#ifdef _WIN32
    char exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        char *last_slash = strrchr(exe_path, '\\');
        if (last_slash) {
            *last_slash = '\0';
        }
        snprintf(out, out_size, "%s", exe_path);
    } else {
        snprintf(out, out_size, ".");
    }
#else
    char exe_path[512];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        char *last_slash = strrchr(exe_path, '/');
        if (last_slash) {
            *last_slash = '\0';
        }
        snprintf(out, out_size, "%s", exe_path);
    } else {
        snprintf(out, out_size, ".");
    }
#endif
}

static void ensure_dir(const char *path) {
#ifdef _WIN32
    CreateDirectoryA(path, NULL);
#else
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", path);
    system(cmd);
#endif
}

/* Map Win32 VK codes to the GLFW keycodes the UI expects */
static int map_vk_to_glfw_key(int vk) {
    switch (vk) {
        case VK_ESCAPE:   return 256;
        case VK_RETURN:   return 257;
        case VK_TAB:      return 258;
        case VK_BACK:     return 259;
        case VK_DELETE:   return 261;
        case VK_RIGHT:    return 262;
        case VK_LEFT:     return 263;
        case VK_DOWN:     return 264;
        case VK_UP:       return 265;
        case '0': return 48;
        case '1': return 49;
        case '2': return 50;
        case '3': return 51;
        case '4': return 52;
        case '5': return 53;
        case '6': return 54;
        case '7': return 55;
        case '8': return 56;
        case '9': return 57;
        case 'A': case 'a': return 65;
        default: return vk;
    }
}

static int get_glfw_mods(void) {
    int mods = 0;
    if (GetKeyState(VK_SHIFT)   & 0x8000) mods |= 0x0001; /* GLFW_MOD_SHIFT */
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= 0x0002; /* GLFW_MOD_CONTROL */
    if (GetKeyState(VK_MENU)    & 0x8000) mods |= 0x0004; /* GLFW_MOD_ALT */
    if (GetKeyState(VK_LWIN)    & 0x8000) mods |= 0x0008; /* GLFW_MOD_SUPER */
    if (GetKeyState(VK_RWIN)    & 0x8000) mods |= 0x0008;
    return mods;
}

static int detect_subpixel_mode(void) {
    /* Pixel font + NEAREST looks best with grayscale; subpixel modes
       are kept available via F5 for users who want to experiment. */
    printf("Subpixel mode: grayscale (default for pixel font)\n");
    return 0;
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            int vk = (int)wParam;
            /* F5 cycles subpixel modes: grayscale -> RGB -> BGR */
            if (vk == VK_F5) {
                g_subpixelMode = (g_subpixelMode + 1) % 3;
                const char *modeName = (g_subpixelMode == 0) ? "grayscale" :
                                       (g_subpixelMode == 1) ? "RGB" : "BGR";
                printf("Subpixel mode: %s\n", modeName);
                return 0;
            }
            /* F6 captures framebuffer and diagnoses subpixel pattern */
            if (vk == VK_F6) {
                if (g_ui && g_ui->renderer) {
                    ft_vk_renderer_capture_framebuffer(g_ui->renderer, "freetext_capture.bmp");
                    printf("Capture scheduled for next frame...\n");
                }
                return 0;
            }
            int key = map_vk_to_glfw_key(vk);
            int mods = get_glfw_mods();
            if (g_ui) {
                ft_vulkan_ui_handle_key(g_ui, key, mods);
            }
            /* Don't let DefWindowProc beep on unhandled keys */
            if (msg == WM_SYSKEYDOWN && (vk == VK_MENU || vk == VK_F10))
                return 0;
            break;
        }
        case WM_CHAR: {
            if (g_ui && wParam >= 32) {
                ft_vulkan_ui_handle_char(g_ui, (unsigned int)wParam);
            }
            return 0;
        }
        case WM_ACTIVATE: {
            g_window_focused = (LOWORD(wParam) != WA_INACTIVE);
            break;
        }
        case WM_MOUSEACTIVATE: {
            if (!g_window_focused && LOWORD(lParam) == HTCLIENT) {
                g_ignore_next_click = 1;
            } else {
                g_ignore_next_click = 0;
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            if (g_ignore_next_click) {
                g_ignore_next_click = 0;
                SetCapture(hwnd);
                return 0;
            }
            if (g_ui) {
                int x = GET_X_LPARAM(lParam);
                int y = GET_Y_LPARAM(lParam);
                ft_vulkan_ui_handle_mouse_press(g_ui, (double)x, (double)y);
            }
            SetCapture(hwnd);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (g_ignore_next_click) {
                g_ignore_next_click = 0;
                ReleaseCapture();
                return 0;
            }
            if (g_ui) {
                int x = GET_X_LPARAM(lParam);
                int y = GET_Y_LPARAM(lParam);
                ft_vulkan_ui_handle_mouse_release(g_ui, (double)x, (double)y);
            }
            ReleaseCapture();
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (g_ui) {
                int x = GET_X_LPARAM(lParam);
                int y = GET_Y_LPARAM(lParam);
                ft_vulkan_ui_handle_mouse_move(g_ui, (double)x, (double)y);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (g_ui) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(hwnd, &pt);
                ft_vulkan_ui_handle_scroll(g_ui, 0.0, (double)delta / (double)WHEEL_DELTA);
            }
            return 0;
        }
        case WM_SIZE: {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);
            if (g_ui && g_ui->renderer && width > 0 && height > 0) {
                g_ui->renderer->windowWidth = width;
                g_ui->renderer->windowHeight = height;
                ft_vk_renderer_recreate_swapchain(g_ui->renderer);
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            if (g_ui) {
                ft_vulkan_ui_render(g_ui);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE: {
            g_running = 0;
            return 0;
        }
        case WM_DESTROY: {
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* Parse ip:port strings like "127.0.0.1:7778" into sockaddr_in.
   Returns 0 on success, -1 on parse error. */
static int parse_peer_addr(const char *s, struct sockaddr_in *out)
{
    char buf[64];
    strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *colon = strrchr(buf, ':');
    if (!colon) return -1;
    *colon = '\0';
    int port = atoi(colon + 1);
    if (port <= 0 || port > 65535) return -1;

    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons((uint16_t)port);
#ifdef _WIN32
    out->sin_addr.s_addr = inet_addr(buf);
    if (out->sin_addr.s_addr == INADDR_NONE) return -1;
#else
    if (inet_pton(AF_INET, buf, &out->sin_addr) != 1) return -1;
#endif
    return 0;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    /* Detach the default console window and redirect stdout/stderr to a log file
       so the app runs as a GUI-only process. Falls back to NUL if log file fails. */
    {
        char binary_dir[512];
        char log_path[576];
        char data_dir[576];
        get_binary_dir(binary_dir, sizeof(binary_dir));
        snprintf(data_dir, sizeof(data_dir), "%s/freetext_data", binary_dir);
        snprintf(log_path, sizeof(log_path), "%s/output.log", data_dir);
        CreateDirectoryA(data_dir, NULL); /* ok if already exists */
        FILE *out_fp = NULL, *err_fp = NULL;
        if (freopen_s(&out_fp, log_path, "a", stdout) != 0 || !out_fp) {
            freopen_s(&out_fp, "NUL", "w", stdout);
        }
        if (freopen_s(&err_fp, log_path, "a", stderr) != 0 || !err_fp) {
            freopen_s(&err_fp, "NUL", "w", stderr);
        }
        time_t now = time(NULL);
        main_log( "\n--- FreeText started %s", ctime(&now));
        fflush(stderr);
        FreeConsole();
    }

    /* Vectored handler catches exceptions before any SEH frames,
       including crashes inside window message loops. */
    AddVectoredExceptionHandler(1, fatal_exception_handler);
    /* Also set unhandled filter as fallback for non-vectored paths */
    SetUnhandledExceptionFilter(fatal_exception_handler);
    signal(SIGABRT, crash_signal_handler);
    signal(SIGILL, crash_signal_handler);
    signal(SIGFPE, crash_signal_handler);
    set_terminate(crash_terminate_handler);
#endif
    signal(SIGINT, on_sigint);

    uint16_t port = 7776;
    const char *display_name = "User";
    if (argc > 1) port = (uint16_t)atoi(argv[1]);
    if (argc > 2) display_name = argv[2];

    /* Bootstrap peers from remaining args: ip:port ip:port ... */
    struct sockaddr_in bootstrap_peers[8];
    int bootstrap_peer_count = 0;
    for (int i = 3; i < argc && bootstrap_peer_count < 8; i++) {
        if (parse_peer_addr(argv[i], &bootstrap_peers[bootstrap_peer_count]) == 0) {
            bootstrap_peer_count++;
        } else {
            main_log( "Warning: ignoring invalid peer address '%s'\n", argv[i]);
        }
    }

    printf("FreeText Vulkan starting on port %d as '%s'...\n", port, display_name);

    g_subpixelMode = detect_subpixel_mode();

    if (ft_crypto_init() != 0) {
        main_log( "Failed to initialize crypto\n");
        return 1;
    }

    if (ft_async_http_init() != 0) {
        main_log( "Failed to initialize async HTTP\n");
        return 1;
    }

#ifdef FT_SEPOLIA
    registry_use_testnet(true);
    printf("Sepolia testnet mode enabled (chain ID %lu)\n", (unsigned long)registry_chain_id());
#endif

    /* Data directory lives next to the binary for easy multi-account portability */
    char binary_dir[512];
    get_binary_dir(binary_dir, sizeof(binary_dir));
    char data_dir[512];
    snprintf(data_dir, sizeof(data_dir), "%s/freetext_data", binary_dir);

    char keys_dir[512];
    char l2_dir[512];
    snprintf(keys_dir, sizeof(keys_dir), "%s/keys", data_dir);
    snprintf(l2_dir, sizeof(l2_dir), "%s/l2_client", data_dir);
    ensure_dir(data_dir);
    ensure_dir(keys_dir);
    ensure_dir(l2_dir);

    FtIdentity id = {};
    char keypath[512];
    snprintf(keypath, sizeof(keypath), "%s/keys/local.identity", data_dir);

    bool identity_existed = false;
    if (ft_identity_load(&id, keypath, NULL) != 0) {
        printf("Generating new identity...\n");
        if (ft_identity_generate(&id, display_name) != 0) {
            main_log( "Failed to generate identity\n");
            return 1;
        }
        ft_identity_save(&id, keypath, NULL);
        printf("Identity: %s\n", id.hash_hex);
    } else {
        identity_existed = true;
        printf("Loaded identity: %s\n", id.hash_hex);
    }

    /* Save Ethereum address to file */
    {
        char eth_addr[43];
        ft_identity_eth_address(&id, eth_addr);
        char ethpath[512];
#ifdef FT_SEPOLIA
        snprintf(ethpath, sizeof(ethpath), "%s/eth_address_sepolia", data_dir);
#else
        snprintf(ethpath, sizeof(ethpath), "%s/eth_address", data_dir);
#endif
        FILE *f = fopen(ethpath, "w");
        if (f) {
            fprintf(f, "%s\n", eth_addr);
            fclose(f);
            printf("Saved ETH address to %s\n", ethpath);
        } else {
            main_log( "Failed to save ETH address to %s\n", ethpath);
        }
    }
    FtNet net;
    if (ft_net_init(&net, port) != 0) {
        main_log( "Failed to bind UDP port %d (tried %d-%d)\n", port, port, port + 99);
        return 1;
    }
    main_log( "Bound to UDP port %d\n", net.listen_port);

    /* Win32 window creation */

    HINSTANCE hInstance = GetModuleHandleW(NULL);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"FreeTextWindowClass";
    if (!RegisterClassExW(&wc)) {
        main_log( "Failed to register window class\n");
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0,
        L"FreeTextWindowClass",
        L"FreeText",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1024, 768,
        NULL, NULL, hInstance, NULL);
    if (!hwnd) {
        main_log( "Failed to create window\n");
        return 1;
    }
    g_hwnd = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    VkInstance instance = VK_NULL_HANDLE;
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "FreeText";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "NoEngine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    const char *extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = 2;
    createInfo.ppEnabledExtensionNames = extensions;

    // Vulkan loader is linked via vulkan-1.lib

    VkResult result = vkCreateInstance(&createInfo, NULL, &instance);
    if (result != VK_SUCCESS) {
        main_log( "Failed to create Vulkan instance: %d\n", result);
        DestroyWindow(hwnd);
        return 1;
    }

    // Instance functions are resolved by the Vulkan loader automatically

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkWin32SurfaceCreateInfoKHR surfaceCreateInfo = {};
    surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceCreateInfo.hinstance = hInstance;
    surfaceCreateInfo.hwnd = hwnd;
    if (vkCreateWin32SurfaceKHR(instance, &surfaceCreateInfo, NULL, &surface) != VK_SUCCESS) {
        main_log( "Failed to create window surface\n");
        vkDestroyInstance(instance, NULL);
        DestroyWindow(hwnd);
        return 1;
    }

#include "shaders/spv_vert.h"
#include "shaders/spv_frag.h"

    FtVulkanRenderer renderer = {};
    if (!ft_vk_renderer_init(&renderer, instance, surface,
                             triangle_vert_spv, triangle_vert_spv_len,
                             triangle_frag_spv, triangle_frag_spv_len)) {
        main_log( "Failed to initialize Vulkan renderer\n");
        vkDestroySurfaceKHR(instance, surface, NULL);
        vkDestroyInstance(instance, NULL);
        DestroyWindow(hwnd);
        return 1;
    }

    FtVulkanUI ui = {};
    ft_vulkan_ui_init(&ui, &renderer, NULL, NULL, &net);
    strncpy(ui.dataDir, data_dir, sizeof(ui.dataDir) - 1);
    ui.dataDir[sizeof(ui.dataDir) - 1] = '\0';
    ft_vulkan_ui_startup_init(&ui, &id);

    FtNetworkState network_state = {0};
    ft_network_state_init(&network_state);
    ui.networkState = &network_state;
    if (identity_existed) {
        if (id.first_name[0] && id.last_name[0]) {
            ui.state = FT_STARTUP_ONLINE;
            ui.networking_enabled = true;
        } else {
            /* Identity exists but names are missing — go through onboarding */
            modal_push(&ui.modalManager, MODAL_STARTUP);
            ui.state = FT_STARTUP_ASK_MODE;
        }
    } else {
        /* New identity — go through full onboarding */
        modal_push(&ui.modalManager, MODAL_STARTUP);
        ui.state = FT_STARTUP_ASK_MODE;
    }
    g_ui = &ui;
    ft_upnp_async_init(&g_upnp_async_ctx);

    FtUserTable user_table;
    ft_user_table_init(&user_table);
    {
        char peer_cache_path[512];
        snprintf(peer_cache_path, sizeof(peer_cache_path), "%s/peers.cache", data_dir);
        ft_user_table_load(&user_table, peer_cache_path);
        strncpy(ui.peerCachePath, peer_cache_path, sizeof(ui.peerCachePath) - 1);
        ui.peerCachePath[sizeof(ui.peerCachePath) - 1] = '\0';
    }
    ui.userTable = &user_table;

    FtDocument *doc_ptrs[MAX_CHATS];
    for (int i = 0; i < MAX_CHATS; i++) doc_ptrs[i] = &g_docs[i];

    /* Wire document change observers before gossip starts */
    for (int i = 0; i < MAX_CHATS; i++) {
        g_docs[i].on_change = on_doc_changed;
        g_docs[i].on_change_ctx = (void *)(intptr_t)i;
    }

    FtKeyManager *key_manager = ft_key_manager_new(g_doc_crypto, MAX_CHATS);
    FtGossip gossip;
    FtResolver *resolver = ft_registry_resolver();
    ft_gossip_init(&gossip, &net, &id, doc_ptrs, MAX_CHATS, key_manager, &user_table, id.hash_hex,
                   resolver);
    ft_gossip_set_data_dir(&gossip, ui.dataDir);
    ft_gossip_load_deleted_docs(&gossip);

    /* Open heartbeat log file (truncated on each startup) */
    {
        char hb_path[512];
        snprintf(hb_path, sizeof(hb_path), "%s/heartbeat.log", ui.dataDir);
#ifdef _WIN32
        /* Normalize to backslashes for Windows fopen */
        for (char *p = hb_path; *p; p++) {
            if (*p == '/') *p = '\\';
        }
#endif
        gossip.heartbeat_log = fopen(hb_path, "w");
        if (gossip.heartbeat_log) {
            fprintf(gossip.heartbeat_log, "=== Heartbeat log started ===\n");
            fflush(gossip.heartbeat_log);
            main_log( "[main] heartbeat log opened: %s\n", hb_path);
        } else {
            main_log( "[main] FAILED to open heartbeat log: %s (errno=%d)\n", hb_path, errno);
        }
    }

    /* Callback to auto-create a document when receiving a key/block/delta
       for an unknown doc_id (e.g., a friend added us to their note). */
    gossip.create_doc_cb = [](const char *doc_id, const char *creator_hash_hex, void *ctx) -> int {
        FtVulkanUI *ui = (FtVulkanUI *)ctx;
        return ft_vulkan_ui_add_network_chat_from_gossip(ui, doc_id, creator_hash_hex);
    };
    gossip.create_doc_ctx = &ui;

    ui.identity = &id;
    ui.gossip = &gossip;

    /* Load existing notes from data directory */
    ft_vulkan_ui_load_chats(&ui, id.hash_hex);

    /* No notes yet — prompt user to create the first one */
    if (ui.chatCount == 0 && !modal_is_open(&ui.modalManager)) {
        modal_push(&ui.modalManager, MODAL_NEW_NOTE);
    }

    bool networking_initialized = false;

    printf("Vulkan UI ready. Close window to quit.\n");

    fd_set readfds;
    struct timeval tv;
    uint8_t netbuf[8192];
    struct sockaddr_in from;
    size_t netlen;

    while (g_running) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = 0;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running) break;

        /* Startup flow: poll balance, transition to ONLINE when funded */
        if (ft_vulkan_ui_startup_tick(&ui)) {
            if (modal_top(&ui.modalManager) == MODAL_STARTUP && ft_vulkan_ui_startup_online(&ui)) {
                modal_close(&ui.modalManager, MODAL_STARTUP);
                /* Only auto-create a note if the user has no existing notes */
                if (ui.chatCount == 0) {
                    const char *display_name = (id.display_name[0]) ? id.display_name : "User";
                    const char *local_hash = id.hash_hex;
                    ft_vulkan_ui_add_chat(&ui, display_name, local_hash, true);
                }
            }
        }

        /* Initialize networking once when user goes online */
        if (ft_vulkan_ui_startup_online(&ui) && !networking_initialized) {
            networking_initialized = true;

            /* NOTE: We intentionally do NOT reconnect to cached peers here.
               registry_resolve_endpoint() does synchronous on-chain HTTP lookups
               that can freeze the UI for seconds (especially on Sepolia testnet
               where RPC endpoints are often dead). Peer discovery happens
               lazily via incoming HELOs, peer exchange, and DNS bootstrap. */

            /* Add bootstrap peers supplied on command line and send HELOs
               so they can discover us immediately (essential for local testing). */
            for (int i = 0; i < bootstrap_peer_count; i++) {
                ft_net_add_peer(&net, &bootstrap_peers[i], NULL);
                ft_gossip_send_helo(&gossip, &bootstrap_peers[i]);
            }

            /* DNS bootstrap if no peers yet */
            if (net.peer_count == 0) {
                ft_dns_bootstrap_query(&net, "_freetext._udp.bootstrap.freetext.chat");
            }

            /* Discover public IP and register on-chain.
               Local fallback (127.0.0.1) is used only if discovery fails.
               Use net.listen_port (the actually bound port) rather than the
               requested port, so multiple instances on the same machine each
               register their own unique port. */
            trigger_public_ip_discovery(&id, &network_state, net.listen_port);

            ft_vulkan_ui_set_status(&ui, "Networking enabled — deposit confirmed");
        }

        /* Only process network when online */
        if (ft_vulkan_ui_startup_online(&ui)) {
            /* Re-register endpoint every 24 hours so the on-chain record
               stays fresh even if our public IP changes. */
            if (g_last_register_time > 0 && !g_ip_discovery_pending) {
                time_t now = time(NULL);
                if (now - g_last_register_time >= 86400) {
                    trigger_public_ip_discovery(&id, &network_state, net.listen_port);
                }
            }

            FD_ZERO(&readfds);
            FD_SET((SOCKET)net.sockfd, &readfds);
            tv.tv_sec = 0;
            tv.tv_usec = 0;
            int ret = select(net.sockfd + 1, &readfds, NULL, NULL, &tv);
            if (ret > 0 && FD_ISSET(net.sockfd, &readfds)) {
                int r = ft_net_recv(&net, &from, netbuf, sizeof(netbuf), &netlen);
                if (r > 0) {
                    ft_gossip_handle_packet(&gossip, &from, netbuf, netlen);
                }
            }
            ft_gossip_tick(&gossip);
        }

        /* Poll async UPnP state machine (SSDP + HTTP callbacks, no threads).
           Tick first so SSDP socket is polled; HTTP callbacks run in ft_async_http_poll(). */
        if (g_ip_discovery_pending) {
            FtUpnpAsyncState upnp_state = ft_upnp_async_tick(&g_upnp_async_ctx);
            if (upnp_state == FT_UPNP_ASYNC_SUCCESS || upnp_state == FT_UPNP_ASYNC_ERROR) {
                g_ip_discovery_pending = false;
                ft_upnp_async_cleanup(&g_upnp_async_ctx);
                if (upnp_state == FT_UPNP_ASYNC_SUCCESS) {
                    g_public_ip = g_upnp_async_ctx.public_ip;
                    if (g_upnp_async_ctx.control_url[0]) {
                        strncpy(g_upnp_control_url, g_upnp_async_ctx.control_url,
                                sizeof(g_upnp_control_url) - 1);
                        g_upnp_control_url[sizeof(g_upnp_control_url) - 1] = '\0';
                    }
                    uint32_t local_ip_host = 0;
                    if (g_upnp_async_ctx.local_ip != 0) {
                        local_ip_host = ntohl(g_upnp_async_ctx.local_ip);
                    }
                    FtResolver *resolver = ft_registry_resolver();
                    ft_resolver_register(resolver, &id, ntohl(g_public_ip), net.listen_port,
                                         local_ip_host, net.listen_port, &network_state);
                    g_last_register_time = time(NULL);
                    struct in_addr ia;
                    ia.s_addr = g_public_ip;
                    main_log( "UPnP public IP: %s, registering on-chain (public+local)\n", inet_ntoa(ia));
                } else {
                    main_log( "UPnP async failed, falling back to HTTP discovery\n");
                    if (g_ui) {
                        show_upnp_fallback_warning(g_ui,
                            "No UPnP Internet Gateway Device responded on the local network. "
                            "Your router may have UPnP disabled.");
                    }
                    start_http_fallback_discovery(&id, &network_state, net.listen_port);
                }
            }
        }

        /* Pump async HTTP completions — invokes callbacks on main thread.
           UPnP SOAP callbacks run here and transition the state machine. */
        ft_async_http_poll();

        /* Poll async friend endpoint resolves from on-chain registry */
        ft_gossip_poll_friend_resolves(&gossip, &network_state);

        ft_vulkan_ui_render(&ui);
    }

    printf("\nSaving...\n");
    for (int i = 0; i < ui.chatCount; i++) {
        ft_doc_save(ui.chatDocs[i], &g_doc_crypto[i]);
        ft_doc_free(ui.chatDocs[i]);
    }
    {
        char peer_cache_path[512];
        snprintf(peer_cache_path, sizeof(peer_cache_path), "%s/peers.cache", data_dir);
        ft_user_table_save(&user_table, peer_cache_path);
    }
    ft_net_shutdown(&net);
    ft_crypto_shutdown();
    ft_async_http_shutdown();

    ft_vk_renderer_cleanup(&renderer);
    vkDestroySurfaceKHR(instance, surface, NULL);
    vkDestroyInstance(instance, NULL);
    DestroyWindow(hwnd);
    return 0;
}
