// SPDX-License-Identifier: GPL-2.0-only
/*
 * Vulkan 1.1: what version 1.1 adds to the core, as far as the layer offers it.
 *
 * Queries about a physical device are answered from the driver and then cut
 * down to what the layer can honour. The device-level commands are built on
 * the 1.0 ones wherever 1.1 only gave them a wider signature.
 */
#include "zss_layer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zss_feats_gen.h"

/* The highest version the layer presents. ZSS_VULKAN=1.0 gives the old behaviour. */
uint32_t zss_api_version(void)
{
    static uint32_t version;

    if (!version) {
        const char *e = getenv("ZSS_VULKAN");

        version = e && !strcmp(e, "1.0") ? VK_API_VERSION_1_0 : VK_API_VERSION_1_1;
    }
    return version;
}

/* What a device on this GPU reports: never more than the driver has. */
uint32_t zss_gpu_api_version(const struct zss_gpu *gpu)
{
    uint32_t ours = zss_api_version(), theirs = VK_MAKE_API_VERSION(0, VK_API_VERSION_MAJOR(gpu->props.apiVersion),
                                                                    VK_API_VERSION_MINOR(gpu->props.apiVersion), 0);

    return theirs < ours ? theirs : ours;
}

/* ---- feature structures ------------------------------------------------------------ */

static const struct zss_feat_struct *feat_struct(VkStructureType t)
{
    for (size_t i = 0; i < sizeof(zss_feat_structs) / sizeof(zss_feat_structs[0]); i++)
        if (zss_feat_structs[i].stype == t)
            return &zss_feat_structs[i];
    return NULL;
}

static bool feat_offered(const struct zss_feat_struct *fs, uint32_t i)
{
    return (fs->offered[i / 64] >> (i % 64)) & 1;
}

#define ZSS_NFEAT (sizeof(zss_feat_structs) / sizeof(zss_feat_structs[0]))

/*
 * Reads, once, which switches this GPU's driver has in every feature
 * structure the layer offers anything of. Kept as bits so that the group's
 * common set can be worked out later without asking any driver.
 */
void zss_feats_cache(struct zss_gpu *gpu, VkPhysicalDevice pd)
{
    PFN_vkGetPhysicalDeviceFeatures2 get = gpu->drv->fn.GetPhysicalDeviceFeatures2
                                               ? gpu->drv->fn.GetPhysicalDeviceFeatures2
                                               : gpu->drv->fn.GetPhysicalDeviceFeatures2KHR;

    gpu->featbits = calloc(ZSS_NFEAT * 2, sizeof(uint64_t));
    if (!get || !gpu->featbits)
        return;
    for (size_t i = 0; i < ZSS_NFEAT; i++) {
        const struct zss_feat_struct *fs = &zss_feat_structs[i];
        struct { VkBaseOutStructure head; VkBool32 b[128]; } q = { .head = { .sType = fs->stype } };
        VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &q };

        if (!fs->offered[0] && !fs->offered[1])
            continue;
        get(pd, &f2);
        for (uint32_t k = 0; k < fs->n; k++)
            if (q.b[k])
                gpu->featbits[i * 2 + k / 64] |= 1ull << (k % 64);
    }
}

/* Whether every GPU in the group has switch `k` of structure number `i`; *lacking is one that does not. */
static bool feat_common(const struct zss_gpu *self, size_t i, uint32_t k, const struct zss_gpu **lacking)
{
    for (int g = 0; g < zss_ngpus; g++) {
        const struct zss_gpu *m = zss_gpus[g];

        if (m == self || !zss_profile_member(self, m))
            continue;
        if (!m->featbits || !((m->featbits[i * 2 + k / 64] >> (k % 64)) & 1)) {
            if (lacking)
                *lacking = m;
            return false;
        }
    }
    return true;
}

/* Cuts a filled-in chain down further, to what the whole group has. */
static void feats_common(const struct zss_gpu *self, void *pnext)
{
    for (VkBaseOutStructure *s = pnext; s; s = s->pNext) {
        const struct zss_feat_struct *fs = feat_struct(s->sType);
        VkBool32 *b = (VkBool32 *)(s + 1);

        for (uint32_t k = 0; fs && k < fs->n; k++)
            if (b[k] && !feat_common(self, (size_t)(fs - zss_feat_structs), k, NULL))
                b[k] = VK_FALSE;
    }
}

