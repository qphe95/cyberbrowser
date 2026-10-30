/* Keep-alive connection pool + HTTP/1.1 message framing.
 *
 * An in-process HTTP server (plain TCP on 127.0.0.1, mbedtls_net sockets so
 * it builds on Windows too) serves canned responses; the tests check bodies
 * and how many TCP connections the client opened for them. */
#include "test_runner.h"
#include "http_download.h"
#include "tls_client.h"
#include "net_loopback.h"
#include "platform.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mbedtls_net_context g_listen;
static char g_port[8];
static bool g_server_up = false;
static pthread_mutex_t g_srv_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_accepted = 0;          /* connections accepted so far */
static int g_open = 0;              /* connections currently open */
static int g_open_max = 0;          /* high-water mark of g_open */

static bool srv_send(mbedtls_net_context *c, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        int w = mbedtls_net_send(c, (const unsigned char *)buf + off, len - off);
        if (w <= 0) return false;
        off += (size_t)w;
    }
    return true;
}

static void fill_body(char *b, size_t n) {
    for (size_t i = 0; i < n; i++) b[i] = (char)('a' + i % 26);
}

static bool body_ok(const HttpBuffer *b, size_t n) {
    if (b->size != n) return false;
    for (size_t i = 0; i < n; i++)
        if (b->data[i] != (char)('a' + i % 26)) return false;
    return true;
}

static void *srv_conn(void *arg) {
    mbedtls_net_context *c = (mbedtls_net_context *)arg;
    char req[8192];
    size_t have = 0;
    bool drop_next = false;
    for (;;) {
        /* One request head (requests here carry no body). */
        char *end = NULL;
        while (!(end = strstr(req, "\r\n\r\n"))) {
            if (have + 1 >= sizeof(req)) goto done;
            int r = mbedtls_net_recv(c, (unsigned char *)req + have, sizeof(req) - 1 - have);
            if (r <= 0) goto done;
            have += (size_t)r;
            req[have] = 0;
        }
        size_t used = (size_t)(end + 4 - req);
        char method[16] = "", path[512] = "";
        sscanf(req, "%15s %511s", method, path);
        memmove(req, req + used, have - used);
        have -= used;
        req[have] = 0;
        if (drop_next) goto done;           /* close without answering */

        char head[512];
        bool close_after = false;
        if (strncmp(path, "/cl/", 4) == 0 || strncmp(path, "/slow/", 6) == 0) {
            bool slow = path[1] == 's';
            size_t n = (size_t)strtoul(path + (slow ? 6 : 4), NULL, 10);
            if (slow) platform_sleep_ms(60);
            char *body = malloc(n + 1);
            fill_body(body, n);
            int hl = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n", n);
            bool ok = srv_send(c, head, (size_t)hl);
            /* Send the body in pieces so the client sees several reads. */
            for (size_t off = 0; ok && off < n; off += 7000)
                ok = srv_send(c, body + off, n - off < 7000 ? n - off : 7000);
            free(body);
            if (!ok) goto done;
        } else if (strcmp(path, "/chunked") == 0) {
            const char *parts[] = {
                "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n",
                "6\r\nhello \r\n", "5;ext=1\r\nworld\r\n", "0\r\nX-Trailer: 1\r\n", "\r\n"
            };
            for (int i = 0; i < 5; i++) {
                if (!srv_send(c, parts[i], strlen(parts[i]))) goto done;
                platform_sleep_ms(5);
            }
        } else if (strcmp(path, "/empty") == 0) {
            const char *r = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
            if (!srv_send(c, r, strlen(r))) goto done;
        } else if (strcmp(path, "/nocontent") == 0) {
            const char *r = "HTTP/1.1 204 No Content\r\n\r\n";
            if (!srv_send(c, r, strlen(r))) goto done;
        } else if (strcmp(path, "/head") == 0) {
            /* A HEAD response announces a length but carries no body. */
            const char *r = "HTTP/1.1 200 OK\r\nContent-Length: 1234\r\n\r\n";
            if (!srv_send(c, r, strlen(r))) goto done;
        } else if (strcmp(path, "/close") == 0) {
            const char *r = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: close\r\n\r\nabc";
            srv_send(c, r, strlen(r));
            close_after = true;
        } else if (strcmp(path, "/eof") == 0) {
            const char *r = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nuntil-eof";
            srv_send(c, r, strlen(r));
            close_after = true;
        } else if (strcmp(path, "/dropnext") == 0) {
            /* Answer, then close this connection when the next request
             * arrives: a pooled connection the server dropped. */
            const char *r = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
            if (!srv_send(c, r, strlen(r))) goto done;
            drop_next = true;
        } else {
            const char *r = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
            if (!srv_send(c, r, strlen(r))) goto done;
        }
        if (close_after) goto done;
    }
done:
    mbedtls_net_free(c);
    free(c);
    pthread_mutex_lock(&g_srv_mu);
    g_open--;
    pthread_mutex_unlock(&g_srv_mu);
    return NULL;
}

