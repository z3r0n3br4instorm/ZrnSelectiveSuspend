// SPDX-License-Identifier: GPL-2.0
/*
 * zssd: takes a GPU out of service and brings it back.
 *
 * One poll loop serves every connection. A detach or attach runs to
 * completion inside that loop, pumping client messages while it waits for
 * applications to report, so only one such operation is ever in flight.
 */
#include "zssd.h"

#include <errno.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "zss_ipc.h"

#define MAX_CLIENTS 64
#define OUTCOME_TIMEOUT_MS 120000
#define DRIVER_TIMEOUT_MS 15000

enum outcome { OC_NONE, OC_PENDING, OC_MIGRATED, OC_PARKED, OC_FAILED };

struct gpu_view {
    bool holds, migratable;
    int devices, origin, parked;
    char reason[160];
};

struct client {
    int fd;
    struct zss_reader rd;
    pid_t pid;
    uid_t uid;
    gid_t gid;
    bool registered, subscribed, frozen;
    char name[64];
    struct gpu_view view[ZSSD_MAX_GPUS];

    long long pending_id;
    enum outcome outcome;
    char outcome_error[64];
    char outcome_reason[256];
};

static struct gpu gpus[ZSSD_MAX_GPUS];
static int ngpus;
static struct client clients[MAX_CLIENTS];
static int listen_fd = -1;
static long long next_id = 1;
static bool busy;
static bool allow_software;
static gid_t admin_gid = (gid_t)-1;
static bool owner_access = true;
static bool auto_attach = true;
static volatile sig_atomic_t stop;

const char *state_name(enum gpu_state s)
{
    switch (s) {
    case GS_ATTACHED: return "attached";
    case GS_DETACHING: return "detaching";
    case GS_POWERED_OFF: return "powered-off";
    case GS_SAFE_TO_REMOVE: return "safe-to-remove";
    case GS_ATTACHING: return "attaching";
    }
    return "?";
}

static void logmsg(const char *fmt, ...)
{
    va_list ap;

    fputs("zssd: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static struct gpu *gpu_find(const char *pci)
{
    for (int i = 0; i < ngpus; i++)
        if (!strcmp(gpus[i].pci, pci))
            return &gpus[i];
    return NULL;
}

static struct client *client_by_pid(pid_t pid)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0 && clients[i].registered && clients[i].pid == pid)
            return &clients[i];
    return NULL;
}

static void client_drop(struct client *c)
{
    if (c->frozen)
        cgroup_thaw(c->pid);
    close(c->fd);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

static void set_state(struct gpu *g, enum gpu_state s)
{
    enum gpu_state old = g->state;

    if (old == s)
        return;
    g->state = s;
    logmsg("%s: %s -> %s", g->pci, state_name(old), state_name(s));
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct zj_out o;

        if (clients[i].fd < 0 || !clients[i].subscribed)
            continue;
        zj_begin(&o, "event");
        zj_add_str(&o, "gpu", g->pci);
        zj_add_str(&o, "old", state_name(old));
        zj_add_str(&o, "new", state_name(s));
        zss_send(clients[i].fd, &o);
    }
}

/* ---- authorisation ----------------------------------------------------------------- */

static bool in_admin_group(const struct client *c)
{
    char path[64], line[1024];
    bool yes = false;
    FILE *f;

    if (admin_gid == (gid_t)-1)
        return false;
    if (c->gid == admin_gid)
        return true;
    snprintf(path, sizeof(path), "/proc/%d/status", c->pid);
    f = fopen(path, "r");
    while (f && fgets(line, sizeof(line), f)) {
        char *tok, *save = NULL;

        if (strncmp(line, "Groups:", 7))
            continue;
        for (tok = strtok_r(line + 7, " \t\n", &save); tok; tok = strtok_r(NULL, " \t\n", &save))
            if ((gid_t)strtoul(tok, NULL, 10) == admin_gid)
                yes = true;
    }
    if (f)
        fclose(f);
    return yes;
}

/* Root, the user the daemon itself runs as, or a member of the configured group. */
static bool authorised(const struct client *c)
{
    return c->uid == 0 || (owner_access && c->uid == geteuid()) || in_admin_group(c);
}

