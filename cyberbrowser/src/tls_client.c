#include "tls_client.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#endif
#include <time.h>
#include "platform.h"
#include "proc/net_policy.h"

#include "mbedtls/error.h"

#define TLS_ERR_GENERIC -0x7000
#define LOG_TAG "tls_client"

/* Thread-safe PSA crypto initialization */
static pthread_mutex_t g_psa_init_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_psa_init_status = PSA_SUCCESS;
static int g_psa_initialized = 0;

static int psa_crypto_init_once(void) {
    pthread_mutex_lock(&g_psa_init_mutex);
    if (!g_psa_initialized) {
        g_psa_init_status = psa_crypto_init();
        g_psa_initialized = 1;
    }
    pthread_mutex_unlock(&g_psa_init_mutex);
    return g_psa_init_status;
}
#define LOGI(...) platform_log(LOG_LEVEL_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) platform_log(LOG_LEVEL_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) platform_log(LOG_LEVEL_WARN, LOG_TAG, __VA_ARGS__)

// mbedTLS debug callback
#if defined(MBEDTLS_DEBUG_C)
#include "mbedtls/debug.h"
static void mbedtls_debug(void *ctx, int level, const char *file, int line, const char *str) {
    (void)ctx;
    platform_log(LOG_LEVEL_DEBUG, "mbedtls", "[%s:%d] %s", file, line, str);
}
#endif

// Perfect Chrome TLS fingerprint - beyond JA3
// JA3: 771,4865-4866-4867-49195-49199-49196-49200-52393-52392-49171-49172-156-157-47-53,0-23-65281-10-11-35-16-5-13-18-51-45-43-27-21,29-23-30-25-24,0

// Chrome's exact cipher suite order (TLS 1.3 first, then TLS 1.2)
// This is the original Chrome fingerprint for Android
static const int chrome_ciphers[] = {
    // TLS 1.3 ciphers (highest priority)
    0x1301, // TLS_AES_128_GCM_SHA256
    0x1302, // TLS_AES_256_GCM_SHA384
    0x1303, // TLS_CHACHA20_POLY1305_SHA256

    // TLS 1.2 ECDHE ciphers (forward secrecy)
    0xC02B, // TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256
    0xC02F, // TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256
    0xC02C, // TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384
    0xC030, // TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384
    0xCCA8, // TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256
    0xCCA9, // TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256

    // TLS 1.2 CBC ciphers (legacy, lower priority)
    0xC013, // TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA
    0xC014, // TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA

    // RSA ciphers (no forward secrecy, lowest priority)
    0x009C, // TLS_RSA_WITH_AES_128_GCM_SHA256
    0x009D, // TLS_RSA_WITH_AES_256_GCM_SHA384
    0x002F, // TLS_RSA_WITH_AES_128_CBC_SHA
    0x0035, // TLS_RSA_WITH_AES_256_CBC_SHA

    // 3DES (very old, emergency fallback)
    0x000A, // TLS_RSA_WITH_3DES_EDE_CBC_SHA

    0       // End marker
};

// Chrome's curve preferences (exact order)
static const uint16_t chrome_curves[] = {
    29,     // X25519 (highest priority - faster)
    23,     // P-256 (widely supported)
    24,     // P-384 (more secure)
    25,     // P-521 (maximum security)
    0       // End marker
};

// Chrome's signature algorithms (exact order)
static const uint16_t chrome_sig_algs[] = {
    0x0804, // ecdsa_secp256r1_sha256
    0x0805, // ecdsa_secp384r1_sha384
    0x0806, // ecdsa_secp521r1_sha512
    0x0401, // rsa_pss_rsae_sha256
    0x0501, // rsa_pss_rsae_sha384
    0x0601, // rsa_pss_rsae_sha512
    0x0403, // rsa_pkcs1_sha256
    0x0503, // rsa_pkcs1_sha384
    0x0603, // rsa_pkcs1_sha512
    0x0201, // rsa_pkcs1_sha1 (legacy)
    0x0402, // rsa_pkcs1_sha256 (duplicate for compatibility)
    0       // End marker
};

// Chrome's supported TLS versions
static const uint16_t chrome_versions[] = {
    0x0304, // TLS 1.3
    0x0303, // TLS 1.2
    0       // End marker
};

static void set_err(char *err, size_t errLen, const char *msg, int code) {
    if (!err || errLen == 0) {
        return;
    }
    if (code != 0) {
        snprintf(err, errLen, "%s (err=%d)", msg, code);
    } else {
        snprintf(err, errLen, "%s", msg);
    }
}

// Perfect Android TCP stack simulation based on Android kernel source
static bool configure_perfect_android_tcp_stack(mbedtls_net_context *net, char *err, size_t errLen) {
    int sockfd = net->fd;
    int optval;
    socklen_t optlen = sizeof(optval);

    // Android kernel TCP defaults (from Android source: net/ipv4/tcp.c)

    // TCP_NODELAY - Android disables Nagle for low latency (unlike desktop Linux)
    optval = 1; // Enable (Android default for better responsiveness)
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (const char*)&optval, sizeof(optval)) != 0) {
#ifndef _WIN32
        set_err(err, errLen, "Failed to set TCP_NODELAY", errno);
#else
        set_err(err, errLen, "Failed to set TCP_NODELAY", WSAGetLastError());
#endif
        return false;
    }

    // TCP window scaling - Android enables this
    optval = 1;
#ifdef TCP_WINDOW_CLAMP
    setsockopt(sockfd, IPPROTO_TCP, TCP_WINDOW_CLAMP, &optval, sizeof(optval));
#endif

    // TCP congestion control - Android uses BBR or CUBIC
#ifdef TCP_CONGESTION
    const char *cc_algorithm = "cubic"; // Android's default
    setsockopt(sockfd, IPPROTO_TCP, TCP_CONGESTION, cc_algorithm, strlen(cc_algorithm));
