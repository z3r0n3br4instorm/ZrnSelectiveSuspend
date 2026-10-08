// SPDX-License-Identifier: GPL-2.0-only
/*
 * The portable profile: what a GPU is reported as being able to do.
 *
 * An application enables whatever it is offered. If it is offered something
 * only one GPU has, it can never leave that GPU, and that is found out only
 * when it is asked to. So each GPU the layer presents reports what every GPU
 * in its group has: the GPUs its applications may be moved to. A capability
 * is in the profile if all of them have it; a limit takes the weakest value.
 *
 * ZSS_PROFILE=native turns this off: each GPU then reports what its own
 * driver offers, and a move is refused later if the target lacks something
 * in use.
 *
 * Everything here is answered from what was read off each driver when its
 * GPU was found, so it does not matter which drivers are open or which
 * devices are powered at the time of asking.
 */
#define ZSS_GEN_LIMITS
#include "zss_layer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zss_feats_gen.h"

/* Formats of the core specification have consecutive numbers up to here; they are the ones read in advance. */
#define ZSS_CORE_FORMATS ((uint32_t)VK_FORMAT_ASTC_12x12_SRGB_BLOCK + 1)

static bool portable(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("ZSS_PROFILE");

        on = !(e && !strcmp(e, "native"));
    }
    return on;
}

/* Whether `g` is one of the GPUs an application on `self` may end up on. */
bool zss_profile_member(const struct zss_gpu *self, const struct zss_gpu *g)
{
    if (g == self)
        return true;
    if (!portable())
        return false;
    /* The software renderer is somewhere to go only if falling back to it is allowed. */
    if (g->software)
        return zss_control_software_allowed();
    return true;
}

static bool alone(const struct zss_gpu *self)
{
    for (int i = 0; i < zss_ngpus; i++)
        if (zss_gpus[i] != self && zss_profile_member(self, zss_gpus[i]))
            return false;
    return true;
}

/* Read once, while the GPU's driver is open. */
void zss_profile_cache(struct zss_gpu *gpu, VkPhysicalDevice pd)
{
    gpu->fmt = calloc(ZSS_CORE_FORMATS, sizeof(*gpu->fmt));
    for (uint32_t f = 1; gpu->fmt && f < ZSS_CORE_FORMATS; f++)
        gpu->drv->fn.GetPhysicalDeviceFormatProperties(pd, (VkFormat)f, &gpu->fmt[f]);
    zss_feats_cache(gpu, pd);
}

uint32_t zss_profile_api_version(const struct zss_gpu *self)
{
    uint32_t v = zss_gpu_api_version(self);

    for (int i = 0; i < zss_ngpus; i++)
        if (zss_profile_member(self, zss_gpus[i]) && zss_gpu_api_version(zss_gpus[i]) < v)
            v = zss_gpu_api_version(zss_gpus[i]);
    return v;
}

/* The weaker of two values of one limit, written to `a`. */
static void weaker(const struct zss_limit *l, void *a, const void *b)
{
    for (uint32_t i = 0; i < l->n; i++) {
        /* In a range the low end is better lower and the high end better higher. */
        bool most = l->kind == ZL_MOST || (l->kind == ZL_RANGE && i == 0);

        switch (l->type) {
#define PICK(T) do { \
            T *x = (T *)a + i; const T *y = (const T *)b + i; \
            if (l->kind == ZL_BOTH) *x = (T)((uint64_t)*x & (uint64_t)*y); \
            else if (most ? *y > *x : *y < *x) *x = *y; \
        } while (0)
        case ZT_U32: PICK(uint32_t); break;
        case ZT_I32: {
            int32_t *x = (int32_t *)a + i;
            const int32_t *y = (const int32_t *)b + i;

            if (most ? *y > *x : *y < *x)
                *x = *y;
            break;
        }
        case ZT_F32: {
            float *x = (float *)a + i;
            const float *y = (const float *)b + i;

            if (most ? *y > *x : *y < *x)
                *x = *y;
            break;
        }
        case ZT_SIZE: PICK(size_t); break;
        case ZT_U64: PICK(uint64_t); break;
#undef PICK
        }
    }
}

void zss_profile_limits(const struct zss_gpu *self, VkPhysicalDeviceLimits *lim)
{
    for (int i = 0; i < zss_ngpus; i++) {
        const struct zss_gpu *g = zss_gpus[i];

        if (g == self || !zss_profile_member(self, g))
            continue;
        for (size_t k = 0; k < sizeof(zss_limits) / sizeof(zss_limits[0]); k++)
            weaker(&zss_limits[k], (char *)lim + zss_limits[k].off, (const char *)&g->props.limits + zss_limits[k].off);
    }
}

/* Core features: on only where every member has them. Returns the member that turned `which` off, if asked. */
void zss_profile_features(const struct zss_gpu *self, VkPhysicalDeviceFeatures *f)
{
    VkBool32 *out = (VkBool32 *)f;

    for (int i = 0; i < zss_ngpus; i++) {
        const VkBool32 *have = (const VkBool32 *)&zss_gpus[i]->features;

        if (zss_gpus[i] == self || !zss_profile_member(self, zss_gpus[i]))
            continue;
        for (size_t k = 0; k < sizeof(*f) / sizeof(VkBool32); k++)
            out[k] = out[k] && have[k];
    }
}

