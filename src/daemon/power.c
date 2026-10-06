// SPDX-License-Identifier: GPL-2.0-only
/*
 * What zssd needs around a power-off under a running desktop: who may keep
 * the device open, where wake requests show up, whether the device is
 * driving a display, the services to stop, the marker that lets a crashed
 * daemon's successor put things back, and the text console for progress.
 */
#include "zssd.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/vt.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/sysmacros.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define NVIDIA_WAKE_POLL "/proc/driver/nvidia/zss_wake"
#define NVIDIA_WAKE_COUNT "/sys/module/nvidia/parameters/zss_wake_requests"

/* ---- holder rules -------------------------------------------------------------- */

enum holder_verdict holder_verdict(const struct holder_facts *f, const char **why)
{
    *why = "";
    if (f->registered) {
        if (f->migratable)
            return HV_MIGRATE;
        /* Cannot be moved, but can wait where it is, like any process outside the layer. */
        if (f->freezable)
            return HV_FREEZE;
        *why = "it uses something the ZSS layer cannot move";
        return HV_BLOCK;
    }
    if (f->listed_service)
        return HV_STOP;
    if (f->display_server) {
        /* On the console nobody is looking at the desktop, so it may wait for the device. */
        if (f->wake_support || f->console)
            return HV_ALLOW;
        *why = "a display server is using this GPU and its driver cannot wake the device on demand";
        return HV_BLOCK;
    }
    /*
     * A process the layer cannot move. With a device that stays in place it
     * can simply be stopped until the device is back, as a system sleep would
     * do to it; left running, it would wake the device every time it looked.
     */
    if (f->freezable)
        return HV_FREEZE;
    *why = "not started under the ZSS layer";
    return HV_BLOCK;
}

/* ---- wake requests ---------------------------------------------------------------- */

const char *gpu_wake_path(struct gpu *g)
{
    if (zssd_cfg.wake_file && zssd_cfg.wake_file[0])
        return zssd_cfg.wake_file;
    if (g->backend && g->backend->wake_file)
        return g->backend->wake_file(g);
    if (strcmp(g->driver, "nvidia"))
        return "";
    return path_exists(NVIDIA_WAKE_POLL) ? NVIDIA_WAKE_POLL : NVIDIA_WAKE_COUNT;
}

bool gpu_wake_supported(struct gpu *g)
{
    const char *path = gpu_wake_path(g);

    return path[0] && path_exists(path);
}

long gpu_wake_count(struct gpu *g)
{
    char text[32];

    if (read_text(gpu_wake_path(g), text, sizeof(text)) < 0)
        return -1;
    return strtol(text, NULL, 10);
}

/*
 * The request count, and who is asleep on the suspended driver. *nwaiters is
 * -1 when the driver does not say (an older patch, or more callers than it
 * can list), which has to be read as "anyone, the display server included".
 */
