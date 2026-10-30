/* coord_plan.c - how the coordinator performs its core's actions
 * (coord_plan.h).  No I/O, no library calls, no allocation, bounded loops:
 * proved with Frama-C/WP (tools/prove.sh plan).  It calls Windows only
 * through coord_do, coord_tab_signaled and coord_webgpu_signaled, the
 * shim's, whose contracts are assumptions. */
#include "coord_plan.h"

/*@ requires \valid_read(a + (0 .. SITE_MAX - 1)) && \valid_read(b + (0 .. SITE_MAX - 1));
    assigns \nothing;
    ensures \result <==> same_label(a, b);
 */
static bool same(const char *a, const char *b) {
    /*@ loop invariant 0 <= i <= SITE_MAX;
        loop invariant \forall integer k; 0 <= k < i ==> a[k] == b[k];
        loop assigns i;
        loop variant SITE_MAX - i;
     */
    for (int i = 0; i < SITE_MAX; i++)
        if (a[i] != b[i]) return false;
    return true;
}

/*@ requires \valid(dst + (0 .. SITE_MAX - 1)) && \valid_read(src + (0 .. SITE_MAX - 1));
    requires \separated(dst + (0 .. SITE_MAX - 1), src + (0 .. SITE_MAX - 1));
    assigns dst[0 .. SITE_MAX - 1];
    ensures same_label(dst, src);
 */
static void copy_site(char *dst, const char *src) {
    /*@ loop invariant 0 <= i <= SITE_MAX;
        loop invariant \forall integer k; 0 <= k < i ==> dst[k] == src[k];
        loop assigns i, dst[0 .. SITE_MAX - 1];
        loop variant SITE_MAX - i;
     */
    for (int i = 0; i < SITE_MAX; i++) dst[i] = src[i];
}

void coord_tables_init(CoordTables *t, bool gpu) {
    /*@ loop invariant 0 <= i <= COORD_MAX_TABS;
        loop invariant \forall integer k; 0 <= k < i ==> !t->tab_proc[k] && !t->tab_ctl[k] && t->tab_webgpu[k] == 0;
        loop assigns i, t->tab_proc[0 .. COORD_MAX_TABS - 1], t->tab_ctl[0 .. COORD_MAX_TABS - 1],
            t->tab_webgpu[0 .. COORD_MAX_TABS - 1], t->tab_site[0 .. COORD_MAX_TABS - 1][0];
        loop variant COORD_MAX_TABS - i;
     */
    for (int i = 0; i < COORD_MAX_TABS; i++) {
        t->tab_proc[i] = false;
        t->tab_ctl[i] = false;
        t->tab_webgpu[i] = 0;
        t->tab_site[i][0] = 0;
    }
    /*@ loop invariant 0 <= w <= COORD_MAX_WEBGPU;
        loop invariant \forall integer k; 0 <= k < w ==> !t->webgpu_proc[k];
        loop assigns w, t->webgpu_proc[0 .. COORD_MAX_WEBGPU - 1], t->webgpu_site[0 .. COORD_MAX_WEBGPU - 1][0];
        loop variant COORD_MAX_WEBGPU - w;
     */
    for (int w = 0; w < COORD_MAX_WEBGPU; w++) {
        t->webgpu_proc[w] = false;
        t->webgpu_site[w][0] = 0;
    }
    t->gpu = gpu;
}

/* ---- plans ------------------------------------------------------------------ */

/* Frames.  op_vals_ok reads only the slots' states, ids and labels, the
 * web-GPU slots' states and labels, the tables' processes, control channels
 * and labels, and the action's slot, id, web-GPU index and site; writing a
 * plan changes none of these.  Said once here, as lemmas, so that the
 * provers need not carry each operation's conditions across every write to
 * the plan. */
