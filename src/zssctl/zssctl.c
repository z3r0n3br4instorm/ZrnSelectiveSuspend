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
          "       zssctl lend PCI [--with-group] [--to PCI|software]   hand the card to a virtual machine\n"
          "       zssctl lend --check PCI [--with-group] [--json]      say what stands in the way, changing nothing\n"
          "           --with-group: devices sharing the card's isolation group go with it (no host driver meanwhile)\n"
          "       zssctl reclaim PCI [--stay]             take a lent card back\n"
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
        } else if (zj_is(&m, "obstacle")) {
            printf("in the way: %s\n    remedy: %s\n", zj_str(&m, "reason", ""), zj_str(&m, "remedy", ""));
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

static void json_str(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            printf("\\%c", *s);
        else if ((unsigned char)*s < 0x20)
            printf("\\u%04x", *s);
        else
            putchar(*s);
    }
    putchar('"');
}

/* The answer to "lend --check": as text, or as one JSON document with the same content. */
static int show_check(struct zss_reader *rd, bool json)
{
    struct zj_msg m;
    int nfn = 0, nobs = 0, naff = 0, section = 0;

    /* The daemon sends functions, then affected programs, then obstacles, then the result. */
    while (zss_recv(rd, &m, -1) == 1) {
        if (zj_is(&m, "function")) {
            if (json) {
                printf("%s{\"pci\":", nfn ? "," : "{\"functions\":[");
                json_str(zj_str(&m, "pci", ""));
                printf(",\"driver\":");
                json_str(zj_str(&m, "driver", ""));
                printf(",\"companion\":%s}", zj_bool(&m, "companion", false) ? "true" : "false");
            } else {
                printf("%s  %s  driver %s%s\n", nfn ? "" : "Functions that would be handed over:\n", zj_str(&m, "pci", ""),
                       zj_str(&m, "driver", "")[0] ? zj_str(&m, "driver", "") : "(none)",
                       zj_bool(&m, "companion", false) ? "   (shares the card's isolation group: no host driver while the card is lent)" : "");
            }
            nfn++;
        } else if (zj_is(&m, "affected") || zj_is(&m, "obstacle") || zj_is(&m, "result")) {
            int want = zj_is(&m, "affected") ? 1 : zj_is(&m, "obstacle") ? 2 : 3;

            if (json) {
                /* Close what is open and open what comes next, so every list is present even when empty. */
                if (section == 0)
                    printf("%s],\"applications\":[", nfn ? "" : "{\"functions\":[");
                if (section < 2 && want >= 2)
                    printf("],\"obstacles\":[");
                if (want == 3)
                    printf("],");
            }
            if (want == 1) {
                if (json) {
                    printf("%s{\"pid\":%lld,\"name\":", naff ? "," : "", zj_int(&m, "pid", 0));
                    json_str(zj_str(&m, "name", ""));
                    printf(",\"what\":");
                    json_str(zj_str(&m, "what", ""));
                    printf("}");
                } else {
                    if (zj_int(&m, "pid", 0))
                        printf("%s  %s (pid %lld): %s\n", naff ? "" : "Also affected:\n", zj_str(&m, "name", ""),
                               zj_int(&m, "pid", 0), zj_str(&m, "what", ""));
                    else
                        printf("%s  %s: %s\n", naff ? "" : "Also affected:\n", zj_str(&m, "name", ""), zj_str(&m, "what", ""));
                }
                naff++;
            } else if (want == 2) {
                if (json) {
                    printf("%s{\"reason\":", nobs ? "," : "");
                    json_str(zj_str(&m, "reason", ""));
                    printf(",\"remedy\":");
                    json_str(zj_str(&m, "remedy", ""));
                    printf("}");
                } else {
                    printf("%s  - %s\n      remedy: %s\n", nobs ? "" : "In the way:\n", zj_str(&m, "reason", ""),
                           zj_str(&m, "remedy", ""));
                }
                nobs++;
            } else {
                bool ok = zj_bool(&m, "ok", false);

                if (json) {
                    printf("\"can_lend\":%s,\"state\":", ok ? "true" : "false");
                    json_str(zj_str(&m, "state", ""));
                    printf(",\"message\":");
                    json_str(zj_str(&m, "message", ""));
                    printf("}\n");
                } else {
                    printf("%s\n", zj_str(&m, "message", ""));
                }
                zj_free(&m);
                return ok ? 0 : 1;
            }
            section = want;
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
    if (!strcmp(argv[1], "lend") && argc >= 3) {
        bool check = false, json = false, group = false;
        const char *pci = NULL, *to = NULL;

        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--check"))
                check = true;
            else if (!strcmp(argv[i], "--json"))
                json = true;
            else if (!strcmp(argv[i], "--with-group"))
                group = true;
            else if (!strcmp(argv[i], "--to") && i + 1 < argc)
                to = argv[++i];
            else if (argv[i][0] != '-' && !pci)
                pci = argv[i];
            else
                return usage();
        }
        if (!pci)
            return usage();
        zj_begin(&o, check ? "lend-check" : "lend");
        zj_add_str(&o, "gpu", pci);
        if (group)
            zj_add_bool(&o, "with_group", true);
        if (to)
            zj_add_str(&o, "to", to);
        zss_send(fd, &o);
        return check ? show_check(&rd, json) : show_result(&rd);
    }
    if (!strcmp(argv[1], "reclaim") && argc >= 3) {
        zj_begin(&o, "reclaim");
        zj_add_str(&o, "gpu", argv[2]);
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
