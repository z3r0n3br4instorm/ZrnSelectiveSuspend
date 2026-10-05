// SPDX-License-Identifier: GPL-2.0
/*
 * The gate. Every entry point passes through it, so a migration can wait
 * until no application thread is inside the layer and keep it that way.
 * While an application is parked the gate simply stays closed.
 */
#include "zss_layer.h"

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int active;
static bool held;

void zss_enter(void)
{
    pthread_mutex_lock(&gate);
    while (held)
        pthread_cond_wait(&gate_cv, &gate);
    active++;
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

void zss_hold_end(void)
{
    pthread_mutex_lock(&gate);
    held = false;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate);
}
