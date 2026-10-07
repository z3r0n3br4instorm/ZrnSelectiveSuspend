// SPDX-License-Identifier: GPL-2.0-only
/*
 * zssd: takes a GPU out of service and brings it back.
 *
 * One poll loop serves every connection. A detach or attach runs to
 * completion inside that loop, pumping client messages while it waits for
 * applications to report, so only one such operation is ever in flight.
 */
#include "zssd.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/netlink.h>
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
    int lost_contents;
};

static struct gpu gpus[ZSSD_MAX_GPUS];
static int ngpus;
static struct client clients[MAX_CLIENTS];
static int listen_fd = -1;
static int uevent_fd = -1;
static bool bus_changed;
static long long next_id = 1;
static bool busy;
static bool allow_software;
static gid_t admin_gid = (gid_t)-1;
static bool owner_access = true;
static bool auto_attach = true;
static const char *default_target = ""; /* where applications go when a request names nowhere */
static bool console_key; /* a key was pressed on the text console */
static bool wake_event;  /* a pollable wake file became readable */

struct zssd_config zssd_cfg = {
    .runtime_dir = "/run/zss",
    .stop_services = "nvidia-persistenced",
    .hide_while_off = "auto",
    .service_cmd = "systemctl",
    .wake_file = "",
    .idle_timeout = 0,
    .quick_wake = 60000,
};
static volatile sig_atomic_t stop;

