#ifndef COORD_PLAN_H
#define COORD_PLAN_H

/* How the coordinator performs its core's actions (SANDBOX_DESIGN.md section
 * 8.7.3, step 2), as a pure function: coord_plan turns one action of
 * coord_step, with the shim's tables of processes and channels, into a list
 * of operations, each naming the handle, the process and the badge: "a new
 * channel", "start slot s's tab process with these channels", "grant
 * channel c's server end to server v, badged with the action's site", "copy
 * channel c into slot s's process", "end web-GPU process w".  coord_run
 * performs a plan through coord_do, the shim's one entry to Windows, and
 * keeps the tables; coord_next_tab_exit and coord_next_webgpu_exit pair a
 * process's exit with its table entry.  None of this calls Windows, so it
 * can be proved (M9, tools/prove.sh plan).
 *
 * The tables (CoordTables) mirror the shim's: which slots hold a tab
 * process and an open control channel, which web-GPU processes run, and the
 * label each process was started with.  The shim's own tables of HANDLEs
 * change only as coord_do performs an operation, and these change with them
 * (coord_run), so what the proofs say of the operations is what the shim
 * does with its handles. */

#include <stdbool.h>
#include <stdint.h>
#include "coord_core.h"

#define COORD_MAX_OPS (COORD_MAX_TABS + 4)

typedef enum {
    OP_CHAN,            /* ch, size: a new channel, the coordinator's until the plan ends */
    OP_SPAWN_TAB,       /* slot: start a tab process for slot, labeled with the action's site, given
                           (inherited at its start) the plan's CH_CTL, CH_UI, CH_NET, CH_DRAW and,
                           if made, CH_STORE */
    OP_GRANT,           /* srv, w, ch: ch's server end to server srv (web-GPU process w), for the
                           action's tab, badged with the action's site */
    OP_GIVE_UI,         /* ch: ch's handles to the UI, with CU_TAB_STARTED for the action's tab */
    OP_GIVE_TAB,        /* slot, ch, msg, w: ch's handles into slot's process, told with msg on its
                           control channel (CT_GPU, or CT_WEBGPU: web-GPU process w's) */
    OP_COMMIT_TAB,      /* slot: the new process and CH_CTL become slot's */
    OP_DISCARD_TAB,     /* slot: end the new process, never slot's */
    OP_REVOKE,          /* srv, w: revoke the action's tab at server srv (web-GPU process w) */
    OP_KILL_TAB,        /* slot: end slot's process */
    OP_CLOSE_CTL,       /* slot: close the coordinator's end of slot's control channel */
    OP_FORGET_WEBGPU,   /* slot: slot was given no web-GPU process (the tables only) */
    OP_SEND_TAB,        /* slot, msg, quick: an empty msg (CT_WEBGPU_FAILED) on slot's control channel */
    OP_SPAWN_WEBGPU,    /* w: start web-GPU process w, labeled with the action's site */
    OP_KILL_WEBGPU,     /* w: end it */
    OP_SEND_UI,         /* the action's type and data, to the UI */
    OP_RESTART_GPU,     /* a GPU process again, connected to the UI */
    OP_EXIT,            /* end every process and exit */
} CoordOpKind;

typedef enum { SRV_NET, SRV_STORE, SRV_GPU, SRV_WEBGPU } CoordServer;

/* The channels a plan makes, by what they are for. */
typedef enum { CH_CTL, CH_UI, CH_NET, CH_STORE, CH_DRAW, CH_WEBGPU, CH_COUNT } CoordChan;

typedef struct CoordOp {
    CoordOpKind kind;
    int slot;                   /* a tab slot */
    int w;                      /* a web-GPU process */
    int srv;                    /* a CoordServer */
    int ch;                     /* a CoordChan */
    uint16_t msg;               /* CT_* */
    uint32_t size;              /* OP_CHAN: its rings' bytes */
    bool quick;                 /* OP_SEND_TAB: wait 100 ms for room, not 1 s */
    int undo;                   /* -1: in order; else done after the others, only if one at index
                                   >= undo failed */
} CoordOp;