/*@ predicate plan_same{L1, L2}(CoordState *s, CoordTables *t, CoordAction *a) =
        (\forall integer i; 0 <= i < COORD_MAX_TABS ==>
            \at(s->slots[i].state, L1) == \at(s->slots[i].state, L2) &&
            \at(s->slots[i].tab, L1) == \at(s->slots[i].tab, L2) &&
            \at(t->tab_proc[i], L1) == \at(t->tab_proc[i], L2) &&
            \at(t->tab_ctl[i], L1) == \at(t->tab_ctl[i], L2) &&
            (\forall integer k; 0 <= k < SITE_MAX ==>
                \at(s->slots[i].site[k], L1) == \at(s->slots[i].site[k], L2) &&
                \at(t->tab_site[i][k], L1) == \at(t->tab_site[i][k], L2))) &&
        (\forall integer w; 0 <= w < COORD_MAX_WEBGPU ==>
            \at(s->webgpu[w].state, L1) == \at(s->webgpu[w].state, L2) &&
            \at(t->webgpu_proc[w], L1) == \at(t->webgpu_proc[w], L2) &&
            (\forall integer k; 0 <= k < SITE_MAX ==>
                \at(s->webgpu[w].site[k], L1) == \at(s->webgpu[w].site[k], L2) &&
                \at(t->webgpu_site[w][k], L1) == \at(t->webgpu_site[w][k], L2))) &&
        \at(a->slot, L1) == \at(a->slot, L2) && \at(a->tab, L1) == \at(a->tab, L2) &&
        \at(a->webgpu, L1) == \at(a->webgpu, L2) &&
        (\forall integer k; 0 <= k < SITE_MAX ==> \at(a->site[k], L1) == \at(a->site[k], L2));

    lemma client_ok_kept{L1, L2}: \forall CoordState *s, CoordTables *t, CoordAction *a;
        client_ok{L1}(s, a) ==> plan_same{L1, L2}(s, t, a) ==> client_ok{L2}(s, a);

    // (With the action's site.)
    predicate a_tab_is{L}(CoordTables *t, integer i, CoordAction *a) = tab_is(t, i, &a->site[0]);
    predicate a_webgpu_is{L}(CoordState *s, CoordTables *t, integer w, CoordAction *a) =
        webgpu_is(s, t, w, &a->site[0]);

    lemma tab_is_kept{L1, L2}: \forall CoordState *s, CoordTables *t, CoordAction *a, integer i;
        a_tab_is{L1}(t, i, a) ==> plan_same{L1, L2}(s, t, a) ==> a_tab_is{L2}(t, i, a);

    lemma webgpu_is_kept{L1, L2}: \forall CoordState *s, CoordTables *t, CoordAction *a, integer w;
        a_webgpu_is{L1}(s, t, w, a) ==> plan_same{L1, L2}(s, t, a) ==> a_webgpu_is{L2}(s, t, w, a);

    lemma op_vals_kept{L1, L2}: \forall CoordState *s, CoordTables *t, CoordAction *a,
            integer kind, slot, w, srv, ch, msg;
        op_vals_ok{L1}(s, t, a, kind, slot, w, srv, ch, msg) ==> plan_same{L1, L2}(s, t, a) ==>
        op_vals_ok{L2}(s, t, a, kind, slot, w, srv, ch, msg);

    predicate ops_ok{L}(CoordState *s, CoordTables *t, CoordAction *a, CoordPlan *p) =
        \forall integer k; 0 <= k < p->n ==> op_ok(s, t, a, &p->o[k]);
 */

/* Tab slot i holds a process started labeled site. */
/*@ requires \valid_read(t) && \valid_read(site + (0 .. SITE_MAX - 1));
    assigns \nothing;
    ensures \result ==> tab_is(t, i, site);
 */
static bool tab_valid(const CoordTables *t, int i, const char *site) {
    return i >= 0 && i < COORD_MAX_TABS && t->tab_proc[i] && same(t->tab_site[i], site);
}

/* Web-GPU process w runs, labeled site. */
/*@ requires \valid_read(s) && \valid_read(t) && \valid_read(site + (0 .. SITE_MAX - 1));
    assigns \nothing;
    ensures \result ==> webgpu_is(s, t, w, site);
 */
static bool webgpu_valid(const CoordState *s, const CoordTables *t, int w, const char *site) {
    return w >= 0 && w < COORD_MAX_WEBGPU && t->webgpu_proc[w] && s->webgpu[w].state == SLOT_RUNNING &&
           site[0] != 0 && same(t->webgpu_site[w], site) && same(s->webgpu[w].site, site);
}

/* Does op_vals_ok allow this operation?  Each plan below asks only for
 * operations its own checks have shown allowed; this checks again, where
 * the operation is written, so that the proof of each needs only the state
 * it is written in. */
/*@ requires \valid_read(s) && \valid_read(t) && \valid_read(a);
    assigns \nothing;
    ensures \result ==> op_vals_ok(s, t, a, kind, slot, w, srv, ch, msg);
 */