const char *state_name(enum gpu_state s)
{
    switch (s) {
    case GS_ATTACHED: return "attached";
    case GS_DETACHING: return "detaching";
    case GS_POWERED_OFF: return "powered-off";
    case GS_SAFE_TO_REMOVE: return "safe-to-remove";
    case GS_ATTACHING: return "attaching";
    case GS_LOST: return "lost";
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

/*
 * One line of progress for a power transition. It goes to the journal, to
 * the zssctl that asked, and to the text console when the screen is on it.
 */
static void progress(struct client *req, const char *fmt, ...)
{
    char line[400];
    struct zj_out o;
    va_list ap;
    int n;

    n = snprintf(line, sizeof(line), "[ZrnSelectiveSuspend] ");
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);

    fprintf(stderr, "%s\n", line);
    console_print(line);
    if (req && req->fd >= 0) {
        zj_begin(&o, "progress");
        zj_add_str(&o, "text", line);
        zss_send(req->fd, &o);
    }
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

/* Processes holding the GPU's nodes, including stale handles to a device that has left. */
static int find_holders(struct gpu *g, pid_t *pids, int max)
{
    /* A device that is off keeps the nodes it had; nothing is asked of its driver. */
    if (!g->off && pci_present(g->pci)) {
        dev_t fresh[32];
        int n = gpu_nodes(g->pci, fresh, 32);

        if (n > 0) {
            memcpy(g->nodes, fresh, (size_t)n * sizeof(fresh[0]));
            g->nnodes = n;
        }
    }
    {
        int n = node_holders(g->nodes, g->nnodes, pids, max);

        if (g->backend && g->backend->extra_holders)
            n += g->backend->extra_holders(g, pids + n, max - n);
        return n;
    }
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
/*
 * Tells the service manager that the daemon is alive. If the daemon ever
 * stops turning (asleep in a suspended driver, say) the manager kills it and
 * the unit's recovery step powers every device back on, instead of leaving
 * the desktop waiting on a daemon that will never answer.
 */
static void watchdog_ping(void)
{
    static long long last;
    static int fd = -2;
    static struct sockaddr_un to;
    const char *path;

    if (fd == -2) {
        fd = -1;
        path = getenv("NOTIFY_SOCKET");
        if (path && (path[0] == '/' || path[0] == '@') && strlen(path) < sizeof(to.sun_path)) {
            to.sun_family = AF_UNIX;
            memcpy(to.sun_path, path, strlen(path));
            if (path[0] == '@')
                to.sun_path[0] = '\0';
            fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        }
    }
    if (fd < 0 || now_ms() - last < 1000)
        return;
    last = now_ms();
    sendto(fd, "WATCHDOG=1", 10, MSG_NOSIGNAL, (struct sockaddr *)&to,
           (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(getenv("NOTIFY_SOCKET"))));
}

static bool pump(int timeout_ms)
{
    struct pollfd pfd[MAX_CLIENTS + 8];
    struct client *who[MAX_CLIENTS + 8];
    int n = 0;

    watchdog_ping();

    pfd[n].fd = listen_fd;
    pfd[n].events = POLLIN;
    who[n++] = NULL;
    if (uevent_fd >= 0) {
        pfd[n].fd = uevent_fd;
        pfd[n].events = POLLIN;
        who[n++] = NULL;
    }
    if (console_input_fd() >= 0) {
        pfd[n].fd = console_input_fd();
        pfd[n].events = POLLIN;
        who[n++] = NULL;
    }
    for (int i = 0; i < ngpus; i++) {
        if (gpus[i].off && gpus[i].wake_fd >= 0) {
            pfd[n].fd = gpus[i].wake_fd;
            pfd[n].events = POLLIN | POLLPRI;
            who[n++] = NULL;
        }
    }
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

        if (!c) {
            char junk[4096];

            if (!pfd[i].revents) {
                continue;
            } else if (pfd[i].fd == uevent_fd) {
                /* Kernel uevent: something on a bus changed. The contents do not matter. */
                while (recv(uevent_fd, junk, sizeof(junk), MSG_DONTWAIT) > 0)
                    ;
                bus_changed = true;
            } else if (pfd[i].fd == console_input_fd()) {
                while (read(pfd[i].fd, junk, sizeof(junk)) > 0)
                    ;
                console_key = true;
            } else {
                /* A wake file: reading it re-arms it; the main loop compares the count. */
                if (pread(pfd[i].fd, junk, sizeof(junk), 0) < 0)
                    junk[0] = '\0';
                wake_event = true;
            }
            continue;
        }
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
/* Whether the process can still answer: gone, a zombie, or being dumped after a crash, it cannot. */
static bool process_can_answer(pid_t pid)
{
    char path[64], line[512], *p;
    FILE *f;
    bool ok = false;

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    f = fopen(path, "r");
    if (!f)
        return false;
    p = fgets(line, sizeof(line), f) ? strrchr(line, ')') : NULL;
    fclose(f);
    if (p && p[1] == ' ' && p[2] != 'Z' && p[2] != 'X')
        ok = true;
    if (ok) {
        /* A crashed process keeps its sockets open for as long as its core is being written. */
        snprintf(path, sizeof(path), "/proc/%d/status", pid);
        f = fopen(path, "r");
        while (f && fgets(line, sizeof(line), f))
            if (!strncmp(line, "CoreDumping:", 12) && atoi(line + 12) == 1)
                ok = false;
        if (f)
            fclose(f);
    }
    return ok;
}

static void wait_outcomes(void)
{
    long long deadline = now_ms() + OUTCOME_TIMEOUT_MS;

    for (;;) {
        bool pending = false;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd < 0 || clients[i].outcome != OC_PENDING)
                continue;
            /* No point holding everything up for an application that died of the loss. */
            if (!process_can_answer(clients[i].pid)) {
                clients[i].outcome = OC_FAILED;
                snprintf(clients[i].outcome_reason, sizeof(clients[i].outcome_reason), "the application has died");
                continue;
            }
            pending = true;
        }
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
    if (default_target[0] && strcmp(default_target, g->pci)) {
        snprintf(out, n, "%.*s", (int)n - 1, default_target);
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

/* Whether `pid` is the process `of`, or one of its ancestors. */
static bool runs_process(pid_t pid, pid_t of)
{
    for (int depth = 0; of > 1 && depth < 64; depth++) {
        char path[64], line[256], *p;
        FILE *f;

        if (of == pid)
            return true;
        snprintf(path, sizeof(path), "/proc/%d/stat", of);
        f = fopen(path, "r");
        if (!f)
            return false;
        p = fgets(line, sizeof(line), f) ? strrchr(line, ')') : NULL;
        fclose(f);
        if (!p || sscanf(p + 1, " %*c %d", &of) != 1)
            return false;
    }
    return false;
}

/*
 * Blockers for a power-off under a running desktop. Unlike a detach, a
 * display server and listed services may keep the device open (see
 * holder_verdict). Fills `seen` with a short description of every holder.
 */
static int off_blockers(struct client *req, struct gpu *g, const pid_t *holders, int nh, bool wake,
                        bool console, bool freezable, char *seen, size_t seen_len)
{
    int gi = (int)(g - gpus), n = 0;
    struct holder_facts f = { .wake_support = wake, .console = console, .freezable = freezable };
    const char *why;

    seen[0] = '\0';
    g->nfrozen = 0;
#define SEEN(...) snprintf(seen + strlen(seen), seen_len - strlen(seen), __VA_ARGS__)
    for (int i = 0; i < nh; i++) {
        char comm[64];

        if (client_by_pid(holders[i]))
            continue;
        pid_comm(holders[i], comm, sizeof(comm));
        f.registered = false;
        f.display_server = is_display_server(holders[i], comm);
        f.listed_service = service_listed(comm);
        switch (holder_verdict(&f, &why)) {
        case HV_STOP:
            SEEN("%s%s (service, will be stopped)", seen[0] ? ", " : "", comm);
            break;
        case HV_ALLOW:
            SEEN("%s%s (display server, idle)", seen[0] ? ", " : "", comm);
            break;
        case HV_FREEZE:
            /* Freezing the terminal the request came from would leave no way to ask for the device back. */
            if (req && !console && runs_process(holders[i], req->pid)) {
                why = "this command was run from it, and freezing it would leave no way to power the device on; "
                      "run zssctl from somewhere else, or use --console";
                send_blocker(req, holders[i], comm, "non-migratable", why);
                n++;
                break;
            }
            if (g->nfrozen < 32) {
                g->frozen[g->nfrozen++] = holders[i];
                SEEN("%s%s (not under the ZSS layer, will be frozen)", seen[0] ? ", " : "", comm);
                break;
            }
            why = "too many processes to freeze";
            /* fall through */
        default:
            send_blocker(req, holders[i], comm, f.display_server ? "display-server" : "non-migratable", why);
            n++;
            break;
        }
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = &clients[i];

        if (c->fd < 0 || !wants_migrate(c, gi, holders, nh))
            continue;
        f.registered = true;
        f.migratable = c->view[gi].migratable;
        if (holder_verdict(&f, &why) == HV_FREEZE && g->nfrozen < 32) {
            g->frozen[g->nfrozen++] = c->pid;
            SEEN("%s%s (cannot be moved, will be frozen)", seen[0] ? ", " : "", c->name);
        } else if (holder_verdict(&f, &why) != HV_MIGRATE) {
            send_blocker(req, c->pid, c->name, "non-migratable", c->view[gi].reason[0] ? c->view[gi].reason : why);
            n++;
        } else if (c->view[gi].devices > 0) {
            SEEN("%s%s (application, will be moved)", seen[0] ? ", " : "", c->name);
        }
    }
#undef SEEN
    if (!seen[0])
        snprintf(seen, seen_len, "nothing");
    return n;
}

/* Undoes the console switch of a power-off that did not go through. */
static void off_abandoned(struct gpu *g)
{
    if (g->console) {
        console_leave();
        g->console = false;
    }
}

/*
 * Takes a device out of service.
 *
 *   detach   the full rule set: nothing may hold the device.
 *   off      a suspend-in-place device under a running desktop: the display
 *            server and listed services may stay, the device is watched for
 *            wake requests afterwards, and each step is reported.
 *
 * Returns true if the device ended up released.
 */
static bool will_freeze(const struct gpu *g, pid_t pid)
{
    for (int i = 0; i < g->nfrozen; i++)
        if (g->frozen[i] == pid)
            return true;
    return false;
}

static bool do_release(struct client *req, struct gpu *g, const char *to, bool off, bool console, bool automatic)
{
    char err[ZSSD_ERR] = "", target[64], msg[512], seen[400] = "";
    int gi = (int)(g - gpus), nh, pstate, moved = 0, parked = 0, steps = off ? 7 : 0;
    long long t0 = now_ms(), t;
    pid_t holders[128];
    bool wake;

#define REFUSE(error, text) do { \
        snprintf(g->last_refusal, sizeof(g->last_refusal), "%.150s", text); \
        send_result(req, false, error, text, g); \
        return false; \
    } while (0)

    if (g->state == GS_LOST)
        REFUSE("", "the device is already gone; it left without a detach");
    if (g->state != GS_ATTACHED || g->dry_detached)
        REFUSE("", "the device is not attached");
    if (!g->backend)
        REFUSE("", "no power backend is available for this device");
    if (off && (g->backend->strategy != RS_SUSPEND || g->backend->dry_run))
        REFUSE("", "this device is released by unbinding, not powered off in place; use detach");
    if (!g->backend->dry_run && g->backend->probe(g, err) < 0)
        REFUSE("", err);

    pci_driver(g->pci, g->driver, sizeof(g->driver));
    wake = off && gpu_wake_supported(g);
    if (off) {
        char which[64];

        if (gpu_drives_display(g, which, sizeof(which))) {
            snprintf(msg, sizeof(msg), "the device is driving a display (%s)", which);
            REFUSE("", msg);
        }
    }

    nh = find_holders(g, holders, 128);
    /* The idle timer never freezes anything: a process holding the device means it is in use. */
    if ((off ? off_blockers(req, g, holders, nh, wake, console, !automatic, seen, sizeof(seen))
             : report_blockers(req, gi, holders, nh, g->backend->dry_run)) > 0)
        REFUSE(ZSS_ERR_DETACH_BLOCKED, "processes that cannot be migrated are using the device");
    g->last_refusal[0] = '\0';

    if (off && console) {
        if (console_enter() < 0)
            REFUSE("", "cannot switch to the text console");
        g->console = true;
    }
#undef REFUSE

    if (off) {
        progress(req, "Suspending device: %s  %s", g->pci, g->name);
        progress(req, "  driver %s, power through %s, wake on demand: %s%s", g->driver[0] ? g->driver : "none",
                 g->backend->name, wake ? "yes" : "no", automatic ? ", idle timer" : "");
        progress(req, "  [1/%d] In use by: %s", steps, seen);
    }

    set_state(g, GS_DETACHING);
    choose_target(g, to, target, sizeof(target));
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = &clients[i];
        struct zj_out o;

        c->outcome = OC_NONE;
        if (c->fd < 0 || !wants_migrate(c, gi, holders, nh) || (off && will_freeze(g, c->pid)))
            continue;
        zj_begin(&o, "migrate");
        zj_add_str(&o, "from", g->pci);
        zj_add_str(&o, "to", target);
        ask(c, &o);
    }
    wait_outcomes();

    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = &clients[i];

        if (c->fd >= 0 && c->outcome == OC_MIGRATED && c->view[gi].origin > 0)
            moved++;
        if (c->fd >= 0 && c->outcome == OC_PARKED)
            parked++;
        if (c->fd < 0 || c->outcome != OC_FAILED)
            continue;
        snprintf(msg, sizeof(msg), "%s (pid %d) could not be migrated: %s", c->name, c->pid,
                 c->outcome_reason);
        if (off)
            progress(req, "  FAILED: %s", msg);
        off_abandoned(g);
        set_state(g, GS_ATTACHED);
        send_result(req, false, "", msg, g);
        return false;
    }
    if (off)
        progress(req, "  [2/%d] Applications: %d moved to %s, %d parked", steps, moved,
                 target[0] ? target : "nowhere", parked);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = &clients[i];

        if (c->fd >= 0 && c->outcome == OC_PARKED && !c->frozen) {
            if (cgroup_freeze(c->pid) == 0)
                c->frozen = true;
            else
                logmsg("could not freeze parked %s (%d); it stays stopped inside the layer", c->name, c->pid);
        }
    }

    /* Nothing that was asked to leave may still have the device open. */
    for (int tries = 0; tries < 30; tries++) {
        int all = find_holders(g, holders, 128);

        /* In a dry run and in a power-off, only the applications that were asked to leave count. */
        nh = 0;
        for (int i = 0; i < all; i++)
            if ((!g->backend->dry_run && !off) || (client_by_pid(holders[i]) && !(off && will_freeze(g, holders[i]))))
                holders[nh++] = holders[i];
        if (nh == 0)
            break;
        pump(100);
    }
    if (nh > 0) {
        char comm[64];

        pid_comm(holders[0], comm, sizeof(comm));
        snprintf(msg, sizeof(msg), "%s (pid %d) still has the device open", comm, holders[0]);
        if (off)
            progress(req, "  FAILED: %s", msg);
        off_abandoned(g);
        set_state(g, GS_ATTACHED);
        send_result(req, false, "", msg, g);
        return false;
    }

    if (g->backend->dry_run) {
        g->dry_detached = true;
        set_state(g, GS_ATTACHED);
        send_result(req, true, "", "dry run: applications were moved, the driver and power were not changed", g);
        return true;
    }

    if (off) {
        char names[300] = "";
        int done = 0;

        /*
         * Processes the layer cannot move are stopped until the device is back.
         * They keep whatever they had on the device: the driver saves and
         * restores it across the power cut, as it does across a system sleep.
         */
        for (; done < g->nfrozen; done++) {
            char comm[64];

            pid_comm(g->frozen[done], comm, sizeof(comm));
            if (cgroup_freeze(g->frozen[done]) < 0) {
                snprintf(msg, sizeof(msg), "%s (pid %d) is using the device and could not be frozen: %s", comm,
                         g->frozen[done], strerror(errno));
                break;
            }
            snprintf(names + strlen(names), sizeof(names) - strlen(names), "%s%s (pid %d)", names[0] ? ", " : "",
                     comm, g->frozen[done]);
        }
        if (done < g->nfrozen) {
            for (int i = 0; i < done; i++)
                cgroup_thaw(g->frozen[i]);
            g->nfrozen = 0;
            progress(req, "  FAILED: %s", msg);
            off_abandoned(g);
            set_state(g, GS_ATTACHED);
            send_result(req, false, "", msg, g);
            return false;
        }
        services_stop(g);
        progress(req, "  [3/%d] Services stopped: %s; processes frozen: %s", steps, g->stopped[0] ? g->stopped : "none",
                 names[0] ? names : "none");
        /* Switched off on request, the device is not offered to programs that start meanwhile. */
        if (!automatic) {
            char what[900];
            int hidden = gpu_hide(g, what, sizeof(what));

            if (hidden)
                progress(req, "        Hidden from new programs (%d): %s", hidden, what);
        }
    }

    t = now_ms();
    if (g->backend->strategy == RS_UNBIND) {
        if (pci_remove(g->pci, err) < 0) {
            set_state(g, GS_ATTACHED);
            send_result(req, false, "", err, g);
            return false;
        }
    } else if (driver_suspend(g, err) < 0) {
        /* The driver would not quiesce, so power stays on. */
        if (off) {
            progress(req, "  FAILED: %s; power was not cut", err);
            gpu_unhide(g);
            services_start(g);
            for (int i = 0; i < g->nfrozen; i++)
                cgroup_thaw(g->frozen[i]);
            g->nfrozen = 0;
        }
        off_abandoned(g);
        set_state(g, GS_ATTACHED);
        send_result(req, false, "", err, g);
        return false;
    }
    if (off) {
        if (g->kmod)
            progress(req, "  [4/%d] PCI state: saved and restored in the kernel (zss module)", steps);
        else
            progress(req, "  [4/%d] PCI configuration saved: %zu bytes", steps, g->config_len);
        progress(req, "  [5/%d] Driver %s suspended (%.2f s)", steps, g->driver[0] ? g->driver : "(none)",
                 (double)(now_ms() - t) / 1000.0);
    }

    /* Power is only ever cut once the driver has let go. */
    if (g->backend->strategy == RS_UNBIND && pci_any_driver(g->pci)) {
        send_result(req, false, "", "a kernel driver is still bound; power was not cut", g);
        return false;
    }
    t = now_ms();
    pstate = g->backend->power_off(g, err) < 0 ? -2 : g->backend->is_powered(g);
    if (pstate != 0) {
        if (pstate != -2)
            snprintf(err, sizeof(err), "the power backend did not confirm power-off");
        if (off || g->backend->strategy == RS_SUSPEND) {
            /* Put the driver back rather than leave it suspended on a powered device. */
            char err2[ZSSD_ERR] = "";

            if (off)
                progress(req, "  FAILED: %s; resuming the driver", err);
            g->backend->power_on(g, err2);
            driver_resume(g, err2);
            gpu_unhide(g);
            services_start(g);
            for (int i = 0; i < g->nfrozen; i++)
                cgroup_thaw(g->frozen[i]);
            g->nfrozen = 0;
            off_abandoned(g);
            set_state(g, GS_ATTACHED);
        }
        send_result(req, false, "", err, g);
        return false;
    }

    if (off) {
        progress(req, "  [6/%d] Power cut through %s (%.2f s); the device has left the bus", steps, g->backend->name,
                 (double)(now_ms() - t) / 1000.0);
        g->off = true;
        g->auto_off = automatic;
        g->off_since = now_ms();
        g->wake_fd = -1;
        g->nwaiting = 0;
        g->serving = false;
        g->served = 0;
        g->serve_ended = 0;
        if (wake && !strncmp(gpu_wake_path(g), "/proc/", 6))
            g->wake_fd = open(gpu_wake_path(g), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        g->wake_base = gpu_wake_count(g);
        marker_write(g);
        progress(req, "  [7/%d] %s", steps, wake ? "Watching for wake requests"
                                              : "No wake support: the device stays off until it is switched on");
        set_state(g, GS_POWERED_OFF);
        progress(req, "Device %s is powered off (%.2f s).", g->pci, (double)(now_ms() - t0) / 1000.0);
        if (g->console)
            progress(req, "Press any key to power it on and return to the desktop.");
        else
            progress(req, "It %s: zssctl on %s",
                     !wake ? "stays off until" : automatic ? "powers on by itself when needed, or with"
                                                           : "stays off (the display server may borrow it for a moment) until",
                     g->pci);
        send_result(req, true, "", "powered off", g);
    } else if (g->backend->removal_safe && g->backend->strategy == RS_UNBIND && !pci_any_driver(g->pci)) {
        set_state(g, GS_SAFE_TO_REMOVE);
        send_result(req, true, "", "the device can be removed", g);
    } else {
        set_state(g, GS_POWERED_OFF);
        send_result(req, true, "", "powered off; physical removal is not supported on this device", g);
    }
    return true;
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

static void do_attach(struct client *req, struct gpu *g, bool restore, const char *why)
{
    char err[ZSSD_ERR] = "", failed[400] = "";
    long long t0 = now_ms(), t;
    bool was_off = g->off;
    enum gpu_state before = g->state;
    bool lost = g->state == GS_LOST;
    bool wait_bus;

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
    /* A device that left by itself has to come back by itself. */
    /*
     * A driver frozen at the moment of a loss is brought back by its resume,
     * with nothing having been saved. The display server and the listed
     * services, idle on the device, come through that. A program that was
     * rendering on it does not: its state on the device is gone while the
     * driver believes it is there, and resuming the driver under it hung the
     * reference laptop. Such programs have to be gone first.
     */
    if (lost && gpu_driver_frozen(g)) {
        char who[300] = "";
        pid_t holders[128];
        int nh = find_holders(g, holders, 128);

        for (int i = 0; i < nh; i++) {
            char comm[64];

            pid_comm(holders[i], comm, sizeof(comm));
            if (is_display_server(holders[i], comm) || service_listed(comm))
                continue;
            snprintf(who + strlen(who), sizeof(who) - strlen(who), "%s%s (pid %d)", who[0] ? ", " : "", comm, holders[i]);
        }
        if (who[0]) {
            char msg[640];

            snprintf(msg, sizeof(msg),
                     "the device was lost while these were using it, and its driver cannot be resumed under them: %.300s. "
                     "Close them first (kill -9 if they do not respond), then try again", who);
            if (req) {
                send_result(req, false, "", msg, g);
            } else if (strncmp(g->last_refusal, who, sizeof(g->last_refusal) - 1)) {
                /* Asked every second while the device answers; said once per set of programs. */
                logmsg("%s: %s", g->pci, msg);
                snprintf(g->last_refusal, sizeof(g->last_refusal), "%s", who);
            }
            return;
        }
    }
    if (lost && g->seen && !g->kmod && !gpu_returned(g)) {
        send_result(req, false, "", "the device is still absent", g);
        return;
    }
    if (!g->backend && !lost) {
        send_result(req, false, "", "no power backend is available for this device", g);
        return;
    }

    if (was_off) {
        progress(req, "Resuming device: %s  %s", g->pci, g->name);
        progress(req, "  reason: %s; it was off for %.1f s", why, (double)(now_ms() - g->off_since) / 1000.0);
    }
    set_state(g, GS_ATTACHING);
    t = now_ms();
    if (g->backend && !g->backend->dry_run && !g->serving && g->backend->power_on(g, err) < 0) {
        if (was_off)
            progress(req, "  FAILED: %s", err);
        set_state(g, before);
        send_result(req, false, "", err, g);
        return;
    }
    if (was_off)
        progress(req, "  [1/4] Power restored through %s (%.2f s)", g->backend->name, (double)(now_ms() - t) / 1000.0);
    t = now_ms();
    /* An address that never existed on the bus is a test device: nothing to wait for. */
    /*
     * A device the kernel module took back never left the kernel's list, so
     * there is nothing to rescan for; and a rescan walks every bus in the
     * machine at a moment when one device on it has only just been revived.
     */
    wait_bus = g->kmod ? false : lost ? g->seen : g->backend->strategy == RS_UNBIND;
    if (wait_bus) {
        long long deadline = now_ms() + DRIVER_TIMEOUT_MS;
        char drv[64] = "";

        pci_rescan();
        /* Ready means present, and bound again if a driver was bound before. */
        for (;;) {
            if (gpu_on_bus(g)) {
                pci_driver(g->pci, drv, sizeof(drv));
                if (drv[0] || !g->driver[0])
                    break;
            }
            if (now_ms() > deadline) {
                set_state(g, before);
                send_result(req, false, "",
                            gpu_on_bus(g) ? "no kernel driver bound to the device in time"
                                                : "the device did not appear on the bus", g);
                return;
            }
            pump(100);
        }
    } else if (!lost && !g->serving && driver_resume(g, err) < 0) {
        if (was_off)
            progress(req, "  FAILED: %s", err);
        set_state(g, before);
        send_result(req, false, "", err, g);
        return;
    }
    if (was_off) {
        progress(req, "  [2/4] PCI configuration restored, driver %s resumed (%.2f s)", g->driver[0] ? g->driver : "(none)",
                 (double)(now_ms() - t) / 1000.0);
        progress(req, "  [3/4] Services started: %s; processes thawed: %d", g->stopped[0] ? g->stopped : "none", g->nfrozen);
        g->serving = false;
        gpu_unhide(g);
        services_start(g);
        for (int i = 0; i < g->nfrozen; i++)
            cgroup_thaw(g->frozen[i]);
        g->nfrozen = 0;
        marker_remove(g);
        if (g->wake_fd >= 0)
            close(g->wake_fd);
        g->wake_fd = -1;
        g->nwaiting = 0;
        g->off = false;
        g->idle_since = now_ms();
    }

    if (restore)
        restore_clients(g, failed, sizeof(failed));
    if (was_off)
        progress(req, "  [4/4] Applications: %s", restore ? (failed[0] ? failed : "returned to the device")
                                                         : "left where they are (zssctl attach brings them back)");
    set_state(g, GS_ATTACHED);
    if (was_off) {
        progress(req, "Device %s is powered on (%.2f s).", g->pci, (double)(now_ms() - t0) / 1000.0);
        if (g->console) {
            console_leave();
            g->console = false;
        }
    }
    {
        pid_t unused[1];

        find_holders(g, unused, 1); /* refresh the node cache for the returned device */
    }
    send_result(req, !failed[0], "", failed[0] ? failed : "attached", g);
}

/* A device that comes back on its own is treated as an attach request. */
static void check_reappeared(void)
{
    for (int i = 0; i < ngpus; i++) {
        struct gpu *g = &gpus[i];

        if (busy)
            continue;
        if (g->state == GS_LOST) {
            if (!g->seen)
                continue;
        } else if (!g->backend || g->backend->strategy != RS_UNBIND || g->backend->dry_run ||
                   (g->state != GS_POWERED_OFF && g->state != GS_SAFE_TO_REMOVE)) {
            continue;
        }
        if (!gpu_returned(g))
            continue;
        logmsg("%s reappeared; attaching", g->pci);
        busy = true;
        do_attach(NULL, g, true, "the device reappeared");
        busy = false;
    }
}

/* ---- wake requests and the idle timer ----------------------------------------------- */

/*
 * A device switched off on request stays that way. The display server cannot
 * be left waiting on it, though, and calls into the driver for reasons of its
 * own (a client that once used the device closes a window, say). So the device
 * is powered for as long as that call takes and switched off again, without
 * undoing anything else: it stays hidden, its services stay stopped, what was
 * frozen stays frozen.
 */
#define SERVE_MS 1500
#define SERVE_MAX_MS 60000

static void serve_begin(struct gpu *g, const char *who)
{
    char err[ZSSD_ERR] = "";
    long long t = now_ms();

    /* Calls that keep coming close together get a longer stay rather than a power cycle each. */
    if (g->serve_ended && t - g->serve_ended < 20000)
        g->serve_for = g->serve_for * 2 > SERVE_MAX_MS ? SERVE_MAX_MS : g->serve_for * 2;
    else
        g->serve_for = SERVE_MS;
    if (g->backend->power_on(g, err) < 0 || driver_resume(g, err) < 0) {
        logmsg("%s: could not be powered for %s: %s", g->pci, who, err);
        return;
    }
    g->serving = true;
    g->served++;
    g->serve_until = now_ms() + g->serve_for;
    progress(NULL, "%s needed %s: on after %.2f s, off again in %.1f s (%d so far; to keep it on: zssctl on %s)", who, g->pci,
             (double)(now_ms() - t) / 1000.0, (double)g->serve_for / 1000.0, g->served, g->pci);
}

static void serve_end(struct gpu *g)
{
    char err[ZSSD_ERR] = "", which[64] = "";

    if (gpu_drives_display(g, which, sizeof(which))) {
        snprintf(err, sizeof(err), "it now drives a display (%s)", which);
    } else if (driver_suspend(g, err) == 0) {
        if (g->backend->power_off(g, err) == 0 && g->backend->is_powered(g) == 0) {
            g->serving = false;
            g->serve_ended = now_ms();
            g->wake_base = gpu_wake_count(g);
            return;
        }
        if (!err[0])
            snprintf(err, sizeof(err), "the power backend did not confirm power-off");
        g->backend->power_on(g, which);
        driver_resume(g, which);
    }
    /* It cannot go off again: give it back completely rather than leave it half hidden. */
    busy = true;
    do_attach(NULL, g, false, err);
    busy = false;
}

/* Something called into the suspended driver, or a key was pressed on the console. */
static void check_wake(void)
{
    for (int i = 0; i < ngpus; i++) {
        struct gpu *g = &gpus[i];
        long long lasted;
        const char *why = NULL;
        static char who[128];
        pid_t waiters[16];
        int nw;
        long count;

        if (busy || !g->off || g->state != GS_POWERED_OFF)
            continue;
        if (g->serving) {
            if (now_ms() >= g->serve_until)
                serve_end(g);
            continue;
        }
        count = gpu_wake_read(g, waiters, 16, &nw);
        /* Callers that gave up are no longer waiting. */
        if (nw >= 0 && nw < g->nwaiting && count == g->wake_base) {
            memcpy(g->waiting, waiters, sizeof(pid_t) * (size_t)nw);
            g->nwaiting = nw;
        }
        if (count >= 0 && g->wake_base >= 0 && count != g->wake_base) {
            /*
             * Switched off by the idle timer, the device comes back for anyone.
             * Switched off on request it stays off: other callers sleep in the
             * driver until it is switched on. Only the display server is let
             * through, because the whole desktop stops while it waits.
             */
            if (g->auto_off && nw > 0) {
                char comm[64];

                pid_comm(waiters[0], comm, sizeof(comm));
                snprintf(who, sizeof(who), "%s (pid %d) called into the suspended driver", comm, waiters[0]);
                why = who;
            } else if (g->auto_off || nw < 0) {
                why = "a program called into the suspended driver";
            }
            for (int k = 0; !why && k < nw; k++) {
                char comm[64];

                pid_comm(waiters[k], comm, sizeof(comm));
                if (is_display_server(waiters[k], comm)) {
                    serve_begin(g, comm);
                    break;
                }
            }
            if (g->serving)
                continue;
            if (!why) {
                for (int k = 0; k < nw; k++) {
                    char comm[64];
                    bool known = false;

                    for (int j = 0; j < g->nwaiting; j++)
                        known |= g->waiting[j] == waiters[k];
                    if (known)
                        continue;
                    pid_comm(waiters[k], comm, sizeof(comm));
                    progress(NULL, "%s (pid %d) asked for %s, which was switched off on request; it waits until: zssctl on %s",
                             comm, waiters[k], g->pci, g->pci);
                }
                memcpy(g->waiting, waiters, sizeof(pid_t) * (size_t)nw);
                g->nwaiting = nw;
                g->wake_base = count;
            }
        } else if (console_key && g->console)
            why = "a key was pressed";
        if (!why)
            continue;

        lasted = now_ms() - g->off_since;
        g->wakes++;
        /* Three automatic power-offs in a row that each ended quickly: wait longer next time. */
        if (g->auto_off && lasted < zssd_cfg.quick_wake) {
            if (++g->quick_wakes >= 3) {
                long long base = g->idle_wait ? g->idle_wait : zssd_cfg.idle_timeout;

                g->idle_wait = base * 2 > 3600000 ? 3600000 : base * 2;
                g->quick_wakes = 0;
                logmsg("%s keeps being woken; next automatic power-off after %.0f s", g->pci,
                       (double)g->idle_wait / 1000.0);
            }
        } else if (g->auto_off) {
            g->quick_wakes = 0;
            g->idle_wait = 0;
        }
        busy = true;
        do_attach(NULL, g, false, why);
        busy = false;
    }
    console_key = false;
    wake_event = false;
}

/* Whether anything is using the device right now, as far as an automatic power-off is concerned. */
static bool gpu_in_use(struct gpu *g)
{
    int gi = (int)(g - gpus);
    char which[64];

    pid_t holders[64];
    int nh;

    if (gpu_drives_display(g, which, sizeof(which)))
        return true;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0 && clients[i].registered && clients[i].view[gi].devices > 0)
            return true;
    /* A process outside the layer with the device open is using it, whatever it is doing. */
    nh = find_holders(g, holders, 64);
    for (int i = 0; i < nh; i++) {
        char comm[64];

        if (client_by_pid(holders[i]))
            continue;
        pid_comm(holders[i], comm, sizeof(comm));
        if (!is_display_server(holders[i], comm) && !service_listed(comm))
            return true;
    }
    return false;
}

static void check_idle(void)
{
    for (int i = 0; i < ngpus; i++) {
        struct gpu *g = &gpus[i];
        long long wait = g->idle_wait ? g->idle_wait : zssd_cfg.idle_timeout;
        char before[sizeof(g->last_refusal)];

        if (zssd_cfg.idle_timeout <= 0 || busy || g->state != GS_ATTACHED || g->dry_detached || !g->backend ||
            g->backend->strategy != RS_SUSPEND || g->backend->dry_run)
            continue;
        if (!g->idle_since || gpu_in_use(g)) {
            g->idle_since = now_ms();
            continue;
        }
        if (now_ms() - g->idle_since < wait)
            continue;

        snprintf(before, sizeof(before), "%s", g->last_refusal);
        busy = true;
        if (!do_release(NULL, g, "", true, false, true)) {
            /* Say why once, then keep quiet until the reason changes. */
            if (strcmp(before, g->last_refusal))
                logmsg("%s: idle, but not powered off: %s", g->pci, g->last_refusal);
            g->idle_since = now_ms();
        }
        busy = false;
    }
}

/* ---- loss -------------------------------------------------------------------------- */

/*
 * The device is gone, or a client says its driver lost it. Nothing can be
 * read from it any more, so its applications are told to rebuild elsewhere
 * from what they hold: on another GPU if it has left the bus, on the same
 * one if it is still there (a driver reset).
 */
static void do_evacuate(struct gpu *g)
{
    int gi = (int)(g - gpus), nh;
    bool gone = !gpu_on_bus(g);
    pid_t holders[128];
    char target[64];

    g->loss_pending = false;
    g->evacuating = true;
    if (gone) {
        g->dry_detached = false;
        set_state(g, GS_LOST);
        choose_target(g, "", target, sizeof(target));
    } else {
        snprintf(target, sizeof(target), "%.15s", g->pci);
    }
    logmsg("%s: evacuating applications to %s", g->pci, target[0] ? target : "nowhere (they will park)");

    nh = find_holders(g, holders, 128);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = &clients[i];
        struct zj_out o;

        c->outcome = OC_NONE;
        if (c->fd < 0 || !wants_migrate(c, gi, holders, nh))
            continue;
        zj_begin(&o, "evacuate");
        zj_add_str(&o, "from", g->pci);
        zj_add_str(&o, "to", target);
        ask(c, &o);
    }
    wait_outcomes();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = &clients[i];

        if (c->fd < 0 || c->outcome == OC_NONE)
            continue;
        if (c->outcome == OC_FAILED) {
            logmsg("%s (pid %d) could not be recovered: %s", c->name, c->pid, c->outcome_reason);
            continue;
        }
        logmsg("%s (pid %d) %s; lost contents: %d", c->name, c->pid,
               c->outcome == OC_PARKED ? "parked" : "recovered", c->lost_contents);
        if (c->outcome == OC_PARKED && !c->frozen && cgroup_freeze(c->pid) == 0)
            c->frozen = true;
    }
    g->evacuating = false;
    g->loss_pending = false; /* reports that arrived meanwhile were about this same loss */
}