long gpu_wake_read(struct gpu *g, pid_t *waiters, int max, int *nwaiters)
{
    char text[1024], *save = NULL, *line;
    long count, total = -1;
    ssize_t len;
    int n = 0;

    int fd = g->wake_fd;

    *nwaiters = -1;
    /* Reading through the watched descriptor is also what re-arms its poll(). */
    if (fd < 0)
        fd = open(gpu_wake_path(g), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    len = pread(fd, text, sizeof(text) - 1, 0);
    if (fd != g->wake_fd)
        close(fd);
    if (len < 0)
        return -1;
    text[len] = '\0';
    line = strtok_r(text, "\n", &save);
    if (!line)
        return -1;
    count = strtol(line, NULL, 10);
    line = strtok_r(NULL, "\n", &save);
    if (!line || sscanf(line, "waiting %ld", &total) != 1)
        return count;
    while ((line = strtok_r(NULL, "\n", &save)) != NULL) {
        pid_t pid = (pid_t)atoi(line);
        bool known = false;

        for (int i = 0; i < n; i++)
            known |= waiters[i] == pid;
        if (pid > 0 && !known && n < max)
            waiters[n++] = pid;
        total--;
    }
    if (total <= 0)
        *nwaiters = n;
    return count;
}

/* ---- hiding a device that is off ----------------------------------------------------- */

/*
 * A device switched off on request is hidden from programs that start
 * afterwards, the way an unplugged one would be: an empty file is mounted
 * over its device nodes and over the files that tell the graphics loaders
 * about its driver. Without this every such program would reach the suspended
 * driver, directly or through the display server, and either wake the device
 * or hang on it. Programs that already hold the device are dealt with before
 * this (moved, frozen or stopped); the display server keeps its handles.
 *
 * The mounts name the device in their source ("<runtime>/<pci>.hidden"), so
 * that they can be found and undone from /proc/self/mountinfo by a later
 * process, and they do not survive a reboot.
 */
static void hidden_source(const struct gpu *g, char *out, size_t n)
{
    snprintf(out, n, "%s/%s.hidden", zssd_cfg.runtime_dir, g->pci);
}

static bool hide_path(const char *source, const char *path, char *what, size_t n)
{
    struct stat st;

    if (lstat(path, &st) < 0 || S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
        return false;
    if (mount(source, path, NULL, MS_BIND, NULL) < 0)
        return false;
    if (what)
        snprintf(what + strlen(what), n - strlen(what), "%s%s", what[0] ? ", " : "", path);
    return true;
}

static bool mentions(const char *path, const char *word)
{
    char text[2048] = "";
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t len;

    if (fd < 0)
        return false;
    len = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (len <= 0)
        return false;
    text[len] = '\0';
    return strstr(text, word) != NULL;
}

/*
 * The NVIDIA driver's own files under /proc put a reader to sleep while it is
 * suspended, and its tools read them before anything else. The two that drive
 * the suspend, and the wake file, stay.
 */
static int hide_driver_proc(const char *source, const char *dirpath, int depth)
{
    DIR *dir = opendir(dirpath);
    struct dirent *de;
    int count = 0;

    while (dir && (de = readdir(dir))) {
        char path[600];
        struct stat st;

        if (de->d_name[0] == '.' || !strncmp(de->d_name, "suspend", 7) || !strcmp(de->d_name, "zss_wake"))
            continue;
        snprintf(path, sizeof(path), "%s/%s", dirpath, de->d_name);
        if (lstat(path, &st) < 0)
            continue;
        if (S_ISDIR(st.st_mode) && depth < 3)
            count += hide_driver_proc(source, path, depth + 1);
        else if (S_ISREG(st.st_mode))
            count += hide_path(source, path, NULL, 0);
    }
    if (dir)
        closedir(dir);
    return count;
}

int gpu_hide(struct gpu *g, char *what, size_t n)
{
    static const char *const node_dirs[] = { "/dev", "/dev/dri" };
    static const char *const manifest_dirs[] = {
        "/usr/share/vulkan/icd.d", "/etc/vulkan/icd.d", "/usr/local/share/vulkan/icd.d",
        "/usr/share/glvnd/egl_vendor.d", "/etc/glvnd/egl_vendor.d",
    };
    const char *mode = zssd_cfg.hide_while_off ? zssd_cfg.hide_while_off : "auto";
    char source[400], path[600], list[1024], *save = NULL;
    bool vendor = !strcmp(g->driver, "nvidia");
    int count = 0, fd;

    what[0] = '\0';
    if (!strcmp(mode, "no"))
        return 0;
    hidden_source(g, source, sizeof(source));
    fd = open(source, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0444);
    if (fd < 0)
        return 0;
    close(fd);

    for (size_t d = 0; d < sizeof(node_dirs) / sizeof(node_dirs[0]); d++) {
        DIR *dir = opendir(node_dirs[d]);
        struct dirent *de;

        while (dir && (de = readdir(dir))) {
            struct stat st;
            bool mine = false;

            snprintf(path, sizeof(path), "%s/%s", node_dirs[d], de->d_name);
            if (lstat(path, &st) < 0 || !S_ISCHR(st.st_mode))
                continue;
            for (int i = 0; i < g->nnodes; i++)
                mine |= g->nodes[i] == st.st_rdev;
            /* The proprietary NVIDIA driver's control nodes lead to the same suspended driver. */
            mine |= vendor && major(st.st_rdev) == 195;
            if (mine)
                count += hide_path(source, path, what, n);
        }
        if (dir)
            closedir(dir);
    }
    /* A vendor driver has loader files of its own; Mesa's are shared with other devices and stay. */
    for (size_t d = 0; vendor && d < sizeof(manifest_dirs) / sizeof(manifest_dirs[0]); d++) {
        DIR *dir = opendir(manifest_dirs[d]);
        struct dirent *de;

        while (dir && (de = readdir(dir))) {
            snprintf(path, sizeof(path), "%s/%s", manifest_dirs[d], de->d_name);
            if (strstr(de->d_name, ".json") && mentions(path, g->driver))
                count += hide_path(source, path, what, n);
        }
        if (dir)
            closedir(dir);
    }
    if (vendor) {
        int files = hide_driver_proc(source, "/proc/driver/nvidia", 0);

        if (files)
            snprintf(what + strlen(what), n - strlen(what), "%s%d files under /proc/driver/nvidia", what[0] ? ", " : "", files);
        count += files;
    }
    if (strcmp(mode, "auto")) {
        snprintf(list, sizeof(list), "%s", mode);
        for (char *tok = strtok_r(list, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            while (*tok == ' ')
                tok++;
            if (*tok == '/')
                count += hide_path(source, tok, what, n);
        }
    }
    return count;
}

/* Undoes gpu_hide, also for mounts made by a daemon that is no longer running. */
int gpu_unhide(struct gpu *g)
{
    char source[400], line[1024], want[80];
    int count = 0;

    snprintf(want, sizeof(want), "/%s.hidden", g->pci);
    for (int pass = 0; pass < 4; pass++) {
        FILE *f = fopen("/proc/self/mountinfo", "r");
        int undone = 0;

        while (f && fgets(line, sizeof(line), f)) {
            char root[400], target[400], *p, *q;
            size_t len;

            /* id parent major:minor root target ... ; octal escapes stand for odd characters. */
            if (sscanf(line, "%*d %*d %*s %399s %399s", root, target) != 2)
                continue;
            len = strlen(root);
            if (len < strlen(want) || strcmp(root + len - strlen(want), want))
                continue;
            for (p = q = target; *p; p++, q++) {
                if (p[0] == '\\' && p[1] && p[2] && p[3]) {
                    *q = (char)strtol((char[]){ p[1], p[2], p[3], 0 }, NULL, 8);
                    p += 3;
                } else {
                    *q = *p;
                }
            }
            *q = '\0';
            if (umount2(target, MNT_DETACH) == 0)
                undone++;
        }
        if (f)
            fclose(f);
        count += undone;
        if (!undone)
            break;
    }
    hidden_source(g, source, sizeof(source));
    unlink(source);
    return count;
}

/* ---- displays --------------------------------------------------------------------- */

bool gpu_drives_display(struct gpu *g, char *which, size_t n)
{
    char dir[300], path[700], text[32];
    struct dirent *card, *conn;
    bool found = false;
    DIR *d, *c;

    which[0] = '\0';
    if (g->backend && g->backend->drives_display)
        return g->backend->drives_display(g, which, n);

    snprintf(dir, sizeof(dir), "/sys/bus/pci/devices/%s/drm", g->pci);
    d = opendir(dir);
    while (d && !found && (card = readdir(d))) {
        if (strncmp(card->d_name, "card", 4))
            continue;
        snprintf(path, sizeof(path), "%s/%.40s", dir, card->d_name);
        c = opendir(path);
        while (c && !found && (conn = readdir(c))) {
            if (strncmp(conn->d_name, "card", 4))
                continue;
            snprintf(path, sizeof(path), "%s/%.40s/%.60s/status", dir, card->d_name, conn->d_name);
            if (read_text(path, text, sizeof(text)) == 0 && !strcmp(text, "connected")) {
                snprintf(which, n, "%.60s", strchr(conn->d_name, '-') ? strchr(conn->d_name, '-') + 1 : conn->d_name);
                found = true;
            }
        }
        if (c)
            closedir(c);
    }
    if (d)
        closedir(d);
    return found;
}

/* ---- services --------------------------------------------------------------------- */

static int run(const char *cmd, const char *a1, const char *a2, const char *a3)
{
    char *argv[] = { (char *)cmd, (char *)a1, (char *)a2, (char *)a3, NULL };
    int status = -1;
    pid_t pid;

    if (posix_spawnp(&pid, cmd, NULL, NULL, argv, environ) != 0)
        return -1;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* A process name is cut to fifteen characters; compare on that prefix. */
bool service_listed(const char *comm)
{
    char list[256], *save = NULL;

    snprintf(list, sizeof(list), "%s", zssd_cfg.stop_services ? zssd_cfg.stop_services : "");
    for (char *name = strtok_r(list, ", ", &save); name; name = strtok_r(NULL, ", ", &save))
        if (!strncmp(name, comm, 15) && strlen(comm) >= (strlen(name) < 15 ? strlen(name) : 15))
            return true;
    return false;
}

void services_stop(struct gpu *g)
{
    char list[256], *save = NULL;

    g->stopped[0] = '\0';
    snprintf(list, sizeof(list), "%s", zssd_cfg.stop_services ? zssd_cfg.stop_services : "");
    for (char *name = strtok_r(list, ", ", &save); name; name = strtok_r(NULL, ", ", &save)) {
        if (run(zssd_cfg.service_cmd, "is-active", "--quiet", name) != 0)
            continue;
        if (run(zssd_cfg.service_cmd, "stop", name, NULL) != 0)
            continue;
        if (g->stopped[0])
            strncat(g->stopped, ",", sizeof(g->stopped) - strlen(g->stopped) - 1);
        strncat(g->stopped, name, sizeof(g->stopped) - strlen(g->stopped) - 1);
    }
}

void services_start(struct gpu *g)
{
    char list[256], *save = NULL;

    snprintf(list, sizeof(list), "%s", g->stopped);
    for (char *name = strtok_r(list, ",", &save); name; name = strtok_r(NULL, ",", &save))
        run(zssd_cfg.service_cmd, "start", name, NULL);
    g->stopped[0] = '\0';
}

/* ---- marker ----------------------------------------------------------------------- */
/*
 * Written when power is cut, removed when it is back. If the daemon dies in
 * between, `zssd --recover` finds it and restores power, so that callers
 * sleeping on the suspended driver are not left there.
 */

static void marker_path(const struct gpu *g, const char *suffix, char *out, size_t n)
{
    snprintf(out, n, "%s/%s.%s", zssd_cfg.runtime_dir, g->pci, suffix);
}

void marker_write(const struct gpu *g)
{
    char path[400];
    FILE *f;
    int fd;

    mkdir(zssd_cfg.runtime_dir, 0755);
    marker_path(g, "cfg", path, sizeof(path));
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
        if (write(fd, g->config, g->config_len) < 0)
            unlink(path);
        close(fd);
    }
    marker_path(g, "off", path, sizeof(path));
    f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "%d %d\n%s\n%s\n%s\n", g->nvidia_suspended, g->rpm_suspended, g->audio_driver, g->stopped, g->driver);
    for (int i = 0; i < g->nfrozen; i++)
        fprintf(f, "%d ", (int)g->frozen[i]);
    fprintf(f, "\n");
    fclose(f);
}

bool marker_read(struct gpu *g)
{
    char path[400], line[300];
    int nv = 0, rpm = 0, fd;
    ssize_t r;
    FILE *f;

    marker_path(g, "off", path, sizeof(path));
    f = fopen(path, "r");
    if (!f)
        return false;
    if (fgets(line, sizeof(line), f))
        sscanf(line, "%d %d", &nv, &rpm);
    g->nvidia_suspended = nv;
    g->rpm_suspended = rpm;
#define LINE(field) \
    if (fgets(line, sizeof(line), f)) { line[strcspn(line, "\n")] = '\0'; snprintf(field, sizeof(field), "%.*s", (int)sizeof(field) - 1, line); }
    LINE(g->audio_driver)
    LINE(g->stopped)
    LINE(g->driver)
#undef LINE
    g->nfrozen = 0;
    if (fgets(line, sizeof(line), f)) {
        char *save = NULL;

        for (char *tok = strtok_r(line, " \n", &save); tok && g->nfrozen < 32; tok = strtok_r(NULL, " \n", &save))
            g->frozen[g->nfrozen++] = (pid_t)atoi(tok);
    }
    fclose(f);

    marker_path(g, "cfg", path, sizeof(path));
    fd = open(path, O_RDONLY | O_CLOEXEC);
    g->config_len = 0;
    if (fd >= 0) {
        r = read(fd, g->config, sizeof(g->config));
        g->config_len = r > 0 ? (size_t)r : 0;
        close(fd);
    }
    return true;
}

void marker_remove(const struct gpu *g)
{
    char path[400];

    marker_path(g, "off", path, sizeof(path));
    unlink(path);
    marker_path(g, "cfg", path, sizeof(path));
    unlink(path);
}

/* ---- description ------------------------------------------------------------------ */

void gpu_describe(struct gpu *g)
{
    char cmd[80], line[256], vendor[16] = "", device[16] = "", path[300];
    FILE *p;

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", g->pci);
    read_text(path, vendor, sizeof(vendor));
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/device", g->pci);
    read_text(path, device, sizeof(device));
    snprintf(g->name, sizeof(g->name), "[%s:%s]", vendor[0] ? vendor + 2 : "?", device[0] ? device + 2 : "?");

    /* lspci knows the marketing name; it is not required. */
    snprintf(cmd, sizeof(cmd), "lspci -s %s 2>/dev/null", g->pci);
    p = popen(cmd, "r");
    if (p) {
        if (fgets(line, sizeof(line), p)) {
            char *colon = strstr(line, ": ");

            line[strcspn(line, "\n")] = '\0';
            if (colon)
                snprintf(g->name, sizeof(g->name), "%.95s", colon + 2);
        }
        pclose(p);
    }
}

/* ---- text console ----------------------------------------------------------------- */
/*
 * Optional: put the screen on a text console while the device is switched,
 * and print progress there. Needed when the desktop cannot be shown while
 * the device is off (a driver without wake support); otherwise a choice.
 */

#define ZSS_VT 63

static int console_fd = -1, console_ctl = -1, console_origin = -1;

int console_enter(void)
{
    struct vt_stat st;
    char path[32];

    if (console_fd >= 0)
        return 0;
    console_ctl = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if (console_ctl < 0 || ioctl(console_ctl, VT_GETSTATE, &st) < 0)
        goto fail;
    console_origin = st.v_active;
    snprintf(path, sizeof(path), "/dev/tty%d", ZSS_VT);
    console_fd = open(path, O_RDWR | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    if (console_fd < 0)
        goto fail;
    if (ioctl(console_ctl, VT_ACTIVATE, ZSS_VT) < 0 || ioctl(console_ctl, VT_WAITACTIVE, ZSS_VT) < 0)
        goto fail;
    /* Clear the screen and home the cursor. */
    if (write(console_fd, "\033[2J\033[H", 7) < 0)
        goto fail;
    return 0;
fail:
    if (console_fd >= 0)
        close(console_fd);
    if (console_ctl >= 0)
        close(console_ctl);
    console_fd = console_ctl = -1;
    return -1;
}

void console_leave(void)
{
    if (console_fd < 0)
        return;
    if (console_origin > 0) {
        ioctl(console_ctl, VT_ACTIVATE, console_origin);
        ioctl(console_ctl, VT_WAITACTIVE, console_origin);
    }
    close(console_fd);
    close(console_ctl);
    console_fd = console_ctl = -1;
}

void console_print(const char *line)
{
    if (console_fd < 0)
        return;
    if (write(console_fd, line, strlen(line)) < 0 || write(console_fd, "\r\n", 2) < 0)
        return;
}

/* Readable when a key is pressed on the console; -1 when the console is not in use. */
int console_input_fd(void)
{
    return console_fd;
}
