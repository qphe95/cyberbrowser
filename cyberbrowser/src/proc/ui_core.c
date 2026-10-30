/* ui_core.c - the UI's reading of a tab's messages (ui_core.h).  No library
 * calls, bounded loops: proved with Frama-C/WP (tools/prove.sh ui). */
#include "ui_core.h"

/*@ requires \valid_read(p + (0 .. 3));
    assigns \nothing;
    ensures \result == u32le(p);
 */
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] + (uint32_t)p[1] * 0x100u + (uint32_t)p[2] * 0x10000u + (uint32_t)p[3] * 0x1000000u;
}

uint16_t ui_tab_message(uint16_t type, const uint8_t *data, uint32_t len, UiTabMsg *out) {
    switch (type) {
    case TU_FRAME: {
        if (len != sizeof(TuFrame)) return 0;
        uint32_t w = rd32(data + 8), h = rd32(data + 12);
        if (w == 0 || h == 0 || w > SURFACE_MAX_W || h > SURFACE_MAX_H) return 0;
        out->frame.frame = rd32(data);
        out->frame.unused = rd32(data + 4);
        out->frame.width = w;
        out->frame.height = h;
        out->frame.scroll_y = rd32(data + 16);
        out->frame.content_height = rd32(data + 20);
        return TU_FRAME;
    }
    case TU_TITLE: {
        if (len > PROTO_TITLE_MAX) return 0;
        /*@ loop invariant 0 <= i <= len;
            loop invariant \forall integer k; 0 <= k < i ==> out->text[k] == (char)data[k];
            loop assigns i, out->text[0 .. PROTO_TITLE_MAX];
            loop variant len - i;
         */
        for (uint32_t i = 0; i < len; i++) out->text[i] = (char)data[i];
        out->text[len] = 0;
        out->text_len = len;
        return TU_TITLE;
    }
    case TU_STATE: {
        if (len != sizeof(TuState)) return 0;
        uint32_t s = rd32(data);
        out->state = s == TAB_LOADED || s == TAB_LOAD_FAILED ? s : (uint32_t)TAB_LOADING;
        return TU_STATE;
    }
    case TU_WANT_WEBGPU:
        return len == 0 ? (uint16_t)TU_WANT_WEBGPU : 0;
    case TU_NAVIGATE: {
        if (len <= sizeof(TuNavigate) || len > sizeof(TuNavigate) + PROTO_URL_MAX) return 0;
        uint32_t n = len - (uint32_t)sizeof(TuNavigate);
        const uint8_t *url = data + sizeof(TuNavigate);
        /*@ loop invariant 0 <= i <= n;
            loop invariant \forall integer k; 0 <= k < i ==> out->text[k] == (char)url[k] && out->text[k] != 0;
            loop assigns i, out->text[0 .. PROTO_URL_MAX];
            loop variant n - i;
         */
        for (uint32_t i = 0; i < n; i++) {
            if (!url[i]) return 0;          /* a NUL would cut the URL short */
            out->text[i] = (char)url[i];
        }
        out->text[n] = 0;
        out->text_len = n;
        out->new_tab = rd32(data) ? 1 : 0;
        return TU_NAVIGATE;
    }
    default:
        return 0;
    }
}

void ui_gesture(UiGesture *g, uint32_t now) {
    g->tick = now;
    g->pending = 1;
}

bool ui_may_open_tab(UiGesture *g, uint32_t now) {
    if (!g->pending || now - g->tick > UI_GESTURE_MS) return false;
    g->pending = 0;
    return true;
}
