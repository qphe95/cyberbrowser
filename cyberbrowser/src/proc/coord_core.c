/* coord_core.c - the coordinator's decisions (coord_core.h).  No I/O, no
 * library calls, no allocation, bounded loops: proved with Frama-C/WP
 * (tools/prove.sh). */
#include "coord_core.h"

/* Frames.  The invariants read only the slots' states, ids and labels and
 * the web-GPU slots' states and labels; a step that keeps those (it writes
 * actions, or want flags) keeps the invariants.  Said once here, so that the
 * provers need not carry each invariant across every write to the actions. */
/*@ predicate webgpu_same{L1, L2}(CoordState *s) =
        \forall integer w; 0 <= w < COORD_MAX_WEBGPU ==>
            \at(s->webgpu[w].state, L1) == \at(s->webgpu[w].state, L2) &&
            \forall integer k; 0 <= k < SITE_MAX ==> \at(s->webgpu[w].site[k], L1) == \at(s->webgpu[w].site[k], L2);

    // (Written out, not with slot_unchanged, so that the provers find the
    // instances they need.)
    predicate core_same{L1, L2}(CoordState *s) =
        (\forall integer i; 0 <= i < COORD_MAX_TABS ==>
            \at(s->slots[i].state, L1) == \at(s->slots[i].state, L2) &&
            \at(s->slots[i].tab, L1) == \at(s->slots[i].tab, L2) &&
            \forall integer k; 0 <= k < SITE_MAX ==> \at(s->slots[i].site[k], L1) == \at(s->slots[i].site[k], L2)) &&
        (\forall integer w; 0 <= w < COORD_MAX_WEBGPU ==>
            \at(s->webgpu[w].state, L1) == \at(s->webgpu[w].state, L2) &&
            \forall integer k; 0 <= k < SITE_MAX ==> \at(s->webgpu[w].site[k], L1) == \at(s->webgpu[w].site[k], L2));

    // An action's fields, as action_ok reads them.
    predicate action_same{L1, L2}(CoordAction *a) =
        \at(a->kind, L1) == \at(a->kind, L2) && \at(a->slot, L1) == \at(a->slot, L2) &&
        \at(a->webgpu, L1) == \at(a->webgpu, L2) && \at(a->tab, L1) == \at(a->tab, L2) &&
        \at(a->grant_net, L1) == \at(a->grant_net, L2) && \at(a->grant_store, L1) == \at(a->grant_store, L2) &&
        \at(a->grant_gpu, L1) == \at(a->grant_gpu, L2) && \at(a->len, L1) == \at(a->len, L2) &&
        (\forall integer k; 0 <= k < SITE_MAX ==> \at(a->site[k], L1) == \at(a->site[k], L2)) &&
        (\forall integer k; 0 <= k < \at(a->len, L1) ==> \at(a->data[k], L1) == \at(a->data[k], L2));

    // The first n actions, as they were.
    predicate actions_same{L1, L2}(CoordActions *out, integer n) =
        \forall integer k; 0 <= k < n ==>
            \at(out->a[k].kind, L1) == \at(out->a[k].kind, L2) && \at(out->a[k].slot, L1) == \at(out->a[k].slot, L2) &&
            \at(out->a[k].webgpu, L1) == \at(out->a[k].webgpu, L2) && \at(out->a[k].tab, L1) == \at(out->a[k].tab, L2) &&
            \at(out->a[k].grant_net, L1) == \at(out->a[k].grant_net, L2) &&
            \at(out->a[k].grant_store, L1) == \at(out->a[k].grant_store, L2) &&
            \at(out->a[k].grant_gpu, L1) == \at(out->a[k].grant_gpu, L2) &&
            \at(out->a[k].len, L1) == \at(out->a[k].len, L2) &&
            (\forall integer j; 0 <= j < SITE_MAX ==> \at(out->a[k].site[j], L1) == \at(out->a[k].site[j], L2)) &&
            (\forall integer j; 0 <= j < \at(out->a[k].len, L1) ==> \at(out->a[k].data[j], L1) == \at(out->a[k].data[j], L2));

    // What the end of a tab's process causes: a word to the UI, the
    // revocation of its channels, and the end of its site's web-GPU process.
    predicate tab_end_actions{L}(CoordState *s, CoordActions *out, integer n, integer slot, integer tab) =
        \forall integer k; 0 <= k < n ==>
            (out->a[k].kind == CA_SEND_UI && out->a[k].len <= PROTO_URL_MAX) ||
            (out->a[k].kind == CA_TAB_GONE && out->a[k].slot == slot && out->a[k].tab == tab) ||
            (out->a[k].kind == CA_KILL_WEBGPU && 0 <= out->a[k].webgpu < COORD_MAX_WEBGPU &&
             same_label(&s->webgpu[out->a[k].webgpu].site[0], &s->slots[slot].site[0]));

    lemma label_ok_kept{L1, L2}: \forall char *p;
        label_ok{L1}(p) ==> (\forall integer k; 0 <= k < SITE_MAX ==> \at(p[k], L1) == \at(p[k], L2)) ==>
        label_ok{L2}(p);

    lemma label_ok_same{L}: \forall char *p, char *q;
        label_ok(p) ==> same_label(p, q) ==> label_ok(q);

    lemma action_ok_kept{L1, L2}: \forall CoordState *s, CoordAction *a;
        action_ok{L1}(s, a) ==> core_same{L1, L2}(s) ==> action_same{L1, L2}(a) ==> action_ok{L2}(s, a);

    // The actions that name only a slot, or say nothing of the state.
    lemma action_ok_plain{L}: \forall CoordState *s, CoordAction *a;
        (a->kind == CA_SEND_UI && a->len <= PROTO_URL_MAX) ||
        ((a->kind == CA_KILL_TAB || a->kind == CA_TAB_GONE || a->kind == CA_WEBGPU_FAILED) &&
         0 <= a->slot < COORD_MAX_TABS) ||
        (a->kind == CA_KILL_WEBGPU && 0 <= a->webgpu < COORD_MAX_WEBGPU) ||
        a->kind == CA_EXIT || a->kind == CA_RESTART_GPU ==>
        action_ok(s, a);

    lemma core_same_trans{L1, L2, L3}: \forall CoordState *s;
        core_same{L1, L2}(s) ==> core_same{L2, L3}(s) ==> core_same{L1, L3}(s);

    lemma webgpu_ok_kept{L1, L2}: \forall CoordState *s;
        webgpu_ok{L1}(s) ==> webgpu_same{L1, L2}(s) ==> webgpu_ok{L2}(s);

    lemma state_ok_kept{L1, L2}: \forall CoordState *s;
        state_ok{L1}(s) ==> core_same{L1, L2}(s) ==> state_ok{L2}(s);

    lemma tabs_unique_kept{L1, L2}: \forall CoordState *s;
        tabs_unique{L1}(s) ==> core_same{L1, L2}(s) ==> tabs_unique{L2}(s);

    lemma labels_kept_same{L1, L2}: \forall CoordState *s;
        core_same{L1, L2}(s) ==> labels_kept{L1, L2}(s);

    // Slots as they were; web-GPU slots in use as they were, labels and all,
    // though perhaps in another state of use.
    predicate same_used{L1, L2}(CoordState *s) =
        (\forall integer i; 0 <= i < COORD_MAX_TABS ==>
            \at(s->slots[i].state, L1) == \at(s->slots[i].state, L2) &&
            \at(s->slots[i].tab, L1) == \at(s->slots[i].tab, L2) &&
            \forall integer k; 0 <= k < SITE_MAX ==> \at(s->slots[i].site[k], L1) == \at(s->slots[i].site[k], L2)) &&
        (\forall integer w; 0 <= w < COORD_MAX_WEBGPU ==>
            (\at(s->webgpu[w].state, L1) == SLOT_FREE <==> \at(s->webgpu[w].state, L2) == SLOT_FREE) &&
            \forall integer k; 0 <= k < SITE_MAX ==> \at(s->webgpu[w].site[k], L1) == \at(s->webgpu[w].site[k], L2));

    lemma core_same_used{L1, L2}: \forall CoordState *s;
        core_same{L1, L2}(s) ==> same_used{L1, L2}(s);

    lemma labels_kept_then_used{L1, L2, L3}: \forall CoordState *s;
        labels_kept{L1, L2}(s) ==> same_used{L2, L3}(s) ==> labels_kept{L1, L3}(s);

    lemma actions_same_trans{L1, L2, L3}: \forall CoordActions *out, integer n;
        actions_same{L1, L2}(out, n) ==> actions_same{L2, L3}(out, n) ==> actions_same{L1, L3}(out, n);

    lemma labels_kept_then_same{L1, L2, L3}: \forall CoordState *s;
        labels_kept{L1, L2}(s) ==> core_same{L2, L3}(s) ==> labels_kept{L1, L3}(s);
 */

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
    ensures actions_same{Old, Here}(out, \old(out->n));
 */
