// Standalone test for URL analysis and JS execution
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "url_analyzer.h"

int main(int argc, char **argv) {
    const char *url = argc > 1 ? argv[1] : "https://www.youtube.com/watch?v=dQw4w9WgXcQ";
    
    printf("Testing URL analysis for: %s\n", url);
    
    MediaUrl media;
    char err[512] = {0};
    
    if (url_analyze(url, &media, err, sizeof(err))) {
        printf("Success! Media URL: %s\n", media.url);
        if (media.mime[0]) {
            printf("MIME type: %s\n", media.mime);
        }
        return 0;
    } else {
        printf("Failed: %s\n", err);
        return 1;
    }
}
