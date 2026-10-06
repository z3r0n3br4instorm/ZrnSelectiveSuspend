// SPDX-License-Identifier: GPL-2.0-only
/*
 * Retention store.
 *
 *   $XDG_CACHE_HOME/zss/store/<hash>-<size>     one file per distinct blob
 *   $XDG_CACHE_HOME/zss/store/pin-<pid>/<name>  hard links to blobs a process needs
 *
 * The thread that submits an upload only hashes it and, if the blob is new,
 * copies it into a bounded queue. One writer thread moves the queue to disk.
 * Trimming removes the oldest blobs that nothing pins, so a reader never
 * loses a file it depends on.
 *
 * ZSS_RETAIN=disk|ram|off   ZSS_RETAIN_LIMIT_MB (4096)   ZSS_RETAIN_QUEUE_MB (256)
 */
#include "zss_retain.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "zss_hash.h"

#define BUCKETS 1024
#define TRIM_EVERY (32ull << 20)

enum mode { RM_OFF, RM_RAM, RM_DISK };

struct entry {
    struct entry *next, *qnext;
    uint8_t hash[16];
    uint64_t size;
    uint32_t refs;
    uint8_t *data; /* held while queued, and for good in RAM mode */
    bool queued;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER, drained = PTHREAD_COND_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static struct entry *table[BUCKETS], *qhead, *qtail;
static enum mode mode = RM_DISK;
static uint64_t queue_bytes, queue_limit = 256ull << 20, store_limit = 4096ull << 20, since_trim;
static char store_dir[512], pin_dir[600];
static bool writer_started;

static void name_of(const uint8_t hash[16], uint64_t size, char *out, size_t n)
{
    char hex[33];

    for (int i = 0; i < 16; i++)
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    snprintf(out, n, "%s-%llu", hex, (unsigned long long)size);
}

static void blob_path(const uint8_t hash[16], uint64_t size, const char *dir, char *out, size_t n)
{
    char name[64];

    name_of(hash, size, name, sizeof(name));
    snprintf(out, n, "%s/%s", dir, name);
}

static void cleanup(void)
{
    struct dirent *de;
    char path[900];
    DIR *d;

    if (mode != RM_DISK || !pin_dir[0])
        return;
    zss_retain_flush(2000);
    d = opendir(pin_dir);
    while (d && (de = readdir(d))) {
        if (de->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s", pin_dir, de->d_name);
        unlink(path);
    }
    if (d)
        closedir(d);
    rmdir(pin_dir);
}

static void init(void)
{
    const char *m = getenv("ZSS_RETAIN"), *cache = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    const char *lim = getenv("ZSS_RETAIN_LIMIT_MB"), *q = getenv("ZSS_RETAIN_QUEUE_MB");
    char base[400];

    if (m && !strcmp(m, "off"))
        mode = RM_OFF;
    else if (m && !strcmp(m, "ram"))
        mode = RM_RAM;
    if (lim)
        store_limit = strtoull(lim, NULL, 10) << 20;
    if (q)
        queue_limit = strtoull(q, NULL, 10) << 20;
    if (mode != RM_DISK)
        return;

    if (cache && *cache)
        snprintf(base, sizeof(base), "%s/zss", cache);
    else
        snprintf(base, sizeof(base), "%s/.cache/zss", home ? home : "/tmp");
    mkdir(base, 0700);
    snprintf(store_dir, sizeof(store_dir), "%s/store", base);
    mkdir(store_dir, 0700);
    snprintf(pin_dir, sizeof(pin_dir), "%s/pin-%d", store_dir, (int)getpid());
    if (mkdir(pin_dir, 0700) < 0 && errno != EEXIST) {
        /* No usable store: keep working, in memory. */
        mode = RM_RAM;
        return;
    }
    atexit(cleanup);
}

static struct entry **slot(const uint8_t hash[16], uint64_t size)
{
    uint32_t h;
    struct entry **pp;

    memcpy(&h, hash, sizeof(h));
    for (pp = &table[h % BUCKETS]; *pp; pp = &(*pp)->next)
        if ((*pp)->size == size && !memcmp((*pp)->hash, hash, 16))
            break;
    return pp;
}

static void pin(const struct entry *e)
{
    char src[700], dst[700];

    blob_path(e->hash, e->size, store_dir, src, sizeof(src));
    blob_path(e->hash, e->size, pin_dir, dst, sizeof(dst));
    link(src, dst);
}

static void unpin(const struct entry *e)
{
    char dst[700];

    if (mode != RM_DISK)
        return;
    blob_path(e->hash, e->size, pin_dir, dst, sizeof(dst));
    unlink(dst);
}

/* Unlinks and frees an entry nobody refers to. Called with the lock held. */
static void drop(struct entry *e)
{
    struct entry **pp = slot(e->hash, e->size);

    if (*pp == e)
        *pp = e->next;
    unpin(e);
    free(e->data);
    free(e);
}

static bool write_blob(const struct entry *e)
{
    char path[700], tmp[760];
    size_t off = 0;
    int fd;

    blob_path(e->hash, e->size, store_dir, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp-%d", path, (int)getpid());
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    while (off < e->size) {
        ssize_t n = write(fd, e->data + off, e->size - off);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    close(fd);
    if (off != e->size || rename(tmp, path) < 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

static void *writer(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lock);
    for (;;) {
        struct entry *e;
        bool ok, trim;

        while (!qhead) {
            pthread_cond_broadcast(&drained);
            pthread_cond_wait(&wake, &lock);
        }
        e = qhead;
        pthread_mutex_unlock(&lock);
        /* e->data stays put while the entry is queued. */
        ok = write_blob(e);
        pthread_mutex_lock(&lock);

        qhead = e->qnext;
        if (!qhead)
            qtail = NULL;
        e->queued = false;
        queue_bytes -= e->size;
        if (ok) {
            pin(e);
            free(e->data);
            e->data = NULL;
            since_trim += e->size;
        }
        /* If the write failed the data simply stays in memory. */
        if (e->refs == 0)
            drop(e);
        trim = since_trim >= TRIM_EVERY;
        if (trim) {
            since_trim = 0;
            pthread_mutex_unlock(&lock);
            zss_retain_trim();
            pthread_mutex_lock(&lock);
        }
    }
    return NULL;
}

bool zss_retain_put(const void *data, size_t size, struct zss_ret *out)
{
    struct entry **pp, *e;
    uint8_t hash[16];
    char path[700];

    pthread_once(&once, init);
    out->valid = false;
    if (mode == RM_OFF || !size)
        return false;
    zss_hash128(data, size, hash);

    pthread_mutex_lock(&lock);
    pp = slot(hash, size);
    e = *pp;
    if (!e) {
        bool on_disk = false;

        if (mode == RM_DISK) {
            blob_path(hash, size, store_dir, path, sizeof(path));
            on_disk = access(path, R_OK) == 0;
        }
        if (!on_disk && mode == RM_DISK && queue_bytes + size > queue_limit) {
            pthread_mutex_unlock(&lock);
            return false;
        }
        e = calloc(1, sizeof(*e));
        memcpy(e->hash, hash, 16);
        e->size = size;
        *pp = e;
        if (on_disk) {
            /* Already stored by this or an earlier run: nothing to write. */
            pin(e);
            utimensat(AT_FDCWD, path, NULL, 0);
        } else {
            e->data = malloc(size);
            memcpy(e->data, data, size);
            if (mode == RM_DISK) {
                e->queued = true;
                queue_bytes += size;
                if (qtail)
                    qtail->qnext = e;
                else
                    qhead = e;
                qtail = e;
                if (!writer_started) {
                    pthread_t tid;

                    writer_started = pthread_create(&tid, NULL, writer, NULL) == 0;
                    if (writer_started)
                        pthread_detach(tid);
                }
                pthread_cond_signal(&wake);
            }
        }
    }
    e->refs++;
    pthread_mutex_unlock(&lock);

    memcpy(out->hash, hash, 16);
    out->size = size;
    out->valid = true;
    return true;
}

bool zss_retain_get(const struct zss_ret *ret, void *dst)
{
    struct entry *e;
    char path[700];
    size_t off = 0;
    int fd;

    if (!ret->valid)
        return false;
    pthread_mutex_lock(&lock);
    e = *slot(ret->hash, ret->size);
    if (e && e->data) {
        memcpy(dst, e->data, ret->size);
        pthread_mutex_unlock(&lock);
        return true;
    }
    pthread_mutex_unlock(&lock);
    if (mode != RM_DISK)
        return false;

    /* Read through the pin, which survives trimming. */
    blob_path(ret->hash, ret->size, pin_dir, path, sizeof(path));
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        blob_path(ret->hash, ret->size, store_dir, path, sizeof(path));
        fd = open(path, O_RDONLY | O_CLOEXEC);
    }
    if (fd < 0)
        return false;
    while (off < ret->size) {
        ssize_t n = read(fd, (uint8_t *)dst + off, ret->size - off);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    close(fd);
    return off == ret->size;
}

void zss_retain_release(struct zss_ret *ret)
{
    struct entry *e;

    if (!ret->valid)
        return;
    ret->valid = false;
    pthread_mutex_lock(&lock);
    e = *slot(ret->hash, ret->size);
    /* A queued entry is dropped by the writer once it is done with it. */
    if (e && --e->refs == 0 && !e->queued)
        drop(e);
    pthread_mutex_unlock(&lock);
}

void zss_retain_flush(int timeout_ms)
{
    struct timespec until;

    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeout_ms / 1000;
    until.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
    if (until.tv_nsec >= 1000000000) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&lock);
    while (qhead && pthread_cond_timedwait(&drained, &lock, &until) == 0)
        ;
    pthread_mutex_unlock(&lock);
}

struct victim {
    char name[80];
    off_t size;
    time_t mtime;
};

static int by_age(const void *a, const void *b)
{
    const struct victim *x = a, *y = b;

    return x->mtime < y->mtime ? -1 : x->mtime > y->mtime;
}

static void remove_dead_pins(const char *name)
{
    char path[1100], file[1400], proc[300];
    struct dirent *de;
    DIR *d;

    snprintf(proc, sizeof(proc), "/proc/%s", name + 4);
    if (access(proc, F_OK) == 0)
        return;
    snprintf(path, sizeof(path), "%s/%s", store_dir, name);
    d = opendir(path);
    while (d && (de = readdir(d))) {
        if (de->d_name[0] == '.')
            continue;
        snprintf(file, sizeof(file), "%s/%s", path, de->d_name);
        unlink(file);
    }
    if (d)
        closedir(d);
    rmdir(path);
}

void zss_retain_trim(void)
{
    struct victim *v = NULL;
    size_t n = 0, cap = 0;
    uint64_t total = 0;
    struct dirent *de;
    DIR *d;

    pthread_once(&once, init);
    if (mode != RM_DISK)
        return;
    d = opendir(store_dir);
    if (!d)
        return;
    while ((de = readdir(d))) {
        if (!strncmp(de->d_name, "pin-", 4))
            remove_dead_pins(de->d_name);
    }
    rewinddir(d);
    while ((de = readdir(d))) {
        char path[1100];
        struct stat st;

        if (de->d_name[0] == '.' || !strncmp(de->d_name, "pin-", 4))
            continue;
        snprintf(path, sizeof(path), "%s/%s", store_dir, de->d_name);
        if (stat(path, &st) < 0 || !S_ISREG(st.st_mode))
            continue;
        total += (uint64_t)st.st_size;
        /* A second link is some process's pin: that blob is in use. */
        if (st.st_nlink > 1 || strlen(de->d_name) >= sizeof(v->name))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            v = realloc(v, cap * sizeof(*v));
        }
        snprintf(v[n].name, sizeof(v[n].name), "%s", de->d_name);
        v[n].size = st.st_size;
        v[n].mtime = st.st_mtime;
        n++;
    }
    closedir(d);

    qsort(v, n, sizeof(*v), by_age);
    for (size_t i = 0; i < n && total > store_limit; i++) {
        char path[1100];

        snprintf(path, sizeof(path), "%s/%s", store_dir, v[i].name);
        if (unlink(path) == 0)
            total -= (uint64_t)v[i].size;
    }
    free(v);
}
