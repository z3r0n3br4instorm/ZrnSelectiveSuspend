// SPDX-License-Identifier: GPL-2.0
/*
 * The gate. Every entry point passes through it, so a migration can wait
 * until no application thread is inside the layer and keep it that way.
 * While an application is parked the gate simply stays closed.
 */
#include "zss_layer.h"

#include <time.h>

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int active;
static bool held;

/*
 * Counts replacements of a real device. A thread notes the count when it
 * enters; if it differs later, the device it was calling into was replaced
 * while it was inside (it had been stuck in a dead driver and left behind).
 * Threads that never enter, such as the control thread, are never stale.
 */
uint32_t zss_epoch;
static __thread uint32_t entered_epoch = UINT32_MAX;

bool zss_stale(void)
{
    return entered_epoch != UINT32_MAX && entered_epoch != zss_epoch;
}

void zss_enter(void)
{
    pthread_mutex_lock(&gate);
    while (held)
        pthread_cond_wait(&gate_cv, &gate);
    active++;
    entered_epoch = zss_epoch;
    pthread_mutex_unlock(&gate);
}

void zss_leave(void)
{
    pthread_mutex_lock(&gate);
    if (--active == 0)
        pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate);
}

void zss_hold_begin(void)
{
    pthread_mutex_lock(&gate);
    held = true;
    while (active > 0)
        pthread_cond_wait(&gate_cv, &gate);
    pthread_mutex_unlock(&gate);
}

bool zss_hold_begin_timed(int ms)
{
    struct timespec until;
    bool empty;

    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000;
    until.tv_nsec += (long)(ms % 1000) * 1000000;
    if (until.tv_nsec >= 1000000000) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&gate);
    held = true;
    while (active > 0 && pthread_cond_timedwait(&gate_cv, &gate, &until) == 0)
        ;
    empty = active == 0;
    pthread_mutex_unlock(&gate);
    return empty;
}

void zss_hold_end(void)
{
    pthread_mutex_lock(&gate);
    held = false;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate);
}