/* ---- replies ----------------------------------------------------------------------- */

static void send_result(struct client *c, bool ok, const char *error, const char *message,
                        const struct gpu *g)
{
    struct zj_out o;

    if (!c || c->fd < 0)
        return;
    zj_begin(&o, "result");
    zj_add_bool(&o, "ok", ok);
    if (error && error[0])
        zj_add_str(&o, "error", error);
    zj_add_str(&o, "message", message ? message : "");
    if (g) {
        zj_add_str(&o, "gpu", g->pci);
        zj_add_str(&o, "state", state_name(g->state));
        zj_add_bool(&o, "dry_run", g->dry_detached);
    }
    zss_send(c->fd, &o);
}

static void send_blocker(struct client *c, pid_t pid, const char *name, const char *kind,
                         const char *reason)
{
    struct zj_out o;

    if (!c || c->fd < 0)
        return;
    zj_begin(&o, "blocker");
    zj_add_int(&o, "pid", pid);
    zj_add_str(&o, "name", name);
    zj_add_str(&o, "class", kind);
    zj_add_str(&o, "reason", reason);
    zss_send(c->fd, &o);
}

/* ---- message pump ------------------------------------------------------------------ */

static void handle(struct client *c, struct zj_msg *m);

/* Serves sockets for up to timeout_ms. Returns false when asked to stop. */
static bool pump(int timeout_ms)
{
    struct pollfd pfd[MAX_CLIENTS + 1];
    struct client *who[MAX_CLIENTS + 1];
    int n = 0;

    pfd[n].fd = listen_fd;
    pfd[n].events = POLLIN;
    who[n++] = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd < 0)
            continue;
        pfd[n].fd = clients[i].fd;
        pfd[n].events = POLLIN;
        who[n++] = &clients[i];
    }
    if (poll(pfd, (nfds_t)n, timeout_ms) <= 0)
        return !stop;

    if (pfd[0].revents & POLLIN) {
        int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
        struct ucred cred;
        socklen_t len = sizeof(cred);

        if (fd >= 0) {
            struct client *c = NULL;

            for (int i = 0; i < MAX_CLIENTS && !c; i++)
                if (clients[i].fd < 0)
                    c = &clients[i];
            if (!c || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0) {
                close(fd);
            } else {
                memset(c, 0, sizeof(*c));
                c->fd = fd;
                c->pid = cred.pid;
                c->uid = cred.uid;
                c->gid = cred.gid;
                zss_reader_init(&c->rd, fd);
            }
        }
    }
    for (int i = 1; i < n; i++) {
        struct client *c = who[i];
        struct zj_msg m;
        int rc;

        if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR)) || c->fd < 0)
            continue;
        /* Drain what is buffered, without blocking on a quiet socket. */
        while ((rc = zss_recv(&c->rd, &m, 0)) == 1) {
            handle(c, &m);
            zj_free(&m);
            if (c->fd < 0)
                break;
        }
        if (rc < 0 && c->fd >= 0) {
            if (c->registered)
                logmsg("client %s (%d) left", c->name, c->pid);
            client_drop(c);
        }
    }
    return !stop;
}

/* Waits until no client has an outcome pending. Clients that vanish count as failed. */
static void wait_outcomes(void)
{
    long long deadline = now_ms() + OUTCOME_TIMEOUT_MS;

    for (;;) {
        bool pending = false;

        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i].fd >= 0 && clients[i].outcome == OC_PENDING)
                pending = true;
        if (!pending)
            return;
        if (now_ms() > deadline || !pump(200))
            break;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0 && clients[i].outcome == OC_PENDING) {
            clients[i].outcome = OC_FAILED;
            snprintf(clients[i].outcome_reason, sizeof(clients[i].outcome_reason),
                     "no answer within %d seconds", OUTCOME_TIMEOUT_MS / 1000);
        }
    }
}