/* Whether everything switched on in a kept chain is in the group's profile. Names the first that is not, and why. */
bool zss_feats_in_profile(const struct zss_gpu *self, const void *chain, char *reason, size_t rlen)
{
    for (const VkBaseOutStructure *s = chain; s; s = s->pNext) {
        const struct zss_feat_struct *fs = feat_struct(s->sType);
        const VkBool32 *b = (const VkBool32 *)(s + 1);
        const struct zss_gpu *lacking = NULL;

        for (uint32_t k = 0; fs && k < fs->n; k++)
            if (b[k] && !feat_common(self, (size_t)(fs - zss_feat_structs), k, &lacking)) {
                snprintf(reason, rlen, "%s.%s is not in the portable profile: %s lacks it", fs->name, fs->members[k],
                         lacking->props.deviceName);
                return false;
            }
    }
    return true;
}

/*
 * Cuts a chain of feature structures the driver has filled in down to what
 * the layer offers. A structure the layer does not know at all is left as
 * the application passed it, which for a query means zeroed by convention
 * and for anything else is not ours to touch.
 */
static void feats_filter(void *pnext)
{
    for (VkBaseOutStructure *s = pnext; s; s = s->pNext) {
        const struct zss_feat_struct *fs = feat_struct(s->sType);
        VkBool32 *b = (VkBool32 *)(s + 1);

        for (uint32_t i = 0; fs && i < fs->n; i++)
            if (!feat_offered(fs, i))
                b[i] = VK_FALSE;
    }
}

/*
 * The feature structures an application chained to device creation, copied
 * so that the same request can be made of whichever GPU the device is built
 * on. Only switches the layer offers are kept on. Returns the head of the
 * copied chain, or NULL; *base receives a chained VkPhysicalDeviceFeatures2's
 * core features if there was one.
 */
void *zss_feats_keep(const void *pnext, VkPhysicalDeviceFeatures *base, bool *had_base)
{
    VkBaseOutStructure *head = NULL, **tail = &head;

    *had_base = false;
    for (const VkBaseInStructure *s = pnext; s; s = s->pNext) {
        const struct zss_feat_struct *fs = feat_struct(s->sType);
        VkBaseOutStructure *copy;
        size_t size;

        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            *base = ((const VkPhysicalDeviceFeatures2 *)s)->features;
            *had_base = true;
            continue;
        }
        if (!fs)
            continue;
        size = sizeof(*copy) + fs->n * sizeof(VkBool32);
        copy = calloc(1, size);
        memcpy(copy, s, size);
        copy->pNext = NULL;
        for (uint32_t i = 0; i < fs->n; i++) {
            VkBool32 *b = (VkBool32 *)(copy + 1);

            if (b[i] && !feat_offered(fs, i)) {
                zss_dbg("feature %s.%s was asked for but is not offered; left off", fs->name, fs->members[i]);
                b[i] = VK_FALSE;
            }
        }
        *tail = copy;
        tail = &copy->pNext;
    }
    return head;
}

void zss_feats_free(void *chain)
{
    for (VkBaseOutStructure *s = chain, *next; s; s = next) {
        next = s->pNext;
        free(s);
    }
}

/* Whether the GPU has every switch in the chain that is on. Names the first it lacks. */
bool zss_feats_supported(struct zss_gpu *gpu, const void *chain, char *reason, size_t rlen)
{
    VkPhysicalDevice pd = zss_gpu_real(gpu);
    PFN_vkGetPhysicalDeviceFeatures2 get;
    bool ok = true;

    if (!chain)
        return true;
    get = pd ? (gpu->drv->fn.GetPhysicalDeviceFeatures2 ? gpu->drv->fn.GetPhysicalDeviceFeatures2
                                                         : gpu->drv->fn.GetPhysicalDeviceFeatures2KHR) : NULL;
    for (const VkBaseOutStructure *s = chain; s && ok; s = s->pNext) {
        const struct zss_feat_struct *fs = feat_struct(s->sType);
        size_t size = sizeof(*s) + (fs ? fs->n : 0) * sizeof(VkBool32);
        VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        VkBaseOutStructure *probe;

        if (!fs)
            continue;
        probe = calloc(1, size);
        probe->sType = s->sType;
        f2.pNext = probe;
        if (get)
            get(pd, &f2);
        for (uint32_t i = 0; i < fs->n; i++) {
            if (((const VkBool32 *)(s + 1))[i] && !((const VkBool32 *)(probe + 1))[i]) {
                snprintf(reason, rlen, "%s lacks the %s feature", gpu->props.deviceName, fs->members[i]);
                ok = false;
                break;
            }
        }
        free(probe);
    }
    return ok;
}

