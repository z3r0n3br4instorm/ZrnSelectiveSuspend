// SPDX-License-Identifier: GPL-2.0-only
#include "zss_ipc.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

const char *zss_socket_path(void)
{
    const char *p = getenv("ZSS_SOCKET");

    return p && *p ? p : ZSS_DEFAULT_SOCKET;
}

int zss_connect(const char *path)
{
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    int fd;

    if (strlen(path) >= sizeof(sa.sun_path))
        return -1;
    strcpy(sa.sun_path, path);
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void zss_reader_init(struct zss_reader *r, int fd)
{
    r->fd = fd;
    r->len = 0;
}

int zss_recv(struct zss_reader *r, struct zj_msg *m, int timeout_ms)
{
    for (;;) {
        char *nl = memchr(r->buf, '\n', r->len);
        struct pollfd pfd = { .fd = r->fd, .events = POLLIN };
        ssize_t n;
        int rc;

        if (nl) {
            size_t used = (size_t)(nl - r->buf) + 1;

            *nl = '\0';
            rc = zj_parse(r->buf, m);
            memmove(r->buf, r->buf + used, r->len - used);
            r->len -= used;
            if (rc == 0)
                return 1;
            continue;
        }
        if (r->len == sizeof(r->buf) - 1)
            r->len = 0; /* oversized line: drop it */

        rc = poll(&pfd, 1, timeout_ms);
        if (rc == 0)
            return 0;
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        n = read(r->fd, r->buf + r->len, sizeof(r->buf) - 1 - r->len);
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (n <= 0)
            return -1;
        r->len += (size_t)n;
    }
}

int zss_send(int fd, struct zj_out *o)
{
    char *line = zj_end(o);
    size_t len = o->len, off = 0;
    int rc = 0;

    while (off < len) {
        ssize_t n = send(fd, line + off, len - off, MSG_NOSIGNAL);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            rc = -1;
            break;
        }
        off += (size_t)n;
    }
    free(line);
    return rc;
}
