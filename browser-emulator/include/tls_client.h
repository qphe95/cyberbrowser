#ifndef TLS_CLIENT_H
#define TLS_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef _WIN32
#include "win32_compat.h"
#else
#include <sys/types.h>
#endif

#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_CONNECTIONS_PER_HOST 6  // Chrome's limit per host
#define CONNECTION_TIMEOUT 300      // 5 minutes

typedef struct TlsClient {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    bool connected;
    char host[256];
    time_t last_used;
    bool reusable;
} TlsClient;

typedef struct ConnectionPool {
    TlsClient *connections[MAX_CONNECTIONS_PER_HOST];
    char host[256];
    int count;
    pthread_mutex_t mutex;
} ConnectionPool;

bool tls_client_connect(TlsClient *client, const char *host, const char *port,
                        char *err, size_t errLen);
ssize_t tls_client_read(TlsClient *client, unsigned char *buf, size_t len);
ssize_t tls_client_write(TlsClient *client, const unsigned char *buf, size_t len);
void tls_client_close(TlsClient *client);

// Connection pooling functions
ConnectionPool *connection_pool_create(const char *host);
void connection_pool_destroy(ConnectionPool *pool);
TlsClient *connection_pool_get(ConnectionPool *pool, const char *host, const char *port,
                              char *err, size_t errLen);
void connection_pool_return(ConnectionPool *pool, TlsClient *client);
void connection_pool_cleanup_expired(ConnectionPool *pool);

#ifdef __cplusplus
}
#endif

#endif