/* ---- offered extensions and chained structures ---------------------------------------- */

uint32_t zss_offered_ext_count(void)
{
    return zss_api_version() >= VK_API_VERSION_1_1 ? (uint32_t)(sizeof(zss_offered_exts) / sizeof(zss_offered_exts[0])) : 0;
}

const char *zss_offered_ext(uint32_t i)
{
    return zss_offered_exts[i];
}

/* The layer's own name for an extension the application asked for, or NULL if it is not one the layer offers. */
const char *zss_offered_ext_named(const char *name)
{
    for (uint32_t i = 0; i < zss_offered_ext_count(); i++)
        if (!strcmp(zss_offered_exts[i], name))
            return zss_offered_exts[i];
    return NULL;
}

/*
 * Keeps the structures of a creation call's chain that the layer passes on,
 * as copies owned by the object. Anything else in the chain is left out, and
 * said so once in the debug log: it belongs to something not offered.
 */
void *zss_chain_keep(struct zss_obj *o, const void *pnext)
{
    VkBaseOutStructure *head = NULL, **tail = &head;

    for (const VkBaseInStructure *s = pnext; s; s = s->pNext) {
        const struct zss_chain_struct *known = NULL;
        VkBaseOutStructure *copy;

        if (s->sType == VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO) {
            /* The one kept structure that names an object: the layer's handle is kept, and the
             * object it stands for outlives this one. zss_chain_real() puts the driver's in. */
            copy = zss_obj_dup(o, s, sizeof(VkSamplerYcbcrConversionInfo));
            zss_obj_dep(o, ZOBJ(((const VkSamplerYcbcrConversionInfo *)s)->conversion));
            copy->pNext = NULL;
            *tail = copy;
            tail = &copy->pNext;
            continue;
        }
        for (size_t i = 0; i < sizeof(zss_chain_structs) / sizeof(zss_chain_structs[0]); i++)
            if (zss_chain_structs[i].stype == s->sType)
                known = &zss_chain_structs[i];
        if (!known) {
            zss_dbg("a chained structure of type %d is not one the layer passes on; left out", (int)s->sType);
            continue;
        }
        copy = zss_obj_dup(o, s, known->size);
        copy->pNext = NULL;
        if (known->each) {
            uint32_t n = *(const uint32_t *)((const char *)s + known->count);
            const void *list = *(void *const *)((const char *)s + known->list);

            *(void **)((char *)copy + known->list) = zss_obj_dup(o, list, n * known->each);
        }
        *tail = copy;
        tail = &copy->pNext;
    }
    return head;
}

/*
 * The kept chain as the driver must see it. Only a conversion reference
 * differs: it holds the layer's handle, and the driver needs its own. That
 * one structure is copied into the caller's scratch space with the handle
 * swapped; everything else in the chain is used where it lies.
 */
void *zss_chain_real(const void *kept, void *scratch, size_t room)
{
    VkBaseOutStructure *head = NULL, **tail = &head;

    for (VkBaseOutStructure *s = (VkBaseOutStructure *)kept, *next; s; s = next) {
        next = s->pNext;
        if (s->sType == VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO && room >= sizeof(VkSamplerYcbcrConversionInfo)) {
            VkSamplerYcbcrConversionInfo *c = scratch;

            *c = *(const VkSamplerYcbcrConversionInfo *)s;
            c->conversion = ZREAL(VkSamplerYcbcrConversion, c->conversion);
            c->pNext = next;
            *tail = (VkBaseOutStructure *)c;
            return head;
        }
        *tail = s;
        tail = &s->pNext;
    }
    return head;
}

/* Formats whose pixels lie in more than one plane. The layer does not carry such images across. */
bool zss_format_planar(VkFormat f)
{
    return (f >= VK_FORMAT_G8B8G8R8_422_UNORM && f <= VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM) ||
           (f >= VK_FORMAT_G8_B8R8_2PLANE_444_UNORM && f <= VK_FORMAT_G16_B16R16_2PLANE_444_UNORM);
}

/* ---- physical device queries --------------------------------------------------------- */

