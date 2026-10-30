/* drv_coord.c - Eva's driver for cb_coord.exe (coordinator.c, with
 * sandbox.c and spawn.c): its main, with any command line (start URLs of
 * any text, or --remove-profiles).  The thread-pool callback on_exit only
 * sets an event (RegisterWaitForSingleObject: not run here). */
#define main coord_main
#include "coordinator.c"
#undef main
#include "drv.h"

int main(void) {
    static char *argv[FC_MAX_ARGS];
    static const char *const fixed[] = { "--remove-profiles" };
    int argc = fc_command_line(argv, fixed, 1, 3, 4096 + 64);
    return coord_main(argc, argv);
}