static void ask(struct client *c, struct zj_out *o)
{
    c->pending_id = next_id++;
    c->outcome = OC_PENDING;
    c->outcome_error[0] = c->outcome_reason[0] = '\0';
    zj_add_int(o, "id", c->pending_id);
    if (zss_send(c->fd, o) < 0) {
        c->outcome = OC_FAILED;
        snprintf(c->outcome_reason, sizeof(c->outcome_reason), "connection lost");
    }
}

/* ---- detach ------------------------------------------------------------------------ */

static void choose_target(const struct gpu *g, const char *wanted, char *out, size_t n)
{
    out[0] = '\0';
    if (wanted && wanted[0]) {
        snprintf(out, n, "%.*s", (int)n - 1, wanted);
        return;
    }
    for (int i = 0; i < ngpus; i++) {
        if (&gpus[i] != g && gpus[i].state == GS_ATTACHED && !gpus[i].dry_detached) {
            snprintf(out, n, "%.15s", gpus[i].pci);
            return;
        }
    }
    if (other_display_device(g->pci, out, n) == 0)
        return;
    if (allow_software)
        snprintf(out, n, "software");
}

static bool wants_migrate(const struct client *c, int gi, const pid_t *holders, int nh)
{
    if (!c->registered)
        return false;
    if (c->view[gi].holds || c->view[gi].devices > 0)
        return true;
    for (int i = 0; i < nh; i++)
        if (holders[i] == c->pid)
            return true;
    return false;
}

/*
 * Names everything that prevents a detach. Returns the number of blockers.
 * A dry run takes nothing away from processes outside the layer, so there
 * they are listed but do not block.
 */
static int report_blockers(struct client *req, int gi, const pid_t *holders, int nh, bool dry)
{
    int n = 0;

    for (int i = 0; i < nh; i++) {
        struct client *c = client_by_pid(holders[i]);
        char comm[64];

        if (c)
            continue;
        pid_comm(holders[i], comm, sizeof(comm));
        if (is_display_server(holders[i], comm))
            send_blocker(req, holders[i], comm, dry ? "display-server, ignored in dry run" : "display-server",
                         "a display server is using this GPU");
        else
            send_blocker(req, holders[i], comm, dry ? "non-migratable, ignored in dry run" : "non-migratable",
                         "not started under the ZSS layer");
        if (!dry)
            n++;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = &clients[i];

        if (c->fd < 0 || !wants_migrate(c, gi, holders, nh) || c->view[gi].migratable)
            continue;
        send_blocker(req, c->pid, c->name, "non-migratable", c->view[gi].reason);
        n++;
    }
    return n;
}