static bool op_valid(const CoordState *s, const CoordTables *t, const CoordAction *a, CoordOpKind kind, int slot,
                     int w, int srv, int ch, uint16_t msg) {
    int i = a->slot;
    bool client = i >= 0 && i < COORD_MAX_TABS && s->slots[i].state != SLOT_FREE && s->slots[i].tab == a->tab &&
                  same(a->site, s->slots[i].site);
    bool used = slot >= 0 && slot < COORD_MAX_TABS && s->slots[slot].state != SLOT_FREE;
    switch (kind) {
    case OP_CHAN:
        return ch >= 0 && ch < CH_COUNT;
    case OP_SPAWN_TAB:
    case OP_COMMIT_TAB:
    case OP_DISCARD_TAB:
        return client && slot == i && s->slots[slot].state == SLOT_STARTING && !t->tab_proc[slot];
    case OP_GRANT:
        if (!client) return false;
        if (srv == SRV_NET) return ch == CH_NET;
        if (srv == SRV_STORE) return ch == CH_STORE && a->site[0] != 0;
        if (srv == SRV_GPU) return ch == CH_DRAW;
        return srv == SRV_WEBGPU && ch == CH_WEBGPU && w == a->webgpu && webgpu_valid(s, t, w, a->site);
    case OP_GIVE_UI:
        return ch == CH_UI;
    case OP_GIVE_TAB:
        if (!client || slot != i || !tab_valid(t, slot, a->site)) return false;
        if (ch == CH_DRAW && msg == CT_GPU) return true;
        return ch == CH_WEBGPU && msg == CT_WEBGPU && w == a->webgpu && webgpu_valid(s, t, w, a->site);
    case OP_REVOKE:
        return srv == SRV_NET || srv == SRV_STORE || srv == SRV_GPU ||
               (srv == SRV_WEBGPU && w >= 0 && w < COORD_MAX_WEBGPU);
    case OP_KILL_TAB:
        return used && t->tab_proc[slot];
    case OP_SEND_TAB:
        return used && t->tab_ctl[slot] && msg == CT_WEBGPU_FAILED;
    case OP_CLOSE_CTL:
        return slot >= 0 && slot < COORD_MAX_TABS && t->tab_ctl[slot];
    case OP_FORGET_WEBGPU:
        return slot >= 0 && slot < COORD_MAX_TABS;
    case OP_SPAWN_WEBGPU:
        return w >= 0 && w < COORD_MAX_WEBGPU && w == a->webgpu && s->webgpu[w].state == SLOT_STARTING &&
               a->site[0] != 0 && same(a->site, s->webgpu[w].site);
    case OP_KILL_WEBGPU:
        return w >= 0 && w < COORD_MAX_WEBGPU && t->webgpu_proc[w] && s->webgpu[w].state != SLOT_FREE;
    default:                            /* OP_SEND_UI, OP_RESTART_GPU, OP_EXIT */
        return true;
    }
}

/* One more operation, if op_vals_ok allows it (else none, and false): the
 * ones before stay as they were. */
/*@ requires \valid_read(s) && \valid_read(t) && \valid_read(a) && \valid(p) && \separated(p, s, t, a);
    requires 0 <= p->n < COORD_MAX_OPS && ops_ok(s, t, a, p);
    assigns p->n, p->o[p->n];
    ensures \result ? p->n == \old(p->n) + 1 : p->n == \old(p->n);
    ensures ops_ok(s, t, a, p);
 */
static bool push(const CoordState *s, const CoordTables *t, const CoordAction *a, CoordPlan *p, CoordOpKind kind,
                 int slot, int w, int srv, int ch, uint16_t msg, uint32_t size, bool quick, int undo) {
    if (!op_valid(s, t, a, kind, slot, w, srv, ch, msg)) return false;
    CoordOp *o = &p->o[p->n];
    o->kind = kind;
    o->slot = slot;
    o->w = w;
    o->srv = srv;
    o->ch = ch;
    o->msg = msg;
    o->size = size;
    o->quick = quick;
    o->undo = undo;
    p->n++;
    /*@ assert plan_same{Pre, Here}(s, t, a); */
    /*@ assert op_vals_ok(s, t, a, kind, slot, w, srv, ch, msg); */
    /*@ assert \forall integer k; 0 <= k < \at(p->n, Pre) ==>
            p->o[k].kind == \at(p->o[k].kind, Pre) && p->o[k].slot == \at(p->o[k].slot, Pre) &&
            p->o[k].w == \at(p->o[k].w, Pre) && p->o[k].srv == \at(p->o[k].srv, Pre) &&
            p->o[k].ch == \at(p->o[k].ch, Pre) && p->o[k].msg == \at(p->o[k].msg, Pre); */
    /*@ assert \forall integer k; 0 <= k < \at(p->n, Pre) ==>
            op_vals_ok(s, t, a, \at(p->o[k].kind, Pre), \at(p->o[k].slot, Pre), \at(p->o[k].w, Pre),
                       \at(p->o[k].srv, Pre), \at(p->o[k].ch, Pre), \at(p->o[k].msg, Pre)); */
    return true;
}

