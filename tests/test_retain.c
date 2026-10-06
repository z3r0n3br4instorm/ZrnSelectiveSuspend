// SPDX-License-Identifier: GPL-2.0
/*
 * Unit tests for the hash and the retention store. The store reads its
 * settings once per process, so each case runs in a child process started
 * with the environment it needs:  test_retain <case> <cache-dir>
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "zss_hash.h"
#include "zss_retain.h"

static int failures;

#define EXPECT(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void fill(uint8_t *p, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)(seed = seed * 1103515245u + 12345u) >> 3;
}

/* Blobs in the store, not counting pin directories. */
static int blobs(const char *cache, off_t *bytes)
{
    char dir[600], path[900];
    struct dirent *de;
    struct stat st;
    int n = 0;
    DIR *d;

    snprintf(dir, sizeof(dir), "%s/zss/store", cache);
    d = opendir(dir);
    if (bytes)
        *bytes = 0;
    while (d && (de = readdir(d))) {
        if (de->d_name[0] == '.' || !strncmp(de->d_name, "pin-", 4))
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            n++;
            if (bytes)
                *bytes += st.st_size;
        }
    }
    if (d)
        closedir(d);
    return n;
}

static int case_hash(void)
{
    static const struct { const char *text, *hex; } v[] = {
        { "", "00000000000000000000000000000000" },
        { "hello", "cbd8a7b341bd9b025b1e906a48ae1d19" },
        { "The quick brown fox jumps over the lazy dog", "e34bbc7bbc071b6c7a433ca9c49a9347" },
    };

    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        uint8_t out[16];
        uint64_t h1, h2;
        char hex[40];

        zss_hash128(v[i].text, strlen(v[i].text), out);
        memcpy(&h1, out, 8);
        memcpy(&h2, out + 8, 8);
        snprintf(hex, sizeof(hex), "%016llx%016llx", (unsigned long long)h1, (unsigned long long)h2);
        if (strcmp(hex, v[i].hex)) {
            fprintf(stderr, "FAIL hash(\"%s\") = %s, expected %s\n", v[i].text, hex, v[i].hex);
            failures++;
        }
    }
    return failures;
}

/* Stores two distinct blobs and one duplicate; reads them back. */
static int case_store(const char *cache)
{
    static uint8_t a[100000], b[5000], back[100000];
    struct zss_ret ra, rb, ra2;

    fill(a, sizeof(a), 1);
    fill(b, sizeof(b), 2);
    EXPECT(zss_retain_put(a, sizeof(a), &ra));
    EXPECT(zss_retain_put(b, sizeof(b), &rb));
    EXPECT(zss_retain_put(a, sizeof(a), &ra2)); /* identical upload to a second destination */
    EXPECT(zss_retain_get(&ra, back) && !memcmp(a, back, sizeof(a))); /* readable while still queued */
    zss_retain_flush(5000);
    EXPECT(blobs(cache, NULL) == 2);
    EXPECT(zss_retain_get(&rb, back) && !memcmp(b, back, sizeof(b)));
    zss_retain_release(&ra);
    EXPECT(zss_retain_get(&ra2, back) && !memcmp(a, back, sizeof(a))); /* the other reference still holds it */
    return failures;
}

/* A later run uploading the same data: it must be found, not written again. */
static int case_again(const char *cache)
{
    static uint8_t a[100000], back[100000];
    char path[900], dir[600];
    struct dirent *de;
    struct stat before = { 0 }, after = { 0 };
    struct zss_ret ra;
    DIR *d;

    snprintf(dir, sizeof(dir), "%s/zss/store", cache);
    d = opendir(dir);
    while (d && (de = readdir(d))) {
        if (strstr(de->d_name, "-100000")) {
            snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
            stat(path, &before);
        }
    }
    if (d)
        closedir(d);
    EXPECT(before.st_ino != 0);

    fill(a, sizeof(a), 1);
    EXPECT(blobs(cache, NULL) == 2);
    EXPECT(zss_retain_put(a, sizeof(a), &ra));
    zss_retain_flush(5000);
    EXPECT(blobs(cache, NULL) == 2);
    stat(path, &after);
    EXPECT(after.st_ino == before.st_ino); /* same file, not a rewrite */
    EXPECT(after.st_nlink == 2);           /* and now pinned by this process */
    EXPECT(zss_retain_get(&ra, back) && !memcmp(a, back, sizeof(a)));
    return failures;
}

