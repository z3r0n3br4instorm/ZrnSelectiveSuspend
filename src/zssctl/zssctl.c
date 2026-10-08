// SPDX-License-Identifier: GPL-2.0-only
/* zssctl: command-line client for zssd. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zss_ipc.h"

static int usage(void)
{
    fputs("usage: zssctl status [PCI]\n"
          "       zssctl detach PCI [--to PCI|software]\n"
          "       zssctl attach PCI\n"
          "       zssctl off PCI [--to PCI|software] [--console]\n"
          "       zssctl on PCI [--stay]\n"
          "       zssctl resume PID\n"
          "       zssctl monitor\n",
          stderr);
    return 2;
}

/* Prints replies until the final "result". Returns the exit status. */
static int show_result(struct zss_reader *rd)
{
    struct zj_msg m;

    while (zss_recv(rd, &m, -1) == 1) {
        if (zj_is(&m, "progress")) {
            printf("%s\n", zj_str(&m, "text", ""));
            fflush(stdout);
        } else if (zj_is(&m, "blocker")) {
            printf("%s %s (pid %lld, %s): %s\n", strstr(zj_str(&m, "class", ""), "ignored") ? "note:" : "blocked by",
                   zj_str(&m, "name", "?"), zj_int(&m, "pid", 0), zj_str(&m, "class", "?"),
                   zj_str(&m, "reason", ""));
        } else if (zj_is(&m, "result")) {
            bool ok = zj_bool(&m, "ok", false);
            const char *error = zj_str(&m, "error", "");

            if (zj_str(&m, "gpu", "")[0])
                printf("%s: %s%s\n", zj_str(&m, "gpu", ""), zj_str(&m, "state", ""),
                       zj_bool(&m, "dry_run", false) ? " (dry run)" : "");
            if (ok)
                printf("%s\n", zj_str(&m, "message", "ok"));
            else
                fprintf(stderr, "error: %s%s%s\n", error, error[0] ? ": " : "", zj_str(&m, "message", ""));
            zj_free(&m);
            return ok ? 0 : 1;
        }
        zj_free(&m);
    }
    fputs("error: lost connection to zssd\n", stderr);
    return 1;
}

static int show_status(struct zss_reader *rd)
{
    struct zj_msg m;

    while (zss_recv(rd, &m, -1) == 1) {
        if (zj_is(&m, "gpu")) {
            printf("%s  state=%s%s  backend=%s  removal=%s  wake=%s", zj_str(&m, "gpu", ""),
                   zj_str(&m, "state", ""), zj_bool(&m, "dry_run", false) ? " (dry run)" : "",
                   zj_str(&m, "backend", ""),
                   zj_bool(&m, "removal_supported", false) ? "supported" : "not supported",
                   zj_bool(&m, "wake_support", false) ? "yes" : "no");
            if (zj_bool(&m, "driver_frozen", false))
                printf("  driver=frozen");
            printf("  iommu=%s", zj_bool(&m, "iommu", false) ? "yes" : "no");
            if (zj_bool(&m, "serving", false))
                printf("  (on for a moment for the display server)");
            if (zj_int(&m, "served", 0))
                printf("  served=%lld", zj_int(&m, "served", 0));
            if (zj_int(&m, "waiting", 0))
                printf("  waiting=%lld", zj_int(&m, "waiting", 0));
            if (zj_int(&m, "wakes", 0))
                printf("  woken=%lld", zj_int(&m, "wakes", 0));
            if (zj_int(&m, "idle_wait", 0))
                printf("  idle-off=%llds", zj_int(&m, "idle_wait", 0));
            printf("\n");
        } else if (zj_is(&m, "client")) {
            const char *reason = zj_str(&m, "reason", "");

            printf("    pid %-7lld %-16s %s%s%s%s%s\n", zj_int(&m, "pid", 0), zj_str(&m, "name", "?"),
                   zj_str(&m, "class", "?"), zj_int(&m, "parked", 0) ? " parked" : "",
                   zj_bool(&m, "away", false) && !zj_int(&m, "parked", 0) ? " migrated-away" : "",
                   reason[0] ? ": " : "", reason);
        } else if (zj_is(&m, "end")) {
            zj_free(&m);
            return 0;
        }
        zj_free(&m);
    }
    return 1;
}

int main(int argc, char **argv)
{
    struct zss_reader rd;
    struct zj_out o;
    struct zj_msg m;
    int fd;

    if (argc < 2)
        return usage();
    fd = zss_connect(zss_socket_path());
    if (fd < 0) {
        fprintf(stderr, "error: cannot reach zssd at %s\n", zss_socket_path());
        return 1;
    }
    zss_reader_init(&rd, fd);

    if (!strcmp(argv[1], "status")) {
        zj_begin(&o, "status");
        if (argc > 2)
            zj_add_str(&o, "gpu", argv[2]);
        zss_send(fd, &o);
        return show_status(&rd);
    }
    if (!strcmp(argv[1], "detach") && argc >= 3) {
        zj_begin(&o, "detach");
        zj_add_str(&o, "gpu", argv[2]);
        if (argc >= 5 && !strcmp(argv[3], "--to"))
            zj_add_str(&o, "to", argv[4]);
        zss_send(fd, &o);
        return show_result(&rd);
    }
    if (!strcmp(argv[1], "off") && argc >= 3) {
        zj_begin(&o, "off");
        zj_add_str(&o, "gpu", argv[2]);
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--to") && i + 1 < argc)
                zj_add_str(&o, "to", argv[++i]);
            else if (!strcmp(argv[i], "--console"))
                zj_add_bool(&o, "console", true);
            else
                return usage();
        }
        zss_send(fd, &o);
        return show_result(&rd);
    }
    if (!strcmp(argv[1], "on") && argc >= 3) {
        zj_begin(&o, "on");
        zj_add_str(&o, "gpu", argv[2]);
        /* Programs come back to the device unless asked to stay where they are. */
        zj_add_bool(&o, "return", !(argc > 3 && !strcmp(argv[3], "--stay")));
        zss_send(fd, &o);
        return show_result(&rd);
    }
    if (!strcmp(argv[1], "attach") && argc >= 3) {
        zj_begin(&o, "attach");
        zj_add_str(&o, "gpu", argv[2]);
        zss_send(fd, &o);
        return show_result(&rd);
    }
    if (!strcmp(argv[1], "resume") && argc >= 3) {
        zj_begin(&o, "resume");
        zj_add_int(&o, "pid", atoll(argv[2]));
        zss_send(fd, &o);
        return show_result(&rd);
    }
    if (!strcmp(argv[1], "monitor")) {
        zj_begin(&o, "subscribe");
        zss_send(fd, &o);
        setvbuf(stdout, NULL, _IOLBF, 0);
        while (zss_recv(&rd, &m, -1) == 1) {
            if (zj_is(&m, "event"))
                printf("%s %s -> %s\n", zj_str(&m, "gpu", ""), zj_str(&m, "old", ""), zj_str(&m, "new", ""));
            zj_free(&m);
        }
        return 0;
    }
    return usage();
}