static void do_detach(struct client *req, struct gpu *g, const char *to)
{
    char err[ZSSD_ERR] = "", target[64], msg[512];
    int gi = (int)(g - gpus), nh, pstate;
    pid_t holders[128];

    if (g->state != GS_ATTACHED || g->dry_detached) {
        send_result(req, false, "", "the device is not attached", g);
        return;
    }
    if (!g->backend) {
        send_result(req, false, "", "no power backend is available for this device", g);
        return;
    }
    if (!g->backend->dry_run && g->backend->probe(g, err) < 0) {
        send_result(req, false, "", err, g);
        return;
    }

    nh = gpu_holders(g->pci, holders, 128);
    if (report_blockers(req, gi, holders, nh, g->backend->dry_run) > 0) {
        send_result(req, false, ZSS_ERR_DETACH_BLOCKED,
                    "processes that cannot be migrated are using the device", g);
        return;
    }

    set_state(g, GS_DETACHING);
    choose_target(g, to, target, sizeof(target));
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = &clients[i];
        struct zj_out o;

        c->outcome = OC_NONE;
        if (c->fd < 0 || !wants_migrate(c, gi, holders, nh))
            continue;
        zj_begin(&o, "migrate");
        zj_add_str(&o, "from", g->pci);
        zj_add_str(&o, "to", target);
        ask(c, &o);
    }
    wait_outcomes();

    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = &clients[i];

        if (c->fd < 0 || c->outcome != OC_FAILED)
            continue;
        snprintf(msg, sizeof(msg), "%s (pid %d) could not be migrated: %s", c->name, c->pid,
                 c->outcome_reason);
        set_state(g, GS_ATTACHED);
        send_result(req, false, "", msg, g);
        return;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = &clients[i];

        if (c->fd >= 0 && c->outcome == OC_PARKED && !c->frozen) {
            if (cgroup_freeze(c->pid) == 0)
                c->frozen = true;
            else
                logmsg("could not freeze parked %s (%d); it stays stopped inside the layer", c->name, c->pid);
        }
    }

    /* Condition 1 of safe-to-remove: nothing has the device open any more. */
    for (int tries = 0; tries < 30; tries++) {
        int all = gpu_holders(g->pci, holders, 128);

        /* In a dry run only the applications that were asked to leave count. */
        nh = 0;
        for (int i = 0; i < all; i++)
            if (!g->backend->dry_run || client_by_pid(holders[i]))
                holders[nh++] = holders[i];
        if (nh == 0)
            break;
        pump(100);
    }
    if (nh > 0) {
        char comm[64];

        pid_comm(holders[0], comm, sizeof(comm));
        snprintf(msg, sizeof(msg), "%s (pid %d) still has the device open", comm, holders[0]);
        set_state(g, GS_ATTACHED);
        send_result(req, false, "", msg, g);
        return;
    }

    if (g->backend->dry_run) {
        g->dry_detached = true;
        set_state(g, GS_ATTACHED);
        send_result(req, true, "", "dry run: applications were moved, the driver and power were not changed", g);
        return;
    }

    pci_driver(g->pci, g->driver, sizeof(g->driver));
    if (g->backend->strategy == RS_UNBIND) {
        if (pci_remove(g->pci, err) < 0) {
            set_state(g, GS_ATTACHED);
            send_result(req, false, "", err, g);
            return;
        }
    } else if (driver_suspend(g, err) < 0) {
        /* The driver would not quiesce, so power stays on. */
        set_state(g, GS_ATTACHED);
        send_result(req, false, "", err, g);
        return;
    }

    /* Power is only ever cut once the driver has let go. */
    if (g->backend->strategy == RS_UNBIND && pci_any_driver(g->pci)) {
        send_result(req, false, "", "a kernel driver is still bound; power was not cut", g);
        return;
    }
    if (g->backend->power_off(g, err) < 0) {
        send_result(req, false, "", err, g);
        return;
    }
    pstate = g->backend->is_powered(g);
    if (pstate != 0) {
        send_result(req, false, "", "the power backend did not confirm power-off", g);
        return;
    }

    if (g->backend->removal_safe && g->backend->strategy == RS_UNBIND && !pci_any_driver(g->pci)) {
        set_state(g, GS_SAFE_TO_REMOVE);
        send_result(req, true, "", "the device can be removed", g);
    } else {
        set_state(g, GS_POWERED_OFF);
        send_result(req, true, "", "powered off; physical removal is not supported on this device", g);
    }
}

/* ---- attach ------------------------------------------------------------------------ */

static void restore_clients(struct gpu *g, char *failed, size_t n)
{
    failed[0] = '\0';
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = &clients[i];
        struct zj_out o;

        c->outcome = OC_NONE;
        if (c->fd < 0 || !c->registered)
            continue;
        if (c->frozen) {
            cgroup_thaw(c->pid);
            c->frozen = false;
        }
        zj_begin(&o, "restore");
        zj_add_str(&o, "gpu", g->pci);
        ask(c, &o);
    }
    wait_outcomes();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = &clients[i];

        if (c->fd >= 0 && c->outcome == OC_FAILED && !failed[0])
            snprintf(failed, n, "%s (pid %d) could not return: %s", c->name, c->pid, c->outcome_reason);
    }
}