/* Looks for devices that left or returned, and runs any evacuation that is due. */
static void check_bus(void)
{
    bus_changed = false;
    for (int i = 0; i < ngpus; i++) {
        struct gpu *g = &gpus[i];
        bool present = gpu_on_bus(g);

        if (present)
            g->seen = true;
        else if (!busy && g->seen && g->state == GS_ATTACHED)
            g->loss_pending = true;
    }
    for (int i = 0; i < ngpus; i++) {
        if (busy || !gpus[i].loss_pending)
            continue;
        busy = true;
        do_evacuate(&gpus[i]);
        busy = false;
    }
    if (auto_attach)
        check_reappeared();
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
        if (gpu_on_bus(g) && g->state == GS_ATTACHED)
            pci_driver(g->pci, g->driver, sizeof(g->driver));
        zj_add_bool(&o, "wake_support", gpu_wake_supported(g));
        zj_add_bool(&o, "driver_frozen", gpu_driver_frozen(g));
        {
            /* Whether the kernel confines the device's memory access; only known while it is on the bus. */
            char group[300];

            snprintf(group, sizeof(group), "/sys/bus/pci/devices/%.15s/iommu_group", g->pci);
            zj_add_bool(&o, "iommu", path_exists(group));
        }
        zj_add_int(&o, "wakes", g->wakes);
        zj_add_int(&o, "waiting", g->off ? g->nwaiting : 0);
        zj_add_int(&o, "served", g->off ? g->served : 0);
        zj_add_bool(&o, "serving", g->serving);
        zj_add_int(&o, "idle_wait", (g->idle_wait ? g->idle_wait : zssd_cfg.idle_timeout) / 1000);
        zss_send(req->fd, &o);

        nh = find_holders(g, holders, 128);
        for (int i = 0; i < nh; i++) {
            char comm[64];

            if (client_by_pid(holders[i]))
                continue;
            pid_comm(holders[i], comm, sizeof(comm));
            zj_begin(&o, "client");
            zj_add_str(&o, "gpu", g->pci);
            zj_add_int(&o, "pid", holders[i]);
            zj_add_str(&o, "name", comm);
            if (!gpu_on_bus(g) && g->seen) {
                zj_add_str(&o, "class", "stale");
                zj_add_str(&o, "reason", "holds a handle to a device that is gone");
            } else {
                zj_add_str(&o, "class", is_display_server(holders[i], comm) ? "display-server" : "non-migratable");
                zj_add_str(&o, "reason", is_display_server(holders[i], comm) ? "" : "not started under the ZSS layer");
            }
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
        c->lost_contents = (int)zj_int(m, "lost_contents", 0);
    } else if (zj_is(m, "lost")) {
        /* Served from the main loop; a second report during an evacuation is the same loss. */
        g = gpu_find(zj_str(m, "gpu", ""));
        if (g && !g->evacuating && (g->state == GS_ATTACHED || g->state == GS_LOST)) {
            logmsg("%s (pid %d) reports that %s was lost", c->name, c->pid, g->pci);
            g->loss_pending = true;
        }
    } else if (zj_is(m, "subscribe")) {
        c->subscribed = true;
    } else if (zj_is(m, "status")) {
        do_status(c, zj_str(m, "gpu", ""));
    } else if (zj_is(m, "detach") || zj_is(m, "attach") || zj_is(m, "resume") || zj_is(m, "off") || zj_is(m, "on")) {
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
                do_release(c, g, zj_str(m, "to", ""), false, false, false);
            } else if (zj_is(m, "off")) {
                do_release(c, g, zj_str(m, "to", ""), true, zj_bool(m, "console", false), false);
            } else if (zj_is(m, "on")) {
                /* A lost device may simply have lost its power; the kernel module can try to give it back. */
                if (g->state != GS_POWERED_OFF && !(g->state == GS_LOST && g->kmod))
                    send_result(c, false, "", "the device is not powered off", g);
                else
                    do_attach(c, g, zj_bool(m, "return", false), "requested");
            } else {
                do_attach(c, g, true, "requested");
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
    g->seen = pci_present(g->pci);
    g->nnodes = gpu_nodes(g->pci, g->nodes, 32);
    g->wake_fd = -1;
    pci_driver(g->pci, g->driver, sizeof(g->driver));
    gpu_describe(g);
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
    fputs("usage: zssd [--config FILE] [--gpu PCI[=BACKEND]]... [options]\n"
          "       zssd --recover [same options]\n"
          "\n"
          "Settings come from the configuration file (default /etc/zss/zssd.conf, if it\n"
          "exists) and then from the command line. Every option is also a line in the\n"
          "file, written with underscores: idle_timeout = 300\n"
          "\n"
          "  --gpu PCI[=BACKEND]   manage this GPU; BACKEND is apple-gmux, pciehp-slot or\n"
          "                        dry-run, and is detected when left out\n"
          "  --socket PATH         where to listen (default /run/zss/zssd.sock)\n"
          "  --group NAME          group allowed to switch devices (default zss)\n"
          "  --idle-timeout SEC    power a device off after this long unused (default: never)\n"
          "  --stop-services LIST  units stopped around a power-off (default nvidia-persistenced)\n"
          "  --kmod-backend NAME   power backend the kernel module is told to use (default: it chooses)\n"
          "  --hide-while-off WHAT auto (default), no, or extra paths hidden while a device is off on request\n"
          "  --default-target T    where applications are sent when a request names no target\n"
          "  --allow-software      let applications fall back to a software renderer\n"
          "  --no-owner-access     accept requests only from root and the group\n"
          "  --no-auto-attach      wait for a command when a removed device returns\n"
          "  --runtime-dir DIR     markers for recovery (default /run/zss)\n"
          "  --recover             power on any device a previous daemon left off, then exit\n",
          stderr);
}

static bool truthy(const char *v)
{
    return !v[0] || !strcmp(v, "1") || !strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "on");
}

