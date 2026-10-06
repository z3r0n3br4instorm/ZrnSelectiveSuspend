// SPDX-License-Identifier: GPL-2.0
/*
 * Power backends, and the two ways of quiescing a kernel driver in place.
 * A backend only switches power; zssd decides when that is allowed.
 */
#include "zssd.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* ---- dry-run: touches nothing ---------------------------------------------------- */

static int dry_ok(struct gpu *g, char *err)
{
    (void)g;
    (void)err;
    return 0;
}

static int dry_powered(struct gpu *g)
{
    (void)g;
    return 1;
}

static const struct backend dry_run = {
    .name = "dry-run",
    .dry_run = true,
    .strategy = RS_UNBIND,
    .probe = dry_ok,
    .power_off = dry_ok,
    .power_on = dry_ok,
    .is_powered = dry_powered,
};

/* ---- pciehp-slot: a hot-plug slot with its own power controller --------------------- */

static int slot_probe(struct gpu *g, char *err)
{
    DIR *d = opendir("/sys/bus/pci/slots");
    size_t stem = strcspn(g->pci, ".");
    struct dirent *de;

    g->slot[0] = '\0';
    while (d && (de = readdir(d))) {
        char path[300], addr[32];

        if (de->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "/sys/bus/pci/slots/%s/address", de->d_name);
        if (read_text(path, addr, sizeof(addr)) < 0 || strncmp(addr, g->pci, stem) || addr[stem])
            continue;
        snprintf(path, sizeof(path), "/sys/bus/pci/slots/%s/power", de->d_name);
        if (path_exists(path))
            snprintf(g->slot, sizeof(g->slot), "/sys/bus/pci/slots/%s", de->d_name);
    }
    if (d)
        closedir(d);
    if (!g->slot[0]) {
        snprintf(err, ZSSD_ERR, "%s is not in a hot-plug slot with power control", g->pci);
        return -1;
    }
    return 0;
}

static int slot_powered(struct gpu *g)
{
    char path[320], text[8];

    snprintf(path, sizeof(path), "%s/power", g->slot);
    if (read_text(path, text, sizeof(text)) < 0)
        return -1;
    return text[0] == '1';
}

static int slot_set(struct gpu *g, bool on, char *err)
{
    char path[320];

    if (slot_powered(g) == (int)on)
        return 0;
    snprintf(path, sizeof(path), "%s/power", g->slot);
    if (write_text(path, on ? "1" : "0") < 0) {
        snprintf(err, ZSSD_ERR, "cannot switch slot power %s: %s", on ? "on" : "off", strerror(errno));
        return -1;
    }
    for (int tries = 0; tries < 50; tries++) {
        if (slot_powered(g) == (int)on)
            return 0;
        usleep(100000);
    }
    snprintf(err, ZSSD_ERR, "slot did not report power %s", on ? "on" : "off");
    return -1;
}

static int slot_off(struct gpu *g, char *err)
{
    return slot_set(g, false, err);
}

static int slot_on(struct gpu *g, char *err)
{
    return slot_set(g, true, err);
}

static const struct backend pciehp_slot = {
    .name = "pciehp-slot",
    .removal_safe = true,
    .strategy = RS_UNBIND,
    .probe = slot_probe,
    .power_off = slot_off,
    .power_on = slot_on,
    .is_powered = slot_powered,
};

/* ---- apple-gmux: the discrete GPU of a classic-gmux MacBook Pro --------------------- */
/*
 * Port offsets and the two-write power sequence follow gmux_set_discrete_state()
 * in the kernel's drivers/platform/x86/apple-gmux.c: 1 then 0 to power down,
 * 1 then 3 to power up. The kernel driver owns these ports but offers power
 * control only through vga_switcheroo, which the proprietary NVIDIA driver
 * does not join, so the daemon writes them itself through /dev/port.
 */
#define GMUX_PORT_VERSION_MAJOR 0x04
#define GMUX_PORT_VERSION_MINOR 0x05
#define GMUX_PORT_VERSION_RELEASE 0x06
#define GMUX_PORT_DISCRETE_POWER 0x50
#define GMUX_MIN_IO_LEN 0x80

static long gmux_base = -1;

static int port_rw(long port, unsigned char *val, bool write)
{
    int fd = open("/dev/port", (write ? O_WRONLY : O_RDONLY) | O_CLOEXEC);
    ssize_t r;

    if (fd < 0)
        return -1;
    r = write ? pwrite(fd, val, 1, port) : pread(fd, val, 1, port);
    close(fd);
    return r == 1 ? 0 : -1;
}