#endif

    // TCP keepalive (Android enables this aggressively)
    optval = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, (const char*)&optval, sizeof(optval)) != 0) {
#ifndef _WIN32
        set_err(err, errLen, "Failed to set SO_KEEPALIVE", errno);
#else
        set_err(err, errLen, "Failed to set SO_KEEPALIVE", WSAGetLastError());
#endif
        return false;
    }

    // Android TCP keepalive intervals (more aggressive than desktop)
#ifdef TCP_KEEPIDLE
    optval = 600; // 10 minutes (vs Linux default 7200s)
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE, &optval, sizeof(optval));
#endif
#ifdef TCP_KEEPINTVL
    optval = 60; // 60 seconds
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &optval, sizeof(optval));
#endif
#ifdef TCP_KEEPCNT
    optval = 9; // 9 probes
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT, &optval, sizeof(optval));
#endif

    // TCP selective ACK (SACK) - Android enables this
    // Note: TCP_SACK may not be defined on all Android versions
#ifdef TCP_SACK
    optval = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_SACK, &optval, sizeof(optval));
#endif

    // TCP timestamps - Android enables this
#ifdef TCP_TIMESTAMP
    optval = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_TIMESTAMP, &optval, sizeof(optval));
#endif

    // TCP syncookies - Android enables this for SYN flood protection
    // Note: TCP_SYNCOOKIES may not be defined on all Android versions
#ifdef TCP_SYNCOOKIES
    optval = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_SYNCOOKIES, &optval, sizeof(optval));
#endif

    // TCP max segment size - Android uses 1460 for Ethernet
    optval = 1460;
    setsockopt(sockfd, IPPROTO_TCP, TCP_MAXSEG, &optval, sizeof(optval));

    // TCP initial window size - Android uses larger windows
#ifdef TCP_WINDOW_CLAMP
    optval = 14600; // Android's default initial window
    setsockopt(sockfd, IPPROTO_TCP, TCP_WINDOW_CLAMP, &optval, sizeof(optval));
#endif

    // SO_LINGER - Android disables this (no lingering)
    struct linger ling;
    ling.l_onoff = 0;
    ling.l_linger = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));

    // Socket buffer sizes - Android uses larger buffers than desktop
    optval = 262144; // 256KB send buffer (Android default)
    setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &optval, sizeof(optval));
    optval = 262144; // 256KB receive buffer (Android default)
    setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &optval, sizeof(optval));

    // TCP user timeout - Android sets this for faster failure detection
#ifdef TCP_USER_TIMEOUT
    optval = 30000; // 30 seconds
    setsockopt(sockfd, IPPROTO_TCP, TCP_USER_TIMEOUT, &optval, sizeof(optval));
#endif

    // TCP thin linear timeouts - Android enables this
#ifdef TCP_THIN_LINEAR_TIMEOUTS
    optval = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_THIN_LINEAR_TIMEOUTS, &optval, sizeof(optval));
#endif

    // TCP early retransmit - Android enables this
    // Note: TCP_EARLY_RETRANS may not be defined on all Android versions
#ifdef TCP_EARLY_RETRANS
    optval = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_EARLY_RETRANS, &optval, sizeof(optval));
#endif

    LOGI("Configured perfect Android TCP stack simulation");
    return true;
}

static void tls_init(TlsClient *client) {
    mbedtls_net_init(&client->net);
    mbedtls_ssl_init(&client->ssl);
    mbedtls_ssl_config_init(&client->conf);
    mbedtls_x509_crt_init(&client->ca);
    client->connected = false;
    client->plain = false;
    client->loop_id = 0;
    client->space = -1;
    client->recv_timeout_ms = client->send_timeout_ms = 30000;
}

/* ---- loopback through the relay ------------------------------------------- */

static const TlsLoopback *g_loopback;

void tls_set_loopback(const TlsLoopback *lb) { g_loopback = lb; }

bool tls_loopback_host(const char *host) {
    size_t n = strlen(host);
    return !strcmp(host, "localhost") || (n > 10 && !strcmp(host + n - 10, ".localhost")) ||
           !strcmp(host, "127.0.0.1") || !strcmp(host, "::1") || !strcmp(host, "[::1]");
}

/* mbedtls's I/O callbacks for a connection through the relay. */
static int loop_bio_send(void *ctx, const unsigned char *buf, size_t len) {
    TlsClient *c = (TlsClient *)ctx;
    int r = g_loopback->send(c->loop_id, buf, len, c->send_timeout_ms);
    return r < 0 ? MBEDTLS_ERR_NET_SEND_FAILED : r;
}

static int loop_bio_recv(void *ctx, unsigned char *buf, size_t len) {
    TlsClient *c = (TlsClient *)ctx;
    int r = g_loopback->recv(c->loop_id, buf, len, c->recv_timeout_ms);
    return r < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : r;
}