/* The common cases of push. */
#define OP(kind, slot, w, srv, ch, msg) push(s, t, a, p, kind, slot, w, srv, ch, msg, 0, false, -1)
#define UNDO(from, kind, slot, w, srv) push(s, t, a, p, kind, slot, w, srv, 0, 0, 0, false, from)
#define CHAN(ch, size) push(s, t, a, p, OP_CHAN, 0, 0, 0, ch, 0, size, false, -1)

/* A plan the checks before it bore out has all its operations; if one was
 * refused all the same, it has none. */
/*@ requires \valid_read(s) && \valid_read(t) && \valid_read(a) && \valid(p) && \separated(p, s, t, a);
    requires 0 <= p->n <= COORD_MAX_OPS && ops_ok(s, t, a, p) && n > 0;
    assigns p->n;
    ensures \result ? p->n == n : p->n == 0;
    ensures ops_ok(s, t, a, p);
 */
static bool whole(const CoordState *s, const CoordTables *t, const CoordAction *a, CoordPlan *p, int n) {
    (void)s;
    (void)t;
    (void)a;
    if (p->n == n) return true;
    p->n = 0;
    return false;
}

/* CA_START_TAB: its channels, its process, the servers' ends badged with its
 * label, the UI's end, and the slot's; if a grant or the UI's end fails,
 * the grants are revoked and the process ended. */
/*@ requires \valid_read(s) && \valid_read(t) && \valid_read(a) && \valid(p) && \separated(p, s, t, a);
    requires p->n == 0;
    assigns p->n, p->o[0 .. COORD_MAX_OPS - 1];
    ensures 0 <= p->n <= 15 && ops_ok(s, t, a, p);
    ensures \result || p->n == 0;
 */
static bool plan_start_tab(const CoordState *s, const CoordTables *t, const CoordAction *a, CoordPlan *p) {
    /* What the core decided, as action_ok says: the network and GPU
     * processes always, the storage service only for a site.  The rest
     * (the slot, its state, id and label) push checks. */
    bool store = a->site[0] != 0;
    if (!a->grant_net || !a->grant_gpu || a->grant_store != store) return false;
    int i = a->slot;
    CHAN(CH_CTL, 1u << 16);
    CHAN(CH_UI, 1u << 18);
    CHAN(CH_NET, 1u << 20);
    CHAN(CH_DRAW, 1u << 22);
    if (store) CHAN(CH_STORE, 1u << 18);
    OP(OP_SPAWN_TAB, i, 0, 0, 0, 0);
    int granting = store ? 6 : 5;       /* undo from here on */
    OP(OP_GRANT, 0, 0, SRV_NET, CH_NET, 0);
    if (store) OP(OP_GRANT, 0, 0, SRV_STORE, CH_STORE, 0);
    OP(OP_GRANT, 0, 0, SRV_GPU, CH_DRAW, 0);
    OP(OP_GIVE_UI, 0, 0, 0, CH_UI, 0);
    OP(OP_COMMIT_TAB, i, 0, 0, 0, 0);
    UNDO(granting, OP_REVOKE, 0, 0, SRV_NET);
    UNDO(granting, OP_REVOKE, 0, 0, SRV_STORE);
    UNDO(granting, OP_REVOKE, 0, 0, SRV_GPU);
    UNDO(granting, OP_DISCARD_TAB, i, 0, 0);
    return whole(s, t, a, p, store ? 15 : 13);
}

/* CA_REGRANT_GPU: a draw channel, granted to the new GPU process with the
 * slot's label, and copied into the slot's process; revoked if that fails. */
/*@ requires \valid_read(s) && \valid_read(t) && \valid_read(a) && \valid(p) && \separated(p, s, t, a);
    requires p->n == 0;
    assigns p->n, p->o[0 .. COORD_MAX_OPS - 1];
    ensures 0 <= p->n <= 4 && ops_ok(s, t, a, p);
 */
