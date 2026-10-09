// SPDX-License-Identifier: GPL-2.0-only
/*
 * Lending a GPU to a virtual machine (the change zss-vm-passthrough).
 *
 * What is here: finding out whether a card can be lent on this machine, and
 * the hand-over itself, which is the kernel's ordinary interface for giving a
 * PCI function to another driver (driver_override, unbind, drivers_probe).
 * ZSS_Interceptor does the other half: it stops guarding the device, keeps
 * its PCI state, and resets it by a power cycle on the way back.
 *
 * Nothing here starts a virtual machine. A lent card is one a VM manager can
 * take.
 */
#include "zssd.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * The kernel's passthrough driver, as the name a function is handed to.
 * ZSS_TEST_PASS_DRIVER gives another name for the tests: one no driver has,
 * so that the hand-over fails after the host's driver has let go.
 */
#define PASS_DRIVER (pass_driver())

static const char *pass_driver(void)
{
    const char *t = getenv("ZSS_TEST_PASS_DRIVER");

    return t && t[0] ? t : "vfio-pci";
}
#define PCI_DEV "/sys/bus/pci/devices"
#define LEND_MAX_FUNCS 24 /* the card's functions and the devices that share its isolation groups */

static void add(struct lend_obstacle *o, int *n, int max, const char *reason, const char *remedy)
{
    if (*n >= max)
        return;
    snprintf(o[*n].reason, sizeof(o[*n].reason), "%s", reason);
    snprintf(o[*n].remedy, sizeof(o[*n].remedy), "%s", remedy);
    (*n)++;
}

/* The isolation group of a function ("" if the kernel has put it in none). */
static void group_of(const char *fn, char *out, size_t n)
{
    char path[300], link[PATH_MAX];
    ssize_t len;

    out[0] = '\0';
    snprintf(path, sizeof(path), PCI_DEV "/%s/iommu_group", fn);
    len = readlink(path, link, sizeof(link) - 1);
    if (len <= 0)
        return;
    link[len] = '\0';
    snprintf(out, n, "%.60s", strrchr(link, '/') ? strrchr(link, '/') + 1 : link);
}

static bool same_card(const char *a, const char *b)
{
    size_t stem = strcspn(a, ".");

    return !strncmp(a, b, stem) && b[stem] == '.';
}

static bool pass_driver_loaded(void)
{
    return path_exists("/sys/bus/pci/drivers/vfio-pci");
}

/*
 * Devices that share an isolation group with the card and are neither its
 * own functions nor bridges: the kernel hands a group over whole, so while
 * the card is lent these may not have a host driver either. (A bridge keeps
 * its driver; the kernel allows that.)
 */
int lend_companions(struct gpu *g, char out[][16], int max)
{
    char funcs[ZSSD_MAX_FUNCS][16];
    int nf = pci_functions(g->pci, funcs, ZSSD_MAX_FUNCS), n = 0;

    for (int i = 0; i < nf; i++) {
        char group[64], path[300];
        struct dirent *de;
        DIR *gd;

        group_of(funcs[i], group, sizeof(group));
        if (!group[0])
            continue;
        snprintf(path, sizeof(path), "/sys/kernel/iommu_groups/%s/devices", group);
        gd = opendir(path);
        while (gd && (de = readdir(gd)) && n < max) {
            char cls[32] = "";
            bool have = false;

            if (de->d_name[0] == '.' || same_card(g->pci, de->d_name))
                continue;
            snprintf(path, sizeof(path), PCI_DEV "/%.40s/class", de->d_name);
            read_text(path, cls, sizeof(cls));
            if (!strncmp(cls, "0x0604", 6))
                continue;
            for (int k = 0; k < n; k++)
                have = have || !strcmp(out[k], de->d_name);
            if (!have)
                snprintf(out[n++], 16, "%.15s", de->d_name);
        }
        if (gd)
            closedir(gd);
    }
    return n;
}

/* The card's functions, then (if they go with it) the devices that share its groups. */
static int hand_over_list(struct gpu *g, bool with_group, char out[][16], int max)
{
    int n = pci_functions(g->pci, out, max < ZSSD_MAX_FUNCS ? max : ZSSD_MAX_FUNCS);

    if (with_group)
        n += lend_companions(g, out + n, max - n);
    return n;
}

