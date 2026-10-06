// SPDX-License-Identifier: GPL-2.0-only
/*
 * Link to zssd: registers the process, reports which GPUs it holds, and
 * carries out migrate, restore and resume requests. Without a daemon the
 * layer still works; the application just cannot be migrated.
 */
#include "zss_layer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "zss_ipc.h"

static int sock = -1;
static struct zss_reader reader;
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;
static char detached[256]; /* comma-separated PCI addresses, from the welcome */
static char last_state[ZSS_MAX_GPUS][256];
static bool holding; /* gate held because something is parked */
/* One relocation at a time, whether the daemon asked for it or a thread hit a lost device. */
static pthread_mutex_t op_lock = PTHREAD_MUTEX_INITIALIZER;

bool zss_control_detached(const char *pci)
{
    return pci[0] && strstr(detached, pci) != NULL;
}

static void send_msg(struct zj_out *o)
{
    pthread_mutex_lock(&send_lock);
    if (sock >= 0)
        zss_send(sock, o);
    else
        free(zj_end(o));
    pthread_mutex_unlock(&send_lock);
}

void zss_control_state_changed(void)
{
    if (sock < 0)
        return;
    pthread_mutex_lock(&zss_lock);
    for (int g = 0; g < zss_ngpus; g++) {
        struct zss_gpu *gpu = zss_gpus[g];
        int devices = 0, origin = 0, parked = 0;
        bool migratable = true;
        const char *reason = "";
        char key[256];
        struct zj_out o;

        if (!gpu->pci[0])
            continue;
        for (struct zss_dev *d = zss_devices; d; d = d->next_dev) {
            if (d->gpu == gpu)
                devices++;
            if (d->origin == gpu) {
                origin++;
                if (!d->gpu)
                    parked++;
            }
            if (d->gpu == gpu && !d->migratable) {
                migratable = false;
                reason = d->reason;
            }
        }
        snprintf(key, sizeof(key), "%d/%d/%d/%d/%d/%s", zss_gpu_held(gpu), devices, origin, parked,
                 migratable, reason);
        if (!strcmp(key, last_state[g]))
            continue;
        snprintf(last_state[g], sizeof(last_state[g]), "%s", key);

        zj_begin(&o, "state");
        zj_add_str(&o, "gpu", gpu->pci);
        zj_add_bool(&o, "holds", zss_gpu_held(gpu) || devices > 0);
        zj_add_int(&o, "devices", devices);
        zj_add_int(&o, "origin", origin);
        zj_add_int(&o, "parked", parked);
        zj_add_bool(&o, "migratable", migratable);
        zj_add_str(&o, "reason", reason);
        send_msg(&o);
    }
    pthread_mutex_unlock(&zss_lock);
}

static struct zss_gpu *software_gpu(void)
{
    for (int i = 0; i < zss_ngpus; i++)
        if (zss_gpus[i]->software)
            return zss_gpus[i];
    return NULL;
}

static struct zss_gpu *resolve_target(const char *to)
{
    struct zss_gpu *gpu;

    if (!to[0])
        return NULL;
    if (!strcmp(to, "software"))
        return software_gpu();
    gpu = zss_gpu_by_pci(to);
    return gpu && !gpu->detached ? gpu : NULL;
}

static bool any_parked(void)
{
    for (struct zss_dev *d = zss_devices; d; d = d->next_dev)
        if (!d->gpu)
            return true;
    return false;
}

bool zss_control_connected(void)
{
    return sock >= 0;
}

/*
 * Closes the gate for a recovery. Unlike a migration this cannot wait for
 * ever: a thread may be stuck inside the dead driver. Returns false if the
 * grace period ran out with threads still inside.
 */
static bool hold_for_loss(void)
{
    const char *grace = getenv("ZSS_LOSS_GRACE_MS");
    bool empty = true;

    if (!holding)
        empty = zss_hold_begin_timed(grace ? atoi(grace) : 3000);
    holding = true;
    return empty;
}

static void hold(void)
{
    if (!holding)
        zss_hold_begin();
    holding = true;
}

/* Lets the application run again unless something is still parked. */
static void release(void)
{
    if (holding && !any_parked()) {
        holding = false;
        zss_hold_end();
    }
}

static int reply_lost_contents = -1; /* set by a recovery, sent with its outcome */

static void reply(long long id, const char *result, const char *target, const char *error,
                  const char *reason)
{
    struct zj_out o;

    zj_begin(&o, "outcome");
    zj_add_int(&o, "id", id);
    zj_add_str(&o, "result", result);
    zj_add_str(&o, "target", target);
    if (reply_lost_contents >= 0)
        zj_add_int(&o, "lost_contents", reply_lost_contents);
    reply_lost_contents = -1;
    if (error[0])
        zj_add_str(&o, "error", error);
    zj_add_str(&o, "reason", reason);
    send_msg(&o);
}