static void *srv_accept(void *arg) {
    (void)arg;
    for (;;) {
        mbedtls_net_context *c = malloc(sizeof(*c));
        mbedtls_net_init(c);
        if (mbedtls_net_accept(&g_listen, c, NULL, 0, NULL) != 0) {
            free(c);
            continue;
        }
        pthread_mutex_lock(&g_srv_mu);
        g_accepted++;
        g_open++;
        if (g_open > g_open_max) g_open_max = g_open;
        pthread_mutex_unlock(&g_srv_mu);
        pthread_t t;
        if (pthread_create(&t, NULL, srv_conn, c) == 0) pthread_detach(t);
        else { mbedtls_net_free(c); free(c); }
    }
    return NULL;
}

static bool server_start(void) {
    if (g_server_up) return true;
    mbedtls_net_init(&g_listen);
    for (int p = 47311; p < 47411; p++) {
        snprintf(g_port, sizeof(g_port), "%d", p);
        if (mbedtls_net_bind(&g_listen, "127.0.0.1", g_port, MBEDTLS_NET_PROTO_TCP) == 0) {
            pthread_t t;
            if (pthread_create(&t, NULL, srv_accept, NULL) != 0) return false;
            pthread_detach(t);
            g_server_up = true;
            return true;
        }
    }
    return false;
}

static int accepted(void) {
    pthread_mutex_lock(&g_srv_mu);
    int n = g_accepted;
    pthread_mutex_unlock(&g_srv_mu);
    return n;
}

static bool get(const char *path, HttpBuffer *b) {
    char url[128], err[256] = "";
    snprintf(url, sizeof(url), "http://127.0.0.1:%s%s", g_port, path);
    memset(b, 0, sizeof(*b));
    bool ok = http_get_to_memory(url, b, err, sizeof(err));
    if (!ok) printf("    GET %s failed: %s\n", path, err);
    return ok;
}

TEST(test_http_pool_reuses_connection) {
    ASSERT_TRUE(server_start());
    conn_pool_close_idle();
    int a0 = accepted();
    for (int i = 0; i < 3; i++) {
        HttpBuffer b;
        ASSERT_TRUE(get("/cl/10", &b));
        ASSERT_TRUE(body_ok(&b, 10));
        http_free_buffer(&b);
    }
    /* A large body spanning many reads, still on the same connection. */
    HttpBuffer b;
    ASSERT_TRUE(get("/cl/300000", &b));
    ASSERT_TRUE(body_ok(&b, 300000));
    http_free_buffer(&b);
    ASSERT_EQ(1, accepted() - a0);
    return true;
}

TEST(test_http_pool_framing_keeps_connection) {
    ASSERT_TRUE(server_start());
    conn_pool_close_idle();
    int a0 = accepted();
    HttpBuffer b;
    ASSERT_TRUE(get("/chunked", &b));
    ASSERT_EQ(11, (int)b.size);
    ASSERT_TRUE(memcmp(b.data, "hello world", 11) == 0);
    http_free_buffer(&b);

    ASSERT_TRUE(get("/empty", &b));
    ASSERT_EQ(0, (int)b.size);
    http_free_buffer(&b);

    ASSERT_TRUE(get("/nocontent", &b));
    ASSERT_EQ(0, (int)b.size);
    http_free_buffer(&b);

    char url[128], err[256] = "";
    snprintf(url, sizeof(url), "http://127.0.0.1:%s/head", g_port);
    int status = 0;
    memset(&b, 0, sizeof(b));
    ASSERT_TRUE(http_request_to_memory(url, "HEAD", NULL, 0, NULL, 0, &b, &status, err, sizeof(err)));
    ASSERT_EQ(200, status);
    ASSERT_EQ(0, (int)b.size);
    http_free_buffer(&b);

    ASSERT_TRUE(get("/cl/5", &b));
    ASSERT_TRUE(body_ok(&b, 5));
    http_free_buffer(&b);
    /* Every response had a known end: one connection carried them all. */
    ASSERT_EQ(1, accepted() - a0);
    return true;
}

TEST(test_http_pool_close_and_eof) {
    ASSERT_TRUE(server_start());
    conn_pool_close_idle();
    int a0 = accepted();
    HttpBuffer b;
    ASSERT_TRUE(get("/close", &b));
    ASSERT_STR_EQ("abc", b.data);
    http_free_buffer(&b);
    ASSERT_TRUE(get("/eof", &b));
    ASSERT_STR_EQ("until-eof", b.data);
    http_free_buffer(&b);
    ASSERT_TRUE(get("/cl/4", &b));
    ASSERT_TRUE(body_ok(&b, 4));
    http_free_buffer(&b);
    /* "Connection: close" and a read-to-EOF body each end their connection. */
    ASSERT_EQ(3, accepted() - a0);
    return true;
}

