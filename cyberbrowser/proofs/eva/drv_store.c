/* drv_store.c - Eva's driver for cb_store.exe (store_main.c): its main,
 * with any command line (--root's hex of any length).  One thread. */
#define main store_main
#include "store_main.c"
#undef main
#include "drv.h"

int main(void) {
    static char *argv[FC_MAX_ARGS];
    static const char *const fixed[] = { "--chan", NULL, NULL, NULL, "--root", NULL };
    int argc = fc_command_line(argv, fixed, 6, 1, 4096);
    return store_main(argc, argv);
}
