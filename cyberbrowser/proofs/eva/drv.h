/* drv.h - what Eva's drivers (drv_*.c) share: command lines.
 *
 * Each driver includes its process's main file with main renamed, so it
 * can call the process's entry point and, where the process has more
 * threads than Eva follows, their functions too; then main() gives it a
 * command line.  The coordinator writes the command lines; here they are
 * any strings (a handle's number, hex, anything), of any count. */
#ifndef FC_DRV_H
#define FC_DRV_H

#include <stdlib.h>
#include <string.h>
#include "__fc_builtin.h"

/* Any NUL-terminated string of fewer than cap bytes, in s[0 .. cap - 1]. */
static void fc_string(char *s, size_t cap) {
    Frama_C_make_unknown(s, cap - 1);
    s[cap - 1] = 0;
}

/* A command line of 1 to 1 + nfixed + extra words: argv[0], then the words
 * a well-formed line has (fixed[i], or any string instead; NULL: always
 * any string), then any strings.  Words are shorter than cap (<= 8192)
 * bytes.  argv[argc] is NULL.  Eva keeps each argc apart (split). */
#define FC_MAX_ARGS 12
static int fc_command_line(char **argv, const char *const *fixed, int nfixed, int extra, size_t cap) {
    static char words[FC_MAX_ARGS][8192];
    if (cap > sizeof(words[0])) cap = sizeof(words[0]);
    int total = 1 + nfixed + extra;
    if (total > FC_MAX_ARGS - 1) total = FC_MAX_ARGS - 1;
    int argc = Frama_C_interval(1, total);
    //@ split argc;
    argv[0] = "cb.exe";
    //@ loop unroll FC_MAX_ARGS;
    for (int i = 1; i < argc; i++) {
        if (i <= nfixed && fixed[i - 1] && Frama_C_nondet(0, 1)) {
            argv[i] = (char *)fixed[i - 1];
        } else {
            fc_string(words[i], cap);
            argv[i] = words[i];
        }
    }
    argv[argc] = NULL;
    return argc;
}

#endif