/* Whether the passthrough driver is loaded, built in, or can be loaded. */
static bool pass_driver_available(void)
{
    struct utsname u;
    char path[300], line[600];
    bool found = false;
    FILE *f;

    if (pass_driver_loaded())
        return true;
    if (uname(&u) < 0)
        return false;
    for (int which = 0; which < 2 && !found; which++) {
        snprintf(path, sizeof(path), "/lib/modules/%s/%s", u.release, which ? "modules.builtin" : "modules.dep");
        f = fopen(path, "r");
        if (!f)
            continue;
        while (!found && fgets(line, sizeof(line), f))
            found = strstr(line, "/vfio-pci.ko") != NULL;
        fclose(f);
    }
    return found;
}

/*
 * Everything in the way of lending this card, each with what would remove
 * it. Read-only. `holders` are the processes that have the device open, as
 * the caller found them; the caller adds the ones it knows cannot be moved.
 */
int lend_obstacles(struct gpu *g, const pid_t *holders, int nh, bool with_group, struct lend_obstacle *out, int max)
{
    char funcs[ZSSD_MAX_FUNCS][16], text[300], reason[300];
    int nf = pci_functions(g->pci, funcs, ZSSD_MAX_FUNCS), n = 0;
    DIR *d;

    if (nf == 0) {
        snprintf(reason, sizeof(reason), "%s is not on the bus", g->pci);
        add(out, &n, max, reason, "power it on first (zssctl on), or check the address");
    }
    if (!g->kmod)
        add(out, &n, max, "ZSS_Interceptor (the zss kernel module) is not managing this card",
            "install and load it; a lent card has to be reset by a power cycle when it comes back, which the module does");

    d = opendir("/sys/kernel/iommu_groups");
    if (d) {
        struct dirent *de;
        bool any = false;

        while ((de = readdir(d)))
            any = any || de->d_name[0] != '.';
        closedir(d);
        if (!any)
            d = NULL;
    }
    if (!d) {
        bool offered = path_exists("/sys/firmware/acpi/tables/DMAR") || path_exists("/sys/firmware/acpi/tables/IVRS");

        add(out, &n, max,
            offered ? "the IOMMU is not enabled (the firmware offers one)"
                    : "the IOMMU is not enabled, and the firmware does not describe one",
            offered ? "boot with the kernel parameter intel_iommu=on (Intel) or amd_iommu=on (AMD); ZSS does not change the boot configuration"
                    : "enable VT-d or AMD-Vi in the firmware setup if it has the setting; without an IOMMU a card cannot be lent");
    } else {
        char others[LEND_MAX_FUNCS][16];
        int no = lend_companions(g, others, LEND_MAX_FUNCS);

        for (int i = 0; i < nf; i++) {
            char group[64];

            group_of(funcs[i], group, sizeof(group));
            if (!group[0]) {
                snprintf(reason, sizeof(reason), "%s is in no isolation group", funcs[i]);
                add(out, &n, max, reason, "the IOMMU does not cover this device; it cannot be lent on this machine");
            }
        }
        /* A group is lent whole. Other devices in it lose their host drivers for the time, which the user has to ask for. */
        for (int i = 0; i < no && !with_group; i++) {
            char drv[64];

            pci_driver(others[i], drv, sizeof(drv));
            snprintf(reason, sizeof(reason), "%s (driver %s) shares the card's isolation group", others[i], drv[0] ? drv : "none");
            add(out, &n, max, reason,
                "lend with --with-group: that device then has no host driver while the card is lent, and gets it back on reclaim");
        }
    }

    if (!pass_driver_available())
        add(out, &n, max, "the passthrough driver (vfio-pci) is not available in this kernel",
            "install a kernel that has it (CONFIG_VFIO_PCI)");

    for (int i = 0; i < nh; i++) {
        char comm[64];

        pid_comm(holders[i], comm, sizeof(comm));
        if (!is_display_server(holders[i], comm))
            continue;
        snprintf(reason, sizeof(reason), "the display server (%s, pid %d) has the card open", comm, (int)holders[i]);
        add(out, &n, max, reason,
            "a card the display server holds cannot be lent; start the session without this card in the X configuration");
    }
    /*
     * A monitor on the card is the usual case for a card given to a guest,
     * and no obstacle. It is one when this is the host's only display
     * device: the host would be left with nothing to show a console on.
     */
    if (gpu_drives_display(g, text, sizeof(text))) {
        char other[32];

        if (other_display_device(g->pci, other, sizeof(other)) < 0) {
            snprintf(reason, sizeof(reason), "the card drives the host's only display (%.100s)", text);
            add(out, &n, max, reason, "the host needs another display device before this one can be lent");
        }
    }
    return n;
}