static const char *cfg_socket, *cfg_group = "zss";

/* One setting, by its configuration-file name. Returns -1 if the name is unknown. */
static int set_option(const char *key, const char *value)
{
    char *v = strdup(value);

    if (!strcmp(key, "gpu"))
        return add_gpu(v);
    else if (!strcmp(key, "socket"))
        cfg_socket = v;
    else if (!strcmp(key, "group"))
        cfg_group = v;
    else if (!strcmp(key, "default_target"))
        default_target = v;
    else if (!strcmp(key, "stop_services"))
        zssd_cfg.stop_services = v;
    else if (!strcmp(key, "kmod_backend"))
        zssd_cfg.kmod_backend = v;
    else if (!strcmp(key, "hide_while_off"))
        zssd_cfg.hide_while_off = v;
    else if (!strcmp(key, "runtime_dir"))
        zssd_cfg.runtime_dir = v;
    else if (!strcmp(key, "wake_file"))
        zssd_cfg.wake_file = v;
    else if (!strcmp(key, "service_cmd"))
        zssd_cfg.service_cmd = v;
    else if (!strcmp(key, "idle_timeout"))
        zssd_cfg.idle_timeout = (long long)(strtod(v, NULL) * 1000.0);
    else if (!strcmp(key, "quick_wake"))
        zssd_cfg.quick_wake = (long long)(strtod(v, NULL) * 1000.0);
    else if (!strcmp(key, "allow_software"))
        allow_software = truthy(v);
    else if (!strcmp(key, "owner_access"))
        owner_access = truthy(v);
    else if (!strcmp(key, "auto_attach"))
        auto_attach = truthy(v);
    else
        return -1;
    return 0;
}

