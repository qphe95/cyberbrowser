#ifndef WIN32_UNISTD_H
#define WIN32_UNISTD_H

/* Minimal unistd compatibility for Windows test builds */

#include <winsock2.h>
#include <windows.h>
#include <process.h>

static inline void usleep(unsigned int usec) {
    Sleep(usec / 1000);
}

static inline unsigned int sleep(unsigned int seconds) {
    Sleep(seconds * 1000);
    return 0;
}

#endif /* WIN32_UNISTD_H */
