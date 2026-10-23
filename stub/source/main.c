/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * wiidbg stub, Phase 0: scaffolding.
 *
 * This version does NOT open the network yet. Its job is to prove the whole
 * toolchain works end to end (devkitPPC -> .dol -> Dolphin / Homebrew Channel)
 * and to show the logging system running, before any risky code is added.
 */
#include <gccore.h>
#include <stdio.h>
#include <stdlib.h>
#include <wiiuse/wpad.h>

#include "log_sinks.h"
#include "wdlog.h"
#include "wdp.h"
#include "wiidbg_version.h"

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

int main(void)
{
    video_init();
    WPAD_Init();

    wdlog_init(wii_timebase);
    wdlog_add_sink(sink_console, NULL, WDLOG_INFO);

    WDLOG_I(WDLOG_SUB_CORE, "wiidbg stub %s (WDP wire v%u)", WIIDBG_VERSION,
            (unsigned)WDP_VERSION);
    WDLOG_I(WDLOG_SUB_CORE, "Phase 0: scaffolding only, network not started");
    WDLOG_I(WDLOG_SUB_CORE, "Press HOME to exit");

    for (;;) {
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
            break;

        /* The main loop is the ONE place that drains the log into sinks. */
        wdlog_flush();
        VIDEO_WaitVSync();   /* pace the loop to the display refresh */
    }

    WDLOG_I(WDLOG_SUB_CORE, "exiting");
    wdlog_flush();
    return 0;
}