/* ---- what holds a driver inside the kernel ----------------------------------------------- */

/* The kernel module behind the driver bound to a function ("" if none, or built in). */
static void driver_module(const char *fn, char *out, size_t n)
{
    char path[300], link[PATH_MAX];
    ssize_t len;

    out[0] = '\0';
    snprintf(path, sizeof(path), PCI_DEV "/%.15s/driver/module", fn);
    len = readlink(path, link, sizeof(link) - 1);
    if (len <= 0)
        return;
    link[len] = '\0';
    snprintf(out, n, "%.60s", strrchr(link, '/') ? strrchr(link, '/') + 1 : link);
}

/* How many devices the driver bound to a function serves. */
static int driver_devices(const char *fn)
{
    char path[300];
    struct dirent *de;
    int n = 0;
    DIR *d;

    snprintf(path, sizeof(path), PCI_DEV "/%.15s/driver", fn);
    d = opendir(path);
    while (d && (de = readdir(d)))
        n += strchr(de->d_name, ':') != NULL; /* its devices appear there by address */
    if (d)
        closedir(d);
    return n;
}

/*
 * The modules stacked on `mod`, deepest first (the order to unload them in).
 * A module stacked on a driver may keep the driver's device open for itself:
 * the NVIDIA display module does, and the driver then never finishes letting
 * go of its card.
 */
static int stacked_on(const char *mod, char out[][64], int n, int max)
{
    char path[300];
    struct dirent *de;
    DIR *d;

    snprintf(path, sizeof(path), "/sys/module/%.60s/holders", mod);
    d = opendir(path);
    while (d && (de = readdir(d)) && n < max) {
        bool have = false;

        if (de->d_name[0] == '.')
            continue;
        n = stacked_on(de->d_name, out, n, max);
        for (int i = 0; i < n; i++)
            have = have || !strcmp(out[i], de->d_name);
        if (!have && n < max)
            snprintf(out[n++], 64, "%.60s", de->d_name);
    }
    if (d)
        closedir(d);
    return n;
}

/*
 * The modules that would be unloaded to lend the card, deepest first. Only
 * for a driver that serves nothing but the card: where it serves another
 * device too (the sound driver, usually), its modules have to stay.
 */
int lend_stacked_modules(struct gpu *g, char out[][64], int max)
{
    char funcs[ZSSD_MAX_FUNCS][16], mod[64];
    int nf = pci_functions(g->pci, funcs, ZSSD_MAX_FUNCS), n = 0;

    for (int i = 0; i < nf; i++) {
        driver_module(funcs[i], mod, sizeof(mod));
        if (mod[0] && driver_devices(funcs[i]) == 1)
            n = stacked_on(mod, out, n, max);
    }
    return n;
}

static long module_users(const char *mod)
{
    char path[300], text[32] = "";

    snprintf(path, sizeof(path), "/sys/module/%.60s/refcnt", mod);
    if (read_text(path, text, sizeof(text)) < 0)
        return -1;
    return strtol(text, NULL, 10);
}

/* ---- the hand-over ------------------------------------------------------------------- */

