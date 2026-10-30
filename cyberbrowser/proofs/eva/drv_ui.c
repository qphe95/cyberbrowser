/* drv_ui.c - Eva's driver for cb_ui.exe (ui_main.c): its main, with any
 * command line (start URLs, hex, of any length).  Window procedures run
 * from DispatchMessageW with any message (win_models.c); the test script
 * (CYBER_UI_SCRIPT) is any file, or none. */
#define main ui_main
#include "ui_main.c"
#undef main
#include "drv.h"

int main(void) {
    static char *argv[FC_MAX_ARGS];
    static const char *const fixed[] = { "--chan", NULL, NULL, NULL };
    int argc = fc_command_line(argv, fixed, 4, 2, 8192);
    return ui_main(argc, argv);
}
