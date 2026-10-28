/* coord_core.c - the coordinator's decisions (coord_core.h).  No I/O, no
 * library calls, no allocation, bounded loops: proved with Frama-C/WP
 * (tools/prove.sh). */
#include "coord_core.h"

/*@ requires \valid(dst + (0 .. n - 1)) && \valid_read(src + (0 .. n - 1));
    requires \separated(dst + (0 .. n - 1), src + (0 .. n - 1));
    assigns dst[0 .. n - 1];
    ensures \forall integer k; 0 <= k < n ==> dst[k] == src[k];
 */
static void copy_bytes(uint8_t *dst, const uint8_t *src, uint32_t n) {
    /*@ loop invariant 0 <= i <= n;
        loop invariant \forall integer k; 0 <= k < i ==> dst[k] == src[k];
        loop assigns i, dst[0 .. n - 1];
        loop variant n - i;
     */
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
}

/* A site label, whole. */
/*@ requires \valid(dst + (0 .. SITE_MAX - 1)) && \valid_read(src + (0 .. SITE_MAX - 1));
    requires \separated(dst + (0 .. SITE_MAX - 1), src + (0 .. SITE_MAX - 1));
    assigns dst[0 .. SITE_MAX - 1];
    ensures \forall integer k; 0 <= k < SITE_MAX ==> dst[k] == src[k];
 */
static void copy_site(char *dst, const char *src) {
    /*@ loop invariant 0 <= i <= SITE_MAX;
        loop invariant \forall integer k; 0 <= k < i ==> dst[k] == src[k];
        loop assigns i, dst[0 .. SITE_MAX - 1];
        loop variant SITE_MAX - i;
     */
    for (uint32_t i = 0; i < SITE_MAX; i++) dst[i] = src[i];
}

/* Does s (len bytes) start with the n bytes of p. */
/*@ requires \valid_read(s + (0 .. len - 1)) && \valid_read(p + (0 .. n - 1));
    assigns \nothing;
    ensures \result ==> n <= len && \forall integer k; 0 <= k < n ==> s[k] == (uint8_t)p[k];
 */
static bool prefix(const uint8_t *s, uint32_t len, const char *p, uint32_t n) {
    if (n > len) return false;
    /*@ loop invariant 0 <= i <= n;
        loop invariant \forall integer k; 0 <= k < i ==> s[k] == (uint8_t)p[k];
        loop assigns i;
        loop variant n - i;
     */
    for (uint32_t i = 0; i < n; i++)
        if (s[i] != (uint8_t)p[i]) return false;
    return true;
}

bool coord_url_ok(const uint8_t *url, uint32_t len) {
    if (len == 0 || len > PROTO_URL_MAX) return false;
    /*@ loop invariant 0 <= i <= len;
        loop invariant \forall integer k; 0 <= k < i ==> 0x20 < url[k] < 0x7f;
        loop assigns i;
        loop variant len - i;
     */
    for (uint32_t i = 0; i < len; i++)
        if (url[i] <= 0x20 || url[i] >= 0x7f) return false;
    if (len == 11 && prefix(url, len, "about:blank", 11)) return true;
    return (len > 7 && prefix(url, len, "http://", 7)) || (len > 8 && prefix(url, len, "https://", 8));
}

/*@ requires \valid(out) && 0 <= out->n <= COORD_MAX_ACTIONS;
    assigns out->n, out->a[out->n];
    ensures \old(out->n) < COORD_MAX_ACTIONS ==>
        \result == &out->a[\old(out->n)] && out->n == \old(out->n) + 1 && \result->kind == kind &&
        \result->slot == -1 && \result->site[0] == 0 && !\result->grant_net && !\result->grant_store &&
        \result->len == 0;
    ensures \old(out->n) >= COORD_MAX_ACTIONS ==> \result == \null && out->n == \old(out->n);
 */
static CoordAction *add(CoordActions *out, CoordActionKind kind) {
    if (out->n >= COORD_MAX_ACTIONS) return 0;
    CoordAction *a = &out->a[out->n++];
    a->kind = kind;
    a->slot = -1;
    a->tab = a->width = a->height = 0;
    a->site[0] = 0;
    a->grant_net = a->grant_store = false;
    a->type = 0;
    a->len = 0;
    return a;
}

/*@ requires \valid(out) && 0 <= out->n <= COORD_MAX_ACTIONS;
    requires \valid_read(data + (0 .. len - 1));
    requires \separated(out, data + (0 .. len - 1));
    assigns out->n, out->a[out->n];
    ensures \old(out->n) <= out->n <= \old(out->n) + 1;
    ensures out->n > \old(out->n) ==>
        out->a[\old(out->n)].kind == CA_SEND_UI && out->a[\old(out->n)].len <= PROTO_URL_MAX;
 */