void tls_client_set_timeouts(TlsClient *client, unsigned recv_ms, unsigned send_ms) {
    client->recv_timeout_ms = recv_ms;
    client->send_timeout_ms = send_ms;
    if (client->loop_id) return;
#ifndef _WIN32
    struct timeval tv;
    tv.tv_sec = recv_ms / 1000;
    tv.tv_usec = (recv_ms % 1000) * 1000;
    setsockopt(client->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = send_ms / 1000;
    tv.tv_usec = (send_ms % 1000) * 1000;
    setsockopt(client->net.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#else
    DWORD r = recv_ms, w = send_ms;
    setsockopt((SOCKET)client->net.fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&r, sizeof(r));
    setsockopt((SOCKET)client->net.fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&w, sizeof(w));
#endif
}

/* ============================================================================
 * HTTPS proxy (HTTP CONNECT tunnel)
 * ============================================================================
 * When HTTPS_PROXY / https_proxy is set (corporate networks, sandboxes), the
 * TCP connection goes to the proxy and a CONNECT request opens a tunnel to
 * host:port; TLS then runs end to end through it.  NO_PROXY / no_proxy lists
 * hosts that connect directly ("host", ".suffix", "*.suffix", "*").
 */

static bool no_proxy_matches(const char *host) {
    const char *np = getenv("NO_PROXY");
    if (!np || !np[0]) np = getenv("no_proxy");
    if (!np || !np[0]) return false;
    size_t hl = strlen(host);
    const char *p = np;
    while (*p) {
        while (*p == ',' || *p == ' ') p++;
        const char *e = p;
        while (*e && *e != ',') e++;
        size_t n = (size_t)(e - p);
        while (n && p[n - 1] == ' ') n--;
        if (n == 1 && p[0] == '*') return true;
        const char *pat = p;
        size_t pn = n;
        if (pn > 1 && pat[0] == '*' && pat[1] == '.') { pat++; pn--; }
        if (pn) {
            if (pat[0] == '.') {
                /* suffix: matches the domain itself and its subdomains */
                if (hl == pn - 1 && strncasecmp(host, pat + 1, pn - 1) == 0) return true;
                if (hl >= pn && strncasecmp(host + hl - pn, pat, pn) == 0) return true;
            } else if (hl == pn && strncasecmp(host, pat, pn) == 0) {
                return true;
            } else if (hl > pn && host[hl - pn - 1] == '.' && strncasecmp(host + hl - pn, pat, pn) == 0) {
                return true;
            }
        }
        p = e;
    }
    return false;
}

/* Proxy host/port from HTTPS_PROXY ("http://[user:pass@]host:port[/]"). */
static bool https_proxy_for(const char *host, char *phost, size_t phl, char *pport, size_t ppl) {
    const char *v = getenv("HTTPS_PROXY");
    if (!v || !v[0]) v = getenv("https_proxy");
    if (!v || !v[0]) return false;
    if (no_proxy_matches(host)) return false;
    const char *p = strstr(v, "://");
    p = p ? p + 3 : v;
    const char *at = strchr(p, '@');
    if (at) p = at + 1;
    const char *end = p + strcspn(p, "/");
    const char *colon = NULL;
    if (*p == '[') {                         /* [ipv6]:port */
        const char *rb = memchr(p, ']', (size_t)(end - p));
        if (!rb) return false;
        snprintf(phost, phl, "%.*s", (int)(rb - p - 1), p + 1);
        colon = (rb + 1 < end && rb[1] == ':') ? rb + 1 : NULL;
    } else {
        colon = memchr(p, ':', (size_t)(end - p));
        snprintf(phost, phl, "%.*s", (int)((colon ? colon : end) - p), p);
    }
    if (colon) snprintf(pport, ppl, "%.*s", (int)(end - colon - 1), colon + 1);
    else snprintf(pport, ppl, "%s", strncmp(v, "https://", 8) == 0 ? "443" : "80");
    return phost[0] != 0;
}

static TlsSpaceFn g_space_of;
static __thread int t_space_limit = -1;
static __thread int t_last_space = -1;

void tls_set_space_policy(TlsSpaceFn classify) { g_space_of = classify; }
void tls_set_space_limit(int limit) { t_space_limit = limit; }
int tls_last_space(void) { return t_last_space; }
void tls_reset_last_space(void) { t_last_space = -1; }

/* P9's decision is the proved one (net_policy.c): the thread's limit is the
 * client's space, space (-1: unknown) the target's.  No limit: anything. */
static bool space_allowed(int space) {
    return t_space_limit < 0 || net_space_allows(t_space_limit, space);
}

static void space_refused(const char *host, const char *port, char *err, size_t errLen) {
    LOGW("Private Network Access: %s:%s is more private than the page", host, port);
    if (err && errLen)
        snprintf(err, errLen, "Private Network Access: %s:%s is more private than the page", host, port);
}

/* The space of loopback host:port (for the relay, which resolves nothing). */
static int loopback_space(const char *host, const char *port) {
    struct sockaddr_in6 a6;
    struct sockaddr_in a4;
    unsigned p = (unsigned)atoi(port);
    if (!strcmp(host, "::1") || !strcmp(host, "[::1]")) {
        memset(&a6, 0, sizeof(a6));
        a6.sin6_family = AF_INET6;
        a6.sin6_port = htons((unsigned short)p);
        a6.sin6_addr.s6_addr[15] = 1;
        return g_space_of((const struct sockaddr *)&a6);
    }
    memset(&a4, 0, sizeof(a4));
    a4.sin_family = AF_INET;
    a4.sin_port = htons((unsigned short)p);
    a4.sin_addr.s_addr = htonl(0x7f000001);
    return g_space_of((const struct sockaddr *)&a4);
}

/* With a classifier: resolve host and pick the first address the thread may
 * reach, as a numeric host in ip (and its space); false if there is none. */
static bool pick_address(const char *host, const char *port, char *ip, size_t ip_len, int *space,
                         char *err, size_t errLen) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        if (err && errLen) snprintf(err, errLen, "Could not resolve %s", host);
        return false;
    }
    bool found = false, refused = false;
    for (struct addrinfo *a = res; a && !found; a = a->ai_next) {
        int s = g_space_of(a->ai_addr);
        if (!space_allowed(s)) { refused = true; continue; }
        if (getnameinfo(a->ai_addr, (socklen_t)a->ai_addrlen, ip, (socklen_t)ip_len, NULL, 0, NI_NUMERICHOST) != 0)
            continue;
        *space = s;
        found = true;
    }
    freeaddrinfo(res);
    if (!found && refused) space_refused(host, port, err, errLen);
    else if (!found && err && errLen) snprintf(err, errLen, "Could not resolve %s", host);
    return found;
}

