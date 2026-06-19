#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "tls_client.h"

int main() {
    setbuf(stdout, NULL);
    printf("platform_init...\n");
    if (!platform_init()) { return 1; }

    const char *host = "rr1---sn-a5mlrnl6.googlevideo.com";
    const char *port = "443";
    char err[512] = {0};

    printf("TLS connect 1 to %s...\n", host);
    TlsClient client1 = {0};
    if (!tls_client_connect(&client1, host, port, err, sizeof(err))) {
        printf("TLS connect 1 failed: %s\n", err);
        platform_cleanup();
        return 1;
    }
    printf("TLS connect 1 OK\n");

    const char *req = "GET /videoplayback?itag=140 HTTP/1.1\r\nHost: rr1---sn-a5mlrnl6.googlevideo.com\r\nConnection: close\r\n\r\n";
    tls_client_write(&client1, (unsigned char*)req, strlen(req));
    
    char buf[8192];
    ssize_t n;
    size_t total = 0;
    while ((n = tls_client_read(&client1, (unsigned char*)buf, sizeof(buf))) > 0) {
        total += n;
    }
    printf("Read 1: %zu bytes, last n=%zd\n", total, n);
    
    printf("Closing 1...\n");
    tls_client_close(&client1);
    printf("Closed 1\n");

    printf("TLS connect 2 to %s...\n", host);
    TlsClient client2 = {0};
    if (!tls_client_connect(&client2, host, port, err, sizeof(err))) {
        printf("TLS connect 2 failed: %s\n", err);
        platform_cleanup();
        return 1;
    }
    printf("TLS connect 2 OK\n");
    
    tls_client_write(&client2, (unsigned char*)req, strlen(req));
    
    total = 0;
    while ((n = tls_client_read(&client2, (unsigned char*)buf, sizeof(buf))) > 0) {
        total += n;
    }
    printf("Read 2: %zu bytes, last n=%zd\n", total, n);
    
    printf("Closing 2...\n");
    tls_client_close(&client2);
    printf("Closed 2\n");

    platform_cleanup();
    printf("Done.\n");
    return 0;
}
