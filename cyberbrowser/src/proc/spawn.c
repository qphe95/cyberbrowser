/* spawn.c - starting child processes (spawn.h). */
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00             /* the sandbox's process attributes */
#include "spawn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <aclapi.h>

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
                   HANDLE std_out, HANDLE job, DWORD flags, const SpawnSandbox *sb) {
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

    /* A tab inherits 15 (five channels).  Refuse rather than leave one out:
     * a channel missing an event loses its wakeups. */
    HANDLE list[SPAWN_MAX_INHERIT + 1];
    int n = 0;
    if (ninherit > SPAWN_MAX_INHERIT) {
        free(cmd);
        return NULL;
    }
    for (int i = 0; i < ninherit; i++) list[n++] = inherit[i];
    if (std_out) list[n++] = std_out;
    for (int i = 0; i < n; i++) SetHandleInformation(list[i], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    /* The attributes: the handle list, and the sandbox's. */
    SECURITY_CAPABILITIES caps;
    DWORD lpac = PROCESS_CREATION_ALL_APPLICATION_PACKAGES_OPT_OUT;
    DWORD nattrs = 1;
    if (sb) nattrs += (sb->appcontainer ? 1 : 0) + (sb->lpac ? 1 : 0) +
                      (sb->mitigations[0] || sb->mitigations[1] ? 1 : 0);
    SIZE_T attr_bytes = 0;
    InitializeProcThreadAttributeList(NULL, nattrs, 0, &attr_bytes);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_bytes);
    bool ready = attrs && InitializeProcThreadAttributeList(attrs, nattrs, 0, &attr_bytes) &&
                 UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list,
                                           (SIZE_T)n * sizeof(HANDLE), NULL, NULL);
    if (ready && sb && sb->appcontainer) {
        memset(&caps, 0, sizeof(caps));
        caps.AppContainerSid = sb->appcontainer;
        caps.Capabilities = sb->ncaps ? sb->caps : NULL;    /* else ERROR_INVALID_PARAMETER */
        caps.CapabilityCount = sb->ncaps;
        ready = UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &caps,
                                          sizeof(caps), NULL, NULL);
    }
    if (ready && sb && sb->lpac)
        ready = UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY,
                                          &lpac, sizeof(lpac), NULL, NULL);
    if (ready && sb && (sb->mitigations[0] || sb->mitigations[1]))
        ready = UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY,
                                          (void *)sb->mitigations, sizeof(sb->mitigations), NULL, NULL);

    HANDLE proc = NULL;
    if (ready) {
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
        SECURITY_ATTRIBUTES *sa = sb ? sb->object_sa : NULL;
        /* Suspended until it is in its jobs, so nothing it starts escapes them. */
        DWORD how = flags | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED;
        BOOL started = sb && sb->token
            ? CreateProcessAsUserW(sb->token, exe, cmd, sa, sa, TRUE, how, NULL, NULL, &si.StartupInfo, &pi)
            : CreateProcessW(exe, cmd, sa, sa, TRUE, how, NULL, NULL, &si.StartupInfo, &pi);
        if (started) {
            bool jobbed = (!job || AssignProcessToJobObject(job, pi.hProcess)) &&
                          (!sb || !sb->job || AssignProcessToJobObject(sb->job, pi.hProcess));
            /* A label in object_sa needs SeRelabelPrivilege at creation; set
             * afterwards, one no higher than ours needs only WRITE_OWNER. */
            bool dacl = !sb || !sb->label ||
                        (SetSecurityInfo(pi.hProcess, SE_KERNEL_OBJECT, LABEL_SECURITY_INFORMATION, NULL, NULL, NULL,
                                         sb->label) == ERROR_SUCCESS &&
                         SetSecurityInfo(pi.hThread, SE_KERNEL_OBJECT, LABEL_SECURITY_INFORMATION, NULL, NULL, NULL,
                                         sb->label) == ERROR_SUCCESS);
            if (sb && sb->default_dacl) {
                HANDLE tok = NULL;
                TOKEN_DEFAULT_DACL dd = { sb->default_dacl };
                dacl = dacl && OpenProcessToken(pi.hProcess, TOKEN_ADJUST_DEFAULT, &tok) &&
                       SetTokenInformation(tok, TokenDefaultDacl, &dd, sizeof(dd));
                if (tok) CloseHandle(tok);
            }
            if (jobbed && dacl) {
                ResumeThread(pi.hThread);
                proc = pi.hProcess;
            } else {
                fprintf(stderr, "[spawn] %ls: could not put it in its job or label it: %lu\n", exe_name, GetLastError());
                TerminateProcess(pi.hProcess, 1);
                CloseHandle(pi.hProcess);
            }
            CloseHandle(pi.hThread);
        } else {
            fprintf(stderr, "[spawn] CreateProcess %ls failed: %lu\n", exe, GetLastError());
        }
    } else {
        fprintf(stderr, "[spawn] %ls: process attributes refused: %lu\n", exe_name, GetLastError());
    }
    if (attrs) DeleteProcThreadAttributeList(attrs);
    free(attrs);
    for (int i = 0; i < n; i++) SetHandleInformation(list[i], HANDLE_FLAG_INHERIT, 0);
    free(cmd);
    return proc;
}