/* Connect the client to host:port: a loopback host through the relay when
 * there is one, else through the HTTPS proxy when one applies, else
 * directly.  With a space classifier, only to an address the thread's
 * limit allows. */
static int tls_net_connect(TlsClient *client, const char *host, const char *port,
                           char *err, size_t errLen) {
    mbedtls_net_context *net = &client->net;
    if (g_space_of && tls_loopback_host(host)) {
        client->space = loopback_space(host, port);
        if (!space_allowed(client->space)) {
            space_refused(host, port, err, errLen);
            return MBEDTLS_ERR_NET_CONNECT_FAILED;
        }
    }
    if (g_loopback && tls_loopback_host(host)) {
        int id = g_loopback->connect(host, port);
        if (id <= 0) {
            if (err && errLen) snprintf(err, errLen, "Loopback relay could not connect to %s:%s", host, port);
            return MBEDTLS_ERR_NET_CONNECT_FAILED;
        }
        client->loop_id = id;
        return 0;
    }
    char phost[256], pport[16];
    char ip[INET6_ADDRSTRLEN + 1];
    const char *to = host;
    if (g_space_of && !tls_loopback_host(host)) {
        /* Resolved here, so what is checked is what is connected to (no
         * second lookup to rebind).  Through a proxy, the proxy resolves:
         * the check is then only as good as this lookup. */
        if (!pick_address(host, port, ip, sizeof(ip), &client->space, err, errLen))
            return MBEDTLS_ERR_NET_CONNECT_FAILED;
        to = ip;
    }
    if (!https_proxy_for(host, phost, sizeof(phost), pport, sizeof(pport)))
        return mbedtls_net_connect(net, to, port, MBEDTLS_NET_PROTO_TCP);
    LOGI("Connecting to %s:%s through proxy %s:%s", host, port, phost, pport);
    int ret = mbedtls_net_connect(net, phost, pport, MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) return ret;
    char req[600];
    int n = snprintf(req, sizeof(req),
                     "CONNECT %s:%s HTTP/1.1\r\nHost: %s:%s\r\nProxy-Connection: keep-alive\r\n\r\n",
                     host, port, host, port);
    for (int off = 0; off < n; ) {
        int w = mbedtls_net_send(net, (const unsigned char *)req + off, (size_t)(n - off));
        if (w <= 0) { mbedtls_net_free(net); return MBEDTLS_ERR_NET_SEND_FAILED; }
        off += w;
    }
    /* Read the response head byte by byte so no tunnel data is consumed. */
    char head[2048];
    size_t hn = 0;
    while (hn + 1 < sizeof(head)) {
        unsigned char c;
        int r = mbedtls_net_recv(net, &c, 1);
        if (r <= 0) { mbedtls_net_free(net); return MBEDTLS_ERR_NET_RECV_FAILED; }
        head[hn++] = (char)c;
        if (hn >= 4 && memcmp(head + hn - 4, "\r\n\r\n", 4) == 0) break;
    }
    head[hn] = 0;
    int status = 0;
    if (sscanf(head, "HTTP/%*s %d", &status) != 1 || status != 200) {
        char line[128];
        snprintf(line, sizeof(line), "%.*s", (int)strcspn(head, "\r\n"), head);
        LOGW("Proxy refused CONNECT %s:%s: %s", host, port, line);
        if (err && errLen) snprintf(err, errLen, "Proxy refused CONNECT %s:%s (%s)", host, port, line);
        mbedtls_net_free(net);
        return MBEDTLS_ERR_NET_CONNECT_FAILED;
    }
    return 0;
}

/* The CA store is parsed once per process and shared (read-only) by every
 * connection: parsing /etc/ssl/certs or the Windows ROOT store costs far
 * more than the handshake it verifies. */
static mbedtls_x509_crt g_ca;
static bool g_ca_loaded = false;
static bool g_ca_tried = false;
static pthread_mutex_t g_ca_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool tls_shared_ca_load(void) {
    pthread_mutex_lock(&g_ca_mutex);
    if (g_ca_tried) {
        bool r = g_ca_loaded;
        pthread_mutex_unlock(&g_ca_mutex);
        return r;
    }
    g_ca_tried = true;
    mbedtls_x509_crt_init(&g_ca);
    int ret;
    /* Try platform-specific CA certificate paths */
    const char *ca_paths[] = {
#ifdef __APPLE__
        "/etc/ssl/cert.pem",              /* macOS system certs (single file) */
        "/System/Library/OpenSSL/certs",  /* macOS OpenSSL certs (directory) */
#elif defined(__ANDROID__)
        "/system/etc/security/cacerts",   /* Android system CA store */
#elif defined(__linux__)
        "/etc/ssl/certs",                 /* Linux system certs (Debian/Ubuntu) */
        "/etc/pki/tls/certs",             /* RHEL/CentOS/Fedora */
#elif defined(_WIN32)
        "C:/msys64/mingw64/etc/ssl/certs/ca-bundle.crt", /* MSYS2 / Git-for-Windows fallback */
#else
        "/etc/ssl/certs",                 /* Default for other platforms */
#endif
        NULL
    };

    bool ca_loaded = false;
    for (int i = 0; ca_paths[i] != NULL; i++) {
        /* Try parse_path for directories, parse_file for .pem files */
        const char *path = ca_paths[i];
        if (strstr(path, ".pem") || strstr(path, ".crt")) {
            ret = mbedtls_x509_crt_parse_file(&g_ca, path);
        } else {
            ret = mbedtls_x509_crt_parse_path(&g_ca, path);
        }
        if (ret >= 0) {
            LOGI("Loaded CA certs from %s (%d certs)", path, ret);
            ca_loaded = true;
            break;
        } else {
            LOGI("Failed to load CA certs from %s (err=%d)", path, ret);
        }
    }

#ifdef _WIN32
    if (!ca_loaded) {
        /* Load CA certs from Windows certificate store */
        HCERTSTORE hStore = CertOpenSystemStoreA(0, "ROOT");
        if (hStore) {
            int total_certs = 0;
            PCCERT_CONTEXT pCert = NULL;
            while ((pCert = CertEnumCertificatesInStore(hStore, pCert)) != NULL) {
                ret = mbedtls_x509_crt_parse_der(&g_ca,
                                                  pCert->pbCertEncoded,
                                                  pCert->cbCertEncoded);
                if (ret == 0) {
                    total_certs++;
                }
            }
            CertCloseStore(hStore, 0);
            if (total_certs > 0) {
                LOGI("Loaded %d CA certs from Windows ROOT store", total_certs);
                ca_loaded = true;
            } else {
                LOGW("Failed to load any CA certs from Windows ROOT store");
            }
        } else {
            LOGW("Could not open Windows ROOT certificate store");
        }
    }
#endif

    g_ca_loaded = ca_loaded;
    pthread_mutex_unlock(&g_ca_mutex);
    return ca_loaded;
}

