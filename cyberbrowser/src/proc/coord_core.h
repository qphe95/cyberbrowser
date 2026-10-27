#ifndef COORD_CORE_H
#define COORD_CORE_H

/* The coordinator's decisions (SANDBOX_DESIGN.md section 2.1), as a pure
 * state machine: coord_step takes the state and one event and fills a list of
 * actions; it does no I/O and calls nothing outside this file, so it can be
 * proved (M4).  The shim (coordinator.c) turns messages and process exits into
 * events and performs the actions with Win32 calls.
 *
 * M1: the UI asks for tabs; the core checks each request and starts or ends
 * tab processes.  Labels and channel grants between tabs come with M2. */

#include <stdbool.h>
#include <stdint.h>
#include "proto.h"

#define COORD_MAX_TABS 60       /* the shim waits on all their processes at once */

typedef enum { SLOT_FREE = 0, SLOT_STARTING, SLOT_RUNNING, SLOT_CLOSING } CoordSlotState;

typedef struct CoordSlot {
    CoordSlotState state;
    uint32_t tab;               /* the UI's id */
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
    CA_START_TAB,               /* slot, tab, width, height, url (len bytes) */
    CA_KILL_TAB,                /* slot */
    CA_SEND_UI,                 /* type, data (len bytes) */
    CA_EXIT,                    /* end every process and exit */
} CoordActionKind;

typedef struct CoordAction {
    CoordActionKind kind;
    int slot;
    uint32_t tab, width, height;
    uint16_t type;
    uint32_t len;
    uint8_t data[PROTO_URL_MAX];
} CoordAction;

#define COORD_MAX_ACTIONS (COORD_MAX_TABS + 1)

typedef struct CoordActions {
    int n;
    CoordAction a[COORD_MAX_ACTIONS];
} CoordActions;

void coord_init(CoordState *s);
void coord_step(CoordState *s, const CoordEvent *e, CoordActions *out);

/* Is `url` (len bytes) one the coordinator lets a tab load: http:, https: or
 * about:blank, printable ASCII, at most PROTO_URL_MAX bytes. */
bool coord_url_ok(const uint8_t *url, uint32_t len);

#endif
