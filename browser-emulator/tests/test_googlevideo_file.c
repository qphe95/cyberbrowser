#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "http_download.h"

int main() {
    setbuf(stdout, NULL);
    printf("platform_init...\n");
    if (!platform_init()) { return 1; }
    printf("platform_http_init...\n");
    if (!platform_http_init()) { return 1; }

    const char *url = "https://rr1---sn-a5mlrnl6.googlevideo.com/videoplayback?expire=1779318545&ei=seoNarXgCbr3sfIPgY_ZqQQ&ip=108.208.122.253&id=o-AF-5eB8O0fEQwBmknlcqru-3Qi4yy9oNTpJA9CeLmLct&itag=140&source=youtube&requiressl=yes&xpc=EgVo2aDSNQ%3D%3D&cps=230&met=1779296945%2C&mh=LB&mm=31%2C26&mn=sn-a5mlrnl6%2Csn-o097znsz&ms=au%2Conr&mv=m&mvi=4&pl=24&rms=au%2Cau&initcwndbps=1281250&bui=AbKmrwr6uD6cNgHxYPykf3Zr-EUrtZCpUggCUF8GiJ2l2TI2XjTXJV3H7vuKFGFkLSIkeI5EhvOra-PK&vprv=1&svpuc=1&mime=audio%2Fmp4&rqh=1&gir=yes&clen=447193910&dur=27632.000&lmt=1775719740303171&mt=1779296743&fvip=3&keepalive=yes&fexp=51565116%2C51565682&c=ANDROID_VR&txp=6308224&sparams=expire%2Cei%2Cip%2Cid%2Citag%2Csource%2Crequiressl%2Cxpc%2Cbui%2Cvprv%2Csvpuc%2Cmime%2Crqh%2Cgir%2Cclen%2Cdur%2Clmt&sig=AHEqNM4wRgIhALgNJzKnQvreHPZ6M6-KO8KW3UStnSPrR6awQ4bVJWwKAiEAyiA13Rj945ta2eCICBn54F7C2ea7joSKL8AdiPXa0a4%3D&lsparams=cps%2Cmet%2Cmh%2Cmm%2Cmn%2Cms%2Cmv%2Cmvi%2Cpl%2Crms%2Cinitcwndbps&lsig=APaTxxMwRQIgMUl3RnE8HxrqAfvQ5qzySO6dDmxkxAZ47LsuYur9YMkCIQD7DBDL5dg9aAro80loPMAEuVPbayjEkJZDgjT_deJYpQ%3D%3D";
    char err[512] = {0};

    DownloadState ds = {0};
    download_state_init(&ds);

    printf("Downloading to file...\n");
    if (!http_download_to_file(url, "C:/Users/qingping/Music/test_download.m4a", &ds, err, sizeof(err))) {
        printf("Download failed: %s\n", err);
    } else {
        printf("Download OK\n");
    }

    platform_http_cleanup();
    platform_cleanup();
    printf("Done.\n");
    return 0;
}