static void plan_regrant_gpu(const CoordState *s, const CoordTables *t, const CoordAction *a, CoordPlan *p) {
    int i = a->slot;
    if (i < 0 || i >= COORD_MAX_TABS || !t->tab_proc[i] || !t->gpu) return;
    CHAN(CH_DRAW, 1u << 22);
    OP(OP_GRANT, 0, 0, SRV_GPU, CH_DRAW, 0);
    OP(OP_GIVE_TAB, i, 0, 0, CH_DRAW, CT_GPU);
    UNDO(2, OP_REVOKE, 0, 0, SRV_GPU);
    whole(s, t, a, p, 4);
}

/* CA_GRANT_WEBGPU: a channel, granted to the site's web-GPU process w with
 * the slot's label, and copied into the slot's process; revoked if that
 * fails, and the tab told it gets none if the grant or the copy fails. */
/*@ requires \valid_read(s) && \valid_read(t) && \valid_read(a) && \valid(p) && \separated(p, s, t, a);
    requires p->n == 0;
    assigns p->n, p->o[0 .. COORD_MAX_OPS - 1];
    ensures 0 <= p->n <= 5 && ops_ok(s, t, a, p);
 */
static void plan_grant_webgpu(const CoordState *s, const CoordTables *t, const CoordAction *a, CoordPlan *p) {
    int i = a->slot, w = a->webgpu;
    if (i < 0 || i >= COORD_MAX_TABS || !t->tab_proc[i] || w < 0 || w >= COORD_MAX_WEBGPU || !t->webgpu_proc[w])
        return;
    bool tell = t->tab_ctl[i];
    CHAN(CH_WEBGPU, 1u << 22);
    OP(OP_GRANT, 0, w, SRV_WEBGPU, CH_WEBGPU, 0);
    OP(OP_GIVE_TAB, i, w, 0, CH_WEBGPU, CT_WEBGPU);
    UNDO(2, OP_REVOKE, 0, w, SRV_WEBGPU);
    if (tell) push(s, t, a, p, OP_SEND_TAB, i, 0, 0, 0, CT_WEBGPU_FAILED, 0, false, 1);
    whole(s, t, a, p, tell ? 5 : 4);
}

/* CA_TAB_GONE: the slot's control channel closes, and every server revokes
 * the tab's id. */
/*@ requires \valid_read(s) && \valid_read(t) && \valid_read(a) && \valid(p) && \separated(p, s, t, a);
    requires p->n == 0;
    assigns p->n, p->o[0 .. COORD_MAX_OPS - 1];
    ensures 0 <= p->n <= 5 + COORD_MAX_WEBGPU && ops_ok(s, t, a, p);
 */
static void plan_tab_gone(const CoordState *s, const CoordTables *t, const CoordAction *a, CoordPlan *p) {
    int i = a->slot;
    if (i >= 0 && i < COORD_MAX_TABS) {
        if (t->tab_ctl[i]) OP(OP_CLOSE_CTL, i, 0, 0, 0, 0);
        OP(OP_FORGET_WEBGPU, i, 0, 0, 0, 0);
    }
    OP(OP_REVOKE, 0, 0, SRV_NET, 0, 0);
    OP(OP_REVOKE, 0, 0, SRV_STORE, 0, 0);
    OP(OP_REVOKE, 0, 0, SRV_GPU, 0, 0);
    /*@ loop invariant 0 <= w <= COORD_MAX_WEBGPU && 0 <= p->n <= 5 + w;
        loop invariant ops_ok(s, t, a, p);
        loop assigns w, p->n, p->o[0 .. COORD_MAX_OPS - 1];
        loop variant COORD_MAX_WEBGPU - w;
     */
    for (int w = 0; w < COORD_MAX_WEBGPU; w++)
        if (t->webgpu_proc[w]) OP(OP_REVOKE, 0, w, SRV_WEBGPU, 0, 0);
}

/* CA_START_WEBGPU: web-GPU process w, labeled with its site. */
/*@ requires \valid_read(s) && \valid_read(t) && \valid_read(a) && \valid(p) && \separated(p, s, t, a);
    requires p->n == 0;
    assigns p->n, p->o[0 .. COORD_MAX_OPS - 1];
    ensures 0 <= p->n <= 1 && ops_ok(s, t, a, p);
    ensures \result <==> p->n == 1;
 */
static bool plan_start_webgpu(const CoordState *s, const CoordTables *t, const CoordAction *a, CoordPlan *p) {
    return OP(OP_SPAWN_WEBGPU, 0, a->webgpu, 0, 0, 0);
}