TEST(test_http_pool_retries_dropped_connection) {
    ASSERT_TRUE(server_start());
    conn_pool_close_idle();
    int a0 = accepted();
    HttpBuffer b;
    ASSERT_TRUE(get("/dropnext", &b));
    ASSERT_STR_EQ("ok", b.data);
    http_free_buffer(&b);
    /* The pooled connection is closed when this request arrives: the client
     * retries on a new connection and the caller never sees the failure. */
    ASSERT_TRUE(get("/cl/7", &b));
    ASSERT_TRUE(body_ok(&b, 7));
    http_free_buffer(&b);
    ASSERT_EQ(2, accepted() - a0);
    return true;
}

static void *parallel_get(void *arg) {
    int *ok = (int *)arg;
    for (int i = 0; i < 3; i++) {
        HttpBuffer b;
        if (get("/slow/1000", &b) && body_ok(&b, 1000)) (*ok)++;
        http_free_buffer(&b);
    }
    return NULL;
}

TEST(test_http_pool_per_host_cap) {
    ASSERT_TRUE(server_start());
    conn_pool_close_idle();
    platform_sleep_ms(50);                  /* let the server see the closes */
    pthread_mutex_lock(&g_srv_mu);
    g_open_max = g_open;
    pthread_mutex_unlock(&g_srv_mu);
    int a0 = accepted();
    pthread_t t[12];
    int ok[12] = {0};
    for (int i = 0; i < 12; i++) pthread_create(&t[i], NULL, parallel_get, &ok[i]);
    for (int i = 0; i < 12; i++) pthread_join(t[i], NULL);
    int total = 0;
    for (int i = 0; i < 12; i++) total += ok[i];
    ASSERT_EQ(36, total);
    /* 12 threads, 36 requests: at most 6 connections to the host. */
    ASSERT_TRUE(accepted() - a0 <= MAX_CONNECTIONS_PER_HOST);
    pthread_mutex_lock(&g_srv_mu);
    int open_max = g_open_max;
    pthread_mutex_unlock(&g_srv_mu);
    ASSERT_TRUE(open_max <= MAX_CONNECTIONS_PER_HOST);
    ConnPoolStats st;
    conn_pool_get_stats(&st);
    ASSERT_EQ(0, st.active);
    return true;
}

/* A request body larger than one write of the transport arrives whole: a
 * loopback relay that takes 10 bytes a write (as the real one takes one
 * message, and TLS one record) got only the first part of a POST. */
static char *g_lb_got;
static size_t g_lb_n, g_lb_off;
static const char k_lb_resp[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
static int lb_connect(const char *host, const char *port) { (void)host; (void)port; g_lb_n = g_lb_off = 0; return 7; }
static int lb_send(int id, const unsigned char *buf, size_t len, unsigned ms) {
    (void)id; (void)ms;
    size_t n = len < 10 ? len : 10;
    g_lb_got = realloc(g_lb_got, g_lb_n + n);
    memcpy(g_lb_got + g_lb_n, buf, n);
    g_lb_n += n;
    return (int)n;
}
static int lb_recv(int id, unsigned char *buf, size_t len, unsigned ms) {
    (void)id; (void)ms;
    size_t left = sizeof k_lb_resp - 1 - g_lb_off, n = left < len ? left : len;
    memcpy(buf, k_lb_resp + g_lb_off, n);
    g_lb_off += n;
    return (int)n;
}
static bool lb_readable(int id) { (void)id; return true; }
static void lb_close(int id) { (void)id; }

TEST(test_http_large_post_body_whole) {
    static const TlsLoopback lb = { lb_connect, lb_send, lb_recv, lb_readable, lb_close };
    size_t n = 70000;
    char *body = malloc(n);
    for (size_t i = 0; i < n; i++) body[i] = (char)('a' + i % 26);
    tls_set_loopback(&lb);
    HttpBuffer out = { 0 };
    int status = 0;
    char err[256] = "";
    bool ok = http_request_to_memory("http://127.0.0.1:9/sink", "POST", body, n, NULL, 0, &out, &status, err, sizeof err);
    tls_set_loopback(NULL);
    const char *end = NULL;
    for (size_t i = 0; i + 4 <= g_lb_n; i++) if (!memcmp(g_lb_got + i, "\r\n\r\n", 4)) { end = g_lb_got + i + 4; break; }
    size_t got = end ? g_lb_n - (size_t)(end - g_lb_got) : 0;
    if (!ok || got != n) printf("    ok %d status %d body %zu of %zu (%s)\n", ok, status, got, n, err);
    bool same = end && got == n && !memcmp(end, body, n);
    http_free_buffer(&out);
    free(body);
    free(g_lb_got);
    g_lb_got = NULL;
    ASSERT_TRUE(ok && status == 200 && same);
    return true;
}

void run_http_pool_tests(void) {
    printf("\n--- HTTP connection pool tests ---\n");
    RUN_TEST(test_http_pool_reuses_connection);
    RUN_TEST(test_http_pool_framing_keeps_connection);
    RUN_TEST(test_http_pool_close_and_eof);
    RUN_TEST(test_http_pool_retries_dropped_connection);
    RUN_TEST(test_http_pool_per_host_cap);
    RUN_TEST(test_http_large_post_body_whole);
}
