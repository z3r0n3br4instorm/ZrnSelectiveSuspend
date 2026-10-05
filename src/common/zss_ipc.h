/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ZSS_IPC_H
#define ZSS_IPC_H

#include <stddef.h>

#include "zss_json.h"

#define ZSS_DEFAULT_SOCKET "/run/zss/zssd.sock"
#define ZSS_LINE_MAX 8192

/* Error names that cross the wire and reach the user. */
#define ZSS_ERR_DETACH_BLOCKED "ZSSDetachBlocked"
#define ZSS_ERR_RESUME_NO_DRM "ZSSFailedResumeNoDRM"

struct zss_reader {
    int fd;
    char buf[ZSS_LINE_MAX];
    size_t len;
};

/* $ZSS_SOCKET if set, otherwise ZSS_DEFAULT_SOCKET. */
const char *zss_socket_path(void);

/* Returns a connected fd or -1. */
int zss_connect(const char *path);

void zss_reader_init(struct zss_reader *r, int fd);

/*
 * Reads one message. Returns 1 with *m filled (caller frees with zj_free),
 * 0 on timeout, -1 when the peer is gone. timeout_ms < 0 waits forever.
 * Malformed lines are skipped.
 */
int zss_recv(struct zss_reader *r, struct zj_msg *m, int timeout_ms);

/* Finishes the message, sends it and frees it. Returns 0 or -1. */
int zss_send(int fd, struct zj_out *o);

#endif