static CoordAction *add(CoordActions *out, CoordActionKind kind) {
    if (out->n >= COORD_MAX_ACTIONS) return 0;
    CoordAction *a = &out->a[out->n++];
    a->kind = kind;
    a->slot = -1;
    a->webgpu = -1;
    a->tab = a->width = a->height = 0;
    a->site[0] = 0;
    a->grant_net = a->grant_store = a->grant_gpu = false;
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
        s->slots[i].want_webgpu = false;
    }
    /*@ loop invariant 0 <= w <= COORD_MAX_WEBGPU;
        loop invariant \forall integer k; 0 <= k < w ==> s->webgpu[k].state == SLOT_FREE;
        loop invariant \forall integer k; 0 <= k < COORD_MAX_TABS ==> s->slots[k].state == SLOT_FREE;
        loop assigns w, s->webgpu[0 .. COORD_MAX_WEBGPU - 1];
        loop variant COORD_MAX_WEBGPU - w;
     */
    for (int w = 0; w < COORD_MAX_WEBGPU; w++) {
        s->webgpu[w].state = SLOT_FREE;
        s->webgpu[w].site[0] = 0;
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
    s->slots[i].want_webgpu = false;
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
    ensures a->grant_net && a->grant_gpu && (a->grant_store <==> a->site[0] != 0);
    ensures a->len == len && \forall integer k; 0 <= k < len ==> 0x20 < a->data[k] < 0x7f;
 */
static void fill_start(CoordAction *a, int slot, uint32_t tab, uint32_t w, uint32_t h, const char *site,
                       const uint8_t *url, uint32_t len) {
    a->kind = CA_START_TAB;
    a->slot = slot;
    a->webgpu = -1;
    a->tab = tab;
    a->width = w;
    a->height = h;
    copy_site(a->site, site);
    /* The network and GPU processes for every tab, the storage service
     * unless the site is opaque. */
    a->grant_net = a->grant_gpu = true;
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

/* A surface size as asked, or the largest if it is 0 or larger. */
/*@ assigns \nothing; */
static uint32_t clamp_size(uint32_t v, uint32_t max) {
    return v == 0 || v > max ? max : v;
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
    w = clamp_size(w, SURFACE_MAX_W);
    h = clamp_size(h, SURFACE_MAX_H);
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
    CoordAction *a;
    start_slot(s, free_slot, tab, site);
Started: __attribute__((unused));
    a = add(out, CA_START_TAB);                 /* the first action: there is room */
    fill_start(a, free_slot, tab, w, h, site, url, len);
    /*@ assert core_same{Started, Here}(s); */
}

/* ---- web-GPU processes (section 5.7) ------------------------------------------ */

/*@ requires \valid_read(a + (0 .. SITE_MAX - 1)) && \valid_read(b + (0 .. SITE_MAX - 1));
    assigns \nothing;
    ensures \result <==> same_label(a, b);
 */
static bool same_site(const char *a, const char *b) {
    /*@ loop invariant 0 <= i <= SITE_MAX;
        loop invariant \forall integer k; 0 <= k < i ==> a[k] == b[k];
        loop assigns i;
        loop variant SITE_MAX - i;
     */
    for (int i = 0; i < SITE_MAX; i++)
        if (a[i] != b[i]) return false;
    return true;
}

/* The web-GPU process of site, starting or running; -1 if none. */
/*@ requires \valid_read(s) && \valid_read(site + (0 .. SITE_MAX - 1));
    assigns \nothing;
    ensures -1 <= \result < COORD_MAX_WEBGPU;
    ensures \result >= 0 ==> (s->webgpu[\result].state == SLOT_STARTING || s->webgpu[\result].state == SLOT_RUNNING) &&
        same_label(&s->webgpu[\result].site[0], site);
 */
static int webgpu_of(const CoordState *s, const char *site) {
    /*@ loop invariant 0 <= w <= COORD_MAX_WEBGPU;
        loop assigns w;
        loop variant COORD_MAX_WEBGPU - w;
     */
    for (int w = 0; w < COORD_MAX_WEBGPU; w++)
        if ((s->webgpu[w].state == SLOT_STARTING || s->webgpu[w].state == SLOT_RUNNING) &&
            same_site(s->webgpu[w].site, site))
            return w;
    return -1;
}

/*@ requires \valid(out) && 0 <= out->n <= COORD_MAX_ACTIONS && 0 <= slot < COORD_MAX_TABS;
    assigns out->n, out->a[out->n];
    ensures \old(out->n) <= out->n <= \old(out->n) + 1;
    ensures out->n > \old(out->n) ==>
        out->a[\old(out->n)].kind == CA_WEBGPU_FAILED && out->a[\old(out->n)].slot == slot;
 */
static void webgpu_failed(CoordActions *out, int slot, uint32_t tab) {
    CoordAction *a = add(out, CA_WEBGPU_FAILED);
    if (!a) return;
    a->slot = slot;
    a->tab = tab;
}

/* A grant, whole: tab slot i's channel to its site's running web-GPU
 * process w, badged with that site.  False if the actions are full.  Here,
 * with the state it reads, so that its callers need not carry the state
 * across the writes to the action. */
/*@ requires \valid_read(s) && \valid(out) && \separated(s, out) && 0 <= out->n <= COORD_MAX_ACTIONS;
    requires 0 <= i < COORD_MAX_TABS && 0 <= w < COORD_MAX_WEBGPU && state_ok(s);
    requires s->slots[i].state == SLOT_RUNNING && s->webgpu[w].state == SLOT_RUNNING;
    requires same_label(&s->slots[i].site[0], &s->webgpu[w].site[0]);
    assigns out->n, out->a[out->n];
    ensures core_same{Old, Here}(s);
    ensures \result <==> out->n > \old(out->n);
    ensures \old(out->n) <= out->n <= \old(out->n) + 1;
    ensures actions_same{Old, Here}(out, \old(out->n));
    ensures \result ==> out->a[\old(out->n)].kind == CA_GRANT_WEBGPU && action_ok(s, &out->a[\old(out->n)]);
 */
static bool push_grant_webgpu(const CoordState *s, CoordActions *out, int i, int w) {
    if (out->n >= COORD_MAX_ACTIONS) return false;    /* (add would find no room) */
    CoordAction *a = add(out, CA_GRANT_WEBGPU);
    a->slot = i;
    a->webgpu = w;
    a->tab = s->slots[i].tab;
    copy_site(a->site, s->webgpu[w].site);
    /*@ assert core_same{Pre, Here}(s); */
    /*@ assert label_ok(&s->webgpu[w].site[0]); */
    /*@ assert label_ok(&a->site[0]) && same_label(&a->site[0], &s->slots[i].site[0]); */
    return true;
}

/* A web-GPU process's start, labeled with its slot's site. */
/*@ requires \valid_read(s) && \valid(out) && \separated(s, out) && 0 <= out->n <= COORD_MAX_ACTIONS;
    requires 0 <= w < COORD_MAX_WEBGPU && state_ok(s) && s->webgpu[w].state == SLOT_STARTING;
    assigns out->n, out->a[out->n];
    ensures core_same{Old, Here}(s);
    ensures \old(out->n) <= out->n <= \old(out->n) + 1;
    ensures actions_same{Old, Here}(out, \old(out->n));
    ensures out->n > \old(out->n) ==>
        out->a[\old(out->n)].kind == CA_START_WEBGPU && action_ok(s, &out->a[\old(out->n)]);
 */
static void push_start_webgpu(const CoordState *s, CoordActions *out, int w) {
    if (out->n >= COORD_MAX_ACTIONS) return;
    CoordAction *a = add(out, CA_START_WEBGPU);
    a->webgpu = w;
    copy_site(a->site, s->webgpu[w].site);
    /*@ assert core_same{Pre, Here}(s); */
    /*@ assert label_ok(&s->webgpu[w].site[0]); */
    /*@ assert label_ok(&a->site[0]) && same_label(&a->site[0], &s->webgpu[w].site[0]); */
}

/* A web-GPU slot's new state; its label as it is. */
/*@ requires \valid(s) && 0 <= w < COORD_MAX_WEBGPU && state_ok(s) && tabs_unique(s);
    requires slot_state_ok(st) && (st != SLOT_FREE ==> s->webgpu[w].state != SLOT_FREE);
    assigns s->webgpu[w].state;
    ensures s->webgpu[w].state == st;
    ensures \forall integer v; 0 <= v < COORD_MAX_WEBGPU && v != w ==> s->webgpu[v].state == \old(s->webgpu[v].state);
    ensures \forall integer v, k; 0 <= v < COORD_MAX_WEBGPU && 0 <= k < SITE_MAX ==>
        s->webgpu[v].site[k] == \old(s->webgpu[v].site[k]);
    ensures state_ok(s) && tabs_unique(s) && labels_kept{Old, Here}(s);
    ensures \forall integer j; 0 <= j < COORD_MAX_TABS ==> slot_unchanged{Old, Here}(s, j);
 */
static void set_webgpu_state(CoordState *s, int w, CoordSlotState st) {
    s->webgpu[w].state = st;
}

/* A slot's want flag (no part of its label or state). */
/*@ requires \valid(s) && 0 <= i < COORD_MAX_TABS && state_ok(s) && tabs_unique(s);
    assigns s->slots[i].want_webgpu;
    ensures s->slots[i].want_webgpu == v;
    ensures core_same{Old, Here}(s);
    ensures state_ok(s) && tabs_unique(s) && labels_kept{Old, Here}(s);
    ensures \forall integer j; 0 <= j < COORD_MAX_TABS ==> slot_unchanged{Old, Here}(s, j);
 */
static void set_want(CoordState *s, int i, bool v) {
    s->slots[i].want_webgpu = v;
}

/* Free web-GPU slot w starts, labeled with tab slot i's site. */
/*@ requires \valid(s) && 0 <= w < COORD_MAX_WEBGPU && 0 <= i < COORD_MAX_TABS && state_ok(s) && tabs_unique(s);
    requires s->webgpu[w].state == SLOT_FREE && s->slots[i].site[0] != 0 && label_ok(&s->slots[i].site[0]);
    assigns s->webgpu[w].state, s->webgpu[w].site[0 .. SITE_MAX - 1];
    ensures s->webgpu[w].state == SLOT_STARTING && same_label(&s->webgpu[w].site[0], &s->slots[i].site[0]);
    ensures \forall integer j; 0 <= j < COORD_MAX_TABS ==> slot_unchanged{Old, Here}(s, j);
    ensures tabs_unique(s);
    ensures state_ok(s);
    ensures labels_kept{Old, Here}(s);
 */
static void start_webgpu(CoordState *s, int w, int i) {
    s->webgpu[w].state = SLOT_STARTING;
    copy_site(s->webgpu[w].site, s->slots[i].site);
    /*@ assert same_label(&s->webgpu[w].site[0], &s->slots[i].site[0]); */
    /*@ assert \forall integer j; 0 <= j < COORD_MAX_TABS ==> slot_unchanged{Pre, Here}(s, j); */
    /*@ assert label_ok(&s->slots[i].site[0]); */
    /*@ assert label_ok(&s->webgpu[w].site[0]); */
}

/* UC_WEBGPU: tab's channel to its site's web-GPU process: now if it runs,
 * once it has started if it is starting, else start one. */
/*@ requires \valid(s) && \valid(out) && \separated(s, out);
    requires state_ok(s) && tabs_unique(s) && out->n == 0;
    assigns s->slots[0 .. COORD_MAX_TABS - 1].want_webgpu, s->webgpu[0 .. COORD_MAX_WEBGPU - 1], out->n, out->a[0];
    ensures state_ok(s) && tabs_unique(s) && labels_kept{Old, Here}(s);
    ensures \forall integer j; 0 <= j < COORD_MAX_TABS ==> slot_unchanged{Old, Here}(s, j);
    ensures 0 <= out->n <= 1;
    ensures out->n == 1 ==> action_ok(s, &out->a[0]) && out->a[0].kind != CA_TAB_GONE && out->a[0].kind != CA_START_TAB;
 */
static void webgpu_request(CoordState *s, CoordActions *out, uint32_t tab) {
    int i = slot_of_tab(s, tab);
    if (i < 0) return;
    if (s->slots[i].state != SLOT_RUNNING || s->slots[i].site[0] == 0) {
        webgpu_failed(out, i, tab);
        return;
    }
    /*@ assert label_ok(&s->slots[i].site[0]); */
    int w = webgpu_of(s, s->slots[i].site);
    if (w >= 0 && s->webgpu[w].state == SLOT_RUNNING) {
        push_grant_webgpu(s, out, i, w);        /* (tab is the slot's: slot_of_tab) */
        return;
    }
    if (w >= 0) {                               /* starting: granted once it has */
        set_want(s, i, true);
        return;
    }
    /*@ loop invariant 0 <= k <= COORD_MAX_WEBGPU && -1 <= w < COORD_MAX_WEBGPU;
        loop invariant w >= 0 ==> s->webgpu[w].state == SLOT_FREE;
        loop assigns k, w;
        loop variant COORD_MAX_WEBGPU - k;
     */
    for (int k = 0; k < COORD_MAX_WEBGPU && w < 0; k++)
        if (s->webgpu[k].state == SLOT_FREE) w = k;
    if (w < 0) {
        webgpu_failed(out, i, tab);
        return;
    }
    start_webgpu(s, w, i);
Labeled: __attribute__((unused));
    set_want(s, i, true);
    push_start_webgpu(s, out, w);
    /*@ assert core_same{Labeled, Here}(s); */
    /*@ assert state_ok(s) && tabs_unique(s) && labels_kept{Pre, Here}(s); */
}

/* CE_WEBGPU_STARTED: it runs; the tabs that wait for it get their channels. */
/*@ requires \valid(s) && \valid(out) && \separated(s, out);
    requires state_ok(s) && tabs_unique(s) && out->n == 0 && 0 <= w < COORD_MAX_WEBGPU;
    assigns s->slots[0 .. COORD_MAX_TABS - 1].want_webgpu, s->webgpu[w].state, out->n, out->a[0 .. COORD_MAX_ACTIONS - 1];
    ensures state_ok(s) && tabs_unique(s) && labels_kept{Old, Here}(s);
    ensures \forall integer j; 0 <= j < COORD_MAX_TABS ==> slot_unchanged{Old, Here}(s, j);
    ensures 0 <= out->n <= COORD_MAX_ACTIONS;
    ensures \forall integer k; 0 <= k < out->n ==> out->a[k].kind == CA_GRANT_WEBGPU && action_ok(s, &out->a[k]);
 */
static void webgpu_started(CoordState *s, CoordActions *out, int w) {
    if (s->webgpu[w].state != SLOT_STARTING) return;
    set_webgpu_state(s, w, SLOT_RUNNING);
    /*@ loop invariant 0 <= i <= COORD_MAX_TABS && 0 <= out->n <= i;
        loop invariant state_ok(s) && tabs_unique(s) && labels_kept{Pre, Here}(s);
        loop invariant s->webgpu[w].state == SLOT_RUNNING;
        // (slot_unchanged{Pre, Here}, written out)
        loop invariant \forall integer j; 0 <= j < COORD_MAX_TABS ==>
            \at(s->slots[j].state, Pre) == s->slots[j].state && \at(s->slots[j].tab, Pre) == s->slots[j].tab &&
            \forall integer k; 0 <= k < SITE_MAX ==> \at(s->slots[j].site[k], Pre) == s->slots[j].site[k];
        loop invariant \forall integer k; 0 <= k < out->n ==> out->a[k].kind == CA_GRANT_WEBGPU && action_ok(s, &out->a[k]);
        loop assigns i, s->slots[0 .. COORD_MAX_TABS - 1].want_webgpu, out->n, out->a[0 .. COORD_MAX_ACTIONS - 1];
        loop variant COORD_MAX_TABS - i;
     */
    for (int i = 0; i < COORD_MAX_TABS; i++) {
        if (!s->slots[i].want_webgpu || !same_site(s->slots[i].site, s->webgpu[w].site)) continue;
        set_want(s, i, false);
        if (s->slots[i].state != SLOT_RUNNING) continue;
        /*@ assert core_same{LoopCurrent, Here}(s); */
        /*@ assert actions_same{LoopCurrent, Here}(out, out->n); */
        /*@ assert same_label(&s->slots[i].site[0], &s->webgpu[w].site[0]); */
        bool granted = push_grant_webgpu(s, out, i, w);
        /*@ assert core_same{LoopCurrent, Here}(s); */
        /*@ assert actions_same{LoopCurrent, Here}(out, \at(out->n, LoopCurrent)); */
        /*@ assert \forall integer k; 0 <= k < \at(out->n, LoopCurrent) ==> action_same{LoopCurrent, Here}(&out->a[k]); */
        /*@ assert \forall integer k; 0 <= k < \at(out->n, LoopCurrent) ==> action_ok{LoopCurrent}(s, &out->a[k]); */
        /*@ assert \forall integer k; 0 <= k < out->n ==> out->a[k].kind == CA_GRANT_WEBGPU && action_ok(s, &out->a[k]); */
        /*@ assert \forall integer j; 0 <= j < COORD_MAX_TABS ==> slot_unchanged{Pre, Here}(s, j); */
        /*@ assert state_ok(s) && tabs_unique(s) && labels_kept{Pre, Here}(s); */
        if (!granted) return;
    }
}

/* CE_WEBGPU_EXITED: it failed to start, or ended.  Its slot frees; the tabs
 * that wait for it hear they get none (those it served see their channels
 * break, and may ask again). */
/*@ requires \valid(s) && \valid(out) && \separated(s, out);
    requires state_ok(s) && tabs_unique(s) && out->n == 0 && 0 <= w < COORD_MAX_WEBGPU;
    assigns s->slots[0 .. COORD_MAX_TABS - 1].want_webgpu, s->webgpu[w].state, out->n, out->a[0 .. COORD_MAX_ACTIONS - 1];
    ensures state_ok(s) && tabs_unique(s) && labels_kept{Old, Here}(s);
    ensures \forall integer j; 0 <= j < COORD_MAX_TABS ==> slot_unchanged{Old, Here}(s, j);
    ensures 0 <= out->n <= COORD_MAX_ACTIONS;
    ensures \forall integer k; 0 <= k < out->n ==> out->a[k].kind == CA_WEBGPU_FAILED && action_ok(s, &out->a[k]);
 */
static void webgpu_exited(CoordState *s, CoordActions *out, int w) {
    if (s->webgpu[w].state == SLOT_FREE) return;
    set_webgpu_state(s, w, SLOT_FREE);
    /*@ loop invariant 0 <= i <= COORD_MAX_TABS && 0 <= out->n <= i;
        loop invariant state_ok(s) && tabs_unique(s) && labels_kept{Pre, Here}(s);
        loop invariant \forall integer j; 0 <= j < COORD_MAX_TABS ==> slot_unchanged{Pre, Here}(s, j);
        loop invariant \forall integer k; 0 <= k < out->n ==> out->a[k].kind == CA_WEBGPU_FAILED && action_ok(s, &out->a[k]);
        loop assigns i, s->slots[0 .. COORD_MAX_TABS - 1].want_webgpu, out->n, out->a[0 .. COORD_MAX_ACTIONS - 1];
        loop variant COORD_MAX_TABS - i;
     */
    for (int i = 0; i < COORD_MAX_TABS; i++) {
        if (!s->slots[i].want_webgpu || !same_site(s->slots[i].site, s->webgpu[w].site)) continue;
        set_want(s, i, false);
        webgpu_failed(out, i, s->slots[i].tab);
        /*@ assert core_same{LoopCurrent, Here}(s); */
        /*@ assert state_ok(s) && tabs_unique(s) && labels_kept{Pre, Here}(s); */
    }
}

/* End web-GPU process w (CA_KILL_WEBGPU); its slot closes until its exit is
 * seen.  The action first, then the slot: the same outcome, with the state
 * carried across the action's writes once. */
/*@ requires \valid(s) && \valid(out) && \separated(s, out) && 0 <= out->n <= COORD_MAX_ACTIONS;
    requires state_ok(s) && tabs_unique(s) && 0 <= w < COORD_MAX_WEBGPU && s->webgpu[w].state != SLOT_FREE;
    assigns s->webgpu[w].state, out->n, out->a[out->n];
    ensures s->webgpu[w].state == SLOT_CLOSING;
    ensures state_ok(s) && tabs_unique(s) && labels_kept{Old, Here}(s);
    ensures same_used{Old, Here}(s);
    ensures \forall integer j; 0 <= j < COORD_MAX_TABS ==> slot_unchanged{Old, Here}(s, j);
    ensures \old(out->n) <= out->n <= \old(out->n) + 1;
    ensures actions_same{Old, Here}(out, \old(out->n));
    ensures out->n > \old(out->n) ==> out->a[\old(out->n)].kind == CA_KILL_WEBGPU && out->a[\old(out->n)].webgpu == w;
 */
static void kill_webgpu(CoordState *s, CoordActions *out, int w) {
    if (out->n < COORD_MAX_ACTIONS) {
        CoordAction *a = add(out, CA_KILL_WEBGPU);
        a->webgpu = w;
    }
    /*@ assert core_same{Pre, Here}(s); */
    /*@ assert state_ok(s) && tabs_unique(s); */
    set_webgpu_state(s, w, SLOT_CLOSING);
}

/* After tab slot i ended: if it was its site's last tab, that site's
 * web-GPU process goes too. */
/*@ requires \valid(s) && \valid(out) && \separated(s, out);
    requires state_ok(s) && tabs_unique(s) && 0 <= i < COORD_MAX_TABS && s->slots[i].state == SLOT_FREE;
    requires 0 <= out->n <= COORD_MAX_ACTIONS;
    assigns s->webgpu[0 .. COORD_MAX_WEBGPU - 1].state, out->n, out->a[out->n];
    ensures state_ok(s) && tabs_unique(s) && labels_kept{Old, Here}(s);
    ensures \forall integer j; 0 <= j < COORD_MAX_TABS ==> slot_unchanged{Old, Here}(s, j);
    ensures \old(out->n) <= out->n <= \old(out->n) + 1;
    ensures actions_same{Old, Here}(out, \old(out->n));
    ensures same_used{Old, Here}(s);
    ensures out->n > \old(out->n) ==> out->a[\old(out->n)].kind == CA_KILL_WEBGPU &&
        0 <= out->a[\old(out->n)].webgpu < COORD_MAX_WEBGPU &&
        same_label(&s->webgpu[out->a[\old(out->n)].webgpu].site[0], &s->slots[i].site[0]);
 */
static void last_of_site(CoordState *s, CoordActions *out, int i) {
    if (s->slots[i].site[0] == 0) return;
    /*@ loop invariant 0 <= j <= COORD_MAX_TABS;
        loop assigns j;
        loop variant COORD_MAX_TABS - j;
     */
    for (int j = 0; j < COORD_MAX_TABS; j++)
        if (s->slots[j].state != SLOT_FREE && same_site(s->slots[j].site, s->slots[i].site)) return;
    int w = webgpu_of(s, s->slots[i].site);
    if (w < 0) return;
    kill_webgpu(s, out, w);
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
    case UC_WEBGPU:
        if (e->len != 4) return;
        webgpu_request(s, out, read_u32(e->data));
        return;
    default:
        return;
    }
}

/* A live tab's new draw channel, whole: slot i's, badged with its label.
 * False if the actions are full. */
/*@ requires \valid_read(s) && \valid(out) && \separated(s, out) && 0 <= out->n <= COORD_MAX_ACTIONS;
    requires 0 <= i < COORD_MAX_TABS && state_ok(s);
    requires s->slots[i].state == SLOT_STARTING || s->slots[i].state == SLOT_RUNNING;
    assigns out->n, out->a[out->n];
    ensures core_same{Old, Here}(s);
    ensures \result <==> out->n > \old(out->n);
    ensures \old(out->n) <= out->n <= \old(out->n) + 1;
    ensures actions_same{Old, Here}(out, \old(out->n));
    ensures \result ==> out->a[\old(out->n)].kind == CA_REGRANT_GPU && action_ok(s, &out->a[\old(out->n)]);
 */
static bool push_regrant(const CoordState *s, CoordActions *out, int i) {
    if (out->n >= COORD_MAX_ACTIONS) return false;    /* (add would find no room) */
    CoordAction *a = add(out, CA_REGRANT_GPU);
    a->slot = i;
    a->tab = s->slots[i].tab;
    copy_site(a->site, s->slots[i].site);
    /*@ assert core_same{Pre, Here}(s); */
    /*@ assert badge_ok(&s->slots[i].site[0]); */
    /*@ assert badge_ok(&a->site[0]); */
    return true;
}

/* The GPU process ended: start another, and give every live tab a draw
 * channel to it, badged with its label.  No slot changes. */
/*@ requires \valid(s) && \valid(out) && \separated(s, out);
    requires state_ok(s) && tabs_unique(s) && out->n == 0;
    assigns out->n, out->a[0 .. COORD_MAX_ACTIONS - 1];
    ensures 0 <= out->n <= COORD_MAX_ACTIONS;
    ensures \forall integer k; 0 <= k < out->n ==>
        out->a[k].kind == CA_RESTART_GPU || (out->a[k].kind == CA_REGRANT_GPU && action_ok(s, &out->a[k]));
 */
static void gpu_exited(const CoordState *s, CoordActions *out) {
    add(out, CA_RESTART_GPU);
    /*@ loop invariant 0 <= i <= COORD_MAX_TABS && 0 <= out->n <= i + 1 && out->n <= COORD_MAX_ACTIONS;
        loop invariant \forall integer k; 0 <= k < out->n ==>
            out->a[k].kind == CA_RESTART_GPU || out->a[k].kind == CA_REGRANT_GPU;
        loop invariant \forall integer k; 0 <= k < out->n && out->a[k].kind == CA_REGRANT_GPU ==>
            action_ok(s, &out->a[k]);
        loop assigns i, out->n, out->a[0 .. COORD_MAX_ACTIONS - 1];
        loop variant COORD_MAX_TABS - i;
     */
    for (int i = 0; i < COORD_MAX_TABS; i++) {
        if (s->slots[i].state != SLOT_STARTING && s->slots[i].state != SLOT_RUNNING) continue;
        if (!push_regrant(s, out, i)) return;
        /*@ assert core_same{LoopCurrent, Here}(s); */
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
    case CE_GPU_EXITED:
        gpu_exited(s, out);
        return;
    case CE_WEBGPU_STARTED:
        if (e->slot >= 0 && e->slot < COORD_MAX_WEBGPU) webgpu_started(s, out, e->slot);
        return;
    case CE_WEBGPU_EXITED:
        if (e->slot >= 0 && e->slot < COORD_MAX_WEBGPU) webgpu_exited(s, out, e->slot);
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
FreedF: __attribute__((unused));
        if (tell) tab_failed(out, tab);
        /*@ assert tab_end_actions(s, out, out->n, e->slot, tab); */
        /*@ assert core_same{FreedF, Here}(s); */
        /*@ assert labels_kept{Pre, Here}(s); */
Failing: __attribute__((unused));
        last_of_site(s, out, e->slot);
        /*@ assert labels_kept{Pre, Here}(s); */
        /*@ assert tab_end_actions(s, out, \at(out->n, Failing), e->slot, tab); */
        /*@ assert tab_end_actions(s, out, out->n, e->slot, tab); */
        return;
    }
    case CE_TAB_EXITED: {
        if (!slot_ok || s->slots[e->slot].state == SLOT_FREE) return;
        bool tell = s->slots[e->slot].state != SLOT_CLOSING;
        uint32_t tab = s->slots[e->slot].tab;
        set_state(s, e->slot, SLOT_FREE);
FreedE: __attribute__((unused));
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
        /*@ assert tab_end_actions(s, out, out->n, e->slot, tab); */
        /*@ assert core_same{FreedE, Here}(s); */
        /*@ assert labels_kept{Pre, Here}(s); */
Ending: __attribute__((unused));
        last_of_site(s, out, e->slot);
        /*@ assert labels_kept{Pre, Here}(s); */
        /*@ assert tab_end_actions(s, out, \at(out->n, Ending), e->slot, tab); */
        /*@ assert tab_end_actions(s, out, out->n, e->slot, tab); */
        return;
    }
    }
}