static int gmux_probe(struct gpu *g, char *err)
{
    DIR *d = opendir("/sys/bus/pnp/devices");
    unsigned char major = 0xff, minor = 0xff, release = 0xff;
    char vendor[16], path[300];
    struct dirent *de;

    gmux_base = -1;
    while (d && (de = readdir(d))) {
        char id[32], line[128];
        unsigned long start, end;
        FILE *f;

        if (de->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "/sys/bus/pnp/devices/%s/id", de->d_name);
        if (read_text(path, id, sizeof(id)) < 0 || strcasecmp(id, "APP000B"))
            continue;
        snprintf(path, sizeof(path), "/sys/bus/pnp/devices/%s/resources", de->d_name);
        f = fopen(path, "r");
        while (f && fgets(line, sizeof(line), f))
            if (sscanf(line, "io 0x%lx-0x%lx", &start, &end) == 2 && end - start + 1 >= GMUX_MIN_IO_LEN)
                gmux_base = (long)start;
        if (f)
            fclose(f);
    }
    if (d)
        closedir(d);
    if (gmux_base < 0) {
        snprintf(err, ZSSD_ERR, "no Apple gmux with I/O ports on this machine");
        return -1;
    }
    /* All-ones version bytes mean an indexed or absent gmux, which this backend does not drive. */
    if (port_rw(gmux_base + GMUX_PORT_VERSION_MAJOR, &major, false) < 0 ||
        port_rw(gmux_base + GMUX_PORT_VERSION_MINOR, &minor, false) < 0 ||
        port_rw(gmux_base + GMUX_PORT_VERSION_RELEASE, &release, false) < 0) {
        snprintf(err, ZSSD_ERR, "cannot read gmux ports through /dev/port: %s", strerror(errno));
        return -1;
    }
    if (major == 0xff && minor == 0xff && release == 0xff) {
        snprintf(err, ZSSD_ERR, "the gmux on this machine is not a classic one");
        return -1;
    }
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", g->pci);
    if (read_text(path, vendor, sizeof(vendor)) == 0 && strtoul(vendor, NULL, 16) == 0x8086) {
        snprintf(err, ZSSD_ERR, "%s is the integrated GPU; gmux powers only the discrete one", g->pci);
        return -1;
    }
    return 0;
}

/* The gmux has no documented power readback, so ask the bus: an unpowered card reads as all ones. */
static int gmux_powered(struct gpu *g)
{
    unsigned char id[2];
    char path[300];
    int fd;
    ssize_t r;

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/config", g->pci);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    r = pread(fd, id, 2, 0);
    close(fd);
    if (r != 2)
        return -1;
    return !(id[0] == 0xff && id[1] == 0xff);
}

static int gmux_set(struct gpu *g, bool on, char *err)
{
    unsigned char first = 1, second = on ? 3 : 0;

    if (gmux_base < 0 && gmux_probe(g, err) < 0)
        return -1;
    if (port_rw(gmux_base + GMUX_PORT_DISCRETE_POWER, &first, true) < 0 ||
        port_rw(gmux_base + GMUX_PORT_DISCRETE_POWER, &second, true) < 0) {
        snprintf(err, ZSSD_ERR, "cannot write the gmux power port: %s", strerror(errno));
        return -1;
    }
    /* The kernel driver allows 200 ms for the switch; the card needs a little longer to answer. */
    for (int tries = 0; tries < 20; tries++) {
        usleep(100000);
        if (gmux_powered(g) == (int)on)
            return 0;
    }
    snprintf(err, ZSSD_ERR, "the discrete GPU did not power %s", on ? "up" : "down");
    return -1;
}

static int gmux_off(struct gpu *g, char *err)
{
    return gmux_set(g, false, err);
}

static int gmux_on(struct gpu *g, char *err)
{
    return gmux_set(g, true, err);
}

static const struct backend apple_gmux = {
    .name = "apple-gmux",
    .removal_safe = false, /* the GPU is soldered */
    .strategy = RS_SUSPEND,
    .probe = gmux_probe,
    .power_off = gmux_off,
    .power_on = gmux_on,
    .is_powered = gmux_powered,
};

/* ---- fake: a suspend-in-place device made of files, for tests ------------------------- */
/*
 * $ZSSD_FAKE_DIR holds:  power ("1"/"0")   events (appended log)   wake (counter)
 *                        display (connector name, if "driving a display")
 *                        fail_suspend (present: the driver refuses to suspend)
 */

static void fake_path(const char *name, char *out, size_t n)
{
    const char *dir = getenv("ZSSD_FAKE_DIR");

    snprintf(out, n, "%s/%s", dir ? dir : "/nonexistent", name);
}