extern __typeof__(*(PFN_vkGetPhysicalDeviceFeatures)0) zss_GetPhysicalDeviceFeatures;
extern __typeof__(*(PFN_vkGetPhysicalDeviceProperties)0) zss_GetPhysicalDeviceProperties;
extern __typeof__(*(PFN_vkGetPhysicalDeviceMemoryProperties)0) zss_GetPhysicalDeviceMemoryProperties;
extern __typeof__(*(PFN_vkGetPhysicalDeviceQueueFamilyProperties)0) zss_GetPhysicalDeviceQueueFamilyProperties;
extern __typeof__(*(PFN_vkGetPhysicalDeviceFormatProperties)0) zss_GetPhysicalDeviceFormatProperties;
extern __typeof__(*(PFN_vkGetPhysicalDeviceImageFormatProperties)0) zss_GetPhysicalDeviceImageFormatProperties;

static VKAPI_ATTR void VKAPI_CALL zss_GetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *out)
{
    struct zss_gpu *gpu = (struct zss_gpu *)pd;
    VkPhysicalDevice real;

    if (out->pNext) {
        /* The driver fills in the chain; its core features are replaced below by the layer's own answer. */
        zss_enter();
        real = zss_gpu_real(gpu);
        if (real && gpu->drv->fn.GetPhysicalDeviceFeatures2)
            gpu->drv->fn.GetPhysicalDeviceFeatures2(real, out);
        else if (real && gpu->drv->fn.GetPhysicalDeviceFeatures2KHR)
            gpu->drv->fn.GetPhysicalDeviceFeatures2KHR(real, out);
        zss_leave();
        feats_filter(out->pNext);
        feats_common(gpu, out->pNext);
    }
    zss_GetPhysicalDeviceFeatures(pd, &out->features);
}

static VKAPI_ATTR void VKAPI_CALL zss_GetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *out)
{
    struct zss_gpu *gpu = (struct zss_gpu *)pd;
    VkPhysicalDevice real;

    if (out->pNext) {
        zss_enter();
        real = zss_gpu_real(gpu);
        if (real && gpu->drv->fn.GetPhysicalDeviceProperties2)
            gpu->drv->fn.GetPhysicalDeviceProperties2(real, out);
        else if (real && gpu->drv->fn.GetPhysicalDeviceProperties2KHR)
            gpu->drv->fn.GetPhysicalDeviceProperties2KHR(real, out);
        zss_leave();
    }
    zss_GetPhysicalDeviceProperties(pd, &out->properties);
}

static VKAPI_ATTR void VKAPI_CALL zss_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice pd, VkFormat format,
                                                                         VkFormatProperties2 *out)
{
    /* Chained outputs describe sharing with other processes and other APIs, none of which is offered. */
    zss_GetPhysicalDeviceFormatProperties(pd, format, &out->formatProperties);
}

static VKAPI_ATTR VkResult VKAPI_CALL zss_GetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *out)
{
    /* An image meant to be shared with another process cannot be made here. */
    for (const VkBaseInStructure *s = info->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO &&
            ((const VkPhysicalDeviceExternalImageFormatInfo *)s)->handleType)
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
    for (VkBaseOutStructure *s = out->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES)
            memset(&((VkExternalImageFormatProperties *)s)->externalMemoryProperties, 0, sizeof(VkExternalMemoryProperties));
    return zss_GetPhysicalDeviceImageFormatProperties(pd, info->format, info->type, info->tiling, info->usage, info->flags,
                                                      &out->imageFormatProperties);
}

static VKAPI_ATTR void VKAPI_CALL zss_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice pd, uint32_t *count,
                                                                              VkQueueFamilyProperties2 *out)
{
    VkQueueFamilyProperties plain[ZSS_MAX_FAMILIES];
    uint32_t n = out ? (*count < ZSS_MAX_FAMILIES ? *count : ZSS_MAX_FAMILIES) : 0;

    if (!out) {
        zss_GetPhysicalDeviceQueueFamilyProperties(pd, count, NULL);
        return;
    }
    zss_GetPhysicalDeviceQueueFamilyProperties(pd, &n, plain);
    for (uint32_t i = 0; i < n; i++)
        out[i].queueFamilyProperties = plain[i];
    *count = n;
}

static VKAPI_ATTR void VKAPI_CALL zss_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice pd,
                                                                         VkPhysicalDeviceMemoryProperties2 *out)
{
    zss_GetPhysicalDeviceMemoryProperties(pd, &out->memoryProperties);
}

static VKAPI_ATTR void VKAPI_CALL zss_GetPhysicalDeviceSparseImageFormatProperties2(
    VkPhysicalDevice pd, const VkPhysicalDeviceSparseImageFormatInfo2 *info, uint32_t *count,
    VkSparseImageFormatProperties2 *out)
{
    (void)pd; (void)info; (void)out;
    *count = 0;
}

