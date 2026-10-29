#ifndef SPAWN_H
#define SPAWN_H

/* Starting child processes that inherit exactly the handles they are given
 * (PROC_THREAD_ATTRIBUTE_HANDLE_LIST), with their handle values on the
 * command line. */

#include <stdbool.h>
#include <windows.h>

/* The folder of this process's executable, with a trailing backslash. */
const wchar_t *spawn_exe_dir(void);

/* How a child is confined (SANDBOX_DESIGN.md section 6); all fields
 * optional.  sandbox.c fills these in for each kind of process. */
typedef struct SpawnSandbox {
    PSID appcontainer;                  /* run in this AppContainer */
    SID_AND_ATTRIBUTES *caps;           /* with these capabilities */
    DWORD ncaps;
    bool lpac;                          /* less privileged AppContainer */
    HANDLE token;                       /* else: CreateProcessAsUser with this */
    DWORD64 mitigations[2];             /* PROCESS_CREATION_MITIGATION_POLICY_* */
    SECURITY_ATTRIBUTES *object_sa;     /* for the process and thread objects */
    PACL label;                         /* integrity label set on them (SACL) */
    PACL default_dacl;                  /* for the objects the child creates */
    HANDLE job;                         /* the child's own job, nested in `job` */
} SpawnSandbox;

/* Start <exe dir>\exe_name with the command line `args` (UTF-8, after the
 * program name).  `inherit` handles are made inheritable for the call only.
 * std_out (may be NULL) becomes the child's stdout and stderr.  The child is
 * put in `job` if not NULL, then in sb->job.  sb may be NULL (no sandbox).
 * Returns the process handle, or NULL (also if ninherit > SPAWN_MAX_INHERIT). */
#define SPAWN_MAX_INHERIT 24
HANDLE spawn_child(const wchar_t *exe_name, const char *args, HANDLE *inherit, int ninherit,
                   HANDLE std_out, HANDLE job, DWORD flags, const SpawnSandbox *sb);

/* A handle value as it goes on a command line, and back. */
unsigned long long spawn_handle_arg(HANDLE h);
HANDLE spawn_handle_from_arg(const char *s);

#endif