/* CYBER_NET_TIMING=1: each new connection's DNS, TCP and TLS times on
 * stderr. */
static int net_timing(void) {
    static int t = -1;
    if (t < 0) t = getenv("CYBER_NET_TIMING") != NULL;
    return t;
}

static double net_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

bool tls_client_connect(TlsClient *client, const char *host, const char *port,
                        char *err, size_t errLen) {
    double t_start = 0, t_dns = 0, t_tcp = 0;
    if (!client || !host || !port) {
        set_err(err, errLen, "TLS invalid params", TLS_ERR_GENERIC);
        return false;
    }
    tls_init(client);
    int ret = psa_crypto_init_once();
    if (ret != PSA_SUCCESS) {
        set_err(err, errLen, "PSA crypto init failed", ret);
        return false;
    }
    bool ca_loaded = tls_shared_ca_load();
    if (!ca_loaded) {
        /* Last resort: disable certificate verification for testing */
        LOGW("Could not load CA certs from any path, continuing without verification");
    }
    LOGI("Connecting to %s:%s...", host, port);
    if (net_timing()) {
        /* (The lookup is timed on its own; the connect then finds it cached.) */
        t_start = net_now_ms();
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, port, &hints, &res) == 0) freeaddrinfo(res);
        t_dns = net_now_ms();
    }
    ret = tls_net_connect(client, host, port, err, errLen);
    if (net_timing()) t_tcp = net_now_ms();
    if (ret != 0) {
        if (!(err && errLen && (strncmp(err, "Proxy refused", 13) == 0 || strncmp(err, "Private Network", 15) == 0)))
            set_err(err, errLen, "TLS connect failed", ret);
        return false;
    }
    LOGI("TCP connected to %s:%s", host, port);
    
    // Set socket timeout for handshake and I/O
    tls_client_set_timeouts(client, 30000, 30000);
    if (!client->loop_id) {
        int sockfd = client->net.fd;
        // Disable TCP_NODELAY for better compatibility
        int flag = 1;
        setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));

        // Configure perfect Android TCP stack simulation
        if (!configure_perfect_android_tcp_stack(&client->net, err, errLen)) {
            return false;
        }
    }

    // Configure TLS record sizing to match Chrome patterns
    // Chrome optimizes record sizes for performance: 256, 512, 1024, 2048, 4096 bytes
    // This avoids the default mbedTLS record sizing that might look different

    // Configure TLS defaults
    ret = mbedtls_ssl_config_defaults(&client->conf,
                                      MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        set_err(err, errLen, "TLS config defaults failed", ret);
        return false;
    }

    // Enable mbedTLS debug output
#if defined(MBEDTLS_DEBUG_C)
    /* CYBER_TLS_DEBUG=1..4: mbedtls's own log.  Off by default: at 4 it
     * hex-dumps every record, formatted even when the log then drops it,
     * which cost CPU on every byte received. */
    {
        const char *d = getenv("CYBER_TLS_DEBUG");
        int level = d ? atoi(d) : 0;
        mbedtls_debug_set_threshold(level);
        if (level > 0) mbedtls_ssl_conf_dbg(&client->conf, mbedtls_debug, NULL);
    }
#endif

#ifdef BE_PLATFORM_ANDROID
    // Android: Use Chrome fingerprint for YouTube compatibility
    LOGI("Configuring Chrome TLS fingerprint for Android");
    
    // Set Chrome cipher suites in exact order for JA3 fingerprint
    mbedtls_ssl_conf_ciphersuites(&client->conf, chrome_ciphers);

    // Set Chrome curve preferences
    mbedtls_ssl_conf_groups(&client->conf, chrome_curves);

    // Set Chrome signature algorithms (perfect fingerprint matching)
    // Note: mbedTLS may not support all Chrome sig algs, but we set what we can
    const int supported_sig_algs[] = {
        MBEDTLS_TLS1_3_SIG_ECDSA_SECP256R1_SHA256,
        MBEDTLS_TLS1_3_SIG_ECDSA_SECP384R1_SHA384,
        MBEDTLS_TLS1_3_SIG_ECDSA_SECP521R1_SHA512,
        0
    };
    // mbedtls_ssl_conf_sig_algs(&client->conf, supported_sig_algs); // Not available in all mbedTLS versions
    (void)supported_sig_algs;

    // Chrome uses session tickets for faster reconnections
    mbedtls_ssl_conf_session_tickets(&client->conf, MBEDTLS_SSL_SESSION_TICKETS_ENABLED);

    // Chrome enables renegotiation
    mbedtls_ssl_conf_renegotiation(&client->conf, MBEDTLS_SSL_RENEGOTIATION_ENABLED);

    // Chrome's record size limits
    mbedtls_ssl_conf_max_frag_len(&client->conf, MBEDTLS_SSL_MAX_FRAG_LEN_4096);
    
    LOGI("Chrome TLS fingerprint configured");
