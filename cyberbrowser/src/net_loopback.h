#ifndef NET_LOOPBACK_H
#define NET_LOOPBACK_H

/* Connections to loopback hosts through another process: the network
 * process runs in an AppContainer, which can't reach loopback, so the
 * loopback relay (cb_loop.exe) connects for it (src/proc/loop_client.c).
 * The TLS client (tls_client.c) uses these for loopback hosts once they are
 * set.  send and recv return the bytes moved, 0 at the end (recv), or -1 on
 * an error or a timeout. */

#include <stdbool.h>
#include <stddef.h>

typedef struct TlsLoopback {
    int (*connect)(const char *host, const char *port);         /* an id > 0, or <= 0 */
    int (*send)(int id, const unsigned char *buf, size_t len, unsigned timeout_ms);
    int (*recv)(int id, unsigned char *buf, size_t len, unsigned timeout_ms);
    bool (*readable)(int id);                                   /* bytes or the end are waiting */
    void (*close)(int id);
} TlsLoopback;

/* Route loopback hosts (tls_loopback_host) through lb, from now on; NULL
 * connects to them directly again. */
void tls_set_loopback(const TlsLoopback *lb);
/* localhost, *.localhost, 127.0.0.1, ::1 or [::1]. */
bool tls_loopback_host(const char *host);

#endif