/* Nothing can be shared with another process or API: every external handle type is unsupported. */
static VKAPI_ATTR void VKAPI_CALL zss_GetPhysicalDeviceExternalBufferProperties(
    VkPhysicalDevice pd, const VkPhysicalDeviceExternalBufferInfo *info, VkExternalBufferProperties *out)
{
    (void)pd; (void)info;
    memset(&out->externalMemoryProperties, 0, sizeof(out->externalMemoryProperties));
}

static VKAPI_ATTR void VKAPI_CALL zss_GetPhysicalDeviceExternalFenceProperties(
    VkPhysicalDevice pd, const VkPhysicalDeviceExternalFenceInfo *info, VkExternalFenceProperties *out)
{
    (void)pd; (void)info;
    out->exportFromImportedHandleTypes = out->compatibleHandleTypes = 0;
    out->externalFenceFeatures = 0;
}

static VKAPI_ATTR void VKAPI_CALL zss_GetPhysicalDeviceExternalSemaphoreProperties(
    VkPhysicalDevice pd, const VkPhysicalDeviceExternalSemaphoreInfo *info, VkExternalSemaphoreProperties *out)
{
    (void)pd; (void)info;
    out->exportFromImportedHandleTypes = out->compatibleHandleTypes = 0;
    out->externalSemaphoreFeatures = 0;
}

/* Every GPU is a group of its own: the layer does not present several as one. */
static VKAPI_ATTR VkResult VKAPI_CALL zss_EnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t *count,
                                                                        VkPhysicalDeviceGroupProperties *out)
{
    VkPhysicalDevice pds[ZSS_MAX_GPUS];
    uint32_t n = ZSS_MAX_GPUS, given;
    PFN_vkEnumeratePhysicalDevices list = (PFN_vkEnumeratePhysicalDevices)zss_instance_proc("vkEnumeratePhysicalDevices");

    list(instance, &n, pds);
    if (!out) {
        *count = n;
        return VK_SUCCESS;
    }
    given = *count < n ? *count : n;
    for (uint32_t i = 0; i < given; i++) {
        out[i].physicalDeviceCount = 1;
        memset(out[i].physicalDevices, 0, sizeof(out[i].physicalDevices));
        out[i].physicalDevices[0] = pds[i];
        out[i].subsetAllocation = VK_FALSE;
    }
    *count = given;
    return given < n ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL zss_EnumerateInstanceVersion(uint32_t *version)
{
    *version = zss_api_version();
    return VK_SUCCESS;
}

/* ---- device commands --------------------------------------------------------------- */

static VKAPI_ATTR void VKAPI_CALL zss_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *queue)
{
    /* The only flag is for protected queues, which are not offered. */
    if (info->flags) {
        *queue = VK_NULL_HANDLE;
        return;
    }
    zss_GetDeviceQueue(device, info->queueFamilyIndex, info->queueIndex, queue);
}

static VKAPI_ATTR VkResult VKAPI_CALL zss_BindBufferMemory2(VkDevice device, uint32_t n, const VkBindBufferMemoryInfo *infos)
{
    VkResult worst = VK_SUCCESS;

    for (uint32_t i = 0; i < n; i++) {
        VkResult r = zss_BindBufferMemory(device, infos[i].buffer, infos[i].memory, infos[i].memoryOffset);

        if (r != VK_SUCCESS)
            worst = r;
    }
    return worst;
}

static VKAPI_ATTR VkResult VKAPI_CALL zss_BindImageMemory2(VkDevice device, uint32_t n, const VkBindImageMemoryInfo *infos)
{
    VkResult worst = VK_SUCCESS;

    for (uint32_t i = 0; i < n; i++) {
        VkResult r;

        /* Binding to a swapchain's memory, or to one plane of an image, is not offered. */
        if (infos[i].pNext) {
            zss_dev_untracked((struct zss_dev *)device, "an image bound through an extended structure");
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        r = zss_BindImageMemory(device, infos[i].image, infos[i].memory, infos[i].memoryOffset);
        if (r != VK_SUCCESS)
            worst = r;
    }
    return worst;
}

/* The layer hands out memory itself, so nothing ever needs or prefers an allocation of its own. */
static void no_dedicated(void *pnext)
{
    for (VkBaseOutStructure *s = pnext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
            ((VkMemoryDedicatedRequirements *)s)->prefersDedicatedAllocation = VK_FALSE;
            ((VkMemoryDedicatedRequirements *)s)->requiresDedicatedAllocation = VK_FALSE;
        }
    }
}

