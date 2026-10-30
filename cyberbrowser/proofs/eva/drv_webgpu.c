/* drv_webgpu.c - Eva's driver for cb_webgpu.exe (webgpu_main.c): its main,
 * with any command line (--site's hex of any length).  One thread.  WebGL
 * and WebGPU are lib_webgpu.c's models. */
#define main webgpu_main
#include "webgpu_main.c"
#undef main
#include "drv.h"

int main(void) {
    static char *argv[FC_MAX_ARGS];
    static const char *const fixed[] = { "--chan", NULL, NULL, NULL, "--site", NULL };
    int argc = fc_command_line(argv, fixed, 6, 1, 1024);
    return webgpu_main(argc, argv);
}
