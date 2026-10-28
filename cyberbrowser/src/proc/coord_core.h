#ifndef COORD_CORE_H
#define COORD_CORE_H

/* The coordinator's decisions (SANDBOX_DESIGN.md section 2.1), as a pure
 * state machine: coord_step takes the state and one event and fills a list of
 * actions; it does no I/O and calls nothing outside this file, so it can be
 * proved (M4).  The shim (coordinator.c) turns messages and process exits into
 * events and performs the actions with Win32 calls.
 *
 * The UI asks for tabs; the core checks each request, labels the tab with
 * its site (site.h), and starts or ends its process.  Starting a tab also
 * grants it channels to the network process and (unless its site is opaque)
 * the storage service, both badged with that site. */

#include <stdbool.h>
#include <stdint.h>
#include "proto.h"
#include "site.h"

#define COORD_MAX_TABS 60       /* the shim waits on all their processes at once */

typedef enum { SLOT_FREE = 0, SLOT_STARTING, SLOT_RUNNING, SLOT_CLOSING } CoordSlotState;

typedef struct CoordSlot {
    CoordSlotState state;
    uint32_t tab;               /* the UI's id */
    char site[SITE_MAX];        /* its label; "" = opaque (no storage) */
} CoordSlot;

typedef struct CoordState {
    CoordSlot slots[COORD_MAX_TABS];
    bool exiting;
} CoordState;

typedef enum {
    CE_UI_MESSAGE,              /* type, data, len: a message from the UI */
    CE_UI_EXITED,               /* the UI process ended, or its channel broke */
    CE_TAB_STARTED,             /* slot: CA_START_TAB succeeded */
    CE_TAB_START_FAILED,        /* slot: it didn't */
    CE_TAB_EXITED,              /* slot, exit_code: its process ended */
    CE_SERVICE_EXITED,          /* the network process or the storage service ended */
} CoordEventKind;

typedef struct CoordEvent {
    CoordEventKind kind;
    int slot;
    uint16_t type;
    const uint8_t *data;
    uint32_t len;
    uint32_t exit_code;
} CoordEvent;

typedef enum {
    CA_START_TAB,               /* slot, tab, width, height, site, url (len bytes), and the
                                   servers to grant it channels to, badged with site */
    CA_KILL_TAB,                /* slot */
    CA_TAB_GONE,                /* slot, tab: revoke its channels to the servers */
    CA_SEND_UI,                 /* type, data (len bytes) */
    CA_EXIT,                    /* end every process and exit */
} CoordActionKind;

typedef struct CoordAction {
    CoordActionKind kind;
    int slot;
    uint32_t tab, width, height;
    char site[SITE_MAX];
    bool grant_net, grant_store;    /* CA_START_TAB */
    uint16_t type;
    uint32_t len;
    uint8_t data[PROTO_URL_MAX];
} CoordAction;

#define COORD_MAX_ACTIONS (COORD_MAX_TABS + 1)

typedef struct CoordActions {
    int n;
    CoordAction a[COORD_MAX_ACTIONS];
} CoordActions;