static VKAPI_ATTR void VKAPI_CALL zss_GetBufferMemoryRequirements2(VkDevice device, const VkBufferMemoryRequirementsInfo2 *info,
                                                                   VkMemoryRequirements2 *out)
{
    zss_GetBufferMemoryRequirements(device, info->buffer, &out->memoryRequirements);
    no_dedicated(out->pNext);
}

static VKAPI_ATTR void VKAPI_CALL zss_GetImageMemoryRequirements2(VkDevice device, const VkImageMemoryRequirementsInfo2 *info,
                                                                  VkMemoryRequirements2 *out)
{
    zss_GetImageMemoryRequirements(device, info->image, &out->memoryRequirements);
    no_dedicated(out->pNext);
}

static VKAPI_ATTR void VKAPI_CALL zss_GetImageSparseMemoryRequirements2(VkDevice device,
                                                                        const VkImageSparseMemoryRequirementsInfo2 *info,
                                                                        uint32_t *count, VkSparseImageMemoryRequirements2 *out)
{
    (void)device; (void)info; (void)out;
    *count = 0;
}

static VKAPI_ATTR void VKAPI_CALL zss_TrimCommandPool(VkDevice device, VkCommandPool pool, VkCommandPoolTrimFlags flags)
{
    /* A hint to give memory back. The layer's recordings are freed when a buffer is reset. */
    (void)device; (void)pool; (void)flags;
}

static VKAPI_ATTR void VKAPI_CALL zss_GetDeviceGroupPeerMemoryFeatures(VkDevice device, uint32_t heap, uint32_t local,
                                                                       uint32_t remote, VkPeerMemoryFeatureFlags *out)
{
    (void)device; (void)heap; (void)local; (void)remote;
    *out = VK_PEER_MEMORY_FEATURE_COPY_SRC_BIT | VK_PEER_MEMORY_FEATURE_COPY_DST_BIT |
           VK_PEER_MEMORY_FEATURE_GENERIC_SRC_BIT | VK_PEER_MEMORY_FEATURE_GENERIC_DST_BIT;
}

/*
 * Whether a layout with these bindings can be created. The driver is asked,
 * with the application's sampler handles replaced by its own.
 */
static VKAPI_ATTR void VKAPI_CALL zss_GetDescriptorSetLayoutSupport(VkDevice device, const VkDescriptorSetLayoutCreateInfo *ci,
                                                                    VkDescriptorSetLayoutSupport *out)
{
    struct zss_dev *dev = (struct zss_dev *)device;
    VkDescriptorSetLayoutBinding *b = calloc(ci->bindingCount ? ci->bindingCount : 1, sizeof(*b));
    VkSampler **owned = calloc(ci->bindingCount ? ci->bindingCount : 1, sizeof(*owned));
    VkDescriptorSetLayoutCreateInfo real = *ci;
    PFN_vkGetDescriptorSetLayoutSupport ask;

    zss_enter();
    real.pNext = NULL;
    real.pBindings = b;
    for (uint32_t i = 0; i < ci->bindingCount; i++) {
        b[i] = ci->pBindings[i];
        if (b[i].pImmutableSamplers) {
            owned[i] = calloc(b[i].descriptorCount ? b[i].descriptorCount : 1, sizeof(VkSampler));
            for (uint32_t j = 0; j < b[i].descriptorCount; j++)
                owned[i][j] = ZREAL(VkSampler, ci->pBindings[i].pImmutableSamplers[j]);
            b[i].pImmutableSamplers = owned[i];
        }
    }
    ask = dev->gpu ? (PFN_vkGetDescriptorSetLayoutSupport)dev->gpu->drv->fn.GetDeviceProcAddr(
                         dev->real, "vkGetDescriptorSetLayoutSupport") : NULL;
    out->supported = VK_TRUE;
    if (ask) {
        void *chain = out->pNext;

        out->pNext = NULL;
        ask(dev->real, &real, out);
        out->pNext = chain;
    }
    zss_leave();
    for (uint32_t i = 0; i < ci->bindingCount; i++)
        free(owned[i]);
    free(owned);
    free(b);
}

/*
 * Descriptor update templates. A template says where in a block of the
 * application's memory each descriptor's data lies. Nothing of it reaches the
 * driver: an update through a template is unpacked into ordinary writes, which
 * the layer already tracks and can replay on another GPU.
 */