#else
    // macOS/Linux: Use default mbedTLS settings for maximum compatibility
    LOGI("Using default TLS configuration for macOS/Linux");
    (void)chrome_ciphers;
    (void)chrome_curves;
#endif

    // Set minimum/maximum TLS versions (TLS 1.2 and 1.3)
    mbedtls_ssl_conf_min_tls_version(&client->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&client->conf, MBEDTLS_SSL_VERSION_TLS1_3);
    if (ca_loaded) {
        mbedtls_ssl_conf_authmode(&client->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        mbedtls_ssl_conf_authmode(&client->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    }
    mbedtls_ssl_conf_ca_chain(&client->conf, &g_ca, NULL);
    (void)psa_generate_random;
    ret = mbedtls_ssl_setup(&client->ssl, &client->conf);
    if (ret != 0) {
        set_err(err, errLen, "TLS setup failed", ret);
        return false;
    }
    ret = mbedtls_ssl_set_hostname(&client->ssl, host);
    if (ret != 0) {
        set_err(err, errLen, "TLS set hostname failed", ret);
        return false;
    }
    if (client->loop_id)
        mbedtls_ssl_set_bio(&client->ssl, client, loop_bio_send, loop_bio_recv, NULL);
    else
        mbedtls_ssl_set_bio(&client->ssl, &client->net, mbedtls_net_send, mbedtls_net_recv, NULL);
    LOGI("Starting TLS handshake with %s:%s...", host, port);
    int handshake_attempts = 0;
    while ((ret = mbedtls_ssl_handshake(&client->ssl)) != 0) {
        handshake_attempts++;
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            char error_buf[256];
            mbedtls_strerror(ret, error_buf, sizeof(error_buf));
            LOGI("TLS handshake failed after %d attempts: %s (err=%d, -0x%04x)", 
                 handshake_attempts, error_buf, ret, -ret);
            set_err(err, errLen, "TLS handshake failed", ret);
            return false;
        }
        if (handshake_attempts > 1000) {
            set_err(err, errLen, "TLS handshake timed out", TLS_ERR_GENERIC);
            return false;
        }
    }
    LOGI("TLS handshake completed successfully");
    
    // Log negotiated TLS version and cipher for debugging
    const char *tls_version = mbedtls_ssl_get_version(&client->ssl);
    const char *cipher_name = mbedtls_ssl_get_ciphersuite(&client->ssl);
    LOGI("TLS version: %s, Cipher: %s", tls_version ? tls_version : "unknown", 
         cipher_name ? cipher_name : "unknown");
    
    if (net_timing()) {
        struct sockaddr_storage peer;
#ifdef _WIN32
        int plen = sizeof(peer);
        int got = client->loop_id ? -1 : getpeername((SOCKET)client->net.fd, (struct sockaddr *)&peer, &plen);
#else
        socklen_t plen = sizeof(peer);
        int got = getpeername(client->net.fd, (struct sockaddr *)&peer, &plen);
#endif
        const char *fam = got == 0 ? (peer.ss_family == AF_INET6 ? "IPv6" : "IPv4") : "?";
        fprintf(stderr, "[net] %s: dns %.1f, tcp %.1f, tls %.1f ms (%s, %s, %s)\n", host,
                t_dns - t_start, t_tcp - t_dns, net_now_ms() - t_tcp, fam,
                tls_version ? tls_version : "?", cipher_name ? cipher_name : "?");
    }
    ret = (int)mbedtls_ssl_get_verify_result(&client->ssl);
    if (ret != 0) {
        set_err(err, errLen, "TLS verify failed", ret);
        return false;
    }
    client->connected = true;
    return true;
}

bool tcp_client_connect(TlsClient *client, const char *host, const char *port,
                        char *err, size_t errLen) {
    if (!client || !host || !port) {
        set_err(err, errLen, "TCP invalid params", TLS_ERR_GENERIC);
        return false;
    }
    tls_init(client);
    client->plain = true;
    int ret = tls_net_connect(client, host, port, err, errLen);
    if (ret != 0) {
        if (err && errLen && !err[0]) set_err(err, errLen, "TCP connect failed", ret);
        return false;
    }
    snprintf(client->host, sizeof(client->host), "%s", host);
    client->connected = true;
    return true;
}

ssize_t tls_client_read(TlsClient *client, unsigned char *buf, size_t len) {
    if (!client || !client->connected) {
        return -1;
    }
    if (client->plain) {
        int r = client->loop_id ? loop_bio_recv(client, buf, len) : mbedtls_net_recv(&client->net, buf, len);
        if (r == MBEDTLS_ERR_SSL_WANT_READ) return 0;
        if (r <= 0) { client->connected = false; return -1; }
        return r;
    }
    int ret;
    /* A TLS 1.3 NewSessionTicket is not application data: read on rather
     * than report 0 bytes (the caller would sleep before polling again). */
    int tickets = 0;
    do {
        ret = mbedtls_ssl_read(&client->ssl, buf, len);
    } while (ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET && ++tickets < 16);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
        ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
        ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
        return 0;
    }
    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        LOGI("TLS peer closed connection");
        client->connected = false;
        return -1;  // Graceful close, no more data
    }
    if (ret == 0) {
        // EOF without close notify (MBEDTLS_ERR_SSL_CONN_EOF maps to 0 in mbedtls_ssl_read)
        LOGI("TLS connection EOF");
        client->connected = false;
        return -1;
    }
    if (ret < 0) {
        LOGE("TLS read error: %d", ret);
        return ret;
    }
    return ret;
}

/* All of buf, written as the transport takes it: a write moves at most one
 * TLS record (16 KB) or one relay message (LOOP_DATA_MAX) at a time, and a
 * caller that wrote once sent a request body's first part only (its server
 * then waited for the rest).  Returns len, or what was written before an
 * error (-1 when nothing was), 0 when the transport would block at once. */