static void do_attach(struct client *req, struct gpu *g)
{
    char err[ZSSD_ERR] = "", failed[400];
    enum gpu_state before = g->state;

    if (g->dry_detached) {
        g->dry_detached = false;
        restore_clients(g, failed, sizeof(failed));
        send_result(req, !failed[0], "", failed[0] ? failed : "dry run: applications were moved back", g);
        return;
    }
    if (g->state == GS_ATTACHED) {
        send_result(req, false, "", "the device is already attached", g);
        return;
    }

    if (!g->backend) {
        send_result(req, false, "", "no power backend is available for this device", g);
        return;
    }
    set_state(g, GS_ATTACHING);
    if (g->backend->power_on(g, err) < 0) {
        set_state(g, before);
        send_result(req, false, "", err, g);
        return;
    }
    if (g->backend->strategy == RS_UNBIND) {
        long long deadline = now_ms() + DRIVER_TIMEOUT_MS;
        char drv[64] = "";

        pci_rescan();
        /* Ready means present, and bound again if a driver was bound before. */
        for (;;) {
            if (pci_present(g->pci)) {
                pci_driver(g->pci, drv, sizeof(drv));
                if (drv[0] || !g->driver[0])
                    break;
            }
            if (now_ms() > deadline) {
                set_state(g, before);
                send_result(req, false, "",
                            pci_present(g->pci) ? "no kernel driver bound to the device in time"
                                                : "the device did not appear on the bus", g);
                return;
            }
            pump(100);
        }
    } else if (driver_resume(g, err) < 0) {
        set_state(g, before);
        send_result(req, false, "", err, g);
        return;
    }

    restore_clients(g, failed, sizeof(failed));
    set_state(g, GS_ATTACHED);
    send_result(req, !failed[0], "", failed[0] ? failed : "attached", g);
}

/* A device that comes back on its own is treated as an attach request. */
static void check_reappeared(void)
{
    for (int i = 0; i < ngpus; i++) {
        struct gpu *g = &gpus[i];

        if (busy || !g->backend || g->backend->strategy != RS_UNBIND || g->backend->dry_run)
            continue;
        if (g->state != GS_POWERED_OFF && g->state != GS_SAFE_TO_REMOVE)
            continue;
        if (!pci_present(g->pci))
            continue;
        logmsg("%s reappeared; attaching", g->pci);
        busy = true;
        do_attach(NULL, g);
        busy = false;
    }
}

/* ---- resume ------------------------------------------------------------------------ */

static void do_resume(struct client *req, pid_t pid)
{
    struct client *c = client_by_pid(pid);
    bool was_frozen;
    struct zj_out o;

    if (!c) {
        send_result(req, false, "", "no such registered application", NULL);
        return;
    }
    was_frozen = c->frozen;
    if (c->frozen) {
        cgroup_thaw(c->pid);
        c->frozen = false;
    }
    for (int i = 0; i < MAX_CLIENTS; i++)
        clients[i].outcome = OC_NONE;
    zj_begin(&o, "resume");
    ask(c, &o);
    wait_outcomes();
    if (c->fd >= 0 && c->outcome == OC_MIGRATED) {
        send_result(req, true, "", "resumed", NULL);
        return;
    }
    if (c->fd >= 0 && was_frozen && cgroup_freeze(c->pid) == 0)
        c->frozen = true;
    send_result(req, false, c->fd >= 0 && c->outcome_error[0] ? c->outcome_error : ZSS_ERR_RESUME_NO_DRM,
                c->fd >= 0 ? c->outcome_reason : "the application exited", NULL);
}

/* ---- status ------------------------------------------------------------------------ */