struct zss_template {
    uint32_t n;
    VkDescriptorUpdateTemplateEntry entry[];
};

static VKAPI_ATTR VkResult VKAPI_CALL zss_CreateDescriptorUpdateTemplate(VkDevice device,
                                                                         const VkDescriptorUpdateTemplateCreateInfo *ci,
                                                                         const VkAllocationCallbacks *alloc,
                                                                         VkDescriptorUpdateTemplate *out)
{
    struct zss_template *t;

    (void)device; (void)alloc;
    /* The other kind pushes descriptors with no set, which belongs to an extension that is not offered. */
    if (ci->templateType != VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    t = malloc(sizeof(*t) + ci->descriptorUpdateEntryCount * sizeof(t->entry[0]));
    if (!t)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    t->n = ci->descriptorUpdateEntryCount;
    memcpy(t->entry, ci->pDescriptorUpdateEntries, t->n * sizeof(t->entry[0]));
    *out = (VkDescriptorUpdateTemplate)(uintptr_t)t;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL zss_DestroyDescriptorUpdateTemplate(VkDevice device, VkDescriptorUpdateTemplate tmpl,
                                                                      const VkAllocationCallbacks *alloc)
{
    (void)device; (void)alloc;
    free((void *)(uintptr_t)tmpl);
}

static VKAPI_ATTR void VKAPI_CALL zss_UpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet set,
                                                                      VkDescriptorUpdateTemplate tmpl, const void *data)
{
    const struct zss_template *t = (const struct zss_template *)(uintptr_t)tmpl;
    VkWriteDescriptorSet *w = calloc(t->n ? t->n : 1, sizeof(*w));
    void **owned = calloc(t->n ? t->n : 1, sizeof(*owned));

    for (uint32_t i = 0; i < t->n; i++) {
        const VkDescriptorUpdateTemplateEntry *e = &t->entry[i];
        const char *src = (const char *)data + e->offset;

        w[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = e->dstBinding,
            .dstArrayElement = e->dstArrayElement, .descriptorCount = e->descriptorCount, .descriptorType = e->descriptorType,
        };
        switch (e->descriptorType) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
            VkDescriptorImageInfo *a = calloc(e->descriptorCount ? e->descriptorCount : 1, sizeof(*a));

            for (uint32_t j = 0; j < e->descriptorCount; j++)
                memcpy(&a[j], src + j * e->stride, sizeof(a[j]));
            w[i].pImageInfo = owned[i] = a;
            break;
        }
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
            VkBufferView *a = calloc(e->descriptorCount ? e->descriptorCount : 1, sizeof(*a));

            for (uint32_t j = 0; j < e->descriptorCount; j++)
                memcpy(&a[j], src + j * e->stride, sizeof(a[j]));
            w[i].pTexelBufferView = owned[i] = a;
            break;
        }
        default: {
            VkDescriptorBufferInfo *a = calloc(e->descriptorCount ? e->descriptorCount : 1, sizeof(*a));

            for (uint32_t j = 0; j < e->descriptorCount; j++)
                memcpy(&a[j], src + j * e->stride, sizeof(a[j]));
            w[i].pBufferInfo = owned[i] = a;
            break;
        }
        }
    }
    zss_UpdateDescriptorSets(device, t->n, w, 0, NULL);
    for (uint32_t i = 0; i < t->n; i++)
        free(owned[i]);
    free(owned);
    free(w);
}

/* ---- device groups and the swapchain ------------------------------------------------- */

