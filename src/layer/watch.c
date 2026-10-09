// SPDX-License-Identifier: GPL-2.0-only
/*
 * Shadows of mapped memory that know which of their pages were written.
 *
 * An application writes mapped memory through the layer's shadow, and the
 * shadow is copied to the real mapping before each submission. Copied whole,
 * that is every mapped buffer on every submission: Zink keeps megabytes
 * mapped and submits several times a frame, gigabytes a second for a frame
 * that changes a few kilobytes.
 *
 * The kernel can say which pages were written: userfaultfd write protection
 * in asynchronous mode (Linux 6.7) marks a written page without stopping the
 * writer, and PAGEMAP_SCAN reports the marked pages and protects them again
 * in one call. Where either is missing, every page counts as written, which
 * is what the layer did before.
 *
 * ZSS_WRITE_WATCH=0 turns it off.
 */
#include "zss_layer.h"
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Older headers (the release is built on Ubuntu 22.04) lack these; the kernel decides at run time. */
#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif
#ifndef UFFD_FEATURE_WP_UNPOPULATED
#define UFFD_FEATURE_WP_UNPOPULATED (1 << 13)
#endif
#ifndef UFFD_FEATURE_WP_ASYNC
#define UFFD_FEATURE_WP_ASYNC (1 << 15)
#endif
#ifndef PAGEMAP_SCAN
struct page_region {
    uint64_t start, end, categories;
};
struct pm_scan_arg {
    uint64_t size, flags, start, end, walk_end, vec, vec_len, max_pages, category_inverted, category_mask,
        category_anyof_mask, return_mask;
};
#define PAGEMAP_SCAN _IOWR('f', 16, struct pm_scan_arg)
#define PAGE_IS_WRITTEN (1 << 1)
#define PM_SCAN_WP_MATCHING (1 << 0)
#define PM_SCAN_CHECK_WPASYNC (1 << 1)
#endif

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int uffd = -1, pagemap = -1;

static void watch_init(void)
{
    const char *e = getenv("ZSS_WRITE_WATCH");
    struct uffdio_api api = { .api = UFFD_API, .features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED };
    int fd;

    if (e && !strcmp(e, "0"))
        return;
    fd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    if (fd < 0) {
        zss_dbg("no userfaultfd; mapped memory is copied whole at each submission");
        return;
    }
    if (ioctl(fd, UFFDIO_API, &api) < 0 || !(api.features & UFFD_FEATURE_WP_ASYNC)) {
        zss_dbg("no asynchronous write protection; mapped memory is copied whole at each submission");
        close(fd);
        return;
    }
    pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (pagemap < 0) {
        close(fd);
        return;
    }
    uffd = fd;
}

static size_t page_size(void)
{
    static size_t ps;

    if (!ps)
        ps = (size_t)sysconf(_SC_PAGESIZE);
    return ps;
}

uint8_t *zss_shadow_alloc(VkDeviceSize size, bool *watched)
{
    size_t len = (size_t)(size ? size : 1);
    void *p;

    *watched = false;
    pthread_once(&once, watch_init);
    if (uffd < 0)
        return calloc(1, len);
    len = (len + page_size() - 1) & ~(page_size() - 1);
    p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return calloc(1, (size_t)(size ? size : 1));
    {
        struct uffdio_register reg = { .range = { (uintptr_t)p, len }, .mode = UFFDIO_REGISTER_MODE_WP };
        struct uffdio_writeprotect wp = { .range = { (uintptr_t)p, len }, .mode = UFFDIO_WRITEPROTECT_MODE_WP };

        if (ioctl(uffd, UFFDIO_REGISTER, &reg) < 0 || ioctl(uffd, UFFDIO_WRITEPROTECT, &wp) < 0) {
            munmap(p, len);
            return calloc(1, (size_t)(size ? size : 1));
        }
    }
    *watched = true;
    return p;
}

void zss_shadow_free(uint8_t *p, VkDeviceSize size, bool watched)
{
    if (!watched) {
        free(p);
        return;
    }
    /* Unmapping ends the registration too. */
    munmap(p, ((size_t)(size ? size : 1) + page_size() - 1) & ~(page_size() - 1));
}

/*
 * The parts of a watched shadow written since the last call, as byte offsets,
 * and protects them again. Returns false when that cannot be known: then all
 * of it counts as written.
 */
bool zss_shadow_written(uint8_t *p, VkDeviceSize size, struct zss_span **spans, uint32_t *n, uint32_t *cap)
{
    struct page_region vec[64];
    uint64_t start = (uintptr_t)p, end = start + ((size + page_size() - 1) & ~(page_size() - 1));

    *n = 0;
    while (start < end) {
        struct pm_scan_arg a = { .size = sizeof(a), .flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC,
                                 .start = start, .end = end, .vec = (uintptr_t)vec, .vec_len = 64,
                                 .category_mask = PAGE_IS_WRITTEN, .return_mask = PAGE_IS_WRITTEN };
        long got = ioctl(pagemap, PAGEMAP_SCAN, &a);

        if (got < 0)
            return false;
        for (long i = 0; i < got; i++) {
            if (*n == *cap) {
                uint32_t c = *cap ? *cap * 2 : 16;
                struct zss_span *s = realloc(*spans, c * sizeof(*s));

                if (!s)
                    return false;
                *spans = s;
                *cap = c;
            }
            (*spans)[*n].off = vec[i].start - (uintptr_t)p;
            (*spans)[*n].end = vec[i].end - (uintptr_t)p;
            (*n)++;
        }
        if (a.walk_end <= start)
            return false;
        start = a.walk_end;
    }
    return true;
}