static void send_ui(CoordActions *out, uint16_t type, const uint8_t *data, uint32_t len) {
    if (len > PROTO_URL_MAX) return;
    CoordAction *a = add(out, CA_SEND_UI);
    if (!a) return;
    a->type = type;
    a->len = len;
    copy_bytes(a->data, data, len);
}

/*@ requires \valid(p + (0 .. 3));
    assigns p[0 .. 3];
 */
static void write_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/*@ requires \valid(out) && 0 <= out->n <= COORD_MAX_ACTIONS;
    assigns out->n, out->a[out->n];
    ensures \old(out->n) <= out->n <= \old(out->n) + 1;
    ensures out->n > \old(out->n) ==>
        out->a[\old(out->n)].kind == CA_SEND_UI && out->a[\old(out->n)].len <= PROTO_URL_MAX;
 */
static void tab_failed(CoordActions *out, uint32_t tab) {
    uint8_t t[4];                   /* UcTab */
    write_u32(t, tab);
    send_ui(out, CU_TAB_FAILED, t, sizeof(t));
}

/*@ requires \valid_read(s);
    assigns \nothing;
    ensures -1 <= \result < COORD_MAX_TABS;
    ensures \result >= 0 ==> s->slots[\result].state != SLOT_FREE && s->slots[\result].tab == tab;
    ensures \result < 0 ==> \forall integer i; 0 <= i < COORD_MAX_TABS && s->slots[i].state != SLOT_FREE ==>
        s->slots[i].tab != tab;
 */
static int slot_of_tab(const CoordState *s, uint32_t tab) {
    /*@ loop invariant 0 <= i <= COORD_MAX_TABS;
        loop invariant \forall integer k; 0 <= k < i && s->slots[k].state != SLOT_FREE ==> s->slots[k].tab != tab;
        loop assigns i;
        loop variant COORD_MAX_TABS - i;
     */
    for (int i = 0; i < COORD_MAX_TABS; i++)
        if (s->slots[i].state != SLOT_FREE && s->slots[i].tab == tab) return i;
    return -1;
}

/*@ requires \valid_read(p + (0 .. 3));
    assigns \nothing;
 */
static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void coord_init(CoordState *s) {
    /*@ loop invariant 0 <= i <= COORD_MAX_TABS;
        loop invariant \forall integer k; 0 <= k < i ==> s->slots[k].state == SLOT_FREE;
        loop assigns i, s->slots[0 .. COORD_MAX_TABS - 1];
        loop variant COORD_MAX_TABS - i;
     */
    for (int i = 0; i < COORD_MAX_TABS; i++) {
        s->slots[i].state = SLOT_FREE;
        s->slots[i].tab = 0;
        s->slots[i].site[0] = 0;
    }
    s->exiting = false;
}

/* The only changes to a slot: a new tab in a free slot, and a new state for
 * a slot in use.  Each keeps the invariants, as proved here once. */
/*@ requires \valid(s) && 0 <= i < COORD_MAX_TABS && state_ok(s) && tabs_unique(s);
    requires slot_state_ok(st) && (st != SLOT_FREE ==> s->slots[i].state != SLOT_FREE);
    assigns s->slots[i].state;
    ensures s->slots[i].state == st;
    ensures state_ok(s) && tabs_unique(s) && labels_kept{Old, Here}(s);
    ensures \forall integer j; 0 <= j < COORD_MAX_TABS && j != i ==> slot_unchanged{Old, Here}(s, j);
 */
static void set_state(CoordState *s, int i, CoordSlotState st) {
    s->slots[i].state = st;
}

/*@ requires \valid(s) && 0 <= i < COORD_MAX_TABS && state_ok(s) && tabs_unique(s);
    requires s->slots[i].state == SLOT_FREE;
    requires \forall integer j; 0 <= j < COORD_MAX_TABS && s->slots[j].state != SLOT_FREE ==> s->slots[j].tab != tab;
    requires \valid_read(site + (0 .. SITE_MAX - 1)) && \separated(s, site + (0 .. SITE_MAX - 1));
    requires badge_ok(site);
    assigns s->slots[i];
    ensures s->slots[i].state == SLOT_STARTING && s->slots[i].tab == tab;
    ensures \forall integer k; 0 <= k < SITE_MAX ==> s->slots[i].site[k] == site[k];
    ensures state_ok(s) && tabs_unique(s) && labels_kept{Old, Here}(s);
    ensures \forall integer j; 0 <= j < COORD_MAX_TABS && j != i ==> slot_unchanged{Old, Here}(s, j);
 */
static void start_slot(CoordState *s, int i, uint32_t tab, const char *site) {
    s->slots[i].state = SLOT_STARTING;
    s->slots[i].tab = tab;
    copy_site(s->slots[i].site, site);
    /*@ assert badge_ok(&s->slots[i].site[0]); */
}