/* What M4 proves of coord_step (SANDBOX_DESIGN.md section 8.3), in ACSL. */
/*@ predicate slot_state_ok(integer st) =
        st == SLOT_FREE || st == SLOT_STARTING || st == SLOT_RUNNING || st == SLOT_CLOSING;

    // A badge: a site label, or "" for an opaque origin (no storage).
    predicate badge_ok{L}(char *site) = site[0] == 0 || label_ok(site);

    predicate state_ok{L}(CoordState *s) =
        \forall integer i; 0 <= i < COORD_MAX_TABS ==>
            slot_state_ok(s->slots[i].state) &&
            (s->slots[i].state != SLOT_FREE ==> badge_ok(&s->slots[i].site[0]));

    // The UI's ids name one tab at a time, so revoking by id ends one tab's channels.
    predicate tabs_unique{L}(CoordState *s) =
        \forall integer i, j; 0 <= i < j < COORD_MAX_TABS &&
            s->slots[i].state != SLOT_FREE && s->slots[j].state != SLOT_FREE ==> s->slots[i].tab != s->slots[j].tab;

    predicate slot_unchanged{L1, L2}(CoordState *s, integer i) =
        \at(s->slots[i].state, L1) == \at(s->slots[i].state, L2) &&
        \at(s->slots[i].tab, L1) == \at(s->slots[i].tab, L2) &&
        \forall integer k; 0 <= k < SITE_MAX ==> \at(s->slots[i].site[k], L1) == \at(s->slots[i].site[k], L2);

    // P3: a process's label never changes while its slot is in use.
    predicate labels_kept{L1, L2}(CoordState *s) =
        \forall integer i; 0 <= i < COORD_MAX_TABS &&
            \at(s->slots[i].state, L1) != SLOT_FREE && \at(s->slots[i].state, L2) != SLOT_FREE ==>
            \at(s->slots[i].tab, L1) == \at(s->slots[i].tab, L2) &&
            \forall integer k; 0 <= k < SITE_MAX ==> \at(s->slots[i].site[k], L1) == \at(s->slots[i].site[k], L2);

    // P3: a tab is started with channels badged with its slot's label, the
    // network process's always and the storage service's only for a site,
    // and with a URL the coordinator accepts; the other actions name a slot.
    predicate action_ok{L}(CoordState *s, CoordAction *a) =
        (a->kind == CA_START_TAB ==>
            0 <= a->slot < COORD_MAX_TABS && s->slots[a->slot].state == SLOT_STARTING &&
            s->slots[a->slot].tab == a->tab && badge_ok(&a->site[0]) &&
            (\forall integer k; 0 <= k < SITE_MAX ==> a->site[k] == s->slots[a->slot].site[k]) &&
            a->grant_net && (a->grant_store <==> a->site[0] != 0) &&
            1 <= a->len <= PROTO_URL_MAX && (\forall integer k; 0 <= k < a->len ==> 0x20 < a->data[k] < 0x7f)) &&
        ((a->kind == CA_KILL_TAB || a->kind == CA_TAB_GONE) ==> 0 <= a->slot < COORD_MAX_TABS) &&
        (a->kind == CA_SEND_UI ==> a->len <= PROTO_URL_MAX);

    predicate tab_event(integer kind) =
        kind == CE_TAB_STARTED || kind == CE_TAB_START_FAILED || kind == CE_TAB_EXITED;
 */

/*@ requires \valid(s);
    assigns s->slots[0 .. COORD_MAX_TABS - 1], s->exiting;
    ensures state_ok(s) && tabs_unique(s);
 */
void coord_init(CoordState *s);

/*@ requires \valid(s) && \valid(out) && \valid_read(e) && \separated(s, out, e);
    requires e->kind == CE_UI_MESSAGE ==>
        \valid_read(e->data + (0 .. e->len - 1)) && \separated(s, out, e, e->data + (0 .. e->len - 1));
    requires state_ok(s) && tabs_unique(s);
    assigns *s, *out;
    ensures state_ok(s) && tabs_unique(s);
    ensures 0 <= out->n <= COORD_MAX_ACTIONS;
    ensures labels_kept{Old, Here}(s);
    ensures \forall integer k; 0 <= k < out->n ==> action_ok(s, &out->a[k]);
    ensures \forall integer k; 0 <= k < out->n && out->a[k].kind == CA_START_TAB ==>
        \let i = out->a[k].slot; \at(s->slots[i].state, Old) == SLOT_FREE;
    ensures \forall integer k; 0 <= k < out->n && out->a[k].kind == CA_TAB_GONE ==>
        \let i = out->a[k].slot; out->a[k].tab == \at(s->slots[i].tab, Old);
    // P6, local respect: an event of a tab's process (it started, failed to,
    // or ended) changes nothing of any other slot and causes actions only
    // about that tab: its revocation, and a message to the UI.
    ensures tab_event(e->kind) ==>
        (\forall integer j; 0 <= j < COORD_MAX_TABS && j != e->slot ==> slot_unchanged{Old, Here}(s, j)) &&
        (\forall integer k; 0 <= k < out->n ==>
            (out->a[k].kind == CA_TAB_GONE && out->a[k].slot == e->slot) || out->a[k].kind == CA_SEND_UI);
 */
void coord_step(CoordState *s, const CoordEvent *e, CoordActions *out);

/* Is `url` (len bytes) one the coordinator lets a tab load: http:, https: or
 * about:blank, printable ASCII, at most PROTO_URL_MAX bytes. */
/*@ requires \valid_read(url + (0 .. len - 1));
    assigns \nothing;
    ensures \result ==> 1 <= len <= PROTO_URL_MAX && \forall integer k; 0 <= k < len ==> 0x20 < url[k] < 0x7f;
 */
bool coord_url_ok(const uint8_t *url, uint32_t len);

#endif
