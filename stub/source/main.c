/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * wiidbg stub, Phase 1: network transport.
 *
 * Brings up the network, listens on TCP port 4242 and answers the WDP commands
 * HELLO, PING, MEM_READ and MEM_WRITE (see docs/protocol.md).
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
#include "wmem_wii.h"
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

/* SELF-TEST AREA. Until the stub can live next to another program (Phase 6) the
 * stub itself is the thing being inspected. Read these addresses from the PC:
 *   - the counter climbs about 1000 times a second: read it twice and see it
 *     change; write 0 to it and watch it restart from zero;
 *   - the scratch buffer is 64 KiB, bigger than one protocol frame, so reading
 *     and writing it exercises the client's chunking on the real thing.
 * `volatile` stops the compiler from optimising the increments away, and the
 * 32-byte alignment puts the counter alone in one cache line (the cache works in
 * 32-byte lines), which makes cache behaviour easy to reason about later. */
volatile u32 selftest_counter __attribute__((aligned(32)));
static u8 selftest_scratch[64 * 1024] __attribute__((aligned(32)));

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
        const wdp_conn_config cfg = { .stub_major = 0, .stub_minor = 2, .build = WIIDBG_VERSION,
                                      .mem = &wmem_wii_ops };
        if (wdp_server_start(&server, WDP_DEFAULT_PORT, &cfg) == 0)
            WDLOG_I(WDLOG_SUB_NET, "try: python -m wiidbg %s hello", ip);
    }
    WDLOG_I(WDLOG_SUB_MEM, "counter @ 0x%08x", (unsigned)&selftest_counter);
    WDLOG_I(WDLOG_SUB_MEM, "scratch @ 0x%08x (64 KiB)", (unsigned)selftest_scratch);
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

        selftest_counter = selftest_counter + 1;   /* see the SELF-TEST AREA above */

        wdp_server_poll(&server);   /* never blocks; does nothing if not started */
        wdlog_flush();              /* the ONE place logs reach the screen */
        usleep(1000);               /* 1 ms: do not spin the CPU flat out */
    }

    WDLOG_I(WDLOG_SUB_CORE, "exiting");
    wdp_server_stop(&server);
    wdlog_flush();
    return 0;
}