typedef struct CoordPlan {
    int n;
    CoordOp o[COORD_MAX_OPS];
    bool stop;                  /* stop at the first operation that fails */
    bool result;                /* an event follows: ok_kind if none failed, else fail_kind */
    CoordEventKind ok_kind, fail_kind;
    int result_slot;            /* its slot */
} CoordPlan;

typedef struct CoordTables {
    bool tab_proc[COORD_MAX_TABS];              /* the slot holds a tab process */
    bool tab_ctl[COORD_MAX_TABS];               /* its control channel is open */
    char tab_site[COORD_MAX_TABS][SITE_MAX];    /* the label its process was started with */
    int tab_webgpu[COORD_MAX_TABS];             /* w + 1 of the web-GPU process it was given; 0 none */
    bool webgpu_proc[COORD_MAX_WEBGPU];         /* web-GPU process w runs */
    char webgpu_site[COORD_MAX_WEBGPU][SITE_MAX];
    bool gpu;                                   /* a GPU process runs */
} CoordTables;

/* What is proved of the plans, in ACSL. */
/*@ predicate srv_chan(integer srv, integer ch) =
        (srv == SRV_NET && ch == CH_NET) || (srv == SRV_STORE && ch == CH_STORE) ||
        (srv == SRV_GPU && ch == CH_DRAW) || (srv == SRV_WEBGPU && ch == CH_WEBGPU);

    predicate in_use{L}(CoordState *s, integer i) = 0 <= i < COORD_MAX_TABS && s->slots[i].state != SLOT_FREE;

    // The action's tab: a slot in use, with the action's id and label.
    predicate client_ok{L}(CoordState *s, CoordAction *a) =
        in_use(s, a->slot) && s->slots[a->slot].tab == a->tab &&
        same_label(&a->site[0], &s->slots[a->slot].site[0]);

    // The process in tab slot i: there is one, started labeled with site.
    predicate tab_is{L}(CoordTables *t, integer i, char *site) =
        0 <= i < COORD_MAX_TABS && t->tab_proc[i] && same_label(&t->tab_site[i][0], site);

    // Web-GPU process w: it runs, the core's and started labeled with site.
    predicate webgpu_is{L}(CoordState *s, CoordTables *t, integer w, char *site) =
        0 <= w < COORD_MAX_WEBGPU && t->webgpu_proc[w] && s->webgpu[w].state == SLOT_RUNNING &&
        site[0] != 0 && same_label(&t->webgpu_site[w][0], site) && same_label(&s->webgpu[w].site[0], site);

    // A tab process about to start in slot i, which holds none.
    predicate new_tab{L}(CoordState *s, CoordTables *t, CoordAction *a, integer i) =
        i == a->slot && client_ok(s, a) && s->slots[i].state == SLOT_STARTING && !t->tab_proc[i];

    // An operation, by its fields.  P3 at the level of handles:
    //  - the server end of a channel goes to the server that channel is for,
    //    badged with the label of the action's slot (and the storage
    //    service's only for a site);
    //  - its other end goes only into the process of that slot: at its
    //    start, or copied into the process the slot holds, which was started
    //    with that label;
    //  - a web-GPU channel comes only from a running web-GPU process of
    //    that same site;
    //  - every slot and index is in range, and an operation on a tab's
    //    process or channel names a slot in use.
    predicate op_vals_ok{L}(CoordState *s, CoordTables *t, CoordAction *a,
                            integer kind, integer slot, integer w, integer srv, integer ch, integer msg) =
        (kind == OP_CHAN ==> 0 <= ch < CH_COUNT) &&
        ((kind == OP_SPAWN_TAB || kind == OP_COMMIT_TAB || kind == OP_DISCARD_TAB) ==> new_tab(s, t, a, slot)) &&
        (kind == OP_GRANT ==>
            srv_chan(srv, ch) && client_ok(s, a) && (srv == SRV_STORE ==> a->site[0] != 0) &&
            (srv == SRV_WEBGPU ==> w == a->webgpu && webgpu_is(s, t, w, &a->site[0]))) &&
        (kind == OP_GIVE_UI ==> ch == CH_UI) &&
        (kind == OP_GIVE_TAB ==>
            slot == a->slot && client_ok(s, a) && tab_is(t, slot, &a->site[0]) &&
            ((ch == CH_DRAW && msg == CT_GPU) ||
             (ch == CH_WEBGPU && msg == CT_WEBGPU && w == a->webgpu && webgpu_is(s, t, w, &a->site[0])))) &&
        (kind == OP_REVOKE ==>
            srv == SRV_NET || srv == SRV_STORE || srv == SRV_GPU || (srv == SRV_WEBGPU && 0 <= w < COORD_MAX_WEBGPU)) &&
        (kind == OP_KILL_TAB ==> in_use(s, slot) && t->tab_proc[slot]) &&
        (kind == OP_SEND_TAB ==> in_use(s, slot) && t->tab_ctl[slot] && msg == CT_WEBGPU_FAILED) &&
        (kind == OP_CLOSE_CTL ==> 0 <= slot < COORD_MAX_TABS && t->tab_ctl[slot]) &&
        (kind == OP_FORGET_WEBGPU ==> 0 <= slot < COORD_MAX_TABS) &&
        (kind == OP_SPAWN_WEBGPU ==>
            0 <= w < COORD_MAX_WEBGPU && w == a->webgpu && s->webgpu[w].state == SLOT_STARTING &&
            a->site[0] != 0 && same_label(&a->site[0], &s->webgpu[w].site[0])) &&
        (kind == OP_KILL_WEBGPU ==> 0 <= w < COORD_MAX_WEBGPU && t->webgpu_proc[w] && s->webgpu[w].state != SLOT_FREE);

    predicate op_ok{L}(CoordState *s, CoordTables *t, CoordAction *a, CoordOp *o) =
        op_vals_ok(s, t, a, o->kind, o->slot, o->w, o->srv, o->ch, o->msg);

    // The event that follows a plan names the action's slot: a tab's start,
    // or a web-GPU process's.
    predicate result_ok{L}(CoordAction *a, CoordPlan *p) =
        p->result ==>
            (a->kind == CA_START_TAB && p->result_slot == a->slot && p->fail_kind == CE_TAB_START_FAILED &&
             (p->ok_kind == CE_TAB_STARTED || p->ok_kind == CE_TAB_START_FAILED)) ||
            (a->kind == CA_START_WEBGPU && p->result_slot == a->webgpu && p->fail_kind == CE_WEBGPU_EXITED &&
             (p->ok_kind == CE_WEBGPU_STARTED || p->ok_kind == CE_WEBGPU_EXITED));

    predicate event_kind_ok(integer k) =
        k == CE_TAB_STARTED || k == CE_TAB_START_FAILED || k == CE_WEBGPU_STARTED || k == CE_WEBGPU_EXITED;
 */