/* A tab's start action, whole. */
/*@ requires \valid(a) && \valid_read(site + (0 .. SITE_MAX - 1)) && \valid_read(url + (0 .. len - 1));
    requires 1 <= len <= PROTO_URL_MAX && badge_ok(site);
    requires \forall integer k; 0 <= k < len ==> 0x20 < url[k] < 0x7f;
    requires \separated(a, site + (0 .. SITE_MAX - 1), url + (0 .. len - 1));
    assigns *a;
    ensures a->kind == CA_START_TAB && a->slot == slot && a->tab == tab && badge_ok(&a->site[0]);
    ensures \forall integer k; 0 <= k < SITE_MAX ==> a->site[k] == site[k];
    ensures a->grant_net && (a->grant_store <==> a->site[0] != 0);
    ensures a->len == len && \forall integer k; 0 <= k < len ==> 0x20 < a->data[k] < 0x7f;
 */
static void fill_start(CoordAction *a, int slot, uint32_t tab, uint32_t w, uint32_t h, const char *site,
                       const uint8_t *url, uint32_t len) {
    a->kind = CA_START_TAB;
    a->slot = slot;
    a->tab = tab;
    a->width = w;
    a->height = h;
    copy_site(a->site, site);
    /* The network process for every tab, the storage service unless the
     * site is opaque. */
    a->grant_net = true;
    a->grant_store = site[0] != 0;
    a->type = 0;
    a->len = len;
    copy_bytes(a->data, url, len);
}

/*@ requires \valid(s) && \valid(out) && \separated(s, out);
    requires state_ok(s) && tabs_unique(s) && out->n == 0;
    assigns s->slots[0 .. COORD_MAX_TABS - 1].state, s->exiting, out->n, out->a[0 .. COORD_MAX_ACTIONS - 1];
    ensures state_ok(s) && tabs_unique(s) && s->exiting;
    ensures 0 <= out->n <= COORD_MAX_ACTIONS;
    ensures \forall integer i; 0 <= i < COORD_MAX_TABS ==>
        (s->slots[i].state == SLOT_FREE <==> \old(s->slots[i].state) == SLOT_FREE);
    ensures \forall integer k; 0 <= k < out->n ==>
        (out->a[k].kind == CA_KILL_TAB && 0 <= out->a[k].slot < COORD_MAX_TABS) || out->a[k].kind == CA_EXIT;
 */
static void exit_all(CoordState *s, CoordActions *out) {
    /*@ loop invariant 0 <= i <= COORD_MAX_TABS && 0 <= out->n <= i;
        loop invariant state_ok(s) && tabs_unique(s);
        loop invariant \forall integer j; 0 <= j < COORD_MAX_TABS ==>
            (s->slots[j].state == SLOT_FREE <==> \at(s->slots[j].state, Pre) == SLOT_FREE);
        loop invariant \forall integer k; 0 <= k < out->n ==>
            out->a[k].kind == CA_KILL_TAB && 0 <= out->a[k].slot < COORD_MAX_TABS;
        loop assigns i, s->slots[0 .. COORD_MAX_TABS - 1].state, out->n, out->a[0 .. COORD_MAX_ACTIONS - 1];
        loop variant COORD_MAX_TABS - i;
     */
    for (int i = 0; i < COORD_MAX_TABS; i++) {
        if (s->slots[i].state == SLOT_STARTING || s->slots[i].state == SLOT_RUNNING) {
            CoordAction *a = add(out, CA_KILL_TAB);
            if (a) a->slot = i;
            set_state(s, i, SLOT_CLOSING);
        }
    }
    add(out, CA_EXIT);
    s->exiting = true;
}

/* A URL's site: the label of the process that loads it, the badge of its
 * channels.  about:blank is opaque (""): no site, no storage. */
/*@ requires \valid_read(url + (0 .. len - 1)) && \valid(site + (0 .. SITE_MAX - 1));
    requires \separated(url + (0 .. len - 1), site + (0 .. SITE_MAX - 1));
    assigns site[0 .. SITE_MAX - 1];
    ensures \result ==> badge_ok(site);
 */
static bool tab_site(const uint8_t *url, uint32_t len, char *site) {
    site[0] = 0;
    if (len == 11 && prefix(url, len, "about:blank", 11)) return true;
    char scheme[8], host[SITE_HOST_MAX + 1];
    return site_url_host(url, len, scheme, sizeof(scheme), host, sizeof(host)) && site_of(scheme, host, site);
}

/* UC_OPEN_TAB: start tab `tab` on url (len bytes) in a free slot, if the
 * URL is one a tab may load and the id isn't in use. */