/* The actions with one operation, if the tables bear them out. */
/*@ requires \valid_read(s) && \valid_read(t) && \valid_read(a) && \valid(p) && \separated(p, s, t, a);
    requires p->n == 0;
    assigns p->n, p->o[0 .. COORD_MAX_OPS - 1];
    ensures 0 <= p->n <= 1 && ops_ok(s, t, a, p);
 */
static void plan_one(const CoordState *s, const CoordTables *t, const CoordAction *a, CoordPlan *p) {
    int i = a->slot, w = a->webgpu;
    switch (a->kind) {
    case CA_KILL_TAB:
        OP(OP_KILL_TAB, i, 0, 0, 0, 0);
        return;
    case CA_KILL_WEBGPU:
        OP(OP_KILL_WEBGPU, 0, w, 0, 0, 0);
        return;
    case CA_WEBGPU_FAILED:
        OP(OP_SEND_TAB, i, 0, 0, 0, CT_WEBGPU_FAILED);
        return;
    case CA_SEND_UI:
        OP(OP_SEND_UI, 0, 0, 0, 0, 0);
        return;
    case CA_RESTART_GPU:
        OP(OP_RESTART_GPU, 0, 0, 0, 0, 0);
        return;
    case CA_EXIT:
        OP(OP_EXIT, 0, 0, 0, 0, 0);
        return;
    default:
        return;
    }
}

void coord_plan(const CoordState *s, const CoordTables *t, const CoordAction *a, CoordPlan *p) {
    p->n = 0;
    p->stop = true;
    p->result = false;
    p->ok_kind = p->fail_kind = CE_TAB_START_FAILED;
    p->result_slot = -1;
    switch (a->kind) {
    case CA_START_TAB:
        p->result = true;
        p->result_slot = a->slot;
        if (plan_start_tab(s, t, a, p)) p->ok_kind = CE_TAB_STARTED;
        return;
    case CA_START_WEBGPU:
        p->result = true;
        p->result_slot = a->webgpu;
        p->fail_kind = CE_WEBGPU_EXITED;
        p->ok_kind = plan_start_webgpu(s, t, a, p) ? CE_WEBGPU_STARTED : CE_WEBGPU_EXITED;
        return;
    case CA_REGRANT_GPU:
        plan_regrant_gpu(s, t, a, p);
        return;
    case CA_GRANT_WEBGPU:
        plan_grant_webgpu(s, t, a, p);
        return;
    case CA_TAB_GONE:
        p->stop = false;                /* every revocation, whatever the others do */
        plan_tab_gone(s, t, a, p);
        return;
    default:
        plan_one(s, t, a, p);
        return;
    }
}

/* ---- performing a plan ------------------------------------------------------ */

/* The tables after operation o of the plan for action a, done (ok) or not:
 * a committed tab process holds the label the plan started it with; a
 * web-GPU process runs if its start worked, with its site's label. */
/*@ requires \valid(t) && \valid_read(a) && \valid_read(o) && \separated(t, a, o);
    assigns *t;
    ensures o->kind == OP_COMMIT_TAB && ok && 0 <= o->slot < COORD_MAX_TABS ==>
        t->tab_proc[o->slot] && t->tab_ctl[o->slot] && same_label(&t->tab_site[o->slot][0], &a->site[0]);
    ensures o->kind == OP_SPAWN_WEBGPU && 0 <= o->w < COORD_MAX_WEBGPU ==>
        (t->webgpu_proc[o->w] <==> ok) && (ok ==> same_label(&t->webgpu_site[o->w][0], &a->site[0]));
    ensures o->kind == OP_GIVE_TAB && ok && o->msg == CT_WEBGPU && 0 <= o->slot < COORD_MAX_TABS &&
        0 <= o->w < COORD_MAX_WEBGPU ==> t->tab_webgpu[o->slot] == o->w + 1;
    ensures o->kind == OP_CLOSE_CTL && 0 <= o->slot < COORD_MAX_TABS ==> !t->tab_ctl[o->slot];
    ensures o->kind == OP_FORGET_WEBGPU && 0 <= o->slot < COORD_MAX_TABS ==> t->tab_webgpu[o->slot] == 0;
    ensures o->kind == OP_RESTART_GPU ==> t->gpu == ok;
 */
