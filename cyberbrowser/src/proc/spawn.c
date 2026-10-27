/* spawn.c - starting child processes (spawn.h). */
#include "spawn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const wchar_t *spawn_exe_dir(void) {
    static wchar_t dir[MAX_PATH];
    if (!dir[0]) {
        DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return L".\\";
        while (n > 0 && dir[n - 1] != L'\\' && dir[n - 1] != L'/') n--;
        dir[n] = 0;
    }
    return dir;
}

unsigned long long spawn_handle_arg(HANDLE h) {
    return (unsigned long long)(uintptr_t)h;
}

HANDLE spawn_handle_from_arg(const char *s) {
    return (HANDLE)(uintptr_t)strtoull(s, NULL, 10);
}

HANDLE spawn_child(const wchar_t *exe_name, const char *args, HANDLE *inherit, int ninherit,
                   HANDLE std_out, HANDLE job, DWORD flags) {
    wchar_t exe[MAX_PATH * 2];
    _snwprintf(exe, MAX_PATH * 2, L"%ls%ls", spawn_exe_dir(), exe_name);
    exe[MAX_PATH * 2 - 1] = 0;

    /* "exe" args, as UTF-16 */
    int alen = MultiByteToWideChar(CP_UTF8, 0, args, -1, NULL, 0);
    size_t cap = wcslen(exe) + (size_t)alen + 8;
    wchar_t *cmd = (wchar_t *)malloc(cap * sizeof(wchar_t));
    if (!cmd) return NULL;
    _snwprintf(cmd, cap, L"\"%ls\" ", exe);
    size_t used = wcslen(cmd);
    MultiByteToWideChar(CP_UTF8, 0, args, -1, cmd + used, (int)(cap - used));

    HANDLE list[16];
    int n = 0;
    for (int i = 0; i < ninherit && n < 14; i++) list[n++] = inherit[i];
    if (std_out) list[n++] = std_out;
    for (int i = 0; i < n; i++) SetHandleInformation(list[i], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    SIZE_T attr_bytes = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_bytes);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_bytes);
    HANDLE proc = NULL;
    if (attrs && InitializeProcThreadAttributeList(attrs, 1, 0, &attr_bytes) &&
        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list,
                                  (SIZE_T)n * sizeof(HANDLE), NULL, NULL)) {
        STARTUPINFOEXW si;
        memset(&si, 0, sizeof(si));
        si.StartupInfo.cb = sizeof(si);
        si.lpAttributeList = attrs;
        if (std_out) {
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            si.StartupInfo.hStdInput = NULL;
            si.StartupInfo.hStdOutput = std_out;
            si.StartupInfo.hStdError = std_out;
        }
        PROCESS_INFORMATION pi;
        memset(&pi, 0, sizeof(pi));
        /* Suspended until it is in the job, so nothing it starts escapes it. */
        if (CreateProcessW(exe, cmd, NULL, NULL, TRUE,
                           flags | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
                           NULL, NULL, &si.StartupInfo, &pi)) {
            if (job) AssignProcessToJobObject(job, pi.hProcess);
            ResumeThread(pi.hThread);
            CloseHandle(pi.hThread);
            proc = pi.hProcess;
        } else {
            fprintf(stderr, "[spawn] CreateProcess %ls failed: %lu\n", exe, GetLastError());
        }
        DeleteProcThreadAttributeList(attrs);
    }
    free(attrs);
    for (int i = 0; i < n; i++) SetHandleInformation(list[i], HANDLE_FLAG_INHERIT, 0);
    free(cmd);
    return proc;
}