static void do_status(struct client *req, const char *only)
{
    for (int gi = 0; gi < ngpus; gi++) {
        struct gpu *g = &gpus[gi];
        pid_t holders[128];
        struct zj_out o;
        int nh;

        if (only[0] && strcmp(only, g->pci))
            continue;
        zj_begin(&o, "gpu");
        zj_add_str(&o, "gpu", g->pci);
        zj_add_str(&o, "state", state_name(g->state));
        zj_add_bool(&o, "dry_run", g->dry_detached);
        zj_add_str(&o, "backend", g->backend ? g->backend->name : "none");
        zj_add_bool(&o, "removal_supported", g->backend && g->backend->removal_safe);
        zss_send(req->fd, &o);

        nh = gpu_holders(g->pci, holders, 128);
        for (int i = 0; i < nh; i++) {
            char comm[64];

            if (client_by_pid(holders[i]))
                continue;
            pid_comm(holders[i], comm, sizeof(comm));
            zj_begin(&o, "client");
            zj_add_str(&o, "gpu", g->pci);
            zj_add_int(&o, "pid", holders[i]);
            zj_add_str(&o, "name", comm);
            zj_add_str(&o, "class", is_display_server(holders[i], comm) ? "display-server" : "non-migratable");
            zj_add_str(&o, "reason", is_display_server(holders[i], comm) ? "" : "not started under the ZSS layer");
            zss_send(req->fd, &o);
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            struct client *c = &clients[i];
            const struct gpu_view *v = &c->view[gi];

            if (c->fd < 0 || !c->registered)
                continue;
            if (!wants_migrate(c, gi, holders, nh) && v->origin == 0)
                continue;
            zj_begin(&o, "client");
            zj_add_str(&o, "gpu", g->pci);
            zj_add_int(&o, "pid", c->pid);
            zj_add_str(&o, "name", c->name);
            zj_add_str(&o, "class", v->devices > 0 && !v->migratable ? "non-migratable" : "migratable");
            zj_add_str(&o, "reason", v->reason);
            zj_add_int(&o, "devices", v->devices);
            zj_add_int(&o, "parked", v->parked);
            zj_add_bool(&o, "away", v->origin > v->devices);
            zss_send(req->fd, &o);
        }
    }
    {
        struct zj_out o;

        zj_begin(&o, "end");
        zss_send(req->fd, &o);
    }
}

/* ---- dispatch ---------------------------------------------------------------------- */

static void handle(struct client *c, struct zj_msg *m)
{
    struct gpu *g;

    if (zj_is(m, "register")) {
        char list[256] = "";
        struct zj_out o;

        c->registered = true;
        snprintf(c->name, sizeof(c->name), "%s", zj_str(m, "name", "?"));
        for (int i = 0; i < ZSSD_MAX_GPUS; i++)
            c->view[i].migratable = true;
        for (int i = 0; i < ngpus; i++) {
            if (gpus[i].state == GS_ATTACHED && !gpus[i].dry_detached)
                continue;
            if (list[0])
                strncat(list, ",", sizeof(list) - strlen(list) - 1);
            strncat(list, gpus[i].pci, sizeof(list) - strlen(list) - 1);
        }
        zj_begin(&o, "welcome");
        zj_add_str(&o, "detached", list);
        zss_send(c->fd, &o);
        logmsg("client %s (%d) registered", c->name, c->pid);
    } else if (zj_is(m, "state")) {
        g = gpu_find(zj_str(m, "gpu", ""));
        if (g) {
            struct gpu_view *v = &c->view[g - gpus];

            v->holds = zj_bool(m, "holds", false);
            v->devices = (int)zj_int(m, "devices", 0);
            v->origin = (int)zj_int(m, "origin", 0);
            v->parked = (int)zj_int(m, "parked", 0);
            v->migratable = zj_bool(m, "migratable", true);
            snprintf(v->reason, sizeof(v->reason), "%s", zj_str(m, "reason", ""));
        }
    } else if (zj_is(m, "outcome")) {
        const char *result = zj_str(m, "result", "failed");

        if (c->outcome != OC_PENDING || zj_int(m, "id", -1) != c->pending_id)
            return;
        c->outcome = !strcmp(result, "migrated") ? OC_MIGRATED : !strcmp(result, "parked") ? OC_PARKED : OC_FAILED;
        snprintf(c->outcome_error, sizeof(c->outcome_error), "%s", zj_str(m, "error", ""));
        snprintf(c->outcome_reason, sizeof(c->outcome_reason), "%s", zj_str(m, "reason", ""));
    } else if (zj_is(m, "subscribe")) {
        c->subscribed = true;
    } else if (zj_is(m, "status")) {
        do_status(c, zj_str(m, "gpu", ""));
    } else if (zj_is(m, "detach") || zj_is(m, "attach") || zj_is(m, "resume")) {
        if (!authorised(c)) {
            send_result(c, false, "", "permission denied", NULL);
            return;
        }
        if (busy) {
            send_result(c, false, "", "another operation is in progress", NULL);
            return;
        }
        busy = true;
        if (zj_is(m, "resume")) {
            do_resume(c, (pid_t)zj_int(m, "pid", 0));
        } else {
            g = gpu_find(zj_str(m, "gpu", ""));
            if (!g) {
                char msg[128];

                snprintf(msg, sizeof(msg), "%s is not a managed GPU", zj_str(m, "gpu", "(none)"));
                send_result(c, false, "", msg, NULL);
            } else if (zj_is(m, "detach")) {
                do_detach(c, g, zj_str(m, "to", ""));
            } else {
                do_attach(c, g);
            }
        }
        busy = false;
    }
}