static void apply(CoordTables *t, const CoordAction *a, const CoordOp *o, bool ok) {
    int i = o->slot, w = o->w;
    bool tab = i >= 0 && i < COORD_MAX_TABS, web = w >= 0 && w < COORD_MAX_WEBGPU;
    switch (o->kind) {
    case OP_COMMIT_TAB:
        if (!ok || !tab) return;
        t->tab_proc[i] = true;
        t->tab_ctl[i] = true;
        copy_site(t->tab_site[i], a->site);
        return;
    case OP_SPAWN_WEBGPU:
        if (!web) return;
        t->webgpu_proc[w] = ok;
        if (ok) copy_site(t->webgpu_site[w], a->site);
        return;
    case OP_GIVE_TAB:
        if (ok && o->msg == CT_WEBGPU && tab && web) t->tab_webgpu[i] = w + 1;
        return;
    case OP_CLOSE_CTL:
        if (tab) t->tab_ctl[i] = false;
        return;
    case OP_FORGET_WEBGPU:
        if (tab) t->tab_webgpu[i] = 0;
        return;
    case OP_RESTART_GPU:
        t->gpu = ok;
        return;
    default:
        return;
    }
}

bool coord_run(CoordTables *t, const CoordAction *a, const CoordPlan *p, CoordEvent *e) {
    int failed = -1;
    /*@ loop invariant 0 <= k <= p->n && -1 <= failed < p->n;
        loop assigns k, failed, *t;
        loop variant p->n - k;
     */
    for (int k = 0; k < p->n; k++) {
        if (p->o[k].undo >= 0) continue;
        bool ok = coord_do(a, &p->o[k]);
        apply(t, a, &p->o[k], ok);
        if (ok) continue;
        if (failed < 0) failed = k;
        if (p->stop) break;
    }
    if (failed >= 0) {
        /*@ loop invariant 0 <= k <= p->n;
            loop assigns k, *t;
            loop variant p->n - k;
         */
        for (int k = 0; k < p->n; k++) {
            if (p->o[k].undo < 0 || failed < p->o[k].undo) continue;
            bool ok = coord_do(a, &p->o[k]);
            apply(t, a, &p->o[k], ok);
        }
    }
    if (!p->result) return false;
    e->kind = failed < 0 ? p->ok_kind : p->fail_kind;
    e->slot = p->result_slot;
    e->type = 0;
    e->data = 0;
    e->len = 0;
    e->exit_code = 0;
    return true;
}

/* ---- exits ------------------------------------------------------------------ */

/*@ requires \valid(e);
    assigns *e;
    ensures e->kind == kind && e->slot == slot && e->exit_code == code;
 */
static void exit_event(CoordEvent *e, CoordEventKind kind, int slot, uint32_t code) {
    e->kind = kind;
    e->slot = slot;
    e->type = 0;
    e->data = 0;
    e->len = 0;
    e->exit_code = code;
}

int coord_next_tab_exit(CoordTables *t, int from, CoordEvent *e) {
    if (from >= COORD_MAX_TABS) return -1;
    int i = from < 0 ? 0 : from;
    uint32_t code = 0;
    /*@ loop invariant 0 <= i <= COORD_MAX_TABS && (from >= 0 ==> i >= from);
        loop assigns i, code;
        loop variant COORD_MAX_TABS - i;
     */
    for (; i < COORD_MAX_TABS; i++) {
        if (!t->tab_proc[i]) continue;
        if (coord_tab_signaled(i, &code)) break;
    }
    if (i >= COORD_MAX_TABS) return -1;
    /* The entry the process was found in is the slot the event names. */
    t->tab_proc[i] = false;
    exit_event(e, CE_TAB_EXITED, i, code);
    return i;
}

/* A web-GPU process's end: CT_WEBGPU_FAILED to tabs in use with an open
 * control channel.  The same frame as the plans', for these notices. */