/*@ requires \valid(t);
    assigns *t;
    ensures t->gpu == gpu;
    ensures \forall integer i; 0 <= i < COORD_MAX_TABS ==> !t->tab_proc[i] && !t->tab_ctl[i] && t->tab_webgpu[i] == 0;
    ensures \forall integer w; 0 <= w < COORD_MAX_WEBGPU ==> !t->webgpu_proc[w];
 */
void coord_tables_init(CoordTables *t, bool gpu);

/* The operations for action a, given the core's state after the step that
 * made it and the tables as they are.  An action the state or the tables
 * don't bear out (none that coord_step makes) gets no operations, and a
 * start a failed result. */
/*@ requires \valid_read(s) && \valid_read(t) && \valid_read(a) && \valid(p);
    requires \separated(p, s, t, a);
    assigns *p;
    ensures 0 <= p->n <= COORD_MAX_OPS;
    ensures \forall integer k; 0 <= k < p->n ==> op_ok(s, t, a, &p->o[k]);
    ensures result_ok(a, p);
 */
void coord_plan(const CoordState *s, const CoordTables *t, const CoordAction *a, CoordPlan *p);

/* The shim's one entry to Windows (coordinator.c): perform operation o of
 * the plan for action a, and say whether it worked.  Trusted, not proved:
 * this contract is what the proofs assume of it.  It touches none of the
 * proved state (the core's, the tables, the plan); the handles it copies,
 * the processes it starts and the badges it gives are those the operation
 * names (coordinator.c says how, wrapper by wrapper). */