/*@ requires \valid(s) && \valid(out) && \valid_read(url + (0 .. len - 1));
    requires \separated(s, out, url + (0 .. len - 1));
    requires state_ok(s) && tabs_unique(s) && out->n == 0;
    assigns s->slots[0 .. COORD_MAX_TABS - 1], out->n, out->a[0];
    ensures state_ok(s) && tabs_unique(s) && labels_kept{Old, Here}(s);
    ensures 0 <= out->n <= 1;
    ensures out->n == 1 ==> action_ok(s, &out->a[0]) &&
        (out->a[0].kind == CA_SEND_UI ||
         (out->a[0].kind == CA_START_TAB && \let i = out->a[0].slot; \at(s->slots[i].state, Old) == SLOT_FREE));
 */
static void open_tab(CoordState *s, CoordActions *out, uint32_t tab, uint32_t w, uint32_t h, const uint8_t *url,
                     uint32_t len) {
    char site[SITE_MAX];
    if (!coord_url_ok(url, len) || slot_of_tab(s, tab) >= 0 || !tab_site(url, len, site)) {
        tab_failed(out, tab);
        return;
    }
    if (w == 0 || w > SURFACE_MAX_W) w = SURFACE_MAX_W;
    if (h == 0 || h > SURFACE_MAX_H) h = SURFACE_MAX_H;
    int free_slot = -1;
    /*@ loop invariant 0 <= i <= COORD_MAX_TABS;
        loop invariant -1 <= free_slot < COORD_MAX_TABS;
        loop invariant free_slot >= 0 ==> s->slots[free_slot].state == SLOT_FREE;
        loop assigns i, free_slot;
        loop variant COORD_MAX_TABS - i;
     */
    for (int i = 0; i < COORD_MAX_TABS && free_slot < 0; i++)
        if (s->slots[i].state == SLOT_FREE) free_slot = i;
    if (free_slot < 0) {
        tab_failed(out, tab);
        return;
    }
    start_slot(s, free_slot, tab, site);
    CoordAction *a = add(out, CA_START_TAB);   /* the first action: there is room */
    fill_start(a, free_slot, tab, w, h, site, url, len);
}

/*@ requires \valid(s) && \valid(out) && \valid_read(e);
    requires \valid_read(e->data + (0 .. e->len - 1));
    requires \separated(s, out, e, e->data + (0 .. e->len - 1));
    requires state_ok(s) && tabs_unique(s) && out->n == 0;
    assigns *s, *out;
    ensures state_ok(s) && tabs_unique(s);
    ensures 0 <= out->n <= COORD_MAX_ACTIONS;
    ensures labels_kept{Old, Here}(s);
    ensures \forall integer k; 0 <= k < out->n ==> action_ok(s, &out->a[k]) && out->a[k].kind != CA_TAB_GONE;
    ensures \forall integer k; 0 <= k < out->n && out->a[k].kind == CA_START_TAB ==>
        \let i = out->a[k].slot; \at(s->slots[i].state, Old) == SLOT_FREE;
 */
static void ui_message(CoordState *s, const CoordEvent *e, CoordActions *out) {
    switch (e->type) {
    case UC_OPEN_TAB:
        if (e->len < 12) return;                    /* sizeof(UcOpenTab) */
        open_tab(s, out, read_u32(e->data), read_u32(e->data + 4), read_u32(e->data + 8), e->data + 12,
                 e->len - 12);
        return;
    case UC_CLOSE_TAB: {
        if (e->len != 4) return;
        int i = slot_of_tab(s, read_u32(e->data));
        if (i < 0 || s->slots[i].state == SLOT_CLOSING) return;
        set_state(s, i, SLOT_CLOSING);
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
        if (slot_ok && s->slots[e->slot].state == SLOT_STARTING) set_state(s, e->slot, SLOT_RUNNING);
        return;
    case CE_TAB_START_FAILED: {
        if (!slot_ok || s->slots[e->slot].state == SLOT_FREE) return;
        /* The slot first, then what to say about it. */
        bool tell = s->slots[e->slot].state != SLOT_CLOSING;
        uint32_t tab = s->slots[e->slot].tab;
        set_state(s, e->slot, SLOT_FREE);
        if (tell) tab_failed(out, tab);
        return;
    }
    case CE_TAB_EXITED: {
        if (!slot_ok || s->slots[e->slot].state == SLOT_FREE) return;
        bool tell = s->slots[e->slot].state != SLOT_CLOSING;
        uint32_t tab = s->slots[e->slot].tab;
        set_state(s, e->slot, SLOT_FREE);
        if (tell) {
            uint8_t x[8];               /* CuTabExited */
            write_u32(x, tab);
            write_u32(x + 4, e->exit_code);
            send_ui(out, CU_TAB_EXITED, x, sizeof(x));
        }
        CoordAction *a = add(out, CA_TAB_GONE);
        if (a) {
            a->slot = e->slot;
            a->tab = tab;
        }
        return;
    }
    }
}
