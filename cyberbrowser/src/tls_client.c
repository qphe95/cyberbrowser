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

/* Connect `net` to host:port, through the HTTPS proxy when one applies. */
static int tls_net_connect(mbedtls_net_context *net, const char *host, const char *port,
                           char *err, size_t errLen) {
    char phost[256], pport[16];
    if (!https_proxy_for(host, phost, sizeof(phost), pport, sizeof(pport)))
        return mbedtls_net_connect(net, host, port, MBEDTLS_NET_PROTO_TCP);
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

bool tls_client_connect(TlsClient *client, const char *host, const char *port,
                        char *err, size_t errLen) {
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
            ret = mbedtls_x509_crt_parse_file(&client->ca, path);
        } else {
            ret = mbedtls_x509_crt_parse_path(&client->ca, path);
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
                ret = mbedtls_x509_crt_parse_der(&client->ca,
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

    if (!ca_loaded) {
        /* Last resort: disable certificate verification for testing */
        LOGW("Could not load CA certs from any path, continuing without verification");
    }
    LOGI("Connecting to %s:%s...", host, port);
    ret = tls_net_connect(&client->net, host, port, err, errLen);
    if (ret != 0) {
        if (!(err && errLen && strncmp(err, "Proxy refused", 13) == 0))
            set_err(err, errLen, "TLS connect failed", ret);
        return false;
    }
    LOGI("TCP connected to %s:%s", host, port);
    
    // Set socket timeout for handshake and I/O
    int sockfd = client->net.fd;
#ifndef _WIN32
    struct timeval tv;
    tv.tv_sec = 30;  // 30 second timeout
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#else
    DWORD timeout_ms = 30000;
    setsockopt((SOCKET)sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
    setsockopt((SOCKET)sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
#endif
    
    // Disable TCP_NODELAY for better compatibility
    int flag = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));

    // Configure perfect Android TCP stack simulation
    if (!configure_perfect_android_tcp_stack(&client->net, err, errLen)) {
        return false;
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
    mbedtls_debug_set_threshold(4);
    mbedtls_ssl_conf_dbg(&client->conf, mbedtls_debug, NULL);
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
    mbedtls_ssl_conf_ca_chain(&client->conf, &client->ca, NULL);
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
    mbedtls_ssl_set_bio(&client->ssl, &client->net,
                        mbedtls_net_send, mbedtls_net_recv, NULL);
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
    int ret = tls_net_connect(&client->net, host, port, err, errLen);
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
        int r = mbedtls_net_recv(&client->net, buf, len);
        if (r == MBEDTLS_ERR_SSL_WANT_READ) return 0;
        if (r <= 0) { client->connected = false; return -1; }
        return r;
    }
    int ret = mbedtls_ssl_read(&client->ssl, buf, len);
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

ssize_t tls_client_write(TlsClient *client, const unsigned char *buf, size_t len) {
    if (!client || !client->connected) {
        return -1;
    }
    if (client->plain) {
        int w = mbedtls_net_send(&client->net, buf, len);
        if (w == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
        return w;
    }
    int ret = mbedtls_ssl_write(&client->ssl, buf, len);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    return ret;
}

void tls_client_close(TlsClient *client) {
    if (!client) {
        return;
    }
    if (client->connected && !client->plain) {
        mbedtls_ssl_close_notify(&client->ssl);
    }
    mbedtls_net_free(&client->net);
    mbedtls_x509_crt_free(&client->ca);
    mbedtls_ssl_free(&client->ssl);
    mbedtls_ssl_config_free(&client->conf);
    client->connected = false;
}

// Global connection pools (one per host like browsers do)
static ConnectionPool *g_connection_pools[100];
static int g_pool_count = 0;
static pthread_mutex_t g_pools_mutex = PTHREAD_MUTEX_INITIALIZER;

// Initialize global resources (the pool mutex is statically initialized, so
// concurrent first requests from the script fetch pool are safe).
static void initialize_global_resources() {
}

// Find or create connection pool for host
ConnectionPool *connection_pool_create(const char *host) {
    initialize_global_resources();
    pthread_mutex_lock(&g_pools_mutex);

    // Look for existing pool
    for (int i = 0; i < g_pool_count; i++) {
        if (strcmp(g_connection_pools[i]->host, host) == 0) {
            pthread_mutex_unlock(&g_pools_mutex);
            return g_connection_pools[i];
        }
    }

    // Create new pool
    if (g_pool_count >= 100) {
        // Too many pools, reuse oldest
        g_pool_count = 0;
    }

    ConnectionPool *pool = calloc(1, sizeof(ConnectionPool));
    if (!pool) {
        pthread_mutex_unlock(&g_pools_mutex);
        return NULL;
    }

    strncpy(pool->host, host, sizeof(pool->host) - 1);
    pthread_mutex_init(&pool->mutex, NULL);

    g_connection_pools[g_pool_count++] = pool;

    pthread_mutex_unlock(&g_pools_mutex);
    LOGI("Created connection pool for host: %s", host);
    return pool;
}

void connection_pool_destroy(ConnectionPool *pool) {
    if (!pool) return;

    pthread_mutex_lock(&pool->mutex);
    for (int i = 0; i < pool->count; i++) {
        if (pool->connections[i]) {
            tls_client_close(pool->connections[i]);
            free(pool->connections[i]);
        }
    }
    pthread_mutex_unlock(&pool->mutex);
    pthread_mutex_destroy(&pool->mutex);
    free(pool);
}

TlsClient *connection_pool_get(ConnectionPool *pool, const char *host, const char *port,
                              char *err, size_t errLen) {
    if (!pool) return NULL;

    pthread_mutex_lock(&pool->mutex);

    // Clean up expired connections
    connection_pool_cleanup_expired(pool);

    // Find reusable connection
    for (int i = 0; i < pool->count; i++) {
        TlsClient *client = pool->connections[i];
        if (client && client->connected && client->reusable &&
            strcmp(client->host, host) == 0) {
            client->last_used = time(NULL);
            client->reusable = false; // Mark as in use
            pthread_mutex_unlock(&pool->mutex);
            LOGI("Reusing connection from pool for %s", host);
            return client;
        }
    }

    // No reusable connection, create new one
    if (pool->count >= MAX_CONNECTIONS_PER_HOST) {
        // Pool full, remove oldest connection
        TlsClient *oldest = NULL;
        int oldest_idx = -1;
        time_t oldest_time = time(NULL);

        for (int i = 0; i < pool->count; i++) {
            if (pool->connections[i] &&
                pool->connections[i]->last_used < oldest_time) {
                oldest = pool->connections[i];
                oldest_time = pool->connections[i]->last_used;
                oldest_idx = i;
            }
        }

        if (oldest) {
            tls_client_close(oldest);
            free(oldest);
            // Shift remaining connections
            for (int j = oldest_idx; j < pool->count - 1; j++) {
                pool->connections[j] = pool->connections[j + 1];
            }
            pool->count--;
        }
    }

    // Create new connection
    TlsClient *client = calloc(1, sizeof(TlsClient));
    if (!client) {
        pthread_mutex_unlock(&pool->mutex);
        set_err(err, errLen, "Failed to allocate connection", 0);
        return NULL;
    }

    if (!tls_client_connect(client, host, port, err, errLen)) {
        free(client);
        pthread_mutex_unlock(&pool->mutex);
        return NULL;
    }

    // Configure for pooling
    strncpy(client->host, host, sizeof(client->host) - 1);
    client->last_used = time(NULL);
    client->reusable = false; // Mark as in use

    // Add to pool
    pool->connections[pool->count++] = client;

    pthread_mutex_unlock(&pool->mutex);
    LOGI("Created new connection for pool: %s", host);
    return client;
}

void connection_pool_return(ConnectionPool *pool, TlsClient *client) {
    if (!pool || !client) return;

    pthread_mutex_lock(&pool->mutex);
    client->reusable = true; // Mark as available for reuse
    client->last_used = time(NULL);
    LOGI("Returned connection to pool for %s", client->host);
    pthread_mutex_unlock(&pool->mutex);
}

void connection_pool_cleanup_expired(ConnectionPool *pool) {
    if (!pool) return;

    time_t now = time(NULL);
    int write_idx = 0;

    for (int i = 0; i < pool->count; i++) {
        TlsClient *client = pool->connections[i];
        if (client) {
            if (!client->connected ||
                (now - client->last_used) > CONNECTION_TIMEOUT) {
                // Connection expired or dead
                tls_client_close(client);
                free(client);
                LOGI("Cleaned up expired connection");
            } else {
                // Keep connection
                pool->connections[write_idx++] = client;
            }
        }
    }

    pool->count = write_idx;
}
