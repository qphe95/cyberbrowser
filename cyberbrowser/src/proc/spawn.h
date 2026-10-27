#ifndef SPAWN_H
#define SPAWN_H

/* Starting child processes that inherit exactly the handles they are given
 * (PROC_THREAD_ATTRIBUTE_HANDLE_LIST), with their handle values on the
 * command line. */

#include <stdbool.h>
#include <windows.h>

/* The folder of this process's executable, with a trailing backslash. */
const wchar_t *spawn_exe_dir(void);

/* Start <exe dir>\exe_name with the command line `args` (UTF-8, after the
 * program name).  `inherit` handles are made inheritable for the call only.
 * std_out (may be NULL) becomes the child's stdout and stderr.  The child is
 * put in `job` if not NULL.  Returns the process handle, or NULL. */
HANDLE spawn_child(const wchar_t *exe_name, const char *args, HANDLE *inherit, int ninherit,
                   HANDLE std_out, HANDLE job, DWORD flags);

/* A handle value as it goes on a command line, and back. */
unsigned long long spawn_handle_arg(HANDLE h);
HANDLE spawn_handle_from_arg(const char *s);

#endif