static const char *gpu_name(const struct zss_gpu *gpu)
{
    if (!gpu)
        return "";
    return gpu->pci[0] ? gpu->pci : "software";
}

static void do_migrate(const struct zj_msg *m)
{
    struct zss_gpu *from = zss_gpu_by_pci(zj_str(m, "from", ""));
    struct zss_gpu *target = resolve_target(zj_str(m, "to", ""));
    enum zss_outcome worst = ZO_MIGRATED;
    char reason[256] = "";

    if (!from) {
        reply(zj_int(m, "id", 0), "migrated", "", "", "this process does not use that GPU");
        return;
    }
    hold();
    for (struct zss_dev *d = zss_devices; d; d = d->next_dev) {
        enum zss_outcome out;

        if (d->gpu != from)
            continue;
        out = zss_migrate(d, target, reason, sizeof(reason));
        if (out == ZO_FAILED) {
            worst = ZO_FAILED;
            break;
        }
        if (out == ZO_PARKED)
            worst = ZO_PARKED;
    }
    if (worst != ZO_FAILED) {
        /* The GPU is going away: let go of it completely. */
        from->detached = true;
        zss_gpu_hold(from, false);
        zss_driver_close(from->drv);
    }
    release();
    reply(zj_int(m, "id", 0),
          worst == ZO_MIGRATED ? "migrated" : worst == ZO_PARKED ? "parked" : "failed",
          worst == ZO_MIGRATED ? gpu_name(target) : "", "", reason);
    zss_control_state_changed();
}

/*
 * The GPU is lost. Nothing can be read from it, so each device on it is
 * rebuilt from memory: on `to`, on the same GPU after a driver reset, or
 * parked if there is nowhere suitable.
 */
static void do_evacuate(const struct zj_msg *m)
{
    struct zss_gpu *from = zss_gpu_by_pci(zj_str(m, "from", ""));
    const char *to = zj_str(m, "to", "");
    struct zss_gpu *target = from && !strcmp(to, from->pci) ? from : resolve_target(to);
    enum zss_outcome worst = ZO_MIGRATED;
    char reason[256] = "";
    int lost = 0;
    bool empty;

    if (!from) {
        reply(zj_int(m, "id", 0), "migrated", "", "", "this process does not use that GPU");
        return;
    }
    empty = hold_for_loss();
    for (struct zss_dev *d = zss_devices; d; d = d->next_dev) {
        enum zss_outcome out;

        if (d->gpu != from)
            continue;
        /* A reset that hit some other application: this device is fine where it is. */
        if (target == from && !d->lost)
            continue;
        out = zss_recover(d, target, !empty, reason, sizeof(reason));
        lost += d->lost_contents;
        if (out == ZO_FAILED)
            worst = ZO_FAILED;
        else if (out == ZO_PARKED && worst != ZO_FAILED)
            worst = ZO_PARKED;
    }
    if (target != from) {
        from->detached = true;
        zss_gpu_hold(from, false);
        if (empty)
            zss_driver_close(from->drv);
    }
    release();
    reply_lost_contents = lost;
    reply(zj_int(m, "id", 0),
          worst == ZO_MIGRATED ? "migrated" : worst == ZO_PARKED ? "parked" : "failed",
          worst == ZO_MIGRATED ? gpu_name(target) : "", "", reason);
    zss_control_state_changed();
}

void zss_control_report_lost(struct zss_dev *dev)
{
    struct zj_out o;

    if (sock < 0 || !dev->gpu || !dev->gpu->pci[0])
        return;
    zj_begin(&o, "lost");
    zj_add_str(&o, "gpu", dev->gpu->pci);
    send_msg(&o);
}

/*
 * A thread hit a lost device and no daemon said where to go. Rebuild in
 * place if the GPU still answers, otherwise on any other suitable GPU. If
 * there is none, the device is left as it is and the application gets the
 * error, exactly as it would without the layer.
 */
void zss_control_recover_local(struct zss_dev *dev)
{
    bool allow_software = getenv("ZSS_ALLOW_SOFTWARE") != NULL;
    struct zss_gpu *target = NULL;
    char reason[256] = "";

    pthread_mutex_lock(&op_lock);
    if (!dev->lost || dev->dead || !dev->gpu) {
        pthread_mutex_unlock(&op_lock);
        return;
    }
    if (!dev->gpu->detached && zss_compatible(dev, dev->gpu, reason, sizeof(reason)))
        target = dev->gpu;
    for (int i = 0; i < zss_ngpus && !target; i++) {
        struct zss_gpu *g = zss_gpus[i];

        if (g->detached || g == dev->gpu || (g->software && !allow_software))
            continue;
        if (zss_compatible(dev, g, reason, sizeof(reason)))
            target = g;
    }
    if (!target) {
        zss_log("no GPU can take over from the lost device: %s", reason[0] ? reason : "none is available");
        zss_dev_unrecoverable(dev);
    } else {
        bool empty = hold_for_loss();

        zss_recover(dev, target, !empty, reason, sizeof(reason));
        release();
        zss_control_state_changed();
    }
    pthread_mutex_unlock(&op_lock);
}