static int run(const char *a, const char *b)
{
    pid_t pid = fork();
    int status = 0;

    if (pid == 0) {
        execlp(a, a, b, (char *)NULL);
        _exit(127);
    }
    if (pid < 0 || waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int fn_write(const char *fn, const char *file, const char *text)
{
    char path[300];

    snprintf(path, sizeof(path), PCI_DEV "/%.15s/%.40s", fn, file);
    return write_text(path, text);
}

/*
 * Takes whatever driver has the function off it. The write does not return
 * until the driver has let go, and a driver can refuse to for as long as
 * something uses its device. So it is done by a child, and waited for only so
 * long: the daemon must not hang with it. A child that is left behind
 * finishes the unbind whenever the driver gives in.
 */
static int fn_unbind(const char *fn)
{
    char drv[64], path[300];
    int status = 0;
    pid_t pid;

    pci_driver(fn, drv, sizeof(drv));
    if (!drv[0])
        return 0;
    snprintf(path, sizeof(path), PCI_DEV "/%.15s/driver/unbind", fn);
    pid = fork();
    if (pid == 0)
        _exit(write_text(path, fn) == 0 ? 0 : (errno & 0x7f) ? (errno & 0x7f) : 1);
    if (pid < 0)
        return -1;
    for (int waited = 0; waited < 15000; waited += 50) {
        pid_t r = waitpid(pid, &status, WNOHANG);

        if (r == pid) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                return 0;
            errno = WIFEXITED(status) ? WEXITSTATUS(status) : EIO;
            return -1;
        }
        usleep(50000);
        zssd_keepalive();
    }
    errno = ETIMEDOUT;
    return -1;
}

/* Gives the function to `driver`, or with "" to whichever driver claims it. */
static int fn_give(const char *fn, const char *driver)
{
    char drv[64];

    /* An empty override is written as a newline: that is how the kernel is told "none". */
    if (fn_write(fn, "driver_override", driver[0] ? driver : "\n") < 0)
        return -1;
    if (write_text("/sys/bus/pci/drivers_probe", fn) < 0)
        return -1;
    pci_driver(fn, drv, sizeof(drv));
    if (driver[0] ? strcmp(drv, driver) != 0 : !drv[0]) {
        errno = ENODEV;
        return -1;
    }
    return 0;
}

static void lent_path(const struct gpu *g, char *out, size_t n)
{
    snprintf(out, n, "%s/lent-%s", zssd_cfg.runtime_dir, g->pci);
}

/* The host drivers of the card's functions, kept so that they can be given back after a restart too. */
static void lent_write(const struct gpu *g, char funcs[][16], char drivers[][64], int nf, char mods[][64], int nm)
{
    char path[400];
    FILE *f;

    lent_path(g, path, sizeof(path));
    f = fopen(path, "w");
    if (!f)
        return;
    for (int i = 0; i < nf; i++)
        fprintf(f, "%s %s\n", funcs[i], drivers[i][0] ? drivers[i] : "-");
    /* The modules that were unloaded, in the order to load them again. */
    for (int i = nm - 1; i >= 0; i--)
        fprintf(f, "module %s\n", mods[i]);
    fclose(f);
}

/* Loads again what was unloaded for the hand-over. */
static void modules_back(const struct gpu *g)
{
    char path[400], a[64], b[64];
    FILE *f;

    lent_path(g, path, sizeof(path));
    f = fopen(path, "r");
    if (!f)
        return;
    while (fscanf(f, "%63s %63s", a, b) == 2)
        if (!strcmp(a, "module"))
            run("modprobe", b);
    fclose(f);
}

static int lent_read(const struct gpu *g, char funcs[][16], char drivers[][64], int max)
{
    char path[400];
    int n = 0;
    FILE *f;

    lent_path(g, path, sizeof(path));
    f = fopen(path, "r");
    if (!f)
        return 0;
    {
        char a[64], b[64];

        while (n < max && fscanf(f, "%63s %63s", a, b) == 2) {
            if (!strcmp(a, "module"))
                continue;
            snprintf(funcs[n], 16, "%.15s", a);
            snprintf(drivers[n], 64, "%s", strcmp(b, "-") ? b : "");
            n++;
        }
    }
    fclose(f);
    return n;
}

/* Puts the host's drivers back on every function, as far as that goes. */
static void give_back(char funcs[][16], char drivers[][64], int nf)
{
    for (int i = 0; i < nf; i++) {
        char drv[64];

        pci_driver(funcs[i], drv, sizeof(drv));
        if (!strcmp(drv, PASS_DRIVER))
            fn_unbind(funcs[i]);
        fn_give(funcs[i], "");
    }
    (void)drivers;
}

/*
 * Hands every function of the card to the passthrough driver. The caller has
 * made sure nothing on the host uses the card. On failure everything is put
 * back and -1 returned with the reason.
 */
int lend_hand_over(struct gpu *g, bool with_group, char *err)
{
    char funcs[LEND_MAX_FUNCS][16], drivers[LEND_MAX_FUNCS][64], mods[16][64];
    int nf = hand_over_list(g, with_group, funcs, LEND_MAX_FUNCS), nm;

    if (!pass_driver_loaded() && run("modprobe", "vfio-pci") != 0 && !pass_driver_loaded()) {
        snprintf(err, ZSSD_ERR, "the passthrough driver (vfio-pci) could not be loaded");
        return -1;
    }
    for (int i = 0; i < nf; i++)
        pci_driver(funcs[i], drivers[i], sizeof(drivers[i]));

    /*
     * Before any driver is asked to let go: nothing inside the kernel may
     * still be using it. The modules stacked on a driver that serves only
     * this card are unloaded (they are loaded again on the way back), and the
     * driver's module must then have no user left. A driver asked to let go
     * of a device in use does not refuse; it waits, for ever if need be.
     */
    nm = lend_stacked_modules(g, mods, 16);
    lent_write(g, funcs, drivers, nf, mods, 0);
    for (int i = 0; i < nm; i++) {
        long users = module_users(mods[i]);

        /*
         * Its users go a moment after the programs do: the picture a program
         * last showed is a buffer shared with the display server, which lets
         * go of it when the program's new picture has replaced it.
         */
        for (int waited = 0; users > 0 && waited < 10000; waited += 100) {
            usleep(100000);
            zssd_keepalive();
            users = module_users(mods[i]);
        }
        if (users > 0 || run("rmmod", mods[i]) != 0) {
            snprintf(err, ZSSD_ERR, "the kernel module %.60s is stacked on the card's driver and is still in use (%ld user(s) after 10 s); "
                                    "it could not be unloaded", mods[i], module_users(mods[i]));
            modules_back(g);
            lend_forget(g);
            return -1;
        }
        lent_write(g, funcs, drivers, nf, mods, i + 1);
    }
    for (int i = 0; i < nf; i++) {
        char mod[64];
        long users;

        driver_module(funcs[i], mod, sizeof(mod));
        if (!mod[0] || driver_devices(funcs[i]) != 1)
            continue;
        users = module_users(mod);
        if (users > 0) {
            snprintf(err, ZSSD_ERR, "the driver %.60s still has %ld user(s) after every program left the card; it would not let go of %.15s", mod, users, funcs[i]);
            modules_back(g);
            lend_forget(g);
            return -1;
        }
    }

    /* From here the module leaves the device alone: no loss is read into a function with no driver. */
    if (gpu_kmod_request(g, "lend", err) < 0) {
        modules_back(g);
        lend_forget(g);
        return -1;
    }
    for (int i = 0; i < nf; i++) {
        const char *step = "unbinding its driver";
        bool stuck = false;

        if (fn_unbind(funcs[i]) == 0) {
            step = "binding the passthrough driver";
            if (fn_give(funcs[i], PASS_DRIVER) == 0)
                continue;
        } else {
            stuck = errno == ETIMEDOUT;
        }
        if (stuck)
            snprintf(err, ZSSD_ERR, "%.15s: its driver did not let go within 15 s and the unbind is still pending in the kernel; "
                                    "the functions before it were given back", funcs[i]);
        else
            snprintf(err, ZSSD_ERR, "%.15s: %s (%.40s) failed: %.80s", funcs[i], step, PASS_DRIVER, strerror(errno));
        /* A function whose unbind is pending cannot be touched: asking for it would wait on the same lock. */
        give_back(funcs, drivers, stuck ? i : nf);
        {
            char unused[ZSSD_ERR];

            gpu_kmod_request(g, "unlend", unused);
        }
        if (!stuck)
            modules_back(g);
        lend_forget(g);
        return -1;
    }
    return 0;
}

/*
 * run() with a time limit, keeping the watchdog fed: for commands that can
 * wait in the kernel for ever. A child left behind finishes whenever the
 * kernel lets it. Returns -1 with ETIMEDOUT when the time ran out.
 */
static int run_limited(const char *a, const char *b, int ms)
{
    pid_t pid = fork();
    int status = 0;

    if (pid == 0) {
        execlp(a, a, b, (char *)NULL);
        _exit(127);
    }
    if (pid < 0)
        return -1;
    for (int waited = 0; waited < ms; waited += 50) {
        pid_t r = waitpid(pid, &status, WNOHANG);

        if (r == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        usleep(50000);
        zssd_keepalive();
    }
    errno = ETIMEDOUT;
    return -1;
}

/* write_text() by a child with a time limit, for sysfs files whose write can wait in the kernel. */
static int write_limited(const char *path, const char *text, int ms)
{
    int status = 0;
    pid_t pid = fork();

    if (pid == 0)
        _exit(write_text(path, text) == 0 ? 0 : (errno & 0x7f) ? (errno & 0x7f) : 1);
    if (pid < 0)
        return -1;
    for (int waited = 0; waited < ms; waited += 50) {
        if (waitpid(pid, &status, WNOHANG) == pid) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                return 0;
            errno = WIFEXITED(status) ? WEXITSTATUS(status) : EIO;
            return -1;
        }
        usleep(50000);
        zssd_keepalive();
    }
    errno = ETIMEDOUT;
    return -1;
}

/*
 * After nvidia_replug: the modules stacked on the driver (nvidia_drm and
 * nvidia_modeset) still serve the card that was given up, and know nothing of
 * the one found again; NVIDIA's own presenting goes through them. They are
 * loaded afresh, which the old card, now taken as unplugged, should no longer
 * stand in the way of. Best effort: if an unload does not finish in time it
 * is left to finish in the kernel, and the card still works for everything
 * that does not present through NVIDIA's display modules (ZSS_AirLock's
 * programs, nvidia-smi).
 */
static void modules_reload(char mods[][64], int n);

static void display_modules_renew(struct gpu *g)
{
    char mods[16][64];
    int nm = lend_stacked_modules(g, mods, 16), done = 0;

    for (int i = 0; i < nm; i++) {
        long users = module_users(mods[i]);

        for (int waited = 0; users > 0 && waited < 3000; waited += 100) {
            usleep(100000);
            zssd_keepalive();
            users = module_users(mods[i]);
        }
        if (users > 0) {
            logmsg("%s: %s is still in use (%ld); NVIDIA's display modules keep serving the card given up",
                   g->pci, mods[i], users);
            break;
        }
        if (run_limited("rmmod", mods[i], 15000) != 0) {
            logmsg("%s: unloading %s %s", g->pci, mods[i],
                   errno == ETIMEDOUT ? "did not finish within 15 s; it is left to finish in the kernel" : "failed");
            if (errno == ETIMEDOUT)
                return;
            break;
        }
        done = i + 1;
    }
    modules_reload(mods, done);
    if (done == nm)
        logmsg("%s: NVIDIA's display modules loaded afresh", g->pci);
}

/*
 * Brings back a card whose NVIDIA driver saw it lose power and gave it up.
 * Unbinding such a driver, or unloading what is stacked on it, waits for
 * ever. The driver does support a device leaving with clients still open,
 * for an eGPU unplugged from its port; patch revision 6 lets the daemon mark
 * this card that way. Then, with the rail still off: the card's functions are
 * taken off the bus (the driver lets go, each client finishes on its own last
 * close), the rail is switched on, the bridge above it is rescanned, and the
 * driver finds the card as a new one.
 */
int nvidia_replug(struct gpu *g, char *err)
{
    char funcs[ZSSD_MAX_FUNCS][16], path[400], spec[64], bridge[PATH_MAX], *slash;
    int nf = pci_functions(g->pci, funcs, ZSSD_MAX_FUNCS);

    snprintf(path, sizeof(path), PCI_DEV "/%.15s", g->pci);
    if (!realpath(path, bridge) || !(slash = strrchr(bridge, '/'))) {
        snprintf(err, ZSSD_ERR, "%.15s: cannot find the bridge above it", g->pci);
        return -1;
    }
    *slash = '\0';
    snprintf(spec, sizeof(spec), "unplugged %.15s", g->pci);
    if (write_text("/proc/driver/nvidia/zss_hold", spec) < 0) {
        snprintf(err, ZSSD_ERR, "the NVIDIA driver did not take the card as unplugged (%.60s); "
                                "this needs revision 6 of the ZSS driver patch", strerror(errno));
        return -1;
    }
    /* The module lets go of the card first: it holds references to the functions being removed. */
    write_text("/sys/kernel/zss/unmanage", g->pci);
    g->kmod = false;
    /* The other functions first: the card's own goes last, with the driver that was waiting on it. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nf; i++) {
            if ((pass == 0) == !strcmp(funcs[i], g->pci))
                continue;
            snprintf(path, sizeof(path), PCI_DEV "/%.15s/remove", funcs[i]);
            if (path_exists(path) && write_limited(path, "1", 15000) < 0) {
                snprintf(err, ZSSD_ERR, "%.15s could not be taken off the bus: %.80s", funcs[i],
                         errno == ETIMEDOUT ? "its driver did not let go within 15 s" : strerror(errno));
                return -1;
            }
        }
    }
    if (gmux_rail(g, true, err) < 0)
        return -1;
    usleep(300000);
    snprintf(path, sizeof(path), "%.380s/rescan", bridge);
    if (write_limited(path, "1", 15000) < 0) {
        snprintf(err, ZSSD_ERR, "rescanning the bridge above %.15s failed: %.80s", g->pci, strerror(errno));
        return -1;
    }
    for (int waited = 0; waited < 10000; waited += 100) {
        char drv[64] = "";

        if (pci_present(g->pci))
            pci_driver(g->pci, drv, sizeof(drv));
        if (drv[0]) {
            if (gpu_kmod_manage(g, err) < 0)
                return -1;
            display_modules_renew(g);
            return 0;
        }
        usleep(100000);
        zssd_keepalive();
    }
    snprintf(err, ZSSD_ERR, "%.15s %s after the rescan", g->pci, pci_present(g->pci) ? "has no driver" : "did not come back");
    return -1;
}

/* Loads the modules unloaded by driver_rebind again, shallowest last. */
static void modules_reload(char mods[][64], int n)
{
    for (int i = n - 1; i >= 0; i--)
        run("modprobe", mods[i]);
}

/*
 * Binds the card's driver afresh, for a driver that will not use a device
 * again once it has seen it vanish (NVIDIA's). The same care as lending: a
 * driver asked to let go of a device that is still in use inside the kernel
 * waits for ever, and NVIDIA's own modeset module is such a user. So the
 * stacked modules go first, the driver must have no user left, and the
 * unbind is done by a child with a time limit. Refuses with the reason rather
 * than hang; what was unloaded is loaded again where that is safe.
 */
int driver_rebind(struct gpu *g, char *err)
{
    char mods[16][64], drv[64], mod[64];
    int nm = lend_stacked_modules(g, mods, 16);

    pci_driver(g->pci, drv, sizeof(drv));
    if (!drv[0])
        return fn_give(g->pci, "") == 0 ? 0 : -1;
    for (int i = 0; i < nm; i++) {
        long users = module_users(mods[i]);

        for (int waited = 0; users > 0 && waited < 10000; waited += 100) {
            usleep(100000);
            zssd_keepalive();
            users = module_users(mods[i]);
        }
        if (users > 0) {
            snprintf(err, ZSSD_ERR, "the kernel module %.60s is stacked on %.60s and is still in use (%ld user(s) after 10 s); "
                                    "the driver was not bound afresh", mods[i], drv, users);
            modules_reload(mods, i);
            return -1;
        }
        if (run_limited("rmmod", mods[i], 15000) != 0) {
            if (errno == ETIMEDOUT) {
                /* Still unloading, inside the driver: loading anything now would wait on the same lock. */
                snprintf(err, ZSSD_ERR, "unloading %.60s did not finish within 15 s and is still pending in the kernel", mods[i]);
                return -1;
            }
            snprintf(err, ZSSD_ERR, "the kernel module %.60s could not be unloaded; the driver was not bound afresh", mods[i]);
            modules_reload(mods, i);
            return -1;
        }
    }
    driver_module(g->pci, mod, sizeof(mod));
    if (mod[0] && driver_devices(g->pci) == 1 && module_users(mod) > 0) {
        snprintf(err, ZSSD_ERR, "the driver %.60s still has %ld user(s); it would not let go of %.15s", mod, module_users(mod), g->pci);
        modules_reload(mods, nm);
        return -1;
    }
    if (fn_unbind(g->pci) < 0) {
        if (errno == ETIMEDOUT) {
            /* The unbind is still pending in the kernel: touching the driver now would wait on the same lock. */
            snprintf(err, ZSSD_ERR, "%.60s did not let go of %.15s within 15 s; the unbind is still pending in the kernel", drv, g->pci);
        } else {
            snprintf(err, ZSSD_ERR, "%.15s: unbinding %.60s failed: %.80s", g->pci, drv, strerror(errno));
            modules_reload(mods, nm);
        }
        return -1;
    }
    if (fn_give(g->pci, "") < 0) {
        snprintf(err, ZSSD_ERR, "%.15s: %.60s did not take it again: %.80s", g->pci, drv, strerror(errno));
        modules_reload(mods, nm);
        return -1;
    }
    modules_reload(mods, nm);
    return 0;
}

/*
 * Takes the card back: the passthrough driver off every function, the card
 * reset by a power cycle, the host's drivers bound again. The caller has made
 * sure no guest holds it.
 */
int lend_take_back(struct gpu *g, char *err)
{
    char funcs[LEND_MAX_FUNCS][16], drivers[LEND_MAX_FUNCS][64], drv[64];
    int nf = lent_read(g, funcs, drivers, LEND_MAX_FUNCS);

    if (nf == 0) {
        nf = pci_functions(g->pci, funcs, ZSSD_MAX_FUNCS);
        for (int i = 0; i < nf; i++)
            drivers[i][0] = '\0';
    }
    for (int i = 0; i < nf; i++) {
        pci_driver(funcs[i], drv, sizeof(drv));
        if (!strcmp(drv, PASS_DRIVER) && fn_unbind(funcs[i]) < 0) {
            snprintf(err, ZSSD_ERR, "%.15s: could not be taken from %.40s: %.80s", funcs[i], PASS_DRIVER, strerror(errno));
            return -1;
        }
        /* No driver may take it before it has been reset. */
        fn_write(funcs[i], "driver_override", "zss-none");
    }
    if (gpu_kmod_lent(g) && gpu_kmod_request(g, "reclaim", err) < 0)
        return -1;
    for (int i = 0; i < nf; i++) {
        if (fn_give(funcs[i], "") == 0 || !drivers[i][0])
            continue;
        snprintf(err, ZSSD_ERR, "%.15s was reset but its driver (%.63s) did not take it back", funcs[i], drivers[i]);
        return -1;
    }
    modules_back(g);
    lend_forget(g);
    return 0;
}

bool lend_recorded(const struct gpu *g)
{
    char path[400];

    lent_path(g, path, sizeof(path));
    return path_exists(path);
}

void lend_forget(const struct gpu *g)
{
    char path[400];

    lent_path(g, path, sizeof(path));
    unlink(path);
}

/* ---- who holds a lent card ---------------------------------------------------------- */

/*
 * A guest has a lent function through its isolation group's file
 * (/dev/vfio/<group>) or, with the newer interface, the function's own
 * (/dev/vfio/devices/vfio<N>). Processes that have either open.
 */
int lend_holders(struct gpu *g, pid_t *pids, int max)
{
    char funcs[ZSSD_MAX_FUNCS][16], want[2 * ZSSD_MAX_FUNCS][96];
    int nf = pci_functions(g->pci, funcs, ZSSD_MAX_FUNCS), nw = 0, n = 0;
    struct dirent *pe;
    DIR *proc;

    for (int i = 0; i < nf; i++) {
        char group[64], path[300];
        struct dirent *de;
        DIR *d;

        group_of(funcs[i], group, sizeof(group));
        if (group[0])
            snprintf(want[nw++], sizeof(want[0]), "/dev/vfio/%.60s", group);
        snprintf(path, sizeof(path), PCI_DEV "/%s/vfio-dev", funcs[i]);
        d = opendir(path);
        while (d && (de = readdir(d)))
            if (de->d_name[0] != '.' && nw < 2 * ZSSD_MAX_FUNCS)
                snprintf(want[nw++], sizeof(want[0]), "/dev/vfio/devices/%.60s", de->d_name);
        if (d)
            closedir(d);
    }
    proc = nw ? opendir("/proc") : NULL;
    while (proc && (pe = readdir(proc)) && n < max) {
        char dir[64], path[400], link[PATH_MAX];
        pid_t pid = (pid_t)atoi(pe->d_name);
        struct dirent *fe;
        bool holds = false;
        DIR *fds;

        if (pid <= 0 || pid == getpid())
            continue;
        snprintf(dir, sizeof(dir), "/proc/%d/fd", pid);
        fds = opendir(dir);
        while (fds && !holds && (fe = readdir(fds))) {
            ssize_t len;

            if (fe->d_name[0] == '.')
                continue;
            snprintf(path, sizeof(path), "%s/%s", dir, fe->d_name);
            len = readlink(path, link, sizeof(link) - 1);
            if (len <= 0)
                continue;
            link[len] = '\0';
            for (int i = 0; i < nw; i++)
                holds = holds || !strcmp(link, want[i]);
        }
        if (fds)
            closedir(fds);
        if (holds)
            pids[n++] = pid;
    }
    if (proc)
        closedir(proc);
    return n;
}
