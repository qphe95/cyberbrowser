#ifndef BIG_CORE_H
#define BIG_CORE_H

/* The assembly of bigmsg.h's messages from their parts, without Win32 or the
 * heap: deciding whether a part continues the message, and copying its bytes
 * into the buffer the shim (bigmsg.c) provides.  Pure C, no library calls,
 * bounded loops: proved in M9 (SANDBOX_DESIGN.md section 8.7.3, step 1) free
 * of undefined behavior for any part and any assembler state (P1), and to
 * assemble exactly the parts' payloads, as the contracts below say.
 *
 * A message's first part has its type and starts with its total size (4
 * bytes, little-endian); the rest have type BIG_MORE and carry only bytes. */

#include <stdint.h>

#define BIG_MORE 0xffff
#define BIG_MAX (256u << 20)        /* largest message */
#define BIG_HDR 4u                  /* the total, at the start of the first part */

/* What the core keeps between parts: the message being assembled.  The
 * shim's buffer holds its first `have` bytes. */
typedef struct BigAsm {
    uint16_t type;
    uint32_t total, have;
    uint8_t busy;                   /* a message is being assembled */
} BigAsm;

enum {
    BIG_REFUSED = -1,               /* the sequence of parts is broken */
    BIG_PART = 0,                   /* taken; the message isn't whole yet */
    BIG_DONE = 1,                   /* taken; the message is whole */
    BIG_GROW = 2,                   /* the buffer must hold *need bytes first: nothing changed */
};

/*@ logic integer big_lim(integer limit) = limit == 0 || limit > BIG_MAX ? BIG_MAX : limit;
    logic integer le32(uint8_t *p) = p[0] + p[1] * 0x100 + p[2] * 0x10000 + p[3] * 0x1000000;
    predicate big_ok(BigAsm s, integer limit) = s.have <= s.total <= big_lim(limit);
 */

/* The largest message a receiver with this limit accepts: limit, or BIG_MAX
 * if it is 0 (unset) or larger. */
/*@ assigns \nothing;
    ensures \result == big_lim(limit);
    ensures 0 < \result <= BIG_MAX;
 */
uint32_t big_core_limit(uint32_t limit);

/* Take one part (its type ptype and its len bytes) into the message being
 * assembled in *s and buf (cap bytes), for a receiver accepting messages of
 * at most `limit` bytes (big_core_limit).  Returns a BIG_* above.  BIG_GROW
 * asks for a buffer of *need bytes, which is at most the limit plus one: the
 * bytes received so far, this part's, and a NUL.  Then the shim grows it and
 * calls again with the same part.  A whole message is buf[0 .. total - 1],
 * followed by a NUL.
 *
 * By induction over the parts: a message starts empty at its first part, each
 * part appends its payload unchanged and leaves the bytes before it alone, so
 * a whole message is exactly the concatenation of its parts' payloads. */
/*@ requires \valid(s) && \valid(need);
    requires \valid_read(part + (0 .. len - 1));
    requires \valid(buf + (0 .. cap - 1));
    requires \separated(s, need, buf + (0 .. cap - 1), part + (0 .. len - 1));
    requires \separated(need, buf + (0 .. cap - 1), part + (0 .. len - 1));
    requires \separated(buf + (0 .. cap - 1), part + (0 .. len - 1));
    assigns *s, *need, buf[0 .. cap - 1];

    ensures BIG_REFUSED <= \result <= BIG_GROW;
    // The broken sequences are refused, and change nothing.
    ensures ptype == BIG_MORE && !\old(s->busy) ==> \result == BIG_REFUSED;
    ensures ptype != BIG_MORE && \old(s->busy) ==> \result == BIG_REFUSED;
    ensures ptype != BIG_MORE && len < BIG_HDR ==> \result == BIG_REFUSED;
    ensures ptype != BIG_MORE && len >= BIG_HDR && le32(part) > big_lim(limit) ==> \result == BIG_REFUSED;
    ensures ptype != BIG_MORE && len >= BIG_HDR && len - BIG_HDR > le32(part) ==> \result == BIG_REFUSED;
    ensures ptype == BIG_MORE && \old(s->busy) && \old(s->have) <= \old(s->total) &&
            len > \old(s->total) - \old(s->have) ==> \result == BIG_REFUSED;
    ensures ptype == BIG_MORE && !big_ok(\old(*s), limit) ==> \result == BIG_REFUSED;
    ensures \result == BIG_REFUSED || \result == BIG_GROW ==>
            *s == \old(*s) && (\forall integer i; 0 <= i < cap ==> buf[i] == \old(buf[i]));

    // Otherwise, where this part's payload goes and how much room it needs.
    ensures \result != BIG_REFUSED ==>
            *need == (ptype == BIG_MORE ? \old(s->have) + len : len - BIG_HDR) + 1 &&
            *need <= big_lim(limit) + 1;
    ensures \result == BIG_GROW ==> cap < *need;

    // A part taken: the message is its first part's type and total, at most
    // the limit; the payload is appended unchanged after what came before.
    ensures \result == BIG_PART || \result == BIG_DONE ==>
            *need <= cap && big_ok(*s, limit) &&
            s->type == (ptype == BIG_MORE ? \old(s->type) : ptype) &&
            s->total == (ptype == BIG_MORE ? \old(s->total) : le32(part)) &&
            s->have == *need - 1;
    ensures \result == BIG_PART || \result == BIG_DONE ==> ptype == BIG_MORE ==>
            (\forall integer i; 0 <= i < \old(s->have) ==> buf[i] == \old(buf[i])) &&
            (\forall integer i; 0 <= i < len ==> buf[\old(s->have) + i] == part[i]);
    ensures \result == BIG_PART || \result == BIG_DONE ==> ptype != BIG_MORE ==>
            (\forall integer i; 0 <= i < len - BIG_HDR ==> buf[i] == part[BIG_HDR + i]);
    ensures \result == BIG_PART ==> s->busy && s->have < s->total;
    ensures \result == BIG_DONE ==> !s->busy && s->have == s->total && buf[s->total] == 0;
 */
int big_core_part(BigAsm *s, uint32_t limit, uint16_t ptype, const uint8_t *part, uint32_t len, uint8_t *buf,
                  uint32_t cap, uint32_t *need);

#endif
