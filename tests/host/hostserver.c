/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * hostserver: a "fake Wii stub" that runs on your PC.
 *
 * It is the same wdp_server loop as the real stub, but on top of BSD sockets
 * instead of libogc. Use it to develop and test the Python client without any
 * emulator:    tests/host/bin/hostserver [port]      (0 or no argument = pick a free port)
 * It prints "listening 127.0.0.1:<port>" and runs until you kill it.
 */
#define _POSIX_C_SOURCE 200809L
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../../common/wdlog.h"
#include "../../common/wdp_server.h"
#include "../../common/wnet.h"
#include "wmem_fake.h"

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void stderr_sink(const wdlog_record *r, void *user)
{
    (void)user;
    fprintf(stderr, "[%-5s %-5s] %s\n", wdlog_level_name(r->level),
            wdlog_subsys_name(r->subsys), r->msg);
}

static wdp_server srv;   /* ~36 KiB: static */

int main(int argc, char **argv)
{
    uint16_t port = argc > 1 ? (uint16_t)atoi(argv[1]) : 0;
    signal(SIGPIPE, SIG_IGN);

    wdlog_init(now_ms);
    wdlog_add_sink(stderr_sink, NULL, WDLOG_DEBUG);

    char ip[16];
    if (wnet_init(ip, sizeof ip) != WNET_OK)
        return 1;

    if (wmem_fake_init() != 0) {
        fprintf(stderr, "cannot allocate fake RAM\n");
        return 1;
    }
    const wdp_conn_config cfg = { .stub_major = 0, .stub_minor = 2, .build = "0.2.0-host",
                                  .mem = &wmem_fake_ops };
    if (wdp_server_start(&srv, port, &cfg) != 0) {
        wdlog_flush();
        return 1;
    }

    /* On this platform a wnet handle IS a file descriptor, so we may ask the OS
     * which port it picked. (The Wii stub has no use for this.) */
    struct sockaddr_in sa;
    socklen_t len = sizeof sa;
    getsockname(srv.listener, (struct sockaddr *)&sa, &len);
    printf("listening %s:%u\n", ip, (unsigned)ntohs(sa.sin_port));
    fflush(stdout);

    for (;;) {
        wdp_server_poll(&srv);
        wdlog_flush();
        struct timespec nap = { 0, 1000000 };   /* 1 ms */
        nanosleep(&nap, NULL);
    }
}
