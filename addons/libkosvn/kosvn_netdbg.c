/* KallistiOS Visual Novel Extension (libkosvn)
 *
 * addons/libkosvn/kosvn_netdbg.c
 * Non-blocking UDP LAN telemetry & dcload debug logging.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <kos/dbglog.h>
#include <kos/net.h>
#include <dc/pvr.h>

#include "kosvn_internal.h"

static struct {
    int                sock_fd;
    struct sockaddr_in dest_addr;
    bool               udp_ready;
} netdbg_st = {
    .sock_fd = -1,
    .udp_ready = false
};

static void send_udp_line(const char *msg, size_t len) {
    if(!netdbg_st.udp_ready || netdbg_st.sock_fd < 0 || !msg || len == 0U)
        return;

    sendto(netdbg_st.sock_fd, msg, len, MSG_DONTWAIT,
           (const struct sockaddr *)&netdbg_st.dest_addr,
           sizeof(netdbg_st.dest_addr));
}

int vn_netdbg_init(const char *dest_ip, uint16_t port) {
    vn_netdbg_shutdown();

    if(port == 0U) {
        dbglog(DBG_INFO, "kosvn_netdbg: UDP telemetry disabled (port=0)\n");
        return 0;
    }

    const char *ip_str = (dest_ip && dest_ip[0] != '\0') ? dest_ip : "255.255.255.255";

    memset(&netdbg_st.dest_addr, 0, sizeof(netdbg_st.dest_addr));
    netdbg_st.dest_addr.sin_family = AF_INET;
    netdbg_st.dest_addr.sin_port = htons(port);
    if(inet_aton(ip_str, &netdbg_st.dest_addr.sin_addr) == 0) {
        dbglog(DBG_WARNING, "kosvn_netdbg: invalid dest_ip '%s'\n", ip_str);
        return -1;
    }

    if(!net_default_dev) {
        dbglog(DBG_INFO, "kosvn_netdbg: no LAN adapter detected; console/dcload logging only\n");
        return 0;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(fd < 0) {
        dbglog(DBG_WARNING, "kosvn_netdbg: socket() failed; console/dcload logging only\n");
        return -1;
    }

    int broadcast = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    fcntl(fd, F_SETFL, O_NONBLOCK);

    netdbg_st.sock_fd = fd;
    netdbg_st.udp_ready = true;
    dbglog(DBG_INFO, "kosvn_netdbg: UDP telemetry -> %s:%u\n", ip_str, (unsigned)port);
    return 0;
}

void vn_netdbg_shutdown(void) {
    if(netdbg_st.sock_fd >= 0) {
        close(netdbg_st.sock_fd);
        netdbg_st.sock_fd = -1;
    }
    netdbg_st.udp_ready = false;
}

void vn_netdbg_log(const char *fmt, ...) {
    char msg[256];
    va_list args;

    va_start(args, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    if(n <= 0)
        return;

    size_t len = (size_t)n;
    if(len >= sizeof(msg))
        len = sizeof(msg) - 1U;

    dbglog(DBG_INFO, "[KOSVN] %s\n", msg);
    send_udp_line(msg, len);
}

void vn_netdbg_send_telemetry(uint32_t frame_cnt, float fps, uint32_t script_line, const char *mode_str) {
    struct mallinfo mi = mallinfo();
    size_t vram_free = pvr_mem_available();

    char pkt[256];
    int n = snprintf(pkt, sizeof(pkt),
                     "{\"frame\":%lu,\"fps\":%.1f,\"line\":%lu,\"mode\":\"%s\","
                     "\"ram_used\":%lu,\"ram_free\":%lu,\"vram_free\":%lu}\n",
                     (unsigned long)frame_cnt,
                     (double)fps,
                     (unsigned long)script_line,
                     mode_str ? mode_str : "IDLE",
                     (unsigned long)mi.uordblks,
                     (unsigned long)mi.fordblks,
                     (unsigned long)vram_free);

    if(n <= 0)
        return;

    size_t len = (size_t)n;
    if(len >= sizeof(pkt))
        len = sizeof(pkt) - 1U;

    dbglog(DBG_DEBUG, "kosvn telemetry: %s", pkt);
    send_udp_line(pkt, len);
}