/* ---- startup ----------------------------------------------------------------------- */

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static int add_gpu(const char *spec)
{
    char pci[16];
    const char *backend;
    struct gpu *g;
    size_t len;

    /* PCI[=backend]; the address itself contains colons. */
    backend = strchr(spec, '=');
    len = backend ? (size_t)(backend - spec) : strlen(spec);
    if (len >= sizeof(pci) || ngpus == ZSSD_MAX_GPUS)
        return -1;
    memcpy(pci, spec, len);
    pci[len] = '\0';

    g = &gpus[ngpus++];
    memset(g, 0, sizeof(*g));
    snprintf(g->pci, sizeof(g->pci), "%s", pci);
    g->state = GS_ATTACHED;
    if (backend) {
        g->backend = backend_by_name(backend + 1);
        if (!g->backend) {
            logmsg("unknown backend '%s'", backend + 1);
            return -1;
        }
    } else {
        g->backend = backend_detect(g);
    }
    logmsg("managing %s with backend %s", g->pci, g->backend ? g->backend->name : "none");
    return 0;
}

static void usage(void)
{
    fputs("usage: zssd --gpu PCI[=BACKEND] [--gpu ...] [--socket PATH] [--group NAME]\n"
          "            [--allow-software] [--no-owner-access] [--no-auto-attach]\n"
          "  BACKEND is pciehp-slot, apple-gmux or dry-run; without it the platform is detected.\n"
          "  --allow-software     let applications fall back to a software renderer\n"
          "  --no-owner-access    accept detach and attach only from root and the group,\n"
          "                       not from the user the daemon runs as\n"
          "  --no-auto-attach     wait for an attach command when a removed device returns\n",
          stderr);
}

int main(int argc, char **argv)
{
    const char *path = zss_socket_path(), *group = "zss";
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    struct group *gr;
    char dir[108], *slash;
    long long last_check = 0;

    for (int i = 0; i < MAX_CLIENTS; i++)
        clients[i].fd = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--gpu") && i + 1 < argc) {
            if (add_gpu(argv[++i]) < 0)
                return 2;
        } else if (!strcmp(argv[i], "--socket") && i + 1 < argc) {
            path = argv[++i];
        } else if (!strcmp(argv[i], "--group") && i + 1 < argc) {
            group = argv[++i];
        } else if (!strcmp(argv[i], "--allow-software")) {
            allow_software = true;
        } else if (!strcmp(argv[i], "--no-owner-access")) {
            owner_access = false;
        } else if (!strcmp(argv[i], "--no-auto-attach")) {
            auto_attach = false;
        } else {
            usage();
            return 2;
        }
    }
    if (ngpus == 0) {
        usage();
        return 2;
    }
    gr = getgrnam(group);
    if (gr)
        admin_gid = gr->gr_gid;

    if (strlen(path) >= sizeof(sa.sun_path)) {
        logmsg("socket path too long");
        return 1;
    }
    strcpy(sa.sun_path, path);
    snprintf(dir, sizeof(dir), "%s", path);
    slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        mkdir(dir, 0755);
    }
    unlink(path);
    listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd < 0 || bind(listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(listen_fd, 16) < 0) {
        logmsg("cannot listen on %s: %s", path, strerror(errno));
        return 1;
    }
    /* Any local user may register; detach and attach are checked per request. */
    chmod(path, 0666);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    logmsg("listening on %s", path);

    while (pump(500)) {
        if (auto_attach && now_ms() - last_check >= 1000) {
            last_check = now_ms();
            check_reappeared();
        }
    }
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0)
            client_drop(&clients[i]);
    unlink(path);
    return 0;
}