/* The GPU is back: bring home everything that started on it. */
static void do_restore(const struct zj_msg *m)
{
    struct zss_gpu *gpu = zss_gpu_by_pci(zj_str(m, "gpu", ""));
    enum zss_outcome worst = ZO_MIGRATED;
    char reason[256] = "";

    if (!gpu) {
        reply(zj_int(m, "id", 0), "migrated", "", "", "");
        return;
    }
    gpu->detached = false;
    hold();
    for (struct zss_dev *d = zss_devices; d; d = d->next_dev) {
        enum zss_outcome out;

        if (d->origin != gpu || d->gpu == gpu)
            continue;
        out = d->gpu ? zss_migrate(d, gpu, reason, sizeof(reason))
                     : zss_resume(d, gpu, reason, sizeof(reason));
        if (out != ZO_MIGRATED)
            worst = ZO_FAILED;
    }
    release();
    reply(zj_int(m, "id", 0), worst == ZO_MIGRATED ? "migrated" : "failed", gpu_name(gpu), "", reason);
    zss_control_state_changed();
}

/* Tries to get parked devices running on whatever suitable GPU exists now. */
static bool resume_anywhere(char *reason, size_t rlen)
{
    bool allow_software = getenv("ZSS_ALLOW_SOFTWARE") != NULL;
    bool all = true;

    for (struct zss_dev *d = zss_devices; d; d = d->next_dev) {
        bool done = false;

        if (d->gpu)
            continue;
        if (!d->origin->detached)
            done = zss_resume(d, d->origin, reason, rlen) == ZO_MIGRATED;
        for (int i = 0; i < zss_ngpus && !done; i++) {
            struct zss_gpu *g = zss_gpus[i];

            if (g->detached || g == d->origin || (g->software && !allow_software))
                continue;
            done = zss_resume(d, g, reason, rlen) == ZO_MIGRATED;
        }
        if (!done)
            all = false;
    }
    return all;
}

static void do_resume(const struct zj_msg *m)
{
    char reason[256] = "";
    bool ok;

    hold();
    ok = resume_anywhere(reason, sizeof(reason));
    release();
    if (ok)
        reply(zj_int(m, "id", 0), "migrated", "", "", "");
    else
        reply(zj_int(m, "id", 0), "failed", "", ZSS_ERR_RESUME_NO_DRM,
              reason[0] ? reason : "no GPU is available for this application");
    zss_control_state_changed();
}

static void *control_thread(void *arg)
{
    struct zj_msg m;

    (void)arg;
    while (zss_recv(&reader, &m, -1) == 1) {
        pthread_mutex_lock(&op_lock);
        if (zj_is(&m, "migrate"))
            do_migrate(&m);
        else if (zj_is(&m, "evacuate"))
            do_evacuate(&m);
        else if (zj_is(&m, "restore"))
            do_restore(&m);
        else if (zj_is(&m, "resume"))
            do_resume(&m);
        pthread_mutex_unlock(&op_lock);
        zj_free(&m);
    }
    /* The daemon is gone. Do not leave a parked application stuck forever. */
    pthread_mutex_lock(&send_lock);
    close(sock);
    sock = -1;
    pthread_mutex_unlock(&send_lock);
    pthread_mutex_lock(&op_lock);
    if (holding) {
        char reason[256];

        for (int i = 0; i < zss_ngpus; i++)
            zss_gpus[i]->detached = false;
        resume_anywhere(reason, sizeof(reason));
        release();
    }
    pthread_mutex_unlock(&op_lock);
    return NULL;
}

void zss_control_start(void)
{
    char name[64] = "unknown";
    struct zj_out o;
    struct zj_msg m;
    pthread_t tid;
    FILE *f;

    sock = zss_connect(zss_socket_path());
    if (sock < 0) {
        zss_dbg("no daemon at %s; running without migration", zss_socket_path());
        return;
    }
    f = fopen("/proc/self/comm", "r");
    if (f) {
        if (fgets(name, sizeof(name), f))
            name[strcspn(name, "\n")] = '\0';
        fclose(f);
    }
    zss_reader_init(&reader, sock);
    zj_begin(&o, "register");
    zj_add_int(&o, "pid", getpid());
    zj_add_str(&o, "name", name);
    send_msg(&o);

    if (zss_recv(&reader, &m, 2000) == 1) {
        if (zj_is(&m, "welcome"))
            snprintf(detached, sizeof(detached), "%s", zj_str(&m, "detached", ""));
        zj_free(&m);
    }
    if (pthread_create(&tid, NULL, control_thread, NULL) == 0)
        pthread_detach(tid);
}
