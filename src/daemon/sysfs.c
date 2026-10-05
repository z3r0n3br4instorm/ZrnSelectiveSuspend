// SPDX-License-Identifier: GPL-2.0
/* Everything zssd learns from, or does through, /sys and /proc. */
#include "zssd.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

int read_text(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t r;

    if (fd < 0)
        return -1;
    r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0)
        return -1;
    buf[r] = '\0';
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

int write_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    ssize_t r;

    if (fd < 0)
        return -1;
    r = write(fd, text, strlen(text));
    close(fd);
    return r < 0 ? -1 : 0;
}

bool path_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

/* Lists every function of the device `pci` names, e.g. the GPU and its audio. */
int pci_functions(const char *pci, char funcs[][16], int max)
{
    DIR *d = opendir("/sys/bus/pci/devices");
    size_t stem = strcspn(pci, ".");
    struct dirent *de;
    int n = 0;

    if (!d)
        return 0;
    while ((de = readdir(d)) && n < max)
        if (!strncmp(de->d_name, pci, stem) && de->d_name[stem] == '.')
            snprintf(funcs[n++], 16, "%.15s", de->d_name);
    closedir(d);
    return n;
}

void pci_driver(const char *pci, char *out, size_t n)
{
    char path[300], link[PATH_MAX];
    ssize_t len;

    out[0] = '\0';
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/driver", pci);
    len = readlink(path, link, sizeof(link) - 1);
    if (len <= 0)
        return;
    link[len] = '\0';
    snprintf(out, n, "%s", strrchr(link, '/') ? strrchr(link, '/') + 1 : link);
}

bool pci_present(const char *pci)
{
    char path[300];

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s", pci);
    return path_exists(path);
}

bool pci_any_driver(const char *pci)
{
    char funcs[ZSSD_MAX_FUNCS][16], drv[64];
    int n = pci_functions(pci, funcs, ZSSD_MAX_FUNCS);

    for (int i = 0; i < n; i++) {
        pci_driver(funcs[i], drv, sizeof(drv));
        if (drv[0])
            return true;
    }
    return false;
}

int pci_remove(const char *pci, char *err)
{
    char funcs[ZSSD_MAX_FUNCS][16], path[300];
    int n = pci_functions(pci, funcs, ZSSD_MAX_FUNCS);

    /* Secondary functions first; function 0 anchors the others. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            bool fn0 = !strcmp(strrchr(funcs[i], '.'), ".0");

            if (fn0 != (pass == 1))
                continue;
            snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/remove", funcs[i]);
            if (write_text(path, "1") < 0 && path_exists(path)) {
                snprintf(err, ZSSD_ERR, "cannot remove %s: %s", funcs[i], strerror(errno));
                return -1;
            }
        }
    }
    for (int tries = 0; tries < 50; tries++) {
        if (pci_functions(pci, funcs, ZSSD_MAX_FUNCS) == 0)
            return 0;
        usleep(100000);
    }
    snprintf(err, ZSSD_ERR, "%s is still present after removal", pci);
    return -1;
}

int pci_rescan(void)
{
    return write_text("/sys/bus/pci/rescan", "1");
}

/* Collects the device numbers of every node user space opens to reach the GPU. */
static int gpu_nodes(const char *pci, dev_t *nodes, int max)
{
    char funcs[ZSSD_MAX_FUNCS][16], path[400], text[64], drv[64];
    int nf = pci_functions(pci, funcs, ZSSD_MAX_FUNCS), n = 0;

    for (int i = 0; i < nf; i++) {
        struct dirent *de;
        DIR *d;

        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/drm", funcs[i]);
        d = opendir(path);
        while (d && (de = readdir(d)) && n < max) {
            unsigned maj, min;

            if (strncmp(de->d_name, "card", 4) && strncmp(de->d_name, "renderD", 7))
                continue;
            snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/drm/%.40s/dev", funcs[i], de->d_name);
            if (read_text(path, text, sizeof(text)) == 0 && sscanf(text, "%u:%u", &maj, &min) == 2)
                nodes[n++] = makedev(maj, min);
        }
        if (d)
            closedir(d);

        /* The proprietary NVIDIA driver has its own character devices. */
        pci_driver(funcs[i], drv, sizeof(drv));
        if (!strcmp(drv, "nvidia") && n < max) {
            char line[256];
            FILE *f;

            snprintf(path, sizeof(path), "/proc/driver/nvidia/gpus/%s/information", funcs[i]);
            f = fopen(path, "r");
            while (f && fgets(line, sizeof(line), f)) {
                unsigned minor;

                if (sscanf(line, "Device Minor: %u", &minor) == 1)
                    nodes[n++] = makedev(195, minor);
            }
            if (f)
                fclose(f);
        }
    }
    return n;
}

int gpu_holders(const char *pci, pid_t *pids, int max)
{
    dev_t nodes[32];
    int nn = gpu_nodes(pci, nodes, 32), n = 0;
    DIR *proc = opendir("/proc");
    struct dirent *pe;

    if (!proc)
        return 0;
    while (nn && (pe = readdir(proc)) && n < max) {
        char dir[64], path[400];
        struct dirent *fe;
        pid_t pid = (pid_t)atoi(pe->d_name);
        bool holds = false;
        DIR *fds;

        if (pid <= 0 || pid == getpid())
            continue;
        snprintf(dir, sizeof(dir), "/proc/%d/fd", pid);
        fds = opendir(dir);
        if (!fds)
            continue;
        while (!holds && (fe = readdir(fds))) {
            struct stat st;

            if (fe->d_name[0] == '.')
                continue;
            snprintf(path, sizeof(path), "%s/%s", dir, fe->d_name);
            if (stat(path, &st) < 0 || !S_ISCHR(st.st_mode))
                continue;
            for (int i = 0; i < nn; i++)
                if (st.st_rdev == nodes[i])
                    holds = true;
        }
        closedir(fds);
        if (holds)
            pids[n++] = pid;
    }
    closedir(proc);
    return n;
}

void pid_comm(pid_t pid, char *out, size_t n)
{
    char path[64];

    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    if (read_text(path, out, n) < 0)
        snprintf(out, n, "?");
}

bool is_display_server(pid_t pid, const char *comm)
{
    static const char *const names[] = {
        "Xorg", "X", "Xwayland", "gnome-shell", "kwin_wayland", "kwin_x11", "sway", "weston",
        "Hyprland", "labwc", "wayfire", "cosmic-comp", "mutter", "river", "niri",
    };

    (void)pid;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(comm, names[i]))
            return true;
    return false;
}

