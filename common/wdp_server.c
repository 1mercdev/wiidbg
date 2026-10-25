/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * wiidbg - Wii debugger
 * Copyright (C) 2026 1mercdev
 *
 * wdp_server implementation. Read wdp_server.h first.
 */
#include "wdp_server.h"

#include <string.h>

#include "wdlog.h"
#include "wnet.h"

static void drop_client(wdp_server *s, const char *why)
{
    if (s->client >= 0) {
        wnet_close(s->client);
        s->client = -1;
        WDLOG_I(WDLOG_SUB_NET, "client dropped: %s", why);
    }
    s->rx_off = s->rx_len = 0;
}

static void adopt_client(wdp_server *s, int fd)
{
    if (s->client >= 0)
        drop_client(s, "replaced by a newer connection");
    s->client = fd;
    s->rx_off = s->rx_len = 0;
    wdp_conn_init(&s->conn, &s->cfg);   /* cfg was validated in start() */
    s->sessions++;
    WDLOG_I(WDLOG_SUB_NET, "client connected (session %u)", s->sessions);
}

int wdp_server_start(wdp_server *s, uint16_t port, const wdp_conn_config *cfg)
{
    if (!s || !cfg)
        return WDP_E_ARG;
    /* Validate the config once, here, so later wdp_conn_init() calls cannot fail. */
    int rc = wdp_conn_init(&s->conn, cfg);
    if (rc < 0)
        return rc;

    /* Keep our OWN copy of the build string: the caller's pointer might refer
     * to a stack buffer that is gone by the time a client connects. (The
     * length was already validated by wdp_conn_init() above.) */
    memset(s->build, 0, sizeof s->build);
    memcpy(s->build, cfg->build, strlen(cfg->build));
    s->cfg = *cfg;
    s->cfg.build = s->build;
    s->active = 0;
    s->client = -1;
    s->listener = -1;
    s->rx_off = s->rx_len = 0;
    s->sessions = 0;

    if (wnet_listen(port, &s->listener) != WNET_OK) {
        WDLOG_E(WDLOG_SUB_NET, "cannot listen on port %u (error %d)",
                (unsigned)port, wnet_last_error());
        s->listener = -1;
        return WDP_E_ARG;
    }
    s->active = 1;
    WDLOG_I(WDLOG_SUB_NET, "listening on port %u", (unsigned)port);
    return 0;
}

void wdp_server_stop(wdp_server *s)
{
    if (!s->active)
        return;
    drop_client(s, "server stopping");
    wnet_close(s->listener);
    s->listener = -1;
    s->active = 0;
}

void wdp_server_poll(wdp_server *s)
{
    if (!s->active)
        return;

    /* A pending connection always wins over the current one (see header). */
    int fd;
    int rc = wnet_accept(s->listener, &fd);
    if (rc == WNET_OK)
        adopt_client(s, fd);
    else if (rc < 0)
        WDLOG_W(WDLOG_SUB_NET, "accept failed (error %d)", wnet_last_error());

    if (s->client < 0)
        return;

    for (unsigned step = 0; step < WDP_SERVER_MAX_STEPS; step++) {
        /* 1. Send anything the protocol layer has queued. */
        size_t out_len;
        const uint8_t *out = wdp_conn_out(&s->conn, &out_len);
        if (out_len > 0) {
            size_t sent = 0;
            rc = wnet_send(s->client, out, out_len, &sent);
            if (rc == WNET_OK) {
                wdp_conn_out_consume(&s->conn, sent);
                continue;                 /* maybe more to send, or to read */
            }
            if (rc == WNET_AGAIN)
                return;                   /* socket buffer full: next poll */
            WDLOG_W(WDLOG_SUB_NET, "send failed (error %d)", wnet_last_error());
            drop_client(s, "send error");
            return;
        }

        /* Output is drained. If the protocol layer asked to hang up, do it now,
         * AFTER the last reply went out. */
        if (wdp_conn_is_closing(&s->conn)) {
            drop_client(s, "closed after final reply");
            return;
        }

        /* 2. Need more input? */
        if (s->rx_off == s->rx_len) {
            size_t got = 0;
            rc = wnet_recv(s->client, s->rx, sizeof s->rx, &got);
            if (rc == WNET_OK) {
                s->rx_off = 0;
                s->rx_len = got;
            } else if (rc == WNET_AGAIN) {
                return;                   /* nothing to read: next poll */
            } else if (rc == WNET_CLOSED) {
                drop_client(s, "peer closed the connection");
                return;
            } else {
                WDLOG_W(WDLOG_SUB_NET, "recv failed (error %d)", wnet_last_error());
                drop_client(s, "receive error");
                return;
            }
        }

        /* 3. Hand bytes to the protocol state machine. */
        size_t used = 0;
        rc = wdp_conn_feed(&s->conn, s->rx + s->rx_off, s->rx_len - s->rx_off, &used);
        s->rx_off += used;
        if (rc < 0) {
            WDLOG_W(WDLOG_SUB_PROTO, "protocol error: %s", wdp_conn_strerror(rc));
            drop_client(s, "protocol error");
            return;
        }
        /* rc == WDP_CONN_CLOSE falls through: the next step drains and closes. */
    }
}
