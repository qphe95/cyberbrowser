#ifndef TLS_CLIENT_H
#define TLS_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#ifdef _WIN32
#include "win32_compat.h"
#else
#include <sys/types.h>
#endif

#include "mbedtls/net_sockets.h"
#include "tls_space.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#include "net_loopback.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_CONNECTIONS_PER_HOST 6  // Chrome's limit per host

typedef struct TlsClient {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    bool connected;
    char host[256];
    time_t last_used;
    bool reusable;
    bool plain;         /* plain TCP (http://): no TLS layer */
    /* Connection pool bookkeeping (conn_pool_*). */
    char pool_key[280]; /* "s|host|port" or "p|host|port" */
    int requests;       /* requests served on this connection */
    /* A connection through the loopback relay (tls_set_loopback) has no
     * socket here: loop_id names it, and the timeouts stand in for the
     * socket's. */
    int loop_id;        /* > 0: through the relay */
    unsigned recv_timeout_ms, send_timeout_ms;
    int space;          /* the peer's address space (tls_set_space_policy), -1 unknown */
} TlsClient;

/* The read and write timeouts of the connection. */
void tls_client_set_timeouts(TlsClient *client, unsigned recv_ms, unsigned send_ms);

bool tls_client_connect(TlsClient *client, const char *host, const char *port,
                        char *err, size_t errLen);
/* Plain TCP connection (for http:// URLs); read/write/close work the same. */
bool tcp_client_connect(TlsClient *client, const char *host, const char *port,
                        char *err, size_t errLen);
ssize_t tls_client_read(TlsClient *client, unsigned char *buf, size_t len);
ssize_t tls_client_write(TlsClient *client, const unsigned char *buf, size_t len);
void tls_client_close(TlsClient *client);

/* ---------------------------------------------------------------------------
 * Process-wide HTTP/1.1 keep-alive connection pool.
 *
 * Every HTTP request (documents, scripts, stylesheets, fonts, images, XHR /
 * fetch) goes through it, keyed by (scheme, host, port) like a browser's
 * socket pool.  At most CYBER_MAX_CONN_PER_HOST (default 6) connections per
 * key are in use at once; further acquirers wait for one to be released.
 *
 * conn_pool_acquire returns an idle connection to the key when one is alive
 * (*reused = true), else opens a new one.  fresh = true skips idle ones (the
 * retry after a reused connection turned out to be closed by the server).
 * conn_pool_release hands it back: keep_alive = true parks it for reuse (the
 * response was fully read and the server did not ask to close), false closes
 * it.  Either way the client must not be used afterwards.
 * ------------------------------------------------------------------------- */
TlsClient *conn_pool_acquire(bool tls, const char *host, const char *port, bool fresh,
                             bool *reused, char *err, size_t errLen);
void conn_pool_release(TlsClient *client, bool keep_alive);

typedef struct ConnPoolStats {
    long connects;        /* new connections opened (successful) */
    long connect_failures;
    long reuses;          /* requests served on a pooled connection */
    long stale;           /* idle connections found dead on reuse */
    long waits;           /* acquirers that waited for the per-host cap */
    int idle;             /* currently parked */
    int active;           /* currently in use */
} ConnPoolStats;
void conn_pool_get_stats(ConnPoolStats *out);
/* Close every idle connection (e.g. at shutdown). */
void conn_pool_close_idle(void);

#ifdef __cplusplus
}
#endif

#endif
