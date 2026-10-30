/* lib_net.c - models of what cb_net.exe links outside M9 (SANDBOX_DESIGN.md
 * 8.7.1): the HTTP, TLS, cookie and URL code (http_download.c, tls_client.c,
 * url_whatwg.c, platform).  TRUSTED, like the Windows contracts: each states
 * what the code needs from the shim (valid strings and buffers, as
 * preconditions Eva checks at the calls) and does what the shim may observe
 * (the buffers and strings it returns, of any content; the shim's callbacks
 * it calls: the address classifier and the loopback relay's functions, with
 * any arguments of the right shape), not how it does it.
 *
 * Where the real function dereferences an argument without checking it,
 * the model requires it valid: http_cookie_jar_version, _save and _load
 * read jar->... (http_download.c). */
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "platform.h"
#include "http_download.h"
#include "net_loopback.h"
#include "tls_space.h"
#include "url_whatwg.h"
#include "__fc_builtin.h"

bool platform_init(void) { return Frama_C_nondet(0, 1); }
bool platform_http_init(void) { return Frama_C_nondet(0, 1); }

static TlsSpaceFn g_classify;
static const TlsLoopback *g_lb;

void tls_set_space_policy(TlsSpaceFn classify) { g_classify = classify; }
void tls_set_space_limit(int limit) { (void)limit; }
int tls_last_space(void) { return Frama_C_interval(-1, 3); }
void tls_reset_last_space(void) {}
/*@ requires lb: lb == \null || \valid_read(lb); */
void tls_set_loopback(const TlsLoopback *lb) { g_lb = lb; }

/* An opaque jar (http_download.c: calloc of its struct). */
typedef struct FcJar { uint64_t version; void *head; } FcJar;

HttpCookieJar *http_cookie_jar_new(void) { return (HttpCookieJar *)calloc(1, sizeof(FcJar)); }

void http_cookie_jar_use(HttpCookieJar *jar) { (void)jar; }

/*@ requires jar: \valid_read((FcJar *)(void *)jar); */
uint64_t http_cookie_jar_version(HttpCookieJar *jar) { return ((FcJar *)(void *)jar)->version; }

/* The jar's persistent cookies as text: malloc'd, *len bytes (and a NUL
 * after them), or NULL. */
/*@ requires jar: \valid_read((FcJar *)(void *)jar);
    requires len: \valid(len); */
char *http_cookie_jar_save(HttpCookieJar *jar, size_t *len) {
    (void)jar;
    size_t n = (size_t)Frama_C_interval(0, 1 << 20);
    char *s = (char *)malloc(n + 1);
    if (!s) return NULL;
    Frama_C_make_unknown(s, n);
    s[n] = 0;
    *len = n;
    return s;
}

/*@ requires jar: \valid((FcJar *)(void *)jar);
    requires data: \valid_read(data + (0 .. len - 1)) && \initialized(data + (0 .. len - 1)); */
void http_cookie_jar_load(HttpCookieJar *jar, const char *data, size_t len) {
    if (len) {
        char first = data[0], last = data[len - 1];
        (void)first;
        (void)last;
    }
    ((FcJar *)(void *)jar)->version = (uint64_t)Frama_C_unsigned_int_interval(0, 0xffffffffu);
}

static char g_cookie_buffer[8192];

/* NULL, or this thread's buffer: a string. */
/*@ requires url: url == \null || valid_read_string(url); */
const char *http_cookie_get_for_document(const char *url) {
    if (!url || Frama_C_nondet(0, 1)) return NULL;
    Frama_C_make_unknown(g_cookie_buffer, sizeof(g_cookie_buffer) - 1);
    g_cookie_buffer[sizeof(g_cookie_buffer) - 1] = 0;
    return g_cookie_buffer;
}

/*@ requires url: url == \null || valid_read_string(url);
    requires line: cookie_line == \null || valid_read_string(cookie_line); */
void http_cookie_set_for_document(const char *url, const char *cookie_line) { (void)url; (void)cookie_line; }

static char t_last_url[2048];

/* The URL the last request of this thread ended at (after redirects). */
const char *http_last_url(void) { return t_last_url; }

/* A sockaddr of either family, any address and port, for the classifier. */
static void classify_any(void) {
    if (!g_classify) return;
    struct sockaddr_storage a;
    Frama_C_make_unknown((char *)&a, sizeof(a));
    a.ss_family = (ADDRESS_FAMILY)(Frama_C_nondet(0, 1) ? AF_INET : AF_INET6);
    int space = g_classify((const struct sockaddr *)&a);
    (void)space;
}

/* What the TLS code does with the relay's functions: a connection to a
 * loopback host (any port text), bytes sent and received through it in
 * buffers of the sizes it passes, closed. */
