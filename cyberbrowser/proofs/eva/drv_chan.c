/* drv_chan.c - Eva's driver for libchan (chan.c) and bigmsg.c, the code
 * every process runs on every channel.  Any sequence of the calls, on a
 * channel this process made or one it was given (the section's bytes are
 * the peer's: unknown, see win_models.c MapViewOfFile), with any sizes. */
#include <stdlib.h>
#include <string.h>
#include "chan.h"
#include "bigmsg.h"
#include "__fc_builtin.h"

static uint8_t payload[CHAN_MSG_MAX + 64];

static HANDLE any_handle(void) {
    return (HANDLE)(uintptr_t)Frama_C_unsigned_int_interval(0, 0xffffffffu);
}

int main(void) {
    static Chan c;
    static BigRx rx;
    static ChanMsg m;
    Frama_C_make_unknown((char *)payload, sizeof(payload));
    if (Frama_C_nondet(0, 1)) {
        if (!chan_create(&c, Frama_C_unsigned_int_interval(0, 0xffffffffu))) return 0;
    } else {
        if (!chan_attach(&c, any_handle(), any_handle(), any_handle(), Frama_C_interval(-1, 2))) return 0;
    }
    while (Frama_C_nondet(0, 1)) {
        switch (Frama_C_interval(0, 7)) {
        case 0: {
            uint32_t len = Frama_C_unsigned_int_interval(0, sizeof(payload));
            chan_send(&c, (uint16_t)Frama_C_interval(0, 0xffff), payload, len, Frama_C_unsigned_int_interval(0, INFINITE));
            break;
        }
        case 1:
            if (chan_recv(&c, &m) > 0) {
                uint8_t last = m.len ? m.data[m.len - 1] : 0;
                (void)last;
            }
            break;
        case 2:
            if (big_recv(&c, &rx) > 0) {
                /* A whole message: total bytes and a NUL after them. */
                uint8_t first = rx.total ? rx.buf[0] : 0, nul = rx.buf[rx.total];
                (void)first;
                (void)nul;
            }
            break;
        case 3: {
            /* Up to three pieces, each in a buffer that big: several
             * parts' worth, so every way a piece splits across parts. */
            static uint8_t big[4 * CHAN_MSG_MAX + 64];
            Frama_C_make_unknown((char *)big, sizeof(big));
            uint32_t n[3];
            const uint8_t *p[3];
            for (int k = 0; k < 3; k++) {
                n[k] = Frama_C_unsigned_int_interval(0, sizeof(big));
                p[k] = Frama_C_nondet(0, 1) ? NULL : big;
            }
            big_send(&c, (uint16_t)Frama_C_interval(0, 0xffff), p[0], p[0] ? n[0] : 0, p[1], p[1] ? n[1] : 0, p[2],
                     p[2] ? n[2] : 0, 1000);
            break;
        }
        case 4: chan_wait(&c, Frama_C_unsigned_int_interval(0, INFINITE)); break;
        case 5:
            if (chan_arm(&c)) WaitForSingleObject(chan_event(&c), 0);
            chan_disarm(&c);
            break;
        case 6: chan_inheritable(&c, Frama_C_nondet(0, 1)); break;
        default: big_free(&rx); break;
        }
    }
    big_free(&rx);
    chan_close(&c);
    return 0;
}