static VKAPI_ATTR VkResult VKAPI_CALL zss_GetDeviceGroupPresentCapabilitiesKHR(VkDevice device,
                                                                               VkDeviceGroupPresentCapabilitiesKHR *out)
{
    (void)device;
    memset(out->presentMask, 0, sizeof(out->presentMask));
    out->presentMask[0] = 1;
    out->modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL zss_GetDeviceGroupSurfacePresentModesKHR(VkDevice device, VkSurfaceKHR surface,
                                                                               VkDeviceGroupPresentModeFlagsKHR *modes)
{
    (void)device; (void)surface;
    *modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL zss_GetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice pd, VkSurfaceKHR surface,
                                                                                uint32_t *count, VkRect2D *rects)
{
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR caps_fn =
        (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)zss_instance_proc("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    VkSurfaceCapabilitiesKHR caps;
    VkResult r;

    if (!rects) {
        *count = 1;
        return VK_SUCCESS;
    }
    if (*count < 1)
        return VK_INCOMPLETE;
    r = caps_fn(pd, surface, &caps);
    if (r != VK_SUCCESS)
        return r;
    rects[0] = (VkRect2D){ { 0, 0 }, caps.currentExtent };
    *count = 1;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL zss_AcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR *info,
                                                               uint32_t *index)
{
    return zss_AcquireNextImageKHR(device, info->swapchain, info->timeout, info->semaphore, info->fence, index);
}

/* ---- tables -------------------------------------------------------------------------- */

#define E(n) { "vk" #n, (PFN_vkVoidFunction)zss_##n }
#define K(n) { "vk" #n "KHR", (PFN_vkVoidFunction)zss_##n }

static const struct zss_entry instance_entries[] = {
    E(EnumerateInstanceVersion), E(EnumeratePhysicalDeviceGroups), K(EnumeratePhysicalDeviceGroups),
    E(GetPhysicalDeviceFeatures2), K(GetPhysicalDeviceFeatures2), E(GetPhysicalDeviceProperties2), K(GetPhysicalDeviceProperties2),
    E(GetPhysicalDeviceFormatProperties2), K(GetPhysicalDeviceFormatProperties2),
    E(GetPhysicalDeviceImageFormatProperties2), K(GetPhysicalDeviceImageFormatProperties2),
    E(GetPhysicalDeviceQueueFamilyProperties2), K(GetPhysicalDeviceQueueFamilyProperties2),
    E(GetPhysicalDeviceMemoryProperties2), K(GetPhysicalDeviceMemoryProperties2),
    E(GetPhysicalDeviceSparseImageFormatProperties2), K(GetPhysicalDeviceSparseImageFormatProperties2),
    E(GetPhysicalDeviceExternalBufferProperties), K(GetPhysicalDeviceExternalBufferProperties),
    E(GetPhysicalDeviceExternalFenceProperties), K(GetPhysicalDeviceExternalFenceProperties),
    E(GetPhysicalDeviceExternalSemaphoreProperties), K(GetPhysicalDeviceExternalSemaphoreProperties),
    E(GetPhysicalDevicePresentRectanglesKHR),
};

static const struct zss_entry device_entries[] = {
    E(GetDeviceQueue2), E(BindBufferMemory2), K(BindBufferMemory2), E(BindImageMemory2), K(BindImageMemory2),
    E(GetBufferMemoryRequirements2), K(GetBufferMemoryRequirements2), E(GetImageMemoryRequirements2),
    K(GetImageMemoryRequirements2), E(GetImageSparseMemoryRequirements2), K(GetImageSparseMemoryRequirements2),
    E(TrimCommandPool), K(TrimCommandPool), E(GetDeviceGroupPeerMemoryFeatures), K(GetDeviceGroupPeerMemoryFeatures),
    E(GetDescriptorSetLayoutSupport), K(GetDescriptorSetLayoutSupport),
    E(CreateSamplerYcbcrConversion), K(CreateSamplerYcbcrConversion), E(DestroySamplerYcbcrConversion),
    K(DestroySamplerYcbcrConversion), E(CreateDescriptorUpdateTemplate), K(CreateDescriptorUpdateTemplate),
    E(DestroyDescriptorUpdateTemplate), K(DestroyDescriptorUpdateTemplate), E(UpdateDescriptorSetWithTemplate),
    K(UpdateDescriptorSetWithTemplate), E(CmdSetDeviceMask), K(CmdSetDeviceMask), E(CmdDispatchBase), K(CmdDispatchBase),
    E(GetDeviceGroupPresentCapabilitiesKHR), E(GetDeviceGroupSurfacePresentModesKHR), E(AcquireNextImage2KHR),
};

static PFN_vkVoidFunction find(const struct zss_entry *table, size_t n, const char *name)
{
    /* With ZSS_VULKAN=1.0 none of this exists, as before. */
    if (zss_api_version() < VK_API_VERSION_1_1)
        return NULL;
    for (size_t i = 0; i < n; i++)
        if (!strcmp(table[i].name, name))
            return table[i].fn;
    return NULL;
}

PFN_vkVoidFunction zss_vk11_instance_proc(const char *name)
{
    return find(instance_entries, sizeof(instance_entries) / sizeof(instance_entries[0]), name);
}

PFN_vkVoidFunction zss_vk11_device_proc(const char *name)
{
    return find(device_entries, sizeof(device_entries) / sizeof(device_entries[0]), name);
}