static int cgroup_dir(pid_t pid, char *out, size_t n)
{
    char path[64], line[PATH_MAX];
    FILE *f;
    int rc = -1;

    snprintf(path, sizeof(path), "/proc/%d/cgroup", pid);
    f = fopen(path, "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "0::", 3))
            continue;
        line[strcspn(line, "\n")] = '\0';
        snprintf(out, n, "/sys/fs/cgroup%.*s", (int)n - 16, line + 3);
        rc = 0;
    }
    fclose(f);
    return rc;
}

/*
 * Freezes one process by moving it into a child cgroup of its own and
 * freezing that, so nothing else in its session is touched.
 */
int cgroup_freeze(pid_t pid)
{
    char base[PATH_MAX], path[PATH_MAX + 64], text[32];
    size_t len;

    if (cgroup_dir(pid, base, sizeof(base)) < 0)
        return -1;
    len = strlen(base);
    if (len && base[len - 1] == '/')
        base[len - 1] = '\0';
    snprintf(path, sizeof(path), "%s/zss-parked-%d", base, pid);
    if (mkdir(path, 0755) < 0 && errno != EEXIST)
        return -1;
    snprintf(text, sizeof(text), "%d", pid);
    snprintf(path, sizeof(path), "%s/zss-parked-%d/cgroup.procs", base, pid);
    if (write_text(path, text) < 0)
        goto fail;
    snprintf(path, sizeof(path), "%s/zss-parked-%d/cgroup.freeze", base, pid);
    if (write_text(path, "1") < 0) {
        snprintf(path, sizeof(path), "%s/cgroup.procs", base);
        write_text(path, text);
        goto fail;
    }
    return 0;
fail:
    snprintf(path, sizeof(path), "%s/zss-parked-%d", base, pid);
    rmdir(path);
    return -1;
}

int cgroup_thaw(pid_t pid)
{
    char dir[PATH_MAX], path[PATH_MAX + 128], text[32], *slash;

    if (cgroup_dir(pid, dir, sizeof(dir)) < 0)
        return -1;
    slash = strrchr(dir, '/');
    if (!slash || strncmp(slash + 1, "zss-parked-", 11))
        return 0; /* not frozen by us */
    snprintf(path, sizeof(path), "%s/cgroup.freeze", dir);
    write_text(path, "0");
    *slash = '\0';
    snprintf(text, sizeof(text), "%d", pid);
    snprintf(path, sizeof(path), "%.*s/cgroup.procs", (int)sizeof(dir) - 1, dir[0] ? dir : "/sys/fs/cgroup");
    if (write_text(path, text) < 0)
        return -1;
    *slash = '/';
    rmdir(dir);
    return 0;
}

/* Finds another display-class PCI device that has a driver, to migrate to. */
int other_display_device(const char *except, char *out, size_t n)
{
    DIR *d = opendir("/sys/bus/pci/devices");
    size_t stem = strcspn(except, ".");
    struct dirent *de;
    int rc = -1;

    if (!d)
        return -1;
    while (rc < 0 && (de = readdir(d))) {
        char path[300], text[32], drv[64];

        if (de->d_name[0] == '.' || !strncmp(de->d_name, except, stem))
            continue;
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/class", de->d_name);
        if (read_text(path, text, sizeof(text)) < 0 || (strtoul(text, NULL, 16) >> 16) != 0x03)
            continue;
        pci_driver(de->d_name, drv, sizeof(drv));
        if (!drv[0])
            continue;
        snprintf(out, n, "%s", de->d_name);
        rc = 0;
    }
    closedir(d);
    return rc;
}