/* The member of the group that lacks core feature number `k`, or NULL. */
const struct zss_gpu *zss_profile_feature_lacking(const struct zss_gpu *self, size_t k)
{
    for (int i = 0; i < zss_ngpus; i++)
        if (zss_gpus[i] != self && zss_profile_member(self, zss_gpus[i]) &&
            !((const VkBool32 *)&zss_gpus[i]->features)[k])
            return zss_gpus[i];
    return NULL;
}

static bool gpu_has_ext(const struct zss_gpu *g, const char *name)
{
    for (uint32_t i = 0; i < g->next; i++)
        if (!strcmp(g->ext[i].extensionName, name))
            return true;
    return false;
}

bool zss_profile_has_ext(const struct zss_gpu *self, const char *name)
{
    /* The layer provides these itself where a driver lacks them. */
    if (zss_ext_emulated(name))
        return true;
    for (int i = 0; i < zss_ngpus; i++)
        if (zss_profile_member(self, zss_gpus[i]) && !gpu_has_ext(zss_gpus[i], name))
            return false;
    return true;
}

/*
 * A format's properties, cut down to what every member can do with it. `p`
 * holds the present GPU's answer. Formats outside the core numbering were
 * not read in advance, so in a group of more than one they are not offered.
 */
void zss_profile_format(const struct zss_gpu *self, VkFormat format, VkFormatProperties *p)
{
    if (alone(self))
        return;
    if ((uint32_t)format >= ZSS_CORE_FORMATS) {
        memset(p, 0, sizeof(*p));
        return;
    }
    for (int i = 0; i < zss_ngpus; i++) {
        const struct zss_gpu *g = zss_gpus[i];

        if (g == self || !zss_profile_member(self, g))
            continue;
        if (!g->fmt) {
            memset(p, 0, sizeof(*p));
            return;
        }
        p->linearTilingFeatures &= g->fmt[format].linearTilingFeatures;
        p->optimalTilingFeatures &= g->fmt[format].optimalTilingFeatures;
        p->bufferFeatures &= g->fmt[format].bufferFeatures;
    }
}

/* Whether images of this format and tiling exist at all on every member. */
bool zss_profile_format_usable(const struct zss_gpu *self, VkFormat format, VkImageTiling tiling)
{
    VkFormatProperties p = { ~0u, ~0u, ~0u };

    zss_profile_format(self, format, &p);
    return tiling == VK_IMAGE_TILING_LINEAR ? p.linearTilingFeatures != 0 : p.optimalTilingFeatures != 0;
}

/*
 * Queue families, as far as every member can provide them. A device is
 * rebuilt on another GPU by giving each family the application used one of
 * the target's that can do the same work and has as many queues (see
 * map_families() in device.c). So a family is reported only if each member
 * has one of its own left to stand in for it, and with no more queues than
 * the smallest of those. Families after the first that cannot be matched
 * are left off the end, so the numbering of the rest does not change.
 */
uint32_t zss_profile_families(const struct zss_gpu *self, VkQueueFamilyProperties *out)
{
    const VkQueueFlags work = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, shown = work | VK_QUEUE_TRANSFER_BIT;
    bool used[ZSS_MAX_GPUS][ZSS_MAX_FAMILIES] = { { false } };
    uint32_t n = 0;

    for (uint32_t f = 0; f < self->nfam; f++) {
        VkQueueFamilyProperties p = self->fam[f];
        bool everywhere = true;

        p.queueFlags &= shown;
        for (int i = 0; i < zss_ngpus && everywhere; i++) {
            const struct zss_gpu *g = zss_gpus[i];
            uint32_t t;

            if (g == self || !zss_profile_member(self, g))
                continue;
            for (t = 0; t < g->nfam; t++)
                if (!used[i][t] && (g->fam[t].queueFlags & p.queueFlags & work) == (p.queueFlags & work))
                    break;
            if (t == g->nfam) {
                everywhere = false;
                break;
            }
            used[i][t] = true;
            p.queueFlags &= g->fam[t].queueFlags | ~VK_QUEUE_TRANSFER_BIT;
            if (g->fam[t].queueCount < p.queueCount)
                p.queueCount = g->fam[t].queueCount;
            if (g->fam[t].timestampValidBits < p.timestampValidBits)
                p.timestampValidBits = g->fam[t].timestampValidBits;
            /* The coarser granularity; all zeroes is the coarsest there is (whole images only). */
            {
                VkExtent3D *a = &p.minImageTransferGranularity;
                const VkExtent3D *b = &g->fam[t].minImageTransferGranularity;

                if (!b->width || !a->width) {
                    *a = (VkExtent3D){ 0, 0, 0 };
                } else {
                    a->width = a->width > b->width ? a->width : b->width;
                    a->height = a->height > b->height ? a->height : b->height;
                    a->depth = a->depth > b->depth ? a->depth : b->depth;
                }
            }
        }
        if (!everywhere)
            break;
        if (out)
            out[n] = p;
        n++;
    }
    return n;
}

void zss_profile_describe(const struct zss_gpu *self)
{
    VkPhysicalDeviceFeatures f = self->features;
    const VkBool32 *own = (const VkBool32 *)&self->features, *cut = (const VkBool32 *)&f;
    int members = 0, lost = 0;

    for (int i = 0; i < zss_ngpus; i++)
        members += zss_profile_member(self, zss_gpus[i]);
    zss_profile_features(self, &f);
    for (size_t k = 0; k < sizeof(f) / sizeof(VkBool32); k++)
        lost += own[k] && !cut[k];
    zss_dbg("profile of %s: %s, %d GPU(s) in its group, %d core feature(s) withheld", self->props.deviceName,
            portable() ? "portable" : "native", members, lost);
}
