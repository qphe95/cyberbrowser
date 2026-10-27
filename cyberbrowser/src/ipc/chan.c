/* chan.c - libchan (chan.h). */
#include "chan.h"

#include <string.h>

#define CHAN_MAGIC 0x6e616863u      /* "chan" */

/* One direction.  The producer owns tail, the consumer owns head and
 * waiting (the consumer is about to sleep); each in its own cache line. */
typedef struct ChanRing {
    uint64_t tail;
    uint8_t pad0[56];
    uint64_t head;
    uint8_t pad1[56];
    uint32_t waiting;
    uint8_t pad2[60];
} ChanRing;

struct ChanShared {
    uint32_t magic;
    uint32_t ring_bytes;
    uint8_t pad[56];
    ChanRing ring[2];
};

#define HDR_BYTES 8u

static uint32_t pad8(uint32_t n) { return (n + 7u) & ~7u; }

static size_t section_bytes(uint32_t ring_bytes) {
    return sizeof(ChanShared) + 2 * (size_t)ring_bytes;
}

static bool map(Chan *c, uint32_t ring_bytes) {
    c->sh = (ChanShared *)MapViewOfFile(c->section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                                        section_bytes(ring_bytes));
    if (!c->sh) return false;
    c->ring_bytes = ring_bytes;
    c->data[0] = (uint8_t *)(c->sh + 1);
    c->data[1] = c->data[0] + ring_bytes;
    return true;
}

bool chan_create(Chan *c, uint32_t ring_bytes) {
    memset(c, 0, sizeof(*c));
    if (ring_bytes < 2 * (CHAN_MSG_MAX + HDR_BYTES) || (ring_bytes & (ring_bytes - 1))) return false;
    size_t bytes = section_bytes(ring_bytes);
    c->section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                    (DWORD)((uint64_t)bytes >> 32), (DWORD)bytes, NULL);
    c->ev[0] = CreateEventW(NULL, FALSE, FALSE, NULL);
    c->ev[1] = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!c->section || !c->ev[0] || !c->ev[1] || !map(c, ring_bytes)) {
        chan_close(c);
        return false;
    }
    c->sh->magic = CHAN_MAGIC;
    c->sh->ring_bytes = ring_bytes;
    c->side = 0;
    return true;
}

bool chan_attach(Chan *c, HANDLE section, HANDLE ev0, HANDLE ev1, int side) {
    memset(c, 0, sizeof(*c));
    c->section = section;
    c->ev[0] = ev0;
    c->ev[1] = ev1;
    c->side = side ? 1 : 0;
    /* Map the header first to learn the ring size, then the whole section. */
    ChanShared *probe = (ChanShared *)MapViewOfFile(section, FILE_MAP_READ, 0, 0, sizeof(ChanShared));
    if (!probe) return false;
    uint32_t magic = probe->magic, ring_bytes = probe->ring_bytes;
    UnmapViewOfFile(probe);
    if (magic != CHAN_MAGIC || ring_bytes < 2 * (CHAN_MSG_MAX + HDR_BYTES) ||
        ring_bytes > (1u << 28) || (ring_bytes & (ring_bytes - 1)))
        return false;
    return map(c, ring_bytes);
}

void chan_close(Chan *c) {
    if (c->sh) UnmapViewOfFile(c->sh);
    if (c->section) CloseHandle(c->section);
    if (c->ev[0]) CloseHandle(c->ev[0]);
    if (c->ev[1]) CloseHandle(c->ev[1]);
    memset(c, 0, sizeof(*c));
}

void chan_inheritable(Chan *c, bool on) {
    DWORD f = on ? HANDLE_FLAG_INHERIT : 0;
    SetHandleInformation(c->section, HANDLE_FLAG_INHERIT, f);
    SetHandleInformation(c->ev[0], HANDLE_FLAG_INHERIT, f);
    SetHandleInformation(c->ev[1], HANDLE_FLAG_INHERIT, f);
}

/* Copy n bytes into the ring at byte counter pos, wrapping. */
static void ring_write(uint8_t *ring, uint32_t size, uint64_t pos, const void *src, uint32_t n) {
    uint32_t at = (uint32_t)(pos & (size - 1)), first = size - at;
    if (first >= n) {
        memcpy(ring + at, src, n);
    } else {
        memcpy(ring + at, src, first);
        memcpy(ring, (const uint8_t *)src + first, n - first);
    }
}

