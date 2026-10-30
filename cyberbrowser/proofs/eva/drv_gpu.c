/* drv_gpu.c - Eva's driver for cb_gpu.exe (gpu_main.c): its main, with any
 * command line.  One thread.  The rasterizer and Vulkan are lib_gpu.c's
 * models; CYBER_GPU picks either (getenv: any value or none). */
#define main gpu_main
#include "gpu_main.c"
#undef main
#include "drv.h"

int main(void) {
    static char *argv[FC_MAX_ARGS];
    static const char *const fixed[] = { "--chan", NULL, NULL, NULL };
    int argc = fc_command_line(argv, fixed, 4, 1, 32);
    return gpu_main(argc, argv);
}