static void use_loopback(void) {
    if (!g_lb) return;
    static char port[8];
    Frama_C_make_unknown(port, sizeof(port) - 1);
    port[sizeof(port) - 1] = 0;
    const char *host = Frama_C_nondet(0, 1) ? "127.0.0.1" : Frama_C_nondet(0, 1) ? "[::1]" : "localhost";
    int id = g_lb->connect(host, port);
    if (id <= 0) return;
    static unsigned char buf[16384];
    size_t n = (size_t)Frama_C_interval(0, sizeof(buf));
    unsigned timeout = Frama_C_unsigned_int_interval(0, 30000);
    Frama_C_make_unknown((char *)buf, sizeof(buf));
    while (Frama_C_nondet(0, 1)) {
        switch (Frama_C_interval(0, 2)) {
        case 0: g_lb->send(id, buf, n, timeout); break;
        case 1: g_lb->recv(id, buf, n, timeout); break;
        default: g_lb->readable(id); break;
        }
    }
    g_lb->close(id);
}

/* Fills *outBuffer with a malloc'd body and header block (each with a NUL
 * after its size, as http_download.c makes them), *outStatus, and err (a
 * string) on failure. */
/*@ requires url: valid_read_string(url);
    requires method: valid_read_string(method);
    requires body: postDataLen > 0 ==> \valid_read(postData + (0 .. postDataLen - 1));
    requires headers: headerCount > 0 ==> \valid_read(headers + (0 .. headerCount - 1));
    requires header_strings: \forall integer i; 0 <= i < headerCount ==> valid_read_string(headers[i]);
    requires out: \valid(outBuffer) && \valid(outStatus);
    requires err: errLen > 0 && \valid(err + (0 .. errLen - 1)); */
bool http_request_to_memory(const char *url, const char *method, const char *postData, size_t postDataLen,
                            const char **headers, size_t headerCount, HttpBuffer *outBuffer, int *outStatus,
                            char *err, size_t errLen) {
    (void)url; (void)method; (void)postData; (void)postDataLen; (void)headers; (void)headerCount;
    if (Frama_C_nondet(0, 1)) classify_any();
    if (Frama_C_nondet(0, 1)) use_loopback();
    Frama_C_make_unknown(t_last_url, sizeof(t_last_url) - 1);
    t_last_url[sizeof(t_last_url) - 1] = 0;
    if (Frama_C_nondet(0, 1)) {
        Frama_C_make_unknown(err, errLen - 1);
        err[errLen - 1] = 0;
        return false;
    }
    size_t dn = (size_t)Frama_C_interval(0, 1 << 24), hn = (size_t)Frama_C_interval(0, 1 << 16);
    char *d = Frama_C_nondet(0, 1) ? NULL : (char *)malloc(dn + 1);
    char *h = Frama_C_nondet(0, 1) ? NULL : (char *)malloc(hn + 1);
    if (d) { Frama_C_make_unknown(d, dn); d[dn] = 0; }
    if (h) { Frama_C_make_unknown(h, hn); h[hn] = 0; }
    outBuffer->data = d;
    outBuffer->size = d ? dn : 0;
    outBuffer->headers = h;
    outBuffer->headers_size = h ? hn : 0;
    *outStatus = Frama_C_interval(0, 999);
    return true;
}

/*@ requires b: buffer == \null || \valid(buffer);
    requires data: buffer != \null ==> (buffer->data == \null || \freeable(buffer->data));
    requires headers: buffer != \null ==> (buffer->headers == \null || \freeable(buffer->headers)); */
void http_free_buffer(HttpBuffer *buffer) {
    if (!buffer) return;
    free(buffer->data);
    buffer->data = NULL;
    buffer->size = 0;
    free(buffer->headers);
    buffer->headers = NULL;
    buffer->headers_size = 0;
}

/* 0 and *out filled (its strings malloc'd), or -1 and *out empty. */
/*@ requires input: \valid_read(input + (0 .. len - 1));
    requires out: \valid(out); */
int wurl_parse(const char *input, size_t len, const WUrl *base, WUrl *out) {
    (void)base;
    if (len) {
        char first = input[0], last = input[len - 1];
        (void)first;
        (void)last;
    }
    memset(out, 0, sizeof(*out));
    out->port = -1;
    return Frama_C_nondet(0, 1) ? 0 : -1;
}

/* The URL's origin, malloc'd ("null" for an opaque one), or NULL. */
/*@ requires url: \valid_read(url); */
char *wurl_origin(const WUrl *url) {
    (void)url;
    size_t n = (size_t)Frama_C_interval(4, 300);
    char *s = (char *)malloc(n + 1);
    if (!s) return NULL;
    for (size_t i = 0; i < n; i++) s[i] = (char)Frama_C_interval(1, 255);
    s[n] = 0;
    return s;
}

/*@ requires url: \valid(url); */
void wurl_free(WUrl *url) { memset(url, 0, sizeof(*url)); }