static void ring_read(const uint8_t *ring, uint32_t size, uint64_t pos, void *dst, uint32_t n) {
    uint32_t at = (uint32_t)(pos & (size - 1)), first = size - at;
    if (first >= n) {
        memcpy(dst, ring + at, n);
    } else {
        memcpy(dst, ring + at, first);
        memcpy((uint8_t *)dst + first, ring, n - first);
    }
}

bool chan_send(Chan *c, uint16_t type, const void *payload, uint32_t len, DWORD timeout_ms) {
    if (!c->sh || c->broken || len > CHAN_MSG_MAX) return false;
    ChanRing *r = &c->sh->ring[c->side];
    uint8_t *ring = c->data[c->side];
    uint32_t size = c->ring_bytes, need = HDR_BYTES + pad8(len);
    uint64_t tail = r->tail;        /* ours: only we write it */
    DWORD start = GetTickCount();
    for (int spins = 0;; spins++) {
        uint64_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
        uint64_t used = tail - head;
        if (used > size) { c->broken = true; return false; }
        if (size - used >= need) break;
        if (GetTickCount() - start > timeout_ms) return false;
        if (spins < 64) YieldProcessor();
        else Sleep(spins < 1000 ? 0 : 1);
    }
    uint8_t hdr[HDR_BYTES] = { 0 };
    memcpy(hdr, &len, 4);
    memcpy(hdr + 4, &type, 2);
    ring_write(ring, size, tail, hdr, HDR_BYTES);
    if (len) ring_write(ring, size, tail + HDR_BYTES, payload, len);
    __atomic_store_n(&r->tail, tail + need, __ATOMIC_RELEASE);
    /* Pairs with chan_arm: the store of tail before the load of waiting. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&r->waiting, __ATOMIC_RELAXED)) SetEvent(c->ev[1 - c->side]);
    return true;
}

int chan_recv(Chan *c, ChanMsg *m) {
    if (!c->sh || c->broken) return -1;
    int dir = 1 - c->side;
    ChanRing *r = &c->sh->ring[dir];
    const uint8_t *ring = c->data[dir];
    uint32_t size = c->ring_bytes;
    uint64_t head = r->head;        /* ours */
    uint64_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);   /* read once */
    uint64_t avail = tail - head;
    if (avail == 0) return 0;
    if (avail > size || avail < HDR_BYTES) { c->broken = true; return -1; }
    uint8_t hdr[HDR_BYTES];
    ring_read(ring, size, head, hdr, HDR_BYTES);
    uint32_t len;
    uint16_t type;
    memcpy(&len, hdr, 4);
    memcpy(&type, hdr + 4, 2);
    if (len > CHAN_MSG_MAX || HDR_BYTES + (uint64_t)pad8(len) > avail) { c->broken = true; return -1; }
    if (len) ring_read(ring, size, head + HDR_BYTES, m->data, len);
    m->type = type;
    m->len = len;
    __atomic_store_n(&r->head, head + HDR_BYTES + pad8(len), __ATOMIC_RELEASE);
    return 1;
}

bool chan_arm(Chan *c) {
    if (!c->sh) return false;
    ChanRing *r = &c->sh->ring[1 - c->side];
    __atomic_store_n(&r->waiting, 1u, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&r->tail, __ATOMIC_ACQUIRE) != r->head) {
        __atomic_store_n(&r->waiting, 0u, __ATOMIC_RELAXED);
        return false;
    }
    return true;
}

void chan_disarm(Chan *c) {
    if (!c->sh) return;
    __atomic_store_n(&c->sh->ring[1 - c->side].waiting, 0u, __ATOMIC_RELAXED);
}

HANDLE chan_event(Chan *c) {
    return c->ev[c->side];
}

bool chan_wait(Chan *c, DWORD ms) {
    if (!chan_arm(c)) return true;
    DWORD w = WaitForSingleObject(c->ev[c->side], ms);
    chan_disarm(c);
    return w == WAIT_OBJECT_0;
}