static void fake_event(const char *what)
{
    char path[400];
    FILE *f;

    fake_path("events", path, sizeof(path));
    f = fopen(path, "a");
    if (f) {
        fprintf(f, "%s\n", what);
        fclose(f);
    }
}

static int fake_set(const char *value)
{
    char path[400];
    FILE *f;

    fake_path("power", path, sizeof(path));
    f = fopen(path, "w");
    if (!f)
        return -1;
    fputs(value, f);
    fclose(f);
    return 0;
}

static int fake_probe(struct gpu *g, char *err)
{
    (void)g;
    if (!getenv("ZSSD_FAKE_DIR")) {
        snprintf(err, ZSSD_ERR, "the fake backend needs ZSSD_FAKE_DIR");
        return -1;
    }
    return 0;
}

static int fake_off(struct gpu *g, char *err)
{
    (void)g;
    (void)err;
    fake_event("power off");
    return fake_set("0");
}

static int fake_on(struct gpu *g, char *err)
{
    (void)g;
    (void)err;
    fake_event("power on");
    return fake_set("1");
}

static int fake_powered(struct gpu *g)
{
    char path[400], text[8];

    (void)g;
    fake_path("power", path, sizeof(path));
    if (read_text(path, text, sizeof(text)) < 0)
        return 1;
    return text[0] != '0';
}

static int fake_suspend(struct gpu *g, char *err)
{
    char path[400];

    (void)g;
    fake_path("fail_suspend", path, sizeof(path));
    if (path_exists(path)) {
        snprintf(err, ZSSD_ERR, "the fake driver refused to suspend");
        return -1;
    }
    fake_event("suspend");
    return 0;
}

static int fake_resume(struct gpu *g, char *err)
{
    (void)g;
    (void)err;
    fake_event("resume");
    return 0;
}

static bool fake_display(struct gpu *g, char *which, size_t n)
{
    char path[400];

    (void)g;
    fake_path("display", path, sizeof(path));
    return read_text(path, which, n) == 0 && which[0];
}

static const char *fake_wake(struct gpu *g)
{
    static char path[400];

    (void)g;
    fake_path("wake", path, sizeof(path));
    return path;
}

/* $ZSSD_FAKE_DIR/holders: process IDs to treat as having the device open. */
static int fake_holders(struct gpu *g, pid_t *pids, int max)
{
    char path[400], text[256], *save = NULL;
    int n = 0;

    (void)g;
    fake_path("holders", path, sizeof(path));
    if (read_text(path, text, sizeof(text)) < 0)
        return 0;
    for (char *tok = strtok_r(text, " ,\n", &save); tok && n < max; tok = strtok_r(NULL, " ,\n", &save))
        if (atoi(tok) > 0)
            pids[n++] = (pid_t)atoi(tok);
    return n;
}

static const struct backend fake = {
    .name = "fake",
    .strategy = RS_SUSPEND,
    .probe = fake_probe,
    .power_off = fake_off,
    .power_on = fake_on,
    .is_powered = fake_powered,
    .suspend = fake_suspend,
    .resume = fake_resume,
    .drives_display = fake_display,
    .wake_file = fake_wake,
    .extra_holders = fake_holders,
};

static const struct backend *const backends[] = { &dry_run, &pciehp_slot, &apple_gmux, &fake };

const struct backend *backend_by_name(const char *name)
{
    for (size_t i = 0; i < sizeof(backends) / sizeof(backends[0]); i++)
        if (!strcmp(backends[i]->name, name))
            return backends[i];
    return NULL;
}

/* Platform detection for devices with no configured backend. Never picks dry-run. */
const struct backend *backend_detect(struct gpu *g)
{
    char err[ZSSD_ERR];

    if (slot_probe(g, err) == 0)
        return &pciehp_slot;
    if (gmux_probe(g, err) == 0)
        return &apple_gmux;
    return NULL;
}

/* ---- suspending a driver in place ------------------------------------------------- */

#define NVIDIA_SUSPEND "/proc/driver/nvidia/suspend"

static int wait_text(const char *path, const char *want, int tenths)
{
    char text[32];

    for (int i = 0; i < tenths; i++) {
        if (read_text(path, text, sizeof(text)) == 0 && !strcmp(text, want))
            return 0;
        usleep(100000);
    }
    return -1;
}