static int load_config(const char *path, bool required)
{
    char line[512];
    FILE *f = fopen(path, "r");
    int n = 0;

    if (!f) {
        if (required)
            logmsg("cannot read %s: %s", path, strerror(errno));
        return required ? -1 : 0;
    }
    while (fgets(line, sizeof(line), f)) {
        char *key = line + strspn(line, " \t"), *eq, *value, *end;

        n++;
        key[strcspn(key, "#\n")] = '\0';
        if (!key[0])
            continue;
        eq = strchr(key, '=');
        if (!eq) {
            logmsg("%s:%d: expected 'name = value'", path, n);
            fclose(f);
            return -1;
        }
        *eq = '\0';
        value = eq + 1 + strspn(eq + 1, " \t");
        for (end = key + strlen(key); end > key && (end[-1] == ' ' || end[-1] == '\t'); end--)
            end[-1] = '\0';
        for (end = value + strlen(value); end > value && (end[-1] == ' ' || end[-1] == '\t'); end--)
            end[-1] = '\0';
        if (set_option(key, value) < 0) {
            logmsg("%s:%d: unknown or invalid setting '%s'", path, n, key);
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}

/*
 * A previous daemon may have died with a device off, leaving callers asleep
 * on its suspended driver. Put every such device back.
 */
static int recover(void)
{
    int found = 0;

    for (int i = 0; i < ngpus; i++) {
        struct gpu *g = &gpus[i];
        char err[ZSSD_ERR] = "";

        /* Whatever a dead daemon hid comes back, whether or not it got as far as cutting power. */
        if (gpu_unhide(g))
            logmsg("%s: made visible again", g->pci);
        if (!g->backend || !marker_read(g))
            continue;
        found++;
        logmsg("%s was left powered off; restoring it", g->pci);
        if (g->backend->power_on(g, err) < 0 || driver_resume(g, err) < 0)
            logmsg("%s: %s", g->pci, err);
        services_start(g);
        for (int k = 0; k < g->nfrozen; k++)
            cgroup_thaw(g->frozen[k]);
        g->nfrozen = 0;
        marker_remove(g);
    }
    if (!found)
        logmsg("nothing to recover");
    return 0;
}

int main(int argc, char **argv)
{
    static const char *const flags[] = { "allow-software", "no-owner-access", "no-auto-attach" };
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    const char *config = NULL, *path;
    bool recover_only = false;
    struct group *gr;
    char dir[108], *slash;
    long long last_check = 0;

    for (int i = 0; i < MAX_CLIENTS; i++)
        clients[i].fd = -1;

    /* The file first, so the command line can add to it and override it. */
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--config") && i + 1 < argc)
            config = argv[i + 1];
    if (load_config(config ? config : "/etc/zss/zssd.conf", config != NULL) < 0)
        return 2;

    for (int i = 1; i < argc; i++) {
        char key[64];
        bool flag = false;

        if (strncmp(argv[i], "--", 2)) {
            usage();
            return 2;
        }
        if (!strcmp(argv[i], "--config")) {
            i++;
            continue;
        }
        if (!strcmp(argv[i], "--recover")) {
            recover_only = true;
            continue;
        }
        snprintf(key, sizeof(key), "%s", argv[i] + 2);
        for (char *c = key; *c; c++)
            if (*c == '-')
                *c = '_';
        for (size_t f = 0; f < sizeof(flags) / sizeof(flags[0]); f++)
            if (!strcmp(argv[i] + 2, flags[f]))
                flag = true;
        if (flag) {
            /* "--no-x" turns x off; "--x" turns it on. */
            if (!strncmp(key, "no_", 3))
                set_option(key + 3, "no");
            else
                set_option(key, "yes");
            continue;
        }
        if (i + 1 >= argc || set_option(key, argv[i + 1]) < 0) {
            usage();
            return 2;
        }
        i++;
    }
    if (ngpus == 0) {
        usage();
        return 2;
    }
    if (recover_only)
        return recover();

    path = cfg_socket ? cfg_socket : zss_socket_path();
    gr = getgrnam(cfg_group);
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
    if (zssd_cfg.idle_timeout > 0)
        logmsg("idle devices are powered off after %.0f s", (double)zssd_cfg.idle_timeout / 1000.0);

    /* A device left off by a daemon that died is put back before anything else. */
    recover();

    /* Wakes the loop when a device is added or removed; the periodic check is the fallback. */
    uevent_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    if (uevent_fd >= 0) {
        struct sockaddr_nl nl = { .nl_family = AF_NETLINK, .nl_groups = 1 };

        if (bind(uevent_fd, (struct sockaddr *)&nl, sizeof(nl)) < 0) {
            close(uevent_fd);
            uevent_fd = -1;
        }
    }

    for (;;) {
        bool watching = false, reading = false, due;

        /*
         * While a device is off, the loop sleeps on its wake file if that can be
         * waited on; if it is only a counter, the counter is read often instead.
         */
        for (int i = 0; i < ngpus; i++) {
            if (gpus[i].off) {
                watching = true;
                if (gpus[i].wake_fd < 0 && gpus[i].wake_base >= 0)
                    reading = true;
            }
        }
        if (!pump(reading ? 50 : 200))
            break;
        if (watching || wake_event || console_key)
            check_wake();

        due = bus_changed || now_ms() - last_check >= 1000;
        for (int i = 0; i < ngpus; i++)
            if (gpus[i].loss_pending)
                due = true;
        if (due) {
            last_check = now_ms();
            check_bus();
            check_idle();
        }
    }

    /* Nothing may be left asleep on a suspended driver once the daemon is gone. */
    for (int i = 0; i < ngpus; i++)
        if (gpus[i].off)
            do_attach(NULL, &gpus[i], false, "the daemon is stopping");
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0)
            client_drop(&clients[i]);
    unlink(path);
    return 0;
}
