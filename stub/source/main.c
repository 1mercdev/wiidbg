/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * wiidbg stub, Phase 1: network transport.
 *
 * Brings up the network, listens on TCP port 4242 and answers the WDP commands
 * HELLO and PING (see docs/protocol.md). No memory access yet: that is Phase 2.
 */
#include <gccore.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wiiuse/wpad.h>

#include "log_sinks.h"
#include "wdlog.h"
#include "wdp.h"
#include "wdp_server.h"
#include "wiidbg_version.h"
#include "wnet.h"

static void *xfb;           /* external framebuffer: memory the video chip scans out */
static GXRModeObj *rmode;   /* video mode (resolution, interlacing) */

static void video_init(void)
{
    VIDEO_Init();
    rmode = VIDEO_GetPreferredMode(NULL);

    /* Allocate the framebuffer and use its uncached mirror (K1 = 0xC0000000
     * range) so what we draw is seen by the video chip without cache flushes. */
    xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));

    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight,
                 rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();
}

/* Static, not on the stack: it holds two 16 KiB protocol buffers. */
static wdp_server server;

int main(void)
{
    video_init();
    WPAD_Init();

    wdlog_init(wii_timebase);
    wdlog_add_sink(sink_console, NULL, WDLOG_INFO);

    WDLOG_I(WDLOG_SUB_CORE, "wiidbg stub %s (WDP wire v%u)", WIIDBG_VERSION,
            (unsigned)WDP_VERSION);

    /* wnet_init() blocks for seconds while DHCP runs, so flush the log first or
     * the screen would look frozen with nothing on it. */
    WDLOG_I(WDLOG_SUB_NET, "starting network (DHCP)...");
    wdlog_flush();

    char ip[16] = "?";
    if (wnet_init(ip, sizeof ip) != WNET_OK) {
        WDLOG_E(WDLOG_SUB_NET, "network init failed (error %d)", wnet_last_error());
    } else {
        WDLOG_I(WDLOG_SUB_NET, "IP address: %s", ip);
        const wdp_conn_config cfg = { 0, 1, 0, WIIDBG_VERSION };
        if (wdp_server_start(&server, WDP_DEFAULT_PORT, &cfg) == 0)
            WDLOG_I(WDLOG_SUB_NET, "try: python -m wiidbg %s hello", ip);
    }
    WDLOG_I(WDLOG_SUB_CORE, "Press HOME to exit");

    unsigned tick = 0;
    for (;;) {
        /* The network is polled about 1000 times a second. The controller is
         * read about 60 times a second (every 16th pass): reading it faster
         * gains nothing. */
        if ((tick++ & 15) == 0) {
            WPAD_ScanPads();
            if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
                break;
        }

        wdp_server_poll(&server);   /* never blocks; does nothing if not started */
        wdlog_flush();              /* the ONE place logs reach the screen */
        usleep(1000);               /* 1 ms: do not spin the CPU flat out */
    }

    WDLOG_I(WDLOG_SUB_CORE, "exiting");
    wdp_server_stop(&server);
    wdlog_flush();
    return 0;
}