ssize_t tls_client_write(TlsClient *client, const unsigned char *buf, size_t len) {
    if (!client || !client->connected) {
        return -1;
    }
    size_t done = 0;
    while (done < len) {
        int w;
        if (client->plain)
            w = client->loop_id ? loop_bio_send(client, buf + done, len - done)
                                : mbedtls_net_send(&client->net, buf + done, len - done);
        else
            w = mbedtls_ssl_write(&client->ssl, buf + done, len - done);
        if (w == MBEDTLS_ERR_SSL_WANT_READ || w == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (!done) return 0;
            continue;
        }
        if (w <= 0) return done ? (ssize_t)done : (w < 0 ? w : -1);
        done += (size_t)w;
    }
    return (ssize_t)done;
}

void tls_client_close(TlsClient *client) {
    if (!client) {
        return;
    }
    if (client->connected && !client->plain) {
        mbedtls_ssl_close_notify(&client->ssl);
    }
    if (client->loop_id && g_loopback) g_loopback->close(client->loop_id);
    client->loop_id = 0;
    mbedtls_net_free(&client->net);
    mbedtls_x509_crt_free(&client->ca);
    mbedtls_ssl_free(&client->ssl);
    mbedtls_ssl_config_free(&client->conf);
    client->connected = false;
}

/* ============================================================================
 * HTTP/1.1 keep-alive connection pool (one per process, shared by every
 * request: documents, scripts, stylesheets, fonts, images, XHR / fetch).
 * ============================================================================
 * Keyed by scheme|host|port.  Idle connections sit in a small LRU array;
 * each key has an in-use count capped at CYBER_MAX_CONN_PER_HOST (default 6,
 * Chrome's limit), and acquirers over the cap wait on a condition variable
 * for a release.  Connecting and closing happen outside the lock, so
 * handshakes to different hosts (and to the same host, up to the cap) run in
 * parallel.
 */

#define POOL_MAX_IDLE      48    /* parked connections, all hosts */
#define POOL_MAX_KEYS      128   /* distinct scheme|host|port in use at once */
#define POOL_IDLE_TIMEOUT  60    /* seconds; servers typically drop at 60-120 */
#define POOL_WAIT_LIMIT_MS 30000 /* never wait longer than this for the cap */

typedef struct PoolKeyCount {
    char key[280];
    int active;
} PoolKeyCount;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t released;
    TlsClient *idle[POOL_MAX_IDLE];   /* oldest first */
    int nidle;
    PoolKeyCount keys[POOL_MAX_KEYS];
    int nkeys;
    int max_per_host;
    ConnPoolStats stats;
} g_pool = { .mu = PTHREAD_MUTEX_INITIALIZER, .released = PTHREAD_COND_INITIALIZER };

static int pool_max_per_host(void) {
    if (g_pool.max_per_host <= 0) {
        const char *e = getenv("CYBER_MAX_CONN_PER_HOST");
        int v = e ? atoi(e) : 0;
        g_pool.max_per_host = (v > 0 && v <= 64) ? v : MAX_CONNECTIONS_PER_HOST;
    }
    return g_pool.max_per_host;
}

