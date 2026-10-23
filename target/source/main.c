/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * wiidbg test target: a tiny program whose behaviour we fully control, so we
 * can check the debugger against known ground truth.
 *
 * It owns one counter and one function that increments it. Later phases will:
 *   Phase 2: read and reset `counter` from the PC.
 *   Phase 3: put a data watchpoint on `counter` and expect the hit to report
 *            the address of the `stw` inside bump_counter().
 *   Phase 4: put an execution breakpoint on bump_counter().
 */
#include <gccore.h>
#include <stdio.h>
#include <stdlib.h>
#include <wiiuse/wpad.h>

static void *xfb;
static GXRModeObj *rmode;

/* `volatile`: forbid the compiler from caching it in a register or deleting
 * the store. A debugger test needs the memory write to really happen. */
volatile u32 counter __attribute__((aligned(32)));

/* noinline: keep it a real function with a real address we can print and
 * look up with `powerpc-eabi-objdump -d`. */
__attribute__((noinline)) void bump_counter(void)
{
    counter = counter + 1;
}

static void video_init(void)
{
    VIDEO_Init();
    rmode = VIDEO_GetPreferredMode(NULL);
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

    printf("\x1b[2;0H");
    printf("wiidbg test target\n\n");
    printf("counter       @ 0x%08x  (32-byte aligned)\n", (unsigned)&counter);
    printf("bump_counter  @ 0x%08x\n\n", (unsigned)&bump_counter);
    printf("Press HOME to exit\n");

    unsigned frames = 0;
    for (;;) {
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
            break;

        bump_counter();

        if ((frames++ % 15) == 0)   /* redraw ~4x per second at 60 Hz */
            printf("\x1b[10;0Hcounter = %u        ", (unsigned)counter);

        VIDEO_WaitVSync();
    }
    return 0;
}