/* ZSS_RETAIN_LIMIT_MB=1: unpinned blobs go, oldest first; a pinned one stays readable. */
static int case_trim(const char *cache)
{
    static uint8_t mine[400000], back[400000];
    struct zss_ret r;
    off_t bytes;

    /* case_store and case_again left two unpinned blobs; add 3 MB of old filler. */
    for (int i = 0; i < 3; i++) {
        char path[900];
        FILE *f;

        snprintf(path, sizeof(path), "%s/zss/store/filler%d-1048576", cache, i);
        f = fopen(path, "w");
        for (int k = 0; f && k < 1024; k++)
            fwrite(mine, 1, 1024, f);
        if (f)
            fclose(f);
    }
    fill(mine, sizeof(mine), 7);
    EXPECT(zss_retain_put(mine, sizeof(mine), &r));
    zss_retain_flush(5000);
    EXPECT(blobs(cache, &bytes) == 6 && bytes > (3 << 20));
    zss_retain_trim();
    blobs(cache, &bytes);
    EXPECT(bytes <= (1 << 20));
    EXPECT(zss_retain_get(&r, back) && !memcmp(mine, back, sizeof(mine)));
    return failures;
}

/* ZSS_RETAIN_QUEUE_MB=0: nothing new can be queued, and the caller is told so at once. */
static int case_full(const char *cache)
{
    static uint8_t a[4096];
    struct zss_ret r;

    fill(a, sizeof(a), 99);
    EXPECT(!zss_retain_put(a, sizeof(a), &r));
    EXPECT(!r.valid);
    EXPECT(blobs(cache, NULL) == 0);
    return failures;
}

/* ZSS_RETAIN=off writes nothing; ZSS_RETAIN=ram keeps data without files. */
static int case_off(const char *cache)
{
    static uint8_t a[4096];
    struct zss_ret r;

    fill(a, sizeof(a), 5);
    EXPECT(!zss_retain_put(a, sizeof(a), &r));
    EXPECT(blobs(cache, NULL) == 0);
    return failures;
}

static int case_ram(const char *cache)
{
    static uint8_t a[4096], back[4096];
    struct zss_ret r;

    fill(a, sizeof(a), 5);
    EXPECT(zss_retain_put(a, sizeof(a), &r));
    zss_retain_flush(1000);
    EXPECT(blobs(cache, NULL) == 0);
    EXPECT(zss_retain_get(&r, back) && !memcmp(a, back, sizeof(a)));
    zss_retain_release(&r);
    EXPECT(!zss_retain_get(&r, back));
    return failures;
}

static int run(const char *self, const char *name, const char *cache, const char *var, const char *value)
{
    pid_t pid = fork();
    int status = 0;

    if (pid == 0) {
        setenv("XDG_CACHE_HOME", cache, 1);
        unsetenv("ZSS_RETAIN");
        unsetenv("ZSS_RETAIN_LIMIT_MB");
        unsetenv("ZSS_RETAIN_QUEUE_MB");
        if (var)
            setenv(var, value, 1);
        execl(self, self, name, cache, (char *)NULL);
        _exit(127);
    }
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "case '%s' failed\n", name);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    char shared[] = "/tmp/zss-retain-XXXXXX", fresh[4][32];
    int bad = 0;

    if (argc == 3) {
        const char *c = argv[1], *cache = argv[2];

        if (!strcmp(c, "store")) return case_store(cache) != 0;
        if (!strcmp(c, "again")) return case_again(cache) != 0;
        if (!strcmp(c, "trim")) return case_trim(cache) != 0;
        if (!strcmp(c, "full")) return case_full(cache) != 0;
        if (!strcmp(c, "off")) return case_off(cache) != 0;
        if (!strcmp(c, "ram")) return case_ram(cache) != 0;
        return 2;
    }

    bad += case_hash() != 0;
    if (!mkdtemp(shared))
        return 1;
    for (int i = 0; i < 4; i++) {
        strcpy(fresh[i], "/tmp/zss-retain-XXXXXX");
        if (!mkdtemp(fresh[i]))
            return 1;
    }
    /* These three share one store, as three runs of an application would. */
    bad += run(argv[0], "store", shared, NULL, NULL);
    bad += run(argv[0], "again", shared, NULL, NULL);
    bad += run(argv[0], "trim", shared, "ZSS_RETAIN_LIMIT_MB", "1");
    bad += run(argv[0], "full", fresh[0], "ZSS_RETAIN_QUEUE_MB", "0");
    bad += run(argv[0], "off", fresh[1], "ZSS_RETAIN", "off");
    bad += run(argv[0], "ram", fresh[2], "ZSS_RETAIN", "ram");

    {
        char cmd[400];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s' '%s' '%s' '%s'", shared, fresh[0], fresh[1], fresh[2], fresh[3]);
        if (system(cmd) != 0)
            fprintf(stderr, "could not clean up\n");
    }
    if (bad) {
        fprintf(stderr, "%d case(s) failed\n", bad);
        return 1;
    }
    puts("retain: ok");
    return 0;
}