/* Runtime PM: the driver and the PCI core do the whole job, including configuration space. */
static int rpm_suspend(struct gpu *g, char *err)
{
    char path[300];

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/power/control", g->pci);
    if (write_text(path, "auto") < 0) {
        snprintf(err, ZSSD_ERR, "cannot enable runtime PM on %s: %s", g->pci, strerror(errno));
        return -1;
    }
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/power/runtime_status", g->pci);
    if (wait_text(path, "suspended", 200) < 0) {
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/power/control", g->pci);
        write_text(path, "on");
        snprintf(err, ZSSD_ERR, "driver %s did not runtime-suspend %s", g->driver, g->pci);
        return -1;
    }
    g->rpm_suspended = true;
    return 0;
}

static int rpm_resume(struct gpu *g, char *err)
{
    char path[300];

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/power/control", g->pci);
    write_text(path, "on");
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/power/runtime_status", g->pci);
    g->rpm_suspended = false;
    if (wait_text(path, "active", 200) < 0) {
        snprintf(err, ZSSD_ERR, "driver %s did not resume %s", g->driver, g->pci);
        return -1;
    }
    return 0;
}

static void audio_path(const struct gpu *g, char *out, size_t n)
{
    snprintf(out, n, "%.*s.1", (int)strcspn(g->pci, "."), g->pci);
}

/*
 * NVIDIA 470 on Kepler has no runtime PM. Its /proc interface performs the
 * driver's sleep and wake paths, but outside a real system sleep nothing
 * saves or restores PCI configuration space, so that is done here.
 */
static int nvidia_suspend(struct gpu *g, char *err)
{
    char audio[16], path[300];
    int fd;

    audio_path(g, audio, sizeof(audio));
    pci_driver(audio, g->audio_driver, sizeof(g->audio_driver));
    if (g->audio_driver[0]) {
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/driver/unbind", audio);
        if (write_text(path, audio) < 0) {
            snprintf(err, ZSSD_ERR, "cannot unbind %s from %s: %s", audio, g->audio_driver, strerror(errno));
            return -1;
        }
    }
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/config", g->pci);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    g->config_len = 0;
    if (fd >= 0) {
        ssize_t r = pread(fd, g->config, sizeof(g->config), 0);

        g->config_len = r > 0 ? (size_t)r : 0;
        close(fd);
    }
    if (g->config_len < 64) {
        snprintf(err, ZSSD_ERR, "cannot save PCI configuration of %s", g->pci);
        return -1;
    }
    if (write_text(NVIDIA_SUSPEND, "suspend") < 0) {
        snprintf(err, ZSSD_ERR, "the NVIDIA driver refused to suspend: %s", strerror(errno));
        return -1;
    }
    g->nvidia_suspended = true;
    return 0;
}

static int nvidia_resume(struct gpu *g, char *err)
{
    char audio[16], path[300];
    int fd, rc = 0;

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/config", g->pci);
    fd = open(path, O_WRONLY | O_CLOEXEC);
    /*
     * The card comes back blank. Base addresses and capabilities go in first
     * and the command register last, so decoding is never enabled on unset
     * addresses. This is the order run on the reference laptop.
     */
    if (fd < 0 || g->config_len < 64 ||
        pwrite(fd, g->config + 0x10, g->config_len - 0x10, 0x10) != (ssize_t)(g->config_len - 0x10) ||
        pwrite(fd, g->config + 0x0c, 4, 0x0c) != 4 || pwrite(fd, g->config + 0x04, 2, 0x04) != 2) {
        snprintf(err, ZSSD_ERR, "cannot restore PCI configuration of %s: %s", g->pci, strerror(errno));
        rc = -1;
    }
    if (fd >= 0)
        close(fd);
    if (rc == 0 && write_text(NVIDIA_SUSPEND, "resume") < 0) {
        snprintf(err, ZSSD_ERR, "the NVIDIA driver failed to resume: %s", strerror(errno));
        rc = -1;
    }
    if (rc == 0)
        g->nvidia_suspended = false;
    if (g->audio_driver[0]) {
        audio_path(g, audio, sizeof(audio));
        snprintf(path, sizeof(path), "/sys/bus/pci/drivers/%s/bind", g->audio_driver);
        write_text(path, audio);
    }
    return rc;
}

int driver_suspend(struct gpu *g, char *err)
{
    if (g->backend && g->backend->suspend)
        return g->backend->suspend(g, err);
    if (!g->driver[0])
        return 0; /* nothing bound, nothing to quiesce */
    if (!strcmp(g->driver, "nvidia") && path_exists(NVIDIA_SUSPEND))
        return nvidia_suspend(g, err);
    return rpm_suspend(g, err);
}

int driver_resume(struct gpu *g, char *err)
{
    if (g->backend && g->backend->resume)
        return g->backend->resume(g, err);
    if (g->nvidia_suspended)
        return nvidia_resume(g, err);
    if (g->rpm_suspended)
        return rpm_resume(g, err);
    return 0;
}
