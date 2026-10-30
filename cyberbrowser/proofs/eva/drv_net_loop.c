/* drv_net_loop.c - Eva's driver for loop_client.c, the network process's
 * side of the loopback relay (see drv_net.c).  The relay's channel is
 * attached as the grant does it; then either its reader thread runs, or
 * the TLS code's calls through the TlsLoopback table come, with any host
 * and port text and buffers of any size the TLS code passes. */
#include "loop_client.c"
#include "drv.h"

void fc_loop_client(void) {
    HANDLE h[3];
    for (int i = 0; i < 3; i++) h[i] = (HANDLE)(uintptr_t)Frama_C_unsigned_int_interval(0, 0xffffffffu);
    if (!loop_client_start(h[0], h[1], h[2])) return;
    if (Frama_C_nondet(0, 1)) {
        reader(NULL);
        return;
    }
    static char host[300], port[16];
    static unsigned char buf[65536];
    fc_string(host, sizeof(host));
    fc_string(port, sizeof(port));
    Frama_C_make_unknown((char *)buf, sizeof(buf));
    while (Frama_C_nondet(0, 1)) {
        int id = Frama_C_nondet(0, 1) ? g_lb.connect(host, port) : Frama_C_interval(-1, 100);
        size_t n = (size_t)Frama_C_interval(0, sizeof(buf));
        unsigned timeout = Frama_C_unsigned_int_interval(0, 30000);
        switch (Frama_C_interval(0, 3)) {
        case 0: g_lb.send(id, buf, n, timeout); break;
        case 1: g_lb.recv(id, buf, n, timeout); break;
        case 2: g_lb.readable(id); break;
        default: g_lb.close(id); break;
        }
    }
}
