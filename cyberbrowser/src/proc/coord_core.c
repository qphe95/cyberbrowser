/* coord_core.c - the coordinator's decisions (coord_core.h).  No I/O, no
 * library calls, no allocation, bounded loops: kept provable. */
#include "coord_core.h"

static void copy_bytes(uint8_t *dst, const uint8_t *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
}

static bool prefix(const uint8_t *s, uint32_t len, const char *p) {
    uint32_t i = 0;
    for (; p[i]; i++)
        if (i >= len || s[i] != (uint8_t)p[i]) return false;
    return true;
}

bool coord_url_ok(const uint8_t *url, uint32_t len) {
    if (len == 0 || len > PROTO_URL_MAX) return false;
    for (uint32_t i = 0; i < len; i++)
        if (url[i] <= 0x20 || url[i] >= 0x7f) return false;
    if (len == 11 && prefix(url, len, "about:blank")) return true;
    return (len > 7 && prefix(url, len, "http://")) || (len > 8 && prefix(url, len, "https://"));
}

static CoordAction *add(CoordActions *out, CoordActionKind kind) {
    if (out->n >= COORD_MAX_ACTIONS) return 0;
    CoordAction *a = &out->a[out->n++];
    a->kind = kind;
    a->slot = -1;
    a->tab = a->width = a->height = 0;
    a->site[0] = 0;
    a->type = 0;
    a->len = 0;
    return a;
}

static void send_ui(CoordActions *out, uint16_t type, const void *data, uint32_t len) {
    if (len > PROTO_URL_MAX) return;
    CoordAction *a = add(out, CA_SEND_UI);
    if (!a) return;
    a->type = type;
    a->len = len;
    copy_bytes(a->data, (const uint8_t *)data, len);
}

static void tab_failed(CoordActions *out, uint32_t tab) {
    UcTab t;
    t.tab = tab;
    send_ui(out, CU_TAB_FAILED, &t, sizeof(t));
}

static int slot_of_tab(const CoordState *s, uint32_t tab) {
    for (int i = 0; i < COORD_MAX_TABS; i++)
        if (s->slots[i].state != SLOT_FREE && s->slots[i].tab == tab) return i;
    return -1;
}

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void coord_init(CoordState *s) {
    for (int i = 0; i < COORD_MAX_TABS; i++) {
        s->slots[i].state = SLOT_FREE;
        s->slots[i].tab = 0;
        s->slots[i].site[0] = 0;
    }
    s->exiting = false;
}

static void exit_all(CoordState *s, CoordActions *out) {
    for (int i = 0; i < COORD_MAX_TABS; i++) {
        if (s->slots[i].state == SLOT_STARTING || s->slots[i].state == SLOT_RUNNING) {
            CoordAction *a = add(out, CA_KILL_TAB);
            if (a) a->slot = i;
            s->slots[i].state = SLOT_CLOSING;
        }
    }
    add(out, CA_EXIT);
    s->exiting = true;
}

static void ui_message(CoordState *s, const CoordEvent *e, CoordActions *out) {
    switch (e->type) {
    case UC_OPEN_TAB: {
        if (e->len < 12) return;                    /* sizeof(UcOpenTab) */
        uint32_t tab = read_u32(e->data), w = read_u32(e->data + 4), h = read_u32(e->data + 8);
        const uint8_t *url = e->data + 12;
        uint32_t url_len = e->len - 12;
        if (!coord_url_ok(url, url_len) || slot_of_tab(s, tab) >= 0) {
            tab_failed(out, tab);
            return;
        }
        /* Its site: the label of the process, the badge of its channels.
         * about:blank is opaque (""): no site, no storage. */
        char site[SITE_MAX];
        site[0] = 0;
        if (!(url_len == 11 && prefix(url, url_len, "about:blank"))) {
            char scheme[8], host[SITE_HOST_MAX + 1];
            if (!site_url_host(url, url_len, scheme, sizeof(scheme), host, sizeof(host)) || !site_of(scheme, host, site)) {
                tab_failed(out, tab);
                return;
            }
        }
        if (w == 0 || w > SURFACE_MAX_W) w = SURFACE_MAX_W;
        if (h == 0 || h > SURFACE_MAX_H) h = SURFACE_MAX_H;
        int free_slot = -1;
        for (int i = 0; i < COORD_MAX_TABS && free_slot < 0; i++)
            if (s->slots[i].state == SLOT_FREE) free_slot = i;
        if (free_slot < 0) {
            tab_failed(out, tab);
            return;
        }
        CoordAction *a = add(out, CA_START_TAB);
        if (!a) return;
        s->slots[free_slot].state = SLOT_STARTING;
        s->slots[free_slot].tab = tab;
        copy_bytes((uint8_t *)s->slots[free_slot].site, (const uint8_t *)site, SITE_MAX);
        copy_bytes((uint8_t *)a->site, (const uint8_t *)site, SITE_MAX);
        a->slot = free_slot;
        a->tab = tab;
        a->width = w;
        a->height = h;
        a->len = url_len;
        copy_bytes(a->data, url, url_len);
        return;
    }
    case UC_CLOSE_TAB: {
        if (e->len != 4) return;
        int i = slot_of_tab(s, read_u32(e->data));
        if (i < 0 || s->slots[i].state == SLOT_CLOSING) return;
        s->slots[i].state = SLOT_CLOSING;
        CoordAction *a = add(out, CA_KILL_TAB);
        if (a) a->slot = i;
        return;
    }
    case UC_QUIT:
        exit_all(s, out);
        return;
    default:
        return;
    }
}

void coord_step(CoordState *s, const CoordEvent *e, CoordActions *out) {
    out->n = 0;
    if (s->exiting) return;
    bool slot_ok = e->slot >= 0 && e->slot < COORD_MAX_TABS;
    switch (e->kind) {
    case CE_UI_MESSAGE:
        ui_message(s, e, out);
        return;
    case CE_UI_EXITED:
    case CE_SERVICE_EXITED:
        exit_all(s, out);
        return;
    case CE_TAB_STARTED:
        if (slot_ok && s->slots[e->slot].state == SLOT_STARTING) s->slots[e->slot].state = SLOT_RUNNING;
        return;
    case CE_TAB_START_FAILED:
        if (!slot_ok || s->slots[e->slot].state == SLOT_FREE) return;
        if (s->slots[e->slot].state != SLOT_CLOSING) tab_failed(out, s->slots[e->slot].tab);
        s->slots[e->slot].state = SLOT_FREE;
        return;
    case CE_TAB_EXITED: {
        if (!slot_ok || s->slots[e->slot].state == SLOT_FREE) return;
        if (s->slots[e->slot].state != SLOT_CLOSING) {
            CuTabExited x;
            x.tab = s->slots[e->slot].tab;
            x.exit_code = e->exit_code;
            send_ui(out, CU_TAB_EXITED, &x, sizeof(x));
        }
        CoordAction *a = add(out, CA_TAB_GONE);
        if (a) {
            a->slot = e->slot;
            a->tab = s->slots[e->slot].tab;
        }
        s->slots[e->slot].state = SLOT_FREE;
        return;
    }
    }
}