/*@ predicate notice_ok{L}(CoordState *s, CoordTables *t, CoordOp *o) =
        o->kind == OP_SEND_TAB && o->msg == CT_WEBGPU_FAILED && o->undo == -1 &&
        in_use(s, o->slot) && t->tab_ctl[o->slot];

    predicate notices_upto{L}(CoordState *s, CoordTables *t, CoordPlan *p, integer n) =
        \forall integer k; 0 <= k < n ==> notice_ok(s, t, &p->o[k]);

    predicate notice_same{L1, L2}(CoordState *s, CoordTables *t, CoordPlan *p, integer n) =
        (\forall integer k; 0 <= k < n ==>
            \at(p->o[k].kind, L1) == \at(p->o[k].kind, L2) && \at(p->o[k].slot, L1) == \at(p->o[k].slot, L2) &&
            \at(p->o[k].msg, L1) == \at(p->o[k].msg, L2) && \at(p->o[k].undo, L1) == \at(p->o[k].undo, L2)) &&
        (\forall integer i; 0 <= i < COORD_MAX_TABS ==>
            \at(s->slots[i].state, L1) == \at(s->slots[i].state, L2) &&
            \at(t->tab_ctl[i], L1) == \at(t->tab_ctl[i], L2));

    lemma notices_kept{L1, L2}: \forall CoordState *s, CoordTables *t, CoordPlan *p, integer n;
        notices_upto{L1}(s, t, p, n) ==> notice_same{L1, L2}(s, t, p, n) ==> notices_upto{L2}(s, t, p, n);
 */

/* One more notice, to tab slot i, if it is in use with an open control
 * channel. */
/*@ requires \valid_read(s) && \valid_read(t) && \valid(p) && \separated(p, s, t);
    requires 0 <= p->n < COORD_MAX_OPS && notices_upto(s, t, p, p->n);
    assigns p->n, p->o[p->n];
    ensures \old(p->n) <= p->n <= \old(p->n) + 1;
    ensures notices_upto(s, t, p, p->n);
 */
static void push_notice(const CoordState *s, const CoordTables *t, CoordPlan *p, int i, int w) {
    if (i < 0 || i >= COORD_MAX_TABS || !t->tab_ctl[i] || s->slots[i].state == SLOT_FREE) return;
    CoordOp *o = &p->o[p->n];
    o->kind = OP_SEND_TAB;
    o->slot = i;
    o->w = w;
    o->srv = 0;
    o->ch = 0;
    o->msg = CT_WEBGPU_FAILED;
    o->size = 0;
    o->quick = true;
    o->undo = -1;
    p->n++;
    /*@ assert notice_same{Pre, Here}(s, t, p, \at(p->n, Pre)); */
    /*@ assert notices_upto(s, t, p, \at(p->n, Pre)); */
    /*@ assert notice_ok(s, t, &p->o[\at(p->n, Pre)]); */
}

int coord_next_webgpu_exit(const CoordState *s, CoordTables *t, int from, CoordPlan *p, CoordEvent *e) {
    p->n = 0;
    p->stop = false;
    p->result = false;
    p->ok_kind = p->fail_kind = CE_WEBGPU_EXITED;
    p->result_slot = -1;
    if (from >= COORD_MAX_WEBGPU) return -1;
    int w = from < 0 ? 0 : from;
    uint32_t code = 0;          /* (the UI isn't told it) */
    /*@ loop invariant 0 <= w <= COORD_MAX_WEBGPU && (from >= 0 ==> w >= from);
        loop assigns w, code;
        loop variant COORD_MAX_WEBGPU - w;
     */
    for (; w < COORD_MAX_WEBGPU; w++) {
        if (!t->webgpu_proc[w]) continue;
        if (coord_webgpu_signaled(w, &code)) break;
    }
    if (w >= COORD_MAX_WEBGPU) return -1;
    /* The entry the process was found in is the index the event names. */
    t->webgpu_proc[w] = false;
    exit_event(e, CE_WEBGPU_EXITED, w, 0);
    /* The tabs it served: their channels to it are gone. */
    /*@ loop invariant 0 <= i <= COORD_MAX_TABS && 0 <= p->n <= i;
        loop invariant notices_upto(s, t, p, p->n);
        loop assigns i, p->n, p->o[0 .. COORD_MAX_OPS - 1];
        loop variant COORD_MAX_TABS - i;
     */
    for (int i = 0; i < COORD_MAX_TABS; i++)
        if (t->tab_webgpu[i] == w + 1) push_notice(s, t, p, i, w);
Told: __attribute__((unused));
    /*@ loop invariant 0 <= i <= COORD_MAX_TABS;
        loop invariant notice_same{Told, Here}(s, t, p, \at(p->n, Told)) && p->n == \at(p->n, Told);
        loop assigns i, t->tab_webgpu[0 .. COORD_MAX_TABS - 1];
        loop variant COORD_MAX_TABS - i;
     */
    for (int i = 0; i < COORD_MAX_TABS; i++)
        if (t->tab_webgpu[i] == w + 1) t->tab_webgpu[i] = 0;
    /*@ assert notices_upto(s, t, p, p->n); */
    return w;
}