/*@ requires \valid(a) && \valid(o);
    terminates \true;
    exits \true;
    assigns \nothing;
 */
bool coord_do(const CoordAction *a, const CoordOp *o);

/* Perform plan p for action a: its operations in order through coord_do,
 * stopping at the first that fails if p->stop; then, if one failed, the
 * undo operations for it.  The tables follow what was done.  True if an
 * event follows, in *e: the plan's result, for its slot. */
/*@ requires \valid(t) && \valid(a) && \valid(p) && \valid(e);
    requires \separated(t, a, p, e);
    requires 0 <= p->n <= COORD_MAX_OPS;
    exits \true;                // (OP_EXIT ends the coordinator)
    assigns *t, *e;
    ensures \result <==> p->result;
    ensures \result ==> e->slot == p->result_slot && (e->kind == p->ok_kind || e->kind == p->fail_kind);
 */
bool coord_run(CoordTables *t, const CoordAction *a, const CoordPlan *p, CoordEvent *e);

/* Has the process in tab slot i (web-GPU process w) ended?  If so, its exit
 * code, and the shim has let go of its handles.  Trusted (coordinator.c). */
/*@ requires 0 <= i < COORD_MAX_TABS && \valid(code);
    terminates \true;
    exits \false;
    assigns *code;
 */
bool coord_tab_signaled(int i, uint32_t *code);
/*@ requires 0 <= w < COORD_MAX_WEBGPU && \valid(code);
    terminates \true;
    exits \false;
    assigns *code;
 */
bool coord_webgpu_signaled(int w, uint32_t *code);

/* The next tab slot, from `from` on, whose process has ended: its entry
 * empties, and *e is its CE_TAB_EXITED.  -1 if none. */
/*@ requires \valid(t) && \valid(e) && \separated(t, e);
    assigns t->tab_proc[0 .. COORD_MAX_TABS - 1], *e;
    ensures -1 <= \result < COORD_MAX_TABS;
    ensures \result >= 0 ==>
        \result >= from && \old(t->tab_proc[\result]) && !t->tab_proc[\result] &&
        e->kind == CE_TAB_EXITED && e->slot == \result;
    ensures \forall integer i; 0 <= i < COORD_MAX_TABS && i != \result ==> t->tab_proc[i] == \old(t->tab_proc[i]);
 */
int coord_next_tab_exit(CoordTables *t, int from, CoordEvent *e);

/* The next web-GPU process, from `from` on, that has ended: its entry
 * empties, *e is its CE_WEBGPU_EXITED, and *p tells the tabs it served that
 * their channels to it are gone.  -1 if none. */
/*@ requires \valid_read(s) && \valid(t) && \valid(p) && \valid(e) && \separated(s, t, p, e);
    assigns t->webgpu_proc[0 .. COORD_MAX_WEBGPU - 1], t->tab_webgpu[0 .. COORD_MAX_TABS - 1], *p, *e;
    ensures -1 <= \result < COORD_MAX_WEBGPU;
    ensures \result >= 0 ==>
        \result >= from && \old(t->webgpu_proc[\result]) && !t->webgpu_proc[\result] &&
        e->kind == CE_WEBGPU_EXITED && e->slot == \result;
    ensures 0 <= p->n <= COORD_MAX_OPS && !p->result;
    ensures \forall integer k; 0 <= k < p->n ==>
        p->o[k].kind == OP_SEND_TAB && p->o[k].msg == CT_WEBGPU_FAILED && in_use(s, p->o[k].slot) &&
        t->tab_ctl[p->o[k].slot] && p->o[k].undo == -1;
 */
int coord_next_webgpu_exit(const CoordState *s, CoordTables *t, int from, CoordPlan *p, CoordEvent *e);

#endif