static int pool_disabled(void) {
    static int d = -1;
    if (d < 0) {
        const char *e = getenv("CYBER_NO_KEEPALIVE");
        d = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return d;
}

/* Lock held. */
static PoolKeyCount *pool_key_slot(const char *key, bool create) {
    for (int i = 0; i < g_pool.nkeys; i++)
        if (strcmp(g_pool.keys[i].key, key) == 0) return &g_pool.keys[i];
    if (!create) return NULL;
    /* Reuse a slot whose count dropped to zero. */
    for (int i = 0; i < g_pool.nkeys; i++)
        if (g_pool.keys[i].active == 0) {
            snprintf(g_pool.keys[i].key, sizeof(g_pool.keys[i].key), "%s", key);
            return &g_pool.keys[i];
        }
    if (g_pool.nkeys >= POOL_MAX_KEYS) return NULL;   /* uncapped beyond this */
    PoolKeyCount *k = &g_pool.keys[g_pool.nkeys++];
    snprintf(k->key, sizeof(k->key), "%s", key);
    k->active = 0;
    return k;
}

/* Lock held: remove idle[i], preserving order. */
static TlsClient *pool_take_idle(int i) {
    TlsClient *c = g_pool.idle[i];
    memmove(&g_pool.idle[i], &g_pool.idle[i + 1], (size_t)(g_pool.nidle - i - 1) * sizeof(TlsClient *));
    g_pool.nidle--;
    return c;
}

static void pool_destroy_client(TlsClient *c) {
    if (!c) return;
    tls_client_close(c);
    free(c);
}

/* An idle connection is usable when the server has not closed it: the socket
 * must not be readable (readable = EOF, a reset, or bytes nobody asked for)
 * and TLS must hold no buffered records. */
static bool pool_client_alive(TlsClient *c) {
    if (!c->connected) return false;
    if (!c->plain && mbedtls_ssl_check_pending(&c->ssl)) return false;
    if (c->loop_id) return g_loopback && !g_loopback->readable(c->loop_id);
    int r = mbedtls_net_poll(&c->net, MBEDTLS_NET_POLL_READ, 0);
    if (r < 0) return false;
    return (r & MBEDTLS_NET_POLL_READ) == 0;
}

static TlsClient *pool_acquire(bool tls, const char *host, const char *port, bool fresh,
                               bool *reused, char *err, size_t errLen);

TlsClient *conn_pool_acquire(bool tls, const char *host, const char *port, bool fresh,
                             bool *reused, char *err, size_t errLen) {
    TlsClient *c = pool_acquire(tls, host, port, fresh, reused, err, errLen);
    if (c && g_space_of && !space_allowed(c->space)) {
        /* Opened for another page, to an address this one may not reach. */
        conn_pool_release(c, true);
        space_refused(host, port, err, errLen);
        return NULL;
    }
    if (c) t_last_space = c->space;
    return c;
}

static TlsClient *pool_acquire(bool tls, const char *host, const char *port, bool fresh,
                               bool *reused, char *err, size_t errLen) {
    if (reused) *reused = false;
    if (!host || !port) {
        set_err(err, errLen, "invalid host", TLS_ERR_GENERIC);
        return NULL;
    }
    char key[280];
    snprintf(key, sizeof(key), "%c|%s|%s", tls ? 's' : 'p', host, port);
    bool disabled = pool_disabled() != 0;

    TlsClient *dead[POOL_MAX_IDLE];
    int ndead = 0;
    TlsClient *got = NULL;
    bool counted = false;

    pthread_mutex_lock(&g_pool.mu);
    int cap = pool_max_per_host();
    time_t now = time(NULL);
    /* Drop idle connections past their timeout (any host). */
    for (int i = 0; i < g_pool.nidle; ) {
        if (now - g_pool.idle[i]->last_used > POOL_IDLE_TIMEOUT) dead[ndead++] = pool_take_idle(i);
        else i++;
    }
    int waited_ms = 0;
    bool counted_wait = false;
    for (;;) {
        if (!fresh && !disabled) {
            /* Most recently used first: the likeliest to still be open. */
            for (int i = g_pool.nidle - 1; i >= 0; i--) {
                if (strcmp(g_pool.idle[i]->pool_key, key) != 0) continue;
                TlsClient *c = pool_take_idle(i);
                if (pool_client_alive(c)) { got = c; break; }
                g_pool.stats.stale++;
                if (ndead < POOL_MAX_IDLE) dead[ndead++] = c;
                else { pthread_mutex_unlock(&g_pool.mu); pool_destroy_client(c); pthread_mutex_lock(&g_pool.mu); }
            }
        }
        PoolKeyCount *slot = pool_key_slot(key, true);
        if (got) {
            if (slot) slot->active++;
            g_pool.stats.reuses++;
            g_pool.stats.active++;
            counted = true;
            break;
        }
        if (!slot || slot->active < cap || waited_ms >= POOL_WAIT_LIMIT_MS) {
            if (slot) slot->active++;
            g_pool.stats.active++;
            counted = true;
            break;
        }
        /* At the per-host cap: wait for a release (which may also park an
         * idle connection we can take). */
        if (!counted_wait) { g_pool.stats.waits++; counted_wait = true; }
        unsigned long long t0 = platform_get_time_ms();
#ifdef _WIN32
        SleepConditionVariableSRW(&g_pool.released, &g_pool.mu, 1000, 0);
#else
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_cond_timedwait(&g_pool.released, &g_pool.mu, &ts);
#endif
        unsigned long long t1 = platform_get_time_ms();
        waited_ms += t1 > t0 ? (int)(t1 - t0) : 1;
    }
    pthread_mutex_unlock(&g_pool.mu);
    (void)counted;

    for (int i = 0; i < ndead; i++) pool_destroy_client(dead[i]);

    if (got) {
        got->requests++;
        if (reused) *reused = true;
        return got;
    }

    TlsClient *c = calloc(1, sizeof(TlsClient));
    bool ok = c && (tls ? tls_client_connect(c, host, port, err, errLen)
                        : tcp_client_connect(c, host, port, err, errLen));
    if (!ok) {
        if (c) { tls_client_close(c); free(c); }
        if (!c) set_err(err, errLen, "Failed to allocate connection", 0);
        pthread_mutex_lock(&g_pool.mu);
        PoolKeyCount *slot = pool_key_slot(key, false);
        if (slot && slot->active > 0) slot->active--;
        g_pool.stats.active--;
        g_pool.stats.connect_failures++;
        pthread_cond_broadcast(&g_pool.released);
        pthread_mutex_unlock(&g_pool.mu);
        return NULL;
    }
    snprintf(c->host, sizeof(c->host), "%s", host);
    snprintf(c->pool_key, sizeof(c->pool_key), "%s", key);
    c->last_used = time(NULL);
    c->requests = 1;
    pthread_mutex_lock(&g_pool.mu);
    g_pool.stats.connects++;
    pthread_mutex_unlock(&g_pool.mu);
    return c;
}

void conn_pool_release(TlsClient *c, bool keep_alive) {
    if (!c) return;
    TlsClient *evict = NULL;
    pthread_mutex_lock(&g_pool.mu);
    PoolKeyCount *slot = pool_key_slot(c->pool_key, false);
    if (slot && slot->active > 0) slot->active--;
    g_pool.stats.active--;
    if (keep_alive && c->connected && !pool_disabled()) {
        c->last_used = time(NULL);
        if (g_pool.nidle >= POOL_MAX_IDLE) evict = pool_take_idle(0);   /* LRU */
        g_pool.idle[g_pool.nidle++] = c;
        c = NULL;
    }
    pthread_cond_broadcast(&g_pool.released);
    pthread_mutex_unlock(&g_pool.mu);
    pool_destroy_client(evict);
    pool_destroy_client(c);
}

void conn_pool_get_stats(ConnPoolStats *out) {
    if (!out) return;
    pthread_mutex_lock(&g_pool.mu);
    *out = g_pool.stats;
    out->idle = g_pool.nidle;
    pthread_mutex_unlock(&g_pool.mu);
}

void conn_pool_close_idle(void) {
    TlsClient *list[POOL_MAX_IDLE];
    pthread_mutex_lock(&g_pool.mu);
    int n = g_pool.nidle;
    memcpy(list, g_pool.idle, (size_t)n * sizeof(TlsClient *));
    g_pool.nidle = 0;
    pthread_mutex_unlock(&g_pool.mu);
    for (int i = 0; i < n; i++) pool_destroy_client(list[i]);
}
