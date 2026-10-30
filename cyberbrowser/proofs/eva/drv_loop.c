/* drv_loop.c - Eva's driver for cb_loop.exe (loop_main.c): its main, with
 * any command line.  One thread: nothing else to drive. */
#define main loop_main
#include "loop_main.c"
#undef main
#include "drv.h"

int main(void) {
    static char *argv[FC_MAX_ARGS];
    static const char *const fixed[] = { "--chan", NULL, NULL, NULL };
    int argc = fc_command_line(argv, fixed, 4, 1, 32);
    return loop_main(argc, argv);
}
