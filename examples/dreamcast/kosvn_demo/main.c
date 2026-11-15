/* KallistiOS Visual Novel Extension Demo
 *
 * examples/dreamcast/kosvn_demo/main.c
 */

#include <kos.h>
#include <kosvn/kosvn.h>

KOS_INIT_FLAGS(INIT_VN_DEFAULT);

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    vn_config_t cfg = {
        .title           = "KallistiOS-VN Sample",
        .app_id          = "KOSVN_DEMO",
        .net_debug_port  = 47890,
        .net_debug_ip    = "255.255.255.255",
        .text_speed_ms   = 24,
        .auto_advance_ms = 1800
    };

    if(vn_init(&cfg) < 0)
        return -1;

    vn_vmu_draw_logo("KOSVN");
    vn_run_script("/rd/scenario.txt");
    vn_shutdown();
    return 0;
}
