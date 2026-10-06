// SPDX-License-Identifier: GPL-2.0-only
/*
 * Devices and the objects that live on them. Every object keeps what is
 * needed to create it again on another real device (see migrate.c).
 */
#include "zss_layer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ENTER(devh) struct zss_dev *dev = (struct zss_dev *)(devh); zss_enter()
#define TRANSFER_BOTH (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
#define IMG_TRANSFER_BOTH (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)

/* ---- objects ---------------------------------------------------------- */

struct zss_obj *zss_obj_new(struct zss_dev *dev, enum zss_kind kind)
{
    struct zss_obj *o = calloc(1, sizeof(*o));

    if (!o)
        return NULL;
    set_loader_magic_value(o);
    o->kind = kind;
    o->refs = 1;
    o->dev = dev;
    pthread_mutex_lock(&zss_lock);
    o->prev = dev->tail;
    if (dev->tail)
        dev->tail->next = o;
    else
        dev->head = o;
    dev->tail = o;
    pthread_mutex_unlock(&zss_lock);
    return o;
}

void zss_obj_ref(struct zss_obj *o)
{
    if (!o)
        return;
    pthread_mutex_lock(&zss_lock);
    o->refs++;
    pthread_mutex_unlock(&zss_lock);
}

static void slots_free(struct zss_obj *o)
{
    for (uint32_t b = 0; b < o->u.dset.nslots; b++) {
        struct zss_slot *s = &o->u.dset.slot[b];

        for (uint32_t i = 0; i < s->count; i++) {
            zss_obj_unref(s->e[i].sampler);
            zss_obj_unref(s->e[i].view);
            zss_obj_unref(s->e[i].buffer);
        }
        free(s->e);
    }
    free(o->u.dset.slot);
    o->u.dset.slot = NULL;
    o->u.dset.nslots = 0;
}

void zss_obj_unref(struct zss_obj *o)
{
    struct zss_dev *dev;

    if (!o)
        return;
    pthread_mutex_lock(&zss_lock);
    if (--o->refs > 0) {
        pthread_mutex_unlock(&zss_lock);
        return;
    }
    dev = o->dev;
    if (o->prev)
        o->prev->next = o->next;
    else
        dev->head = o->next;
    if (o->next)
        o->next->prev = o->prev;
    else
        dev->tail = o->prev;

    switch (o->kind) {
    case ZK_MEMORY: free(o->u.mem.shadow); break;
    case ZK_BUFFER:
        free(o->u.buf.saved);
        zss_retain_release(&o->u.buf.ret);
        break;
    case ZK_IMAGE:
        if (o->u.img.ret)
            for (uint32_t i = 0; i < 2 * o->u.img.ci.mipLevels * o->u.img.ci.arrayLayers; i++)
                zss_retain_release(&o->u.img.ret[i]);
        free(o->u.img.ret);
        free(o->u.img.layout);
        free(o->u.img.saved);
        break;
    case ZK_DSET: slots_free(o); break;
    case ZK_CMDBUF: zss_cmd_reset(o); free(o->u.cb.refs); break;
    case ZK_SWAPCHAIN: free(o->u.sc.images); break;
    default: break;
    }
    for (uint32_t i = 0; i < o->ndeps; i++)
        zss_obj_unref(o->deps[i]);
    for (uint32_t i = 0; i < o->nallocs; i++)
        free(o->allocs[i]);
    free(o->deps);
    free(o->allocs);
    free(o);
    pthread_mutex_unlock(&zss_lock);
}

void zss_obj_dep(struct zss_obj *o, struct zss_obj *dep)
{
    if (!dep)
        return;
    zss_obj_ref(dep);
    o->deps = realloc(o->deps, (o->ndeps + 1) * sizeof(*o->deps));
    o->deps[o->ndeps++] = dep;
}

void *zss_obj_dup(struct zss_obj *o, const void *src, size_t size)
{
    void *p;

    if (!src || !size)
        return NULL;
    p = malloc(size);
    memcpy(p, src, size);
    o->allocs = realloc(o->allocs, (o->nallocs + 1) * sizeof(*o->allocs));
    o->allocs[o->nallocs++] = p;
    return p;
}

void zss_obj_kill(struct zss_obj *o)
{
    o->dead = true;
    o->r = (struct zss_real){ 0 };
    zss_obj_unref(o);
}

void zss_dev_untracked(struct zss_dev *dev, const char *feature)
{
    if (!dev->migratable)
        return;
    dev->migratable = false;
    snprintf(dev->reason, sizeof(dev->reason), "uses %s, which the layer does not track", feature);
    zss_dbg("device is not migratable: %s", dev->reason);
    zss_control_state_changed();
}

VkImageAspectFlags zss_format_aspects(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT:
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

/* ---- the real device --------------------------------------------------- */

static uint32_t fam(const struct zss_dev *dev, uint32_t origin_family)
{
    if (origin_family >= ZSS_MAX_FAMILIES)
        return origin_family; /* VK_QUEUE_FAMILY_IGNORED and friends */
    return dev->fam_map[origin_family] == UINT32_MAX ? 0 : dev->fam_map[origin_family];
}

/* Maps the queue families the application asked for onto the target's. */
static bool map_families(const struct zss_dev *dev, const struct zss_gpu *gpu, uint32_t *map)
{
    bool used[ZSS_MAX_FAMILIES] = { false };

    for (uint32_t i = 0; i < ZSS_MAX_FAMILIES; i++)
        map[i] = UINT32_MAX;
    if (gpu == dev->origin || (gpu->drv == dev->origin->drv && gpu->nfam == dev->origin->nfam &&
                               !memcmp(gpu->fam, dev->origin->fam, gpu->nfam * sizeof(gpu->fam[0])))) {
        for (uint32_t i = 0; i < gpu->nfam; i++)
            map[i] = i;
        return true;
    }
    for (uint32_t i = 0; i < dev->nreq; i++) {
        uint32_t f = dev->req[i].family;
        VkQueueFlags need = dev->origin->fam[f].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
        uint32_t t;

        for (t = 0; t < gpu->nfam; t++)
            if (!used[t] && (gpu->fam[t].queueFlags & need) == need &&
                gpu->fam[t].queueCount >= dev->req[i].count)
                break;
        if (t == gpu->nfam)
            return false;
        used[t] = true;
        map[f] = t;
    }
    return true;
}

bool zss_families_fit(const struct zss_dev *dev, const struct zss_gpu *gpu);
bool zss_families_fit(const struct zss_dev *dev, const struct zss_gpu *gpu)
{
    uint32_t map[ZSS_MAX_FAMILIES];

    return map_families(dev, gpu, map);
}

VkResult zss_dev_create_real(struct zss_dev *dev, struct zss_gpu *gpu)
{
    static const float prio[16] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    static const char *const swapchain_ext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceQueueCreateInfo qci[ZSS_MAX_FAMILIES];
    VkDeviceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    };
    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkPhysicalDevice pd = zss_gpu_real(gpu);
    uint32_t util_req = 0, total = 0, q = 0;
    VkResult r;

    if (!pd)
        return VK_ERROR_DEVICE_LOST;
    if (!map_families(dev, gpu, dev->fam_map))
        return VK_ERROR_FEATURE_NOT_PRESENT;

    for (uint32_t i = 0; i < dev->nreq; i++) {
        qci[i] = (VkDeviceQueueCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = dev->fam_map[dev->req[i].family],
            .queueCount = dev->req[i].count,
            .pQueuePriorities = prio,
        };
        total += dev->req[i].count;
        if (dev->origin->fam[dev->req[i].family].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            util_req = i;
    }
    ci.queueCreateInfoCount = dev->nreq;
    ci.pQueueCreateInfos = qci;
    ci.pEnabledFeatures = &dev->features;
    if (dev->want_swapchain) {
        ci.enabledExtensionCount = 1;
        ci.ppEnabledExtensionNames = swapchain_ext;
    }

    r = gpu->drv->fn.CreateDevice(pd, &ci, NULL, &dev->real);
    if (r != VK_SUCCESS)
        return r;
#define X(n) dev->fn.n = (PFN_vk##n)gpu->drv->fn.GetDeviceProcAddr(dev->real, "vk" #n);
    ZSS_DEV_FNS(X)
#undef X
    dev->gpu = gpu;
    gpu->drv->ndevices++;

    if (!dev->queues) {
        dev->queues = calloc(total ? total : 1, sizeof(*dev->queues));
        dev->nqueues = total;
        for (uint32_t i = 0; i < dev->nreq; i++) {
            for (uint32_t j = 0; j < dev->req[i].count; j++, q++) {
                set_loader_magic_value(&dev->queues[q]);
                dev->queues[q].dev = dev;
                dev->queues[q].family = dev->req[i].family;
                dev->queues[q].index = j;
            }
        }
    }
    for (q = 0; q < dev->nqueues; q++)
        dev->fn.GetDeviceQueue(dev->real, dev->fam_map[dev->queues[q].family], dev->queues[q].index,
                               &dev->queues[q].real);

    dev->util_queue = VK_NULL_HANDLE;
    for (q = 0; q < dev->nqueues; q++)
        if (dev->queues[q].family == dev->req[util_req].family && dev->queues[q].index == 0)
            dev->util_queue = dev->queues[q].real;
    pci.queueFamilyIndex = dev->fam_map[dev->req[util_req].family];
    r = dev->fn.CreateCommandPool(dev->real, &pci, NULL, &dev->util_pool);
    if (r == VK_SUCCESS) {
        cai.commandPool = dev->util_pool;
        r = dev->fn.AllocateCommandBuffers(dev->real, &cai, &dev->util_cmd);
    }
    if (r != VK_SUCCESS) {
        zss_dev_destroy_real(dev);
        return r;
    }
    return VK_SUCCESS;
}

void zss_dev_destroy_real(struct zss_dev *dev)
{
    if (!dev->real)
        return;
    if (dev->util_pool)
        dev->fn.DestroyCommandPool(dev->real, dev->util_pool, NULL);
    dev->fn.DestroyDevice(dev->real, NULL);
    dev->gpu->drv->ndevices--;
    dev->real = VK_NULL_HANDLE;
    dev->util_pool = VK_NULL_HANDLE;
    dev->util_cmd = VK_NULL_HANDLE;
    dev->util_queue = VK_NULL_HANDLE;
    dev->gpu = NULL;
}

VkResult zss_util_begin(struct zss_dev *dev)
{
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    return dev->fn.BeginCommandBuffer(dev->util_cmd, &bi);
}

VkResult zss_util_run(struct zss_dev *dev)
{
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &dev->util_cmd,
    };
    VkResult r = dev->fn.EndCommandBuffer(dev->util_cmd);

    if (r == VK_SUCCESS)
        r = dev->fn.QueueSubmit(dev->util_queue, 1, &si, VK_NULL_HANDLE);
    if (r == VK_SUCCESS)
        r = dev->fn.QueueWaitIdle(dev->util_queue);
    dev->fn.ResetCommandBuffer(dev->util_cmd, 0);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL zss_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
                                                const VkAllocationCallbacks *alloc, VkDevice *out)
{
    struct zss_gpu *gpu = (struct zss_gpu *)pd;
    struct zss_dev *dev = calloc(1, sizeof(*dev));
    VkResult r;

    (void)alloc;
    if (!dev)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    zss_enter();
    set_loader_magic_value(dev);
    dev->origin = gpu;
    dev->migratable = true;
    if (ci->pEnabledFeatures)
        dev->features = *ci->pEnabledFeatures;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
        if (!strcmp(ci->ppEnabledExtensionNames[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME))
            dev->want_swapchain = true;
    for (uint32_t i = 0; i < ci->queueCreateInfoCount && i < ZSS_MAX_FAMILIES; i++) {
        dev->req[dev->nreq].family = ci->pQueueCreateInfos[i].queueFamilyIndex;
        dev->req[dev->nreq++].count = ci->pQueueCreateInfos[i].queueCount;
    }

    r = zss_dev_create_real(dev, gpu);
    if (r != VK_SUCCESS) {
        free(dev);
        zss_leave();
        return r;
    }
    pthread_mutex_lock(&zss_lock);
    dev->next_dev = zss_devices;
    zss_devices = dev;
    pthread_mutex_unlock(&zss_lock);
    zss_control_state_changed();
    *out = (VkDevice)dev;
    zss_leave();
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL zss_DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc)
{
    struct zss_dev **pp;

    (void)alloc;
    if (!device)
        return;
    ENTER(device);
    if (dev->real)
        dev->fn.DeviceWaitIdle(dev->real);
    zss_dev_destroy_real(dev);
    pthread_mutex_lock(&zss_lock);
    for (pp = &zss_devices; *pp; pp = &(*pp)->next_dev) {
        if (*pp == dev) {
            *pp = dev->next_dev;
            break;
        }
    }
    /* Anything the application leaked goes with the device. */
    while (dev->head) {
        dev->head->refs = 1;
        dev->head->ndeps = 0;
        if (dev->head->kind == ZK_DSET) {
            for (uint32_t b = 0; b < dev->head->u.dset.nslots; b++)
                dev->head->u.dset.slot[b].count = 0;
        }
        if (dev->head->kind == ZK_CMDBUF)
            dev->head->u.cb.nrefs = 0;
        zss_obj_unref(dev->head);
    }
    pthread_mutex_unlock(&zss_lock);
    free(dev->queues);
    free(dev);
    zss_control_state_changed();
    zss_leave();
}

VKAPI_ATTR void VKAPI_CALL zss_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index,
                                              VkQueue *out)
{
    struct zss_dev *dev = (struct zss_dev *)device;

    *out = VK_NULL_HANDLE;
    for (uint32_t q = 0; q < dev->nqueues; q++)
        if (dev->queues[q].family == family && dev->queues[q].index == index)
            *out = (VkQueue)&dev->queues[q];
}

/* ---- memory ------------------------------------------------------------ */
/*
 * Application memory objects are virtual. Each buffer and image gets its own
 * real allocation on whatever device is current, and mapped memory is a
 * shadow owned by the layer, so pointers survive a change of device.
 */

VkResult zss_backing_alloc(struct zss_dev *dev, const VkMemoryRequirements *req, bool host,
                           VkDeviceMemory *mem, void **map)
{
    const VkPhysicalDeviceMemoryProperties *mp = &dev->gpu->mem;
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req->size,
    };
    int best = -1, best_score = -1;
    VkResult r;

    for (uint32_t i = 0; i < mp->memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = mp->memoryTypes[i].propertyFlags;
        int score = 0;

        if (!(req->memoryTypeBits & (1u << i)))
            continue;
        if (host) {
            if (!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
                continue;
            score = 1 + ((f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? 4 : 0) +
                    ((f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? 2 : 0);
        } else {
            score = 1 + ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? 4 : 0) +
                    ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? 0 : 2);
        }
        if (score > best_score) {
            best_score = score;
            best = (int)i;
        }
    }
    if (best < 0)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    ai.memoryTypeIndex = (uint32_t)best;
    r = dev->fn.AllocateMemory(dev->real, &ai, NULL, mem);
    if (r != VK_SUCCESS)
        return r;
    if (map) {
        *map = NULL;
        if (host) {
            r = dev->fn.MapMemory(dev->real, *mem, 0, VK_WHOLE_SIZE, 0, map);
            if (r != VK_SUCCESS) {
                dev->fn.FreeMemory(dev->real, *mem, NULL);
                *mem = VK_NULL_HANDLE;
            }
        }
    }
    return r;
}

/*
 * Whether a buffer or linear image has application-visible bytes in a shadow
 * that should be pushed to the device now. `only_mem` is an explicit unmap or
 * flush; without it only memory that is currently mapped is pushed.
 */
static uint8_t *shadow_of(const struct zss_obj *o, const struct zss_obj *only_mem, VkDeviceSize *size)
{
    const struct zss_obj *m;
    VkDeviceSize off;

    if (o->dead || !o->r.map)
        return NULL;
    if (o->kind == ZK_BUFFER) {
        m = o->u.buf.mem;
        off = o->u.buf.mem_off;
        *size = o->u.buf.ci.size;
    } else if (o->kind == ZK_IMAGE) {
        m = o->u.img.mem;
        off = o->u.img.mem_off;
        *size = o->u.img.map_size;
    } else {
        return NULL;
    }
    if (!m || !m->u.mem.shadow || off >= m->u.mem.size)
        return NULL;
    if (only_mem ? m != only_mem : !m->u.mem.mapped)
        return NULL;
    if (*size > m->u.mem.size - off)
        *size = m->u.mem.size - off;
    return m->u.mem.shadow + off;
}

void zss_sync_to_device(struct zss_dev *dev, struct zss_obj *only_mem)
{
    /* A lost device's mappings may no longer be backed by anything. */
    if (dev->lost || !dev->real)
        return;
    pthread_mutex_lock(&zss_lock);
    for (struct zss_obj *o = dev->head; o; o = o->next) {
        VkDeviceSize size;
        uint8_t *src = shadow_of(o, only_mem, &size);

        if (src && !(o->kind == ZK_BUFFER && o->u.buf.gpu_written))
            memcpy(o->r.map, src, size);
    }
    pthread_mutex_unlock(&zss_lock);
}

void zss_sync_from_device(struct zss_dev *dev, struct zss_obj *only_mem)
{
    if (dev->lost || !dev->real)
        return;
    pthread_mutex_lock(&zss_lock);
    for (struct zss_obj *o = dev->head; o; o = o->next) {
        if (o->kind != ZK_BUFFER || !o->u.buf.gpu_written || o->dead || !o->r.map ||
            !o->u.buf.mem || !o->u.buf.mem->u.mem.shadow)
            continue;
        if (only_mem && o->u.buf.mem != only_mem)
            continue;
        memcpy(o->u.buf.mem->u.mem.shadow + o->u.buf.mem_off, o->r.map, o->u.buf.ci.size);
        o->u.buf.gpu_written = false;
    }
    pthread_mutex_unlock(&zss_lock);
}

VKAPI_ATTR VkResult VKAPI_CALL zss_AllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info,
                                                  const VkAllocationCallbacks *alloc,
                                                  VkDeviceMemory *out)
{
    ENTER(device);
    struct zss_obj *o = zss_obj_new(dev, ZK_MEMORY);

    (void)alloc;
    o->u.mem.size = info->allocationSize;
    if (info->memoryTypeIndex < dev->origin->mem.memoryTypeCount)
        o->u.mem.flags = dev->origin->mem.memoryTypes[info->memoryTypeIndex].propertyFlags;
    *out = ZHANDLE(VkDeviceMemory, o);
    zss_leave();
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL zss_FreeMemory(VkDevice device, VkDeviceMemory memory,
                                          const VkAllocationCallbacks *alloc)
{
    (void)device;
    (void)alloc;
    if (!memory)
        return;
    zss_enter();
    zss_obj_kill(ZOBJ(memory));
    zss_leave();
}

VKAPI_ATTR VkResult VKAPI_CALL zss_MapMemory(VkDevice device, VkDeviceMemory memory,
                                             VkDeviceSize offset, VkDeviceSize size,
                                             VkMemoryMapFlags flags, void **data)
{
    ENTER(device);
    struct zss_obj *m = ZOBJ(memory);

    (void)size;
    (void)flags;
    if (!m->u.mem.shadow)
        m->u.mem.shadow = calloc(1, m->u.mem.size ? m->u.mem.size : 1);
    if (!m->u.mem.shadow) {
        zss_leave();
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    m->u.mem.mapped = true;
    zss_sync_from_device(dev, m);
    *data = m->u.mem.shadow + offset;
    zss_leave();
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL zss_UnmapMemory(VkDevice device, VkDeviceMemory memory)
{
    ENTER(device);
    struct zss_obj *m = ZOBJ(memory);

    zss_sync_to_device(dev, m);
    m->u.mem.mapped = false;
    zss_leave();
}

VKAPI_ATTR VkResult VKAPI_CALL zss_FlushMappedMemoryRanges(VkDevice device, uint32_t n,
                                                           const VkMappedMemoryRange *ranges)
{
    ENTER(device);
    for (uint32_t i = 0; i < n; i++)
        zss_sync_to_device(dev, ZOBJ(ranges[i].memory));
    zss_leave();
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL zss_InvalidateMappedMemoryRanges(VkDevice device, uint32_t n,
                                                                const VkMappedMemoryRange *ranges)
{
    ENTER(device);
    for (uint32_t i = 0; i < n; i++)
        zss_sync_from_device(dev, ZOBJ(ranges[i].memory));
    zss_leave();
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL zss_GetDeviceMemoryCommitment(VkDevice device, VkDeviceMemory memory,
                                                         VkDeviceSize *committed)
{
    (void)device;
    *committed = ZOBJ(memory)->u.mem.size;
}

static uint32_t all_origin_types(const struct zss_dev *dev)
{
    uint32_t n = dev->origin->mem.memoryTypeCount;

    return n >= 32 ? UINT32_MAX : (1u << n) - 1;
}

/* ---- creating and destroying the real half of an object ------------------ */

static VkResult real_buffer(struct zss_dev *dev, struct zss_obj *o)
{
    VkBufferCreateInfo ci = o->u.buf.ci;
    uint32_t qfi[ZSS_MAX_FAMILIES];
    VkMemoryRequirements req;
    VkBuffer b;
    VkResult r;

    ci.usage |= TRANSFER_BOTH;
    if (ci.sharingMode == VK_SHARING_MODE_CONCURRENT) {
        for (uint32_t i = 0; i < ci.queueFamilyIndexCount && i < ZSS_MAX_FAMILIES; i++)
            qfi[i] = fam(dev, ci.pQueueFamilyIndices[i]);
        ci.pQueueFamilyIndices = qfi;
    }
    r = dev->fn.CreateBuffer(dev->real, &ci, NULL, &b);
    if (r != VK_SUCCESS)
        return r;
    /* Came back from a driver that was replaced meanwhile: `b` belongs to the old device. */
    if (zss_stale())
        return VK_ERROR_DEVICE_LOST;
    o->r.h = (uint64_t)(uintptr_t)b;
    if (!o->u.buf.mem)
        return VK_SUCCESS;

    dev->fn.GetBufferMemoryRequirements(dev->real, b, &req);
    r = zss_backing_alloc(dev, &req, o->u.buf.mem->u.mem.flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                          &o->r.backing, &o->r.map);
    if (r == VK_SUCCESS)
        r = dev->fn.BindBufferMemory(dev->real, b, o->r.backing, 0);
    return r;
}

static VkResult real_image(struct zss_dev *dev, struct zss_obj *o)
{
    VkImageCreateInfo ci = o->u.img.ci;
    uint32_t qfi[ZSS_MAX_FAMILIES];
    VkMemoryRequirements req;
    VkImage img;
    VkResult r;
    bool host;

    ci.usage |= IMG_TRANSFER_BOTH;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (ci.sharingMode == VK_SHARING_MODE_CONCURRENT) {
        for (uint32_t i = 0; i < ci.queueFamilyIndexCount && i < ZSS_MAX_FAMILIES; i++)
            qfi[i] = fam(dev, ci.pQueueFamilyIndices[i]);
        ci.pQueueFamilyIndices = qfi;
    }
    r = dev->fn.CreateImage(dev->real, &ci, NULL, &img);
    if (r != VK_SUCCESS)
        return r;
    if (zss_stale())
        return VK_ERROR_DEVICE_LOST;
    o->r.h = (uint64_t)(uintptr_t)img;
    o->r.standin = o->u.img.swapchain != NULL;
    if (!o->u.img.mem && !o->u.img.swapchain)
        return VK_SUCCESS;

    host = o->u.img.mem && ci.tiling == VK_IMAGE_TILING_LINEAR &&
           (o->u.img.mem->u.mem.flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    dev->fn.GetImageMemoryRequirements(dev->real, img, &req);
    r = zss_backing_alloc(dev, &req, host, &o->r.backing, &o->r.map);
    if (r == VK_SUCCESS)
        r = dev->fn.BindImageMemory(dev->real, img, o->r.backing, 0);
    return r;
}

static VkResult real_pipeline(struct zss_dev *dev, struct zss_obj *o)
{
    VkGraphicsPipelineCreateInfo ci = o->u.pipe.ci;
    VkPipelineShaderStageCreateInfo *stages = malloc(ci.stageCount * sizeof(*stages));
    VkPipeline p;
    VkResult r;

    for (uint32_t i = 0; i < ci.stageCount; i++) {
        stages[i] = ci.pStages[i];
        stages[i].module = ZREAL(VkShaderModule, ci.pStages[i].module);
    }
    ci.pStages = stages;
    ci.layout = ZREAL(VkPipelineLayout, ci.layout);
    ci.renderPass = ZREAL(VkRenderPass, ci.renderPass);
    ci.basePipelineHandle = ZREAL(VkPipeline, ci.basePipelineHandle);
    r = dev->fn.CreateGraphicsPipelines(dev->real, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    free(stages);
    if (r == VK_SUCCESS)
        o->r.h = (uint64_t)(uintptr_t)p;
    return r;
}

VkResult zss_real_create(struct zss_dev *dev, struct zss_obj *o)
{
    VkDevice d = dev->real;
    uint64_t h = 0;
    VkResult r = VK_SUCCESS;

#define OUT(T) ((T *)&h)
    switch (o->kind) {
    case ZK_MEMORY:
        return VK_SUCCESS;
    case ZK_BUFFER:
        return real_buffer(dev, o);
    case ZK_IMAGE:
        return real_image(dev, o);
    case ZK_VIEW: {
        VkImageViewCreateInfo ci = o->u.view.ci;

        ci.image = ZREAL(VkImage, ci.image);
        r = dev->fn.CreateImageView(d, &ci, NULL, OUT(VkImageView));
        break;
    }
    case ZK_SAMPLER:
        r = dev->fn.CreateSampler(d, &o->u.sampler.ci, NULL, OUT(VkSampler));
        break;
    case ZK_SHADER:
        r = dev->fn.CreateShaderModule(d, &o->u.shader.ci, NULL, OUT(VkShaderModule));
        break;
    case ZK_RENDERPASS:
        r = dev->fn.CreateRenderPass(d, &o->u.rp.ci, NULL, OUT(VkRenderPass));
        break;
    case ZK_FRAMEBUFFER: {
        VkFramebufferCreateInfo ci = o->u.fb.ci;
        VkImageView views[32];

        for (uint32_t i = 0; i < ci.attachmentCount && i < 32; i++)
            views[i] = ZREAL(VkImageView, ci.pAttachments[i]);
        ci.pAttachments = views;
        ci.renderPass = ZREAL(VkRenderPass, ci.renderPass);
        r = dev->fn.CreateFramebuffer(d, &ci, NULL, OUT(VkFramebuffer));
        break;
    }
    case ZK_DSL: {
        VkDescriptorSetLayoutCreateInfo ci = o->u.dsl.ci;
        VkDescriptorSetLayoutBinding *b = malloc((ci.bindingCount + 1) * sizeof(*b));
        VkSampler *samplers = NULL;
        uint32_t ns = 0, at = 0;

        for (uint32_t i = 0; i < ci.bindingCount; i++)
            if (ci.pBindings[i].pImmutableSamplers)
                ns += ci.pBindings[i].descriptorCount;
        samplers = malloc((ns + 1) * sizeof(*samplers));
        for (uint32_t i = 0; i < ci.bindingCount; i++) {
            b[i] = ci.pBindings[i];
            if (!b[i].pImmutableSamplers)
                continue;
            for (uint32_t j = 0; j < b[i].descriptorCount; j++)
                samplers[at + j] = ZREAL(VkSampler, ci.pBindings[i].pImmutableSamplers[j]);
            b[i].pImmutableSamplers = &samplers[at];
            at += b[i].descriptorCount;
        }
        ci.pBindings = b;
        r = dev->fn.CreateDescriptorSetLayout(d, &ci, NULL, OUT(VkDescriptorSetLayout));
        free(b);
        free(samplers);
        break;
    }
    case ZK_DPOOL:
        r = dev->fn.CreateDescriptorPool(d, &o->u.dpool.ci, NULL, OUT(VkDescriptorPool));
        break;
    case ZK_DSET: {
        VkDescriptorSetLayout layout = ZREAL(VkDescriptorSetLayout, o->u.dset.layout);
        VkDescriptorSetAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = ZREAL(VkDescriptorPool, o->u.dset.pool),
            .descriptorSetCount = 1,
            .pSetLayouts = &layout,
        };

        r = dev->fn.AllocateDescriptorSets(d, &ai, OUT(VkDescriptorSet));
        break;
    }
    case ZK_PLAYOUT: {
        VkPipelineLayoutCreateInfo ci = o->u.playout.ci;
        VkDescriptorSetLayout layouts[32];

        for (uint32_t i = 0; i < ci.setLayoutCount && i < 32; i++)
            layouts[i] = ZREAL(VkDescriptorSetLayout, ci.pSetLayouts[i]);
        ci.pSetLayouts = layouts;
        r = dev->fn.CreatePipelineLayout(d, &ci, NULL, OUT(VkPipelineLayout));
        break;
    }
    case ZK_PCACHE: {
        VkPipelineCacheCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };

        r = dev->fn.CreatePipelineCache(d, &ci, NULL, OUT(VkPipelineCache));
        break;
    }
    case ZK_PIPELINE:
        return real_pipeline(dev, o);
    case ZK_CPOOL: {
        VkCommandPoolCreateInfo ci = o->u.cpool.ci;

        ci.queueFamilyIndex = fam(dev, ci.queueFamilyIndex);
        r = dev->fn.CreateCommandPool(d, &ci, NULL, OUT(VkCommandPool));
        break;
    }
    case ZK_CMDBUF: {
        VkCommandBufferAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = ZREAL(VkCommandPool, o->u.cb.pool),
            .level = o->u.cb.level,
            .commandBufferCount = 1,
        };

        r = dev->fn.AllocateCommandBuffers(d, &ai, OUT(VkCommandBuffer));
        break;
    }
    case ZK_FENCE: {
        VkFenceCreateInfo ci = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = o->u.fence.signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0,
        };

        r = dev->fn.CreateFence(d, &ci, NULL, OUT(VkFence));
        break;
    }
    case ZK_SEMAPHORE: {
        VkSemaphoreCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

        r = dev->fn.CreateSemaphore(d, &ci, NULL, OUT(VkSemaphore));
        break;
    }
    default:
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
#undef OUT
    if (r == VK_SUCCESS)
        o->r.h = h;
    return r;
}

void zss_real_destroy(struct zss_dev *dev, enum zss_kind kind, struct zss_real *r)
{
    VkDevice d = dev->real;
    uint64_t h = r->h;

    if (!d)
        goto out;
#define H(T) ((T)(uintptr_t)h)
    switch (kind) {
    case ZK_BUFFER: if (h) dev->fn.DestroyBuffer(d, H(VkBuffer), NULL); break;
    case ZK_IMAGE: if (h && !r->borrowed) dev->fn.DestroyImage(d, H(VkImage), NULL); break;
    case ZK_VIEW: if (h) dev->fn.DestroyImageView(d, H(VkImageView), NULL); break;
    case ZK_SAMPLER: if (h) dev->fn.DestroySampler(d, H(VkSampler), NULL); break;
    case ZK_SHADER: if (h) dev->fn.DestroyShaderModule(d, H(VkShaderModule), NULL); break;
    case ZK_RENDERPASS: if (h) dev->fn.DestroyRenderPass(d, H(VkRenderPass), NULL); break;
    case ZK_FRAMEBUFFER: if (h) dev->fn.DestroyFramebuffer(d, H(VkFramebuffer), NULL); break;
    case ZK_DSL: if (h) dev->fn.DestroyDescriptorSetLayout(d, H(VkDescriptorSetLayout), NULL); break;
    case ZK_DPOOL: if (h) dev->fn.DestroyDescriptorPool(d, H(VkDescriptorPool), NULL); break;
    case ZK_PLAYOUT: if (h) dev->fn.DestroyPipelineLayout(d, H(VkPipelineLayout), NULL); break;
    case ZK_PCACHE: if (h) dev->fn.DestroyPipelineCache(d, H(VkPipelineCache), NULL); break;
    case ZK_PIPELINE:
    case ZK_OPAQUE_PIPELINE: if (h) dev->fn.DestroyPipeline(d, H(VkPipeline), NULL); break;
    case ZK_CPOOL: if (h) dev->fn.DestroyCommandPool(d, H(VkCommandPool), NULL); break;
    case ZK_FENCE: if (h) dev->fn.DestroyFence(d, H(VkFence), NULL); break;
    case ZK_SEMAPHORE: if (h) dev->fn.DestroySemaphore(d, H(VkSemaphore), NULL); break;
    case ZK_SWAPCHAIN: if (h) dev->fn.DestroySwapchainKHR(d, H(VkSwapchainKHR), NULL); break;
    case ZK_OPAQUE_EVENT: if (h) dev->fn.DestroyEvent(d, H(VkEvent), NULL); break;
    case ZK_OPAQUE_QUERYPOOL: if (h) dev->fn.DestroyQueryPool(d, H(VkQueryPool), NULL); break;
    case ZK_OPAQUE_BUFFERVIEW: if (h) dev->fn.DestroyBufferView(d, H(VkBufferView), NULL); break;
    default: break; /* sets and command buffers go with their pools */
    }
#undef H
    if (r->backing)
        dev->fn.FreeMemory(d, r->backing, NULL);
out:
    *r = (struct zss_real){ 0 };
}

/* Shared tail of every create entry point. */
/*
 * If the device was lost under a create call, recovery has rebuilt every
 * object in the list, including the one being created. Nothing to repeat:
 * the object either exists on the new device or could not be made there.
 */
static VkResult create_real(struct zss_dev *dev, struct zss_obj *o)
{
    VkResult r = zss_real_create(dev, o);

    if (zss_lost(dev, r))
        r = o->r.h ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
    return r;
}

static VkResult finish_create(struct zss_dev *dev, struct zss_obj *o, uint64_t *out)
{
    VkResult r = create_real(dev, o);

    if (r != VK_SUCCESS) {
        zss_real_destroy(dev, o->kind, &o->r);
        zss_obj_kill(o);
        *out = 0;
    } else {
        *out = (uint64_t)(uintptr_t)o;
    }
    zss_leave();
    return r;
}

static void destroy_obj(struct zss_dev *dev, struct zss_obj *o)
{
    struct zss_real r = o->r;

    zss_real_destroy(dev, o->kind, &r);
    zss_obj_kill(o);
}

#define DESTROY_FN(Name, T) \
    VKAPI_ATTR void VKAPI_CALL zss_Destroy##Name(VkDevice device, T h, \
                                                 const VkAllocationCallbacks *alloc) \
    { \
        (void)alloc; \
        if (!h) \
            return; \
        ENTER(device); \
        destroy_obj(dev, ZOBJ(h)); \
        zss_leave(); \
    }

DESTROY_FN(Buffer, VkBuffer)
DESTROY_FN(ImageView, VkImageView)
DESTROY_FN(Sampler, VkSampler)
DESTROY_FN(ShaderModule, VkShaderModule)
DESTROY_FN(RenderPass, VkRenderPass)
DESTROY_FN(Framebuffer, VkFramebuffer)
DESTROY_FN(DescriptorSetLayout, VkDescriptorSetLayout)
DESTROY_FN(PipelineLayout, VkPipelineLayout)
DESTROY_FN(PipelineCache, VkPipelineCache)
DESTROY_FN(Pipeline, VkPipeline)
DESTROY_FN(Fence, VkFence)
DESTROY_FN(Semaphore, VkSemaphore)
DESTROY_FN(Event, VkEvent)
DESTROY_FN(QueryPool, VkQueryPool)
DESTROY_FN(BufferView, VkBufferView)

/* ---- buffers and images ---------------------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL zss_CreateBuffer(VkDevice device, const VkBufferCreateInfo *ci,
                                                const VkAllocationCallbacks *alloc, VkBuffer *out)
{
    ENTER(device);
    struct zss_obj *o = zss_obj_new(dev, ZK_BUFFER);

    (void)alloc;
    o->u.buf.ci = *ci;
    o->u.buf.ci.pNext = NULL;
    o->u.buf.ci.pQueueFamilyIndices =
        ci->sharingMode == VK_SHARING_MODE_CONCURRENT
            ? zss_obj_dup(o, ci->pQueueFamilyIndices, ci->queueFamilyIndexCount * sizeof(uint32_t))
            : NULL;
    if (ci->flags & (VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT))
        zss_dev_untracked(dev, "sparse buffers");
    return finish_create(dev, o, (uint64_t *)out);
}

VKAPI_ATTR void VKAPI_CALL zss_GetBufferMemoryRequirements(VkDevice device, VkBuffer buffer,
                                                           VkMemoryRequirements *req)
{
    ENTER(device);
    dev->fn.GetBufferMemoryRequirements(dev->real, ZREAL(VkBuffer, buffer), req);
    req->memoryTypeBits = all_origin_types(dev);
    zss_leave();
}

VKAPI_ATTR VkResult VKAPI_CALL zss_BindBufferMemory(VkDevice device, VkBuffer buffer,
                                                    VkDeviceMemory memory, VkDeviceSize offset)
{
    ENTER(device);
    struct zss_obj *o = ZOBJ(buffer), *m = ZOBJ(memory);
    VkMemoryRequirements req;
    VkResult r;

    o->u.buf.mem = m;
    o->u.buf.mem_off = offset;
    zss_obj_dep(o, m);
    dev->fn.GetBufferMemoryRequirements(dev->real, ZREAL(VkBuffer, buffer), &req);
    /* If the device was replaced under this thread, the rebuild has already done the binding. */
    r = zss_stale() ? VK_ERROR_DEVICE_LOST
                    : zss_backing_alloc(dev, &req, m->u.mem.flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                        &o->r.backing, &o->r.map);
    if (r == VK_SUCCESS)
        r = dev->fn.BindBufferMemory(dev->real, ZREAL(VkBuffer, buffer), o->r.backing, 0);
    if (zss_lost(dev, r))
        r = o->r.backing ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
    zss_leave();
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL zss_CreateImage(VkDevice device, const VkImageCreateInfo *ci,
                                               const VkAllocationCallbacks *alloc, VkImage *out)
{
    ENTER(device);
    struct zss_obj *o = zss_obj_new(dev, ZK_IMAGE);
    struct zss_format_info fi;
    VkImageAspectFlags aspects = zss_format_aspects(ci->format);
    uint32_t n = ci->mipLevels * ci->arrayLayers;

    (void)alloc;
    o->u.img.ci = *ci;
    o->u.img.ci.pNext = NULL;
    o->u.img.ci.pQueueFamilyIndices =
        ci->sharingMode == VK_SHARING_MODE_CONCURRENT
            ? zss_obj_dup(o, ci->pQueueFamilyIndices, ci->queueFamilyIndexCount * sizeof(uint32_t))
            : NULL;
    o->u.img.layout = calloc(n ? n : 1, sizeof(VkImageLayout));
    o->u.img.ret = calloc(2 * (n ? n : 1), sizeof(struct zss_ret));
    for (uint32_t i = 0; i < n; i++)
        o->u.img.layout[i] = ci->initialLayout;

    for (VkImageAspectFlags bit = 1; bit <= VK_IMAGE_ASPECT_STENCIL_BIT; bit <<= 1) {
        if ((aspects & bit) && !zss_format_info(ci->format, bit, &fi)) {
            char what[64];

            snprintf(what, sizeof(what), "image format %d", (int)ci->format);
            zss_dev_untracked(dev, what);
        }
    }
    if (ci->flags & (VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT))
        zss_dev_untracked(dev, "sparse images");
    return finish_create(dev, o, (uint64_t *)out);
}

VKAPI_ATTR void VKAPI_CALL zss_DestroyImage(VkDevice device, VkImage image,
                                            const VkAllocationCallbacks *alloc)
{
    (void)alloc;
    if (!image || ZOBJ(image)->u.img.swapchain)
        return; /* swapchain images go with their swapchain */
    ENTER(device);
    destroy_obj(dev, ZOBJ(image));
    zss_leave();
}

VKAPI_ATTR void VKAPI_CALL zss_GetImageMemoryRequirements(VkDevice device, VkImage image,
                                                          VkMemoryRequirements *req)
{
    ENTER(device);
    dev->fn.GetImageMemoryRequirements(dev->real, ZREAL(VkImage, image), req);
    req->memoryTypeBits = all_origin_types(dev);
    zss_leave();
}

VKAPI_ATTR void VKAPI_CALL zss_GetImageSparseMemoryRequirements(
    VkDevice device, VkImage image, uint32_t *count, VkSparseImageMemoryRequirements *req)
{
    (void)device; (void)image; (void)req;
    *count = 0;
}

VKAPI_ATTR VkResult VKAPI_CALL zss_BindImageMemory(VkDevice device, VkImage image,
                                                   VkDeviceMemory memory, VkDeviceSize offset)
{
    ENTER(device);
    struct zss_obj *o = ZOBJ(image), *m = ZOBJ(memory);
    VkMemoryRequirements req;
    VkResult r;
    bool host = o->u.img.ci.tiling == VK_IMAGE_TILING_LINEAR &&
                (m->u.mem.flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

    o->u.img.mem = m;
    o->u.img.mem_off = offset;
    zss_obj_dep(o, m);
    dev->fn.GetImageMemoryRequirements(dev->real, ZREAL(VkImage, image), &req);
    o->u.img.map_size = req.size;
    r = zss_stale() ? VK_ERROR_DEVICE_LOST : zss_backing_alloc(dev, &req, host, &o->r.backing, &o->r.map);
    if (r == VK_SUCCESS)
        r = dev->fn.BindImageMemory(dev->real, ZREAL(VkImage, image), o->r.backing, 0);
    if (zss_lost(dev, r))
        r = o->r.backing ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
    zss_leave();
    return r;
}

VKAPI_ATTR void VKAPI_CALL zss_GetImageSubresourceLayout(VkDevice device, VkImage image,
                                                         const VkImageSubresource *sub,
                                                         VkSubresourceLayout *layout)
{
    ENTER(device);
    dev->fn.GetImageSubresourceLayout(dev->real, ZREAL(VkImage, image), sub, layout);
    zss_leave();
}

VKAPI_ATTR VkResult VKAPI_CALL zss_CreateImageView(VkDevice device, const VkImageViewCreateInfo *ci,
                                                   const VkAllocationCallbacks *alloc,
                                                   VkImageView *out)
{
    ENTER(device);
    struct zss_obj *o = zss_obj_new(dev, ZK_VIEW);

    (void)alloc;
    o->u.view.ci = *ci;
    o->u.view.ci.pNext = NULL;
    zss_obj_dep(o, ZOBJ(ci->image));
    return finish_create(dev, o, (uint64_t *)out);
}

VKAPI_ATTR VkResult VKAPI_CALL zss_CreateSampler(VkDevice device, const VkSamplerCreateInfo *ci,
                                                 const VkAllocationCallbacks *alloc, VkSampler *out)
{
    ENTER(device);
    struct zss_obj *o = zss_obj_new(dev, ZK_SAMPLER);

    (void)alloc;
    o->u.sampler.ci = *ci;
    o->u.sampler.ci.pNext = NULL;
    return finish_create(dev, o, (uint64_t *)out);
}

VKAPI_ATTR VkResult VKAPI_CALL zss_CreateShaderModule(VkDevice device,
                                                      const VkShaderModuleCreateInfo *ci,
                                                      const VkAllocationCallbacks *alloc,
                                                      VkShaderModule *out)
{
    ENTER(device);
    struct zss_obj *o = zss_obj_new(dev, ZK_SHADER);

    (void)alloc;
    o->u.shader.ci = *ci;
    o->u.shader.ci.pNext = NULL;
    o->u.shader.ci.pCode = zss_obj_dup(o, ci->pCode, ci->codeSize);
    return finish_create(dev, o, (uint64_t *)out);
}

/* ---- render passes and framebuffers ----------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL zss_CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *ci,
                                                    const VkAllocationCallbacks *alloc,
                                                    VkRenderPass *out)
{
    ENTER(device);
    struct zss_obj *o = zss_obj_new(dev, ZK_RENDERPASS);
    VkSubpassDescription *sub;

    (void)alloc;
    o->u.rp.ci = *ci;
    o->u.rp.ci.pNext = NULL;
    o->u.rp.ci.pAttachments = zss_obj_dup(o, ci->pAttachments, ci->attachmentCount * sizeof(*ci->pAttachments));
    o->u.rp.ci.pDependencies = zss_obj_dup(o, ci->pDependencies, ci->dependencyCount * sizeof(*ci->pDependencies));
    sub = zss_obj_dup(o, ci->pSubpasses, ci->subpassCount * sizeof(*ci->pSubpasses));
    for (uint32_t i = 0; i < ci->subpassCount; i++) {
        const VkSubpassDescription *s = &ci->pSubpasses[i];

        sub[i].pInputAttachments = zss_obj_dup(o, s->pInputAttachments, s->inputAttachmentCount * sizeof(VkAttachmentReference));
        sub[i].pColorAttachments = zss_obj_dup(o, s->pColorAttachments, s->colorAttachmentCount * sizeof(VkAttachmentReference));
        sub[i].pResolveAttachments = s->pResolveAttachments
            ? zss_obj_dup(o, s->pResolveAttachments, s->colorAttachmentCount * sizeof(VkAttachmentReference)) : NULL;
        sub[i].pDepthStencilAttachment = zss_obj_dup(o, s->pDepthStencilAttachment, sizeof(VkAttachmentReference));
        sub[i].pPreserveAttachments = zss_obj_dup(o, s->pPreserveAttachments, s->preserveAttachmentCount * sizeof(uint32_t));
    }
    o->u.rp.ci.pSubpasses = sub;
    return finish_create(dev, o, (uint64_t *)out);
}

VKAPI_ATTR VkResult VKAPI_CALL zss_CreateFramebuffer(VkDevice device,
                                                     const VkFramebufferCreateInfo *ci,
                                                     const VkAllocationCallbacks *alloc,
                                                     VkFramebuffer *out)
{
    ENTER(device);
    struct zss_obj *o = zss_obj_new(dev, ZK_FRAMEBUFFER);

    (void)alloc;
    o->u.fb.ci = *ci;
    o->u.fb.ci.pNext = NULL;
    o->u.fb.ci.pAttachments = zss_obj_dup(o, ci->pAttachments, ci->attachmentCount * sizeof(VkImageView));
    zss_obj_dep(o, ZOBJ(ci->renderPass));
    for (uint32_t i = 0; i < ci->attachmentCount; i++)
        zss_obj_dep(o, ZOBJ(ci->pAttachments[i]));
    if (ci->attachmentCount > 32)
        zss_dev_untracked(dev, "framebuffers with more than 32 attachments");
    return finish_create(dev, o, (uint64_t *)out);
}

VKAPI_ATTR void VKAPI_CALL zss_GetRenderAreaGranularity(VkDevice device, VkRenderPass rp,
                                                        VkExtent2D *granularity)
{
    ENTER(device);
    dev->fn.GetRenderAreaGranularity(dev->real, ZREAL(VkRenderPass, rp), granularity);
    zss_leave();
}

/* ---- descriptors ----------------------------------------------------------- */

static bool type_has_sampler_array(VkDescriptorType t)
{
    return t == VK_DESCRIPTOR_TYPE_SAMPLER || t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

VKAPI_ATTR VkResult VKAPI_CALL zss_CreateDescriptorSetLayout(
    VkDevice device, const VkDescriptorSetLayoutCreateInfo *ci, const VkAllocationCallbacks *alloc,
    VkDescriptorSetLayout *out)
{
    ENTER(device);
    struct zss_obj *o = zss_obj_new(dev, ZK_DSL);
    VkDescriptorSetLayoutBinding *b;

    (void)alloc;
    o->u.dsl.ci = *ci;
    o->u.dsl.ci.pNext = NULL;
    b = zss_obj_dup(o, ci->pBindings, ci->bindingCount * sizeof(*b));
    for (uint32_t i = 0; i < ci->bindingCount; i++) {
        if (!type_has_sampler_array(b[i].descriptorType) || !b[i].pImmutableSamplers) {
            b[i].pImmutableSamplers = NULL;
            continue;
        }
        b[i].pImmutableSamplers = zss_obj_dup(o, ci->pBindings[i].pImmutableSamplers,
                                              b[i].descriptorCount * sizeof(VkSampler));
        for (uint32_t j = 0; j < b[i].descriptorCount; j++)
            zss_obj_dep(o, ZOBJ(b[i].pImmutableSamplers[j]));
    }
    o->u.dsl.ci.pBindings = b;
    return finish_create(dev, o, (uint64_t *)out);
}

VKAPI_ATTR VkResult VKAPI_CALL zss_CreateDescriptorPool(VkDevice device,
                                                        const VkDescriptorPoolCreateInfo *ci,
                                                        const VkAllocationCallbacks *alloc,
                                                        VkDescriptorPool *out)
{
    ENTER(device);
    struct zss_obj *o = zss_obj_new(dev, ZK_DPOOL);

    (void)alloc;
    o->u.dpool.ci = *ci;
    o->u.dpool.ci.pNext = NULL;
    o->u.dpool.ci.pPoolSizes = zss_obj_dup(o, ci->pPoolSizes, ci->poolSizeCount * sizeof(*ci->pPoolSizes));
    return finish_create(dev, o, (uint64_t *)out);
}

/* Drops every live object of `kind` whose parent (pool) is `parent`. */
static void kill_children(struct zss_dev *dev, enum zss_kind kind, struct zss_obj *parent)
{
    struct zss_obj **victims = NULL;
    uint32_t n = 0, cap = 0;

    pthread_mutex_lock(&zss_lock);
    for (struct zss_obj *o = dev->head; o; o = o->next) {
        if (o->kind != kind || o->dead)
            continue;
        if ((kind == ZK_DSET ? o->u.dset.pool : o->u.cb.pool) != parent)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            victims = realloc(victims, cap * sizeof(*victims));
        }
        victims[n++] = o;
    }
    for (uint32_t i = 0; i < n; i++)
        zss_obj_kill(victims[i]);
    pthread_mutex_unlock(&zss_lock);
    free(victims);
}

VKAPI_ATTR void VKAPI_CALL zss_DestroyDescriptorPool(VkDevice device, VkDescriptorPool pool,
                                                     const VkAllocationCallbacks *alloc)
{
    (void)alloc;
    if (!pool)
        return;
    ENTER(device);
    kill_children(dev, ZK_DSET, ZOBJ(pool));
    destroy_obj(dev, ZOBJ(pool));
    zss_leave();
}

VKAPI_ATTR VkResult VKAPI_CALL zss_ResetDescriptorPool(VkDevice device, VkDescriptorPool pool,
                                                       VkDescriptorPoolResetFlags flags)
{
    ENTER(device);
    VkResult r;

    ZSS_RETRY(dev, r, dev->fn.ResetDescriptorPool(dev->real, ZREAL(VkDescriptorPool, pool), flags));

    kill_children(dev, ZK_DSET, ZOBJ(pool));
    zss_leave();
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL zss_AllocateDescriptorSets(VkDevice device,
                                                          const VkDescriptorSetAllocateInfo *info,
                                                          VkDescriptorSet *out)
{
    ENTER(device);
    VkResult r = VK_SUCCESS;

    for (uint32_t i = 0; i < info->descriptorSetCount; i++)
        out[i] = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < info->descriptorSetCount && r == VK_SUCCESS; i++) {
        struct zss_obj *o = zss_obj_new(dev, ZK_DSET);
        struct zss_obj *layout = ZOBJ(info->pSetLayouts[i]);
        const VkDescriptorSetLayoutCreateInfo *lci = &layout->u.dsl.ci;

        o->u.dset.pool = ZOBJ(info->descriptorPool);
        o->u.dset.layout = layout;
        zss_obj_dep(o, o->u.dset.pool);
        zss_obj_dep(o, layout);
        for (uint32_t b = 0; b < lci->bindingCount; b++)
            if (lci->pBindings[b].binding + 1 > o->u.dset.nslots)
                o->u.dset.nslots = lci->pBindings[b].binding + 1;
        o->u.dset.slot = calloc(o->u.dset.nslots ? o->u.dset.nslots : 1, sizeof(struct zss_slot));
        for (uint32_t b = 0; b < lci->bindingCount; b++) {
            struct zss_slot *s = &o->u.dset.slot[lci->pBindings[b].binding];

            s->type = lci->pBindings[b].descriptorType;
            s->count = lci->pBindings[b].descriptorCount;
            s->e = calloc(s->count ? s->count : 1, sizeof(*s->e));
        }
        r = create_real(dev, o);
        if (r != VK_SUCCESS)
            zss_obj_kill(o);
        else
            out[i] = ZHANDLE(VkDescriptorSet, o);
    }
    if (r != VK_SUCCESS) {
        for (uint32_t i = 0; i < info->descriptorSetCount; i++) {
            if (out[i]) {
                VkDescriptorSet real = ZREAL(VkDescriptorSet, out[i]);

                if (ZOBJ(info->descriptorPool)->u.dpool.ci.flags &
                    VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT)
                    dev->fn.FreeDescriptorSets(dev->real, ZREAL(VkDescriptorPool, info->descriptorPool), 1, &real);
                zss_obj_kill(ZOBJ(out[i]));
                out[i] = VK_NULL_HANDLE;
            }
        }
    }
    zss_leave();
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL zss_FreeDescriptorSets(VkDevice device, VkDescriptorPool pool,
                                                      uint32_t n, const VkDescriptorSet *sets)
{
    ENTER(device);
    for (uint32_t i = 0; i < n; i++) {
        VkDescriptorSet real;

        if (!sets[i])
            continue;
        real = ZREAL(VkDescriptorSet, sets[i]);
        dev->fn.FreeDescriptorSets(dev->real, ZREAL(VkDescriptorPool, pool), 1, &real);
        zss_obj_kill(ZOBJ(sets[i]));
    }
    zss_leave();
    return VK_SUCCESS;
}

static void entry_set(struct zss_slot_entry *e, struct zss_obj *sampler, struct zss_obj *view,
                      struct zss_obj *buffer)
{
    zss_obj_ref(sampler);
    zss_obj_ref(view);
    zss_obj_ref(buffer);
    zss_obj_unref(e->sampler);
    zss_obj_unref(e->view);
    zss_obj_unref(e->buffer);
    e->sampler = sampler;
    e->view = view;
    e->buffer = buffer;
    e->written = true;
}

/* Finds element `elem` of `binding`, rolling over into following bindings. */
static struct zss_slot_entry *slot_at(struct zss_obj *set, uint32_t *binding, uint32_t *elem)
{
    while (*binding < set->u.dset.nslots && *elem >= set->u.dset.slot[*binding].count) {
        *elem -= set->u.dset.slot[*binding].count;
        (*binding)++;
    }
    if (*binding >= set->u.dset.nslots)
        return NULL;
    return &set->u.dset.slot[*binding].e[*elem];
}

/* Pushes every recorded binding of a set to its real set. */
void zss_dset_apply(struct zss_dev *dev, struct zss_obj *set)
{
    VkDescriptorSet real = ZREAL(VkDescriptorSet, set);
    uint32_t total = 0, n = 0;
    VkWriteDescriptorSet *w;
    VkDescriptorImageInfo *ii;
    VkDescriptorBufferInfo *bi;

    if (!real)
        return;
    for (uint32_t b = 0; b < set->u.dset.nslots; b++)
        total += set->u.dset.slot[b].count;
    w = calloc(total + 1, sizeof(*w));
    ii = calloc(total + 1, sizeof(*ii));
    bi = calloc(total + 1, sizeof(*bi));

    for (uint32_t b = 0; b < set->u.dset.nslots; b++) {
        const struct zss_slot *s = &set->u.dset.slot[b];

        for (uint32_t i = 0; i < s->count; i++) {
            const struct zss_slot_entry *e = &s->e[i];

            if (!e->written || (e->sampler && !e->sampler->r.h) || (e->view && !e->view->r.h) ||
                (e->buffer && !e->buffer->r.h))
                continue;
            w[n] = (VkWriteDescriptorSet){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = real,
                .dstBinding = b,
                .dstArrayElement = i,
                .descriptorCount = 1,
                .descriptorType = s->type,
            };
            if (e->buffer) {
                bi[n] = (VkDescriptorBufferInfo){ (VkBuffer)(uintptr_t)e->buffer->r.h, e->offset, e->range };
                w[n].pBufferInfo = &bi[n];
            } else if (e->sampler || e->view) {
                ii[n] = (VkDescriptorImageInfo){
                    e->sampler ? (VkSampler)(uintptr_t)e->sampler->r.h : VK_NULL_HANDLE,
                    e->view ? (VkImageView)(uintptr_t)e->view->r.h : VK_NULL_HANDLE,
                    e->layout,
                };
                w[n].pImageInfo = &ii[n];
            } else {
                continue;
            }
            n++;
        }
    }
    if (n)
        dev->fn.UpdateDescriptorSets(dev->real, n, w, 0, NULL);
    free(w);
    free(ii);
    free(bi);
}

VKAPI_ATTR void VKAPI_CALL zss_UpdateDescriptorSets(VkDevice device, uint32_t nw,
                                                    const VkWriteDescriptorSet *writes, uint32_t nc,
                                                    const VkCopyDescriptorSet *copies)
{
    ENTER(device);

    for (uint32_t i = 0; i < nw; i++) {
        const VkWriteDescriptorSet *w = &writes[i];
        struct zss_obj *set = ZOBJ(w->dstSet);
        uint32_t b = w->dstBinding, el = w->dstArrayElement;

        for (uint32_t j = 0; j < w->descriptorCount; j++, el++) {
            struct zss_slot_entry *e = slot_at(set, &b, &el);

            if (!e)
                break;
            switch (w->descriptorType) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
                entry_set(e, ZOBJ(w->pImageInfo[j].sampler), NULL, NULL);
                break;
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                entry_set(e, ZOBJ(w->pImageInfo[j].sampler), ZOBJ(w->pImageInfo[j].imageView), NULL);
                e->layout = w->pImageInfo[j].imageLayout;
                break;
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                /* Shaders may write it at any time from now on. */
                if (w->pImageInfo[j].imageView)
                    zss_image_dirty(ZOBJ(ZOBJ(w->pImageInfo[j].imageView)->u.view.ci.image), true);
                /* fall through */
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                entry_set(e, NULL, ZOBJ(w->pImageInfo[j].imageView), NULL);
                e->layout = w->pImageInfo[j].imageLayout;
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                entry_set(e, NULL, NULL, ZOBJ(w->pBufferInfo[j].buffer));
                e->offset = w->pBufferInfo[j].offset;
                e->range = w->pBufferInfo[j].range;
                break;
            default:
                zss_dev_untracked(dev, "texel buffer descriptors");
                break;
            }
        }
        if (w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
            w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
            /* Not tracked: hand the write straight to the driver. */
            VkWriteDescriptorSet real = *w;
            VkBufferView *views = malloc(w->descriptorCount * sizeof(*views));

            for (uint32_t j = 0; j < w->descriptorCount; j++)
                views[j] = ZREAL(VkBufferView, w->pTexelBufferView[j]);
            real.dstSet = ZREAL(VkDescriptorSet, w->dstSet);
            real.pTexelBufferView = views;
            dev->fn.UpdateDescriptorSets(dev->real, 1, &real, 0, NULL);
            free(views);
        }
    }
    for (uint32_t i = 0; i < nc; i++) {
        const VkCopyDescriptorSet *c = &copies[i];
        struct zss_obj *src = ZOBJ(c->srcSet), *dst = ZOBJ(c->dstSet);
        uint32_t sb = c->srcBinding, se = c->srcArrayElement;
        uint32_t db = c->dstBinding, de = c->dstArrayElement;

        for (uint32_t j = 0; j < c->descriptorCount; j++, se++, de++) {
            struct zss_slot_entry *s = slot_at(src, &sb, &se), *d = slot_at(dst, &db, &de);

            if (!s || !d)
                break;
            entry_set(d, s->sampler, s->view, s->buffer);
            d->layout = s->layout;
            d->offset = s->offset;
            d->range = s->range;
            d->written = s->written;
        }
    }
    for (uint32_t i = 0; i < nw; i++)
        zss_dset_apply(dev, ZOBJ(writes[i].dstSet));
    for (uint32_t i = 0; i < nc; i++)
        zss_dset_apply(dev, ZOBJ(copies[i].dstSet));
    zss_leave();
}

/* ---- pipelines ------------------------------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL zss_CreatePipelineLayout(VkDevice device,
                                                        const VkPipelineLayoutCreateInfo *ci,
                                                        const VkAllocationCallbacks *alloc,
                                                        VkPipelineLayout *out)
{
    ENTER(device);
    struct zss_obj *o = zss_obj_new(dev, ZK_PLAYOUT);

    (void)alloc;
    o->u.playout.ci = *ci;
    o->u.playout.ci.pNext = NULL;
    o->u.playout.ci.pSetLayouts = zss_obj_dup(o, ci->pSetLayouts, ci->setLayoutCount * sizeof(VkDescriptorSetLayout));
    o->u.playout.ci.pPushConstantRanges =
        zss_obj_dup(o, ci->pPushConstantRanges, ci->pushConstantRangeCount * sizeof(VkPushConstantRange));
    for (uint32_t i = 0; i < ci->setLayoutCount; i++)
        zss_obj_dep(o, ZOBJ(ci->pSetLayouts[i]));
    return finish_create(dev, o, (uint64_t *)out);
}

VKAPI_ATTR VkResult VKAPI_CALL zss_CreatePipelineCache(VkDevice device,
                                                       const VkPipelineCacheCreateInfo *ci,
                                                       const VkAllocationCallbacks *alloc,
                                                       VkPipelineCache *out)
{
    ENTER(device);
    struct zss_obj *o = zss_obj_new(dev, ZK_PCACHE);

    /* Initial data is tied to one driver, so the cache always starts empty. */
    (void)ci;
    (void)alloc;
    return finish_create(dev, o, (uint64_t *)out);
}

VKAPI_ATTR VkResult VKAPI_CALL zss_GetPipelineCacheData(VkDevice device, VkPipelineCache cache,
                                                        size_t *size, void *data)
{
    ENTER(device);
    VkResult r;

    ZSS_RETRY(dev, r, dev->fn.GetPipelineCacheData(dev->real, ZREAL(VkPipelineCache, cache), size, data));

    zss_leave();
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL zss_MergePipelineCaches(VkDevice device, VkPipelineCache dst,
                                                       uint32_t n, const VkPipelineCache *src)
{
    ENTER(device);
    VkPipelineCache *real = malloc((n + 1) * sizeof(*real));
    VkResult r;

    for (uint32_t i = 0; i < n; i++)
        real[i] = ZREAL(VkPipelineCache, src[i]);
    r = dev->fn.MergePipelineCaches(dev->real, ZREAL(VkPipelineCache, dst), n, real);
    free(real);
    zss_leave();
    return r;
}

#define DUP1(field) ci->field = zss_obj_dup(o, ci->field, sizeof(*ci->field))

static void pipeline_copy(struct zss_obj *o, const VkGraphicsPipelineCreateInfo *src)
{
    VkGraphicsPipelineCreateInfo *ci = &o->u.pipe.ci;
    VkPipelineShaderStageCreateInfo *stages;
    bool tess = false, raster = true;

    *ci = *src;
    ci->pNext = NULL;
    stages = zss_obj_dup(o, src->pStages, src->stageCount * sizeof(*stages));
    for (uint32_t i = 0; i < src->stageCount; i++) {
        const VkSpecializationInfo *spec = src->pStages[i].pSpecializationInfo;

        stages[i].pNext = NULL;
        stages[i].pName = zss_obj_dup(o, src->pStages[i].pName, strlen(src->pStages[i].pName) + 1);
        if (spec) {
            VkSpecializationInfo *s = zss_obj_dup(o, spec, sizeof(*spec));

            s->pMapEntries = zss_obj_dup(o, spec->pMapEntries, spec->mapEntryCount * sizeof(*spec->pMapEntries));
            s->pData = zss_obj_dup(o, spec->pData, spec->dataSize);
            stages[i].pSpecializationInfo = s;
        }
        if (stages[i].stage & (VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT))
            tess = true;
        zss_obj_dep(o, ZOBJ(src->pStages[i].module));
    }
    ci->pStages = stages;

    if (src->pVertexInputState) {
        VkPipelineVertexInputStateCreateInfo *v = zss_obj_dup(o, src->pVertexInputState, sizeof(*v));

        v->pNext = NULL;
        v->pVertexBindingDescriptions = zss_obj_dup(o, v->pVertexBindingDescriptions,
            v->vertexBindingDescriptionCount * sizeof(VkVertexInputBindingDescription));
        v->pVertexAttributeDescriptions = zss_obj_dup(o, v->pVertexAttributeDescriptions,
            v->vertexAttributeDescriptionCount * sizeof(VkVertexInputAttributeDescription));
        ci->pVertexInputState = v;
    }
    DUP1(pInputAssemblyState);
    DUP1(pRasterizationState);
    if (src->pRasterizationState && src->pRasterizationState->rasterizerDiscardEnable)
        raster = false;
    ci->pTessellationState = tess ? zss_obj_dup(o, src->pTessellationState, sizeof(*src->pTessellationState)) : NULL;

    /* With rasterisation off the remaining pointers may legally be garbage. */
    if (!raster) {
        ci->pViewportState = NULL;
        ci->pMultisampleState = NULL;
        ci->pDepthStencilState = NULL;
        ci->pColorBlendState = NULL;
    } else {
        if (src->pViewportState) {
            VkPipelineViewportStateCreateInfo *v = zss_obj_dup(o, src->pViewportState, sizeof(*v));

            v->pNext = NULL;
            v->pViewports = zss_obj_dup(o, v->pViewports, v->viewportCount * sizeof(VkViewport));
            v->pScissors = zss_obj_dup(o, v->pScissors, v->scissorCount * sizeof(VkRect2D));
            ci->pViewportState = v;
        }
        if (src->pMultisampleState) {
            VkPipelineMultisampleStateCreateInfo *m = zss_obj_dup(o, src->pMultisampleState, sizeof(*m));

            m->pNext = NULL;
            m->pSampleMask = zss_obj_dup(o, m->pSampleMask,
                                         (((uint32_t)m->rasterizationSamples + 31) / 32) * sizeof(VkSampleMask));
            ci->pMultisampleState = m;
        }
        DUP1(pDepthStencilState);
        if (src->pColorBlendState) {
            VkPipelineColorBlendStateCreateInfo *c = zss_obj_dup(o, src->pColorBlendState, sizeof(*c));

            c->pNext = NULL;
            c->pAttachments = zss_obj_dup(o, c->pAttachments,
                                          c->attachmentCount * sizeof(VkPipelineColorBlendAttachmentState));
            ci->pColorBlendState = c;
        }
    }
    if (src->pDynamicState) {
        VkPipelineDynamicStateCreateInfo *d = zss_obj_dup(o, src->pDynamicState, sizeof(*d));

        d->pNext = NULL;
        d->pDynamicStates = zss_obj_dup(o, d->pDynamicStates, d->dynamicStateCount * sizeof(VkDynamicState));
        ci->pDynamicState = d;
    }
    zss_obj_dep(o, ZOBJ(src->layout));
    zss_obj_dep(o, ZOBJ(src->renderPass));
    zss_obj_dep(o, ZOBJ(src->basePipelineHandle));
}

VKAPI_ATTR VkResult VKAPI_CALL zss_CreateGraphicsPipelines(
    VkDevice device, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *infos,
    const VkAllocationCallbacks *alloc, VkPipeline *out)
{
    ENTER(device);
    VkResult result = VK_SUCCESS;

    (void)cache;
    (void)alloc;
    for (uint32_t i = 0; i < n; i++) {
        struct zss_obj *o = zss_obj_new(dev, ZK_PIPELINE);
        VkResult r;

        pipeline_copy(o, &infos[i]);
        r = create_real(dev, o);
        if (r != VK_SUCCESS) {
            zss_obj_kill(o);
            out[i] = VK_NULL_HANDLE;
            result = r;
        } else {
            out[i] = ZHANDLE(VkPipeline, o);
        }
    }
    zss_leave();
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL zss_CreateComputePipelines(
    VkDevice device, VkPipelineCache cache, uint32_t n, const VkComputePipelineCreateInfo *infos,
    const VkAllocationCallbacks *alloc, VkPipeline *out)
{
    ENTER(device);
    VkResult result = VK_SUCCESS;

    (void)alloc;
    zss_dev_untracked(dev, "compute pipelines");
    for (uint32_t i = 0; i < n; i++) {
        VkComputePipelineCreateInfo ci = infos[i];
        struct zss_obj *o = zss_obj_new(dev, ZK_OPAQUE_PIPELINE);
        VkPipeline p;
        VkResult r;

        ci.stage.module = ZREAL(VkShaderModule, ci.stage.module);
        ci.layout = ZREAL(VkPipelineLayout, ci.layout);
        ci.basePipelineHandle = ZREAL(VkPipeline, ci.basePipelineHandle);
        r = dev->fn.CreateComputePipelines(dev->real, ZREAL(VkPipelineCache, cache), 1, &ci, NULL, &p);
        if (r != VK_SUCCESS) {
            zss_obj_kill(o);
            out[i] = VK_NULL_HANDLE;
            result = r;
        } else {
            o->r.h = (uint64_t)(uintptr_t)p;
            out[i] = ZHANDLE(VkPipeline, o);
        }
    }
    zss_leave();
    return result;
}

/* ---- command pools and buffers ------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL zss_CreateCommandPool(VkDevice device,
                                                     const VkCommandPoolCreateInfo *ci,
                                                     const VkAllocationCallbacks *alloc,
                                                     VkCommandPool *out)
{
    ENTER(device);
    struct zss_obj *o = zss_obj_new(dev, ZK_CPOOL);

    (void)alloc;
    o->u.cpool.ci = *ci;
    o->u.cpool.ci.pNext = NULL;
    return finish_create(dev, o, (uint64_t *)out);
}

VKAPI_ATTR void VKAPI_CALL zss_DestroyCommandPool(VkDevice device, VkCommandPool pool,
                                                  const VkAllocationCallbacks *alloc)
{
    (void)alloc;
    if (!pool)
        return;
    ENTER(device);
    kill_children(dev, ZK_CMDBUF, ZOBJ(pool));
    destroy_obj(dev, ZOBJ(pool));
    zss_leave();
}

VKAPI_ATTR VkResult VKAPI_CALL zss_ResetCommandPool(VkDevice device, VkCommandPool pool,
                                                    VkCommandPoolResetFlags flags)
{
    ENTER(device);
    VkResult r;

    ZSS_RETRY(dev, r, dev->fn.ResetCommandPool(dev->real, ZREAL(VkCommandPool, pool), flags));

    pthread_mutex_lock(&zss_lock);
    for (struct zss_obj *o = dev->head; o; o = o->next) {
        if (o->kind == ZK_CMDBUF && !o->dead && o->u.cb.pool == ZOBJ(pool)) {
            zss_cmd_reset(o);
            o->u.cb.state = ZC_INITIAL;
        }
    }
    pthread_mutex_unlock(&zss_lock);
    zss_leave();
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL zss_AllocateCommandBuffers(VkDevice device,
                                                          const VkCommandBufferAllocateInfo *info,
                                                          VkCommandBuffer *out)
{
    ENTER(device);
    VkResult r = VK_SUCCESS;
    uint32_t i;

    if (info->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY)
        zss_dev_untracked(dev, "secondary command buffers");
    for (i = 0; i < info->commandBufferCount && r == VK_SUCCESS; i++) {
        struct zss_obj *o = zss_obj_new(dev, ZK_CMDBUF);

        o->u.cb.pool = ZOBJ(info->commandPool);
        o->u.cb.level = info->level;
        zss_obj_dep(o, o->u.cb.pool);
        r = create_real(dev, o);
        if (r != VK_SUCCESS)
            zss_obj_kill(o);
        else
            out[i] = (VkCommandBuffer)o;
    }
    if (r != VK_SUCCESS) {
        for (uint32_t j = 0; j + 1 < i; j++) {
            VkCommandBuffer real = (VkCommandBuffer)(uintptr_t)((struct zss_obj *)out[j])->r.h;

            dev->fn.FreeCommandBuffers(dev->real, ZREAL(VkCommandPool, info->commandPool), 1, &real);
            zss_obj_kill((struct zss_obj *)out[j]);
        }
        for (uint32_t j = 0; j < info->commandBufferCount; j++)
            out[j] = VK_NULL_HANDLE;
    }
    zss_leave();
    return r;
}

VKAPI_ATTR void VKAPI_CALL zss_FreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t n,
                                                  const VkCommandBuffer *bufs)
{
    ENTER(device);
    for (uint32_t i = 0; i < n; i++) {
        struct zss_obj *o = (struct zss_obj *)bufs[i];
        VkCommandBuffer real;

        if (!o)
            continue;
        real = (VkCommandBuffer)(uintptr_t)o->r.h;
        if (real)
            dev->fn.FreeCommandBuffers(dev->real, ZREAL(VkCommandPool, pool), 1, &real);
        zss_obj_kill(o);
    }
    zss_leave();
}

/* ---- fences and semaphores ----------------------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL zss_CreateFence(VkDevice device, const VkFenceCreateInfo *ci,
                                               const VkAllocationCallbacks *alloc, VkFence *out)
{
    ENTER(device);
    struct zss_obj *o = zss_obj_new(dev, ZK_FENCE);

    (void)alloc;
    o->u.fence.signaled = ci->flags & VK_FENCE_CREATE_SIGNALED_BIT;
    return finish_create(dev, o, (uint64_t *)out);
}

VKAPI_ATTR VkResult VKAPI_CALL zss_ResetFences(VkDevice device, uint32_t n, const VkFence *fences)
{
    ENTER(device);
    VkFence real[64];
    VkResult r = VK_SUCCESS;

    for (uint32_t i = 0; i < n; i += 64) {
        uint32_t c = n - i < 64 ? n - i : 64;

        do {
            for (uint32_t j = 0; j < c; j++)
                real[j] = ZREAL(VkFence, fences[i + j]);
            r = dev->fn.ResetFences(dev->real, c, real);
        } while (zss_lost(dev, r));
    }
    for (uint32_t i = 0; i < n; i++)
        ZOBJ(fences[i])->u.fence.signaled = ZOBJ(fences[i])->u.fence.pending = false;
    zss_leave();
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL zss_GetFenceStatus(VkDevice device, VkFence fence)
{
    ENTER(device);
    VkResult r;

    ZSS_RETRY(dev, r, dev->fn.GetFenceStatus(dev->real, ZREAL(VkFence, fence)));
    if (r == VK_SUCCESS) {
        ZOBJ(fence)->u.fence.signaled = true;
        ZOBJ(fence)->u.fence.pending = false;
        zss_sync_from_device(dev, NULL);
    }
    zss_leave();
    return r;
}

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/*
 * Long waits are cut into slices with the gate released in between, so a
 * migration is never stuck behind a thread sleeping in the driver.
 */
#define ZSS_SLICE_NS 20000000ull

VKAPI_ATTR VkResult VKAPI_CALL zss_WaitForFences(VkDevice device, uint32_t n, const VkFence *fences,
                                                 VkBool32 wait_all, uint64_t timeout)
{
    struct zss_dev *dev = (struct zss_dev *)device;
    uint64_t start = now_ns();
    VkFence stack[16], *real = n > 16 ? malloc(n * sizeof(*real)) : stack;
    VkResult r;

    for (;;) {
        uint64_t elapsed = now_ns() - start;
        uint64_t left = timeout == UINT64_MAX ? UINT64_MAX : (elapsed >= timeout ? 0 : timeout - elapsed);
        uint64_t slice = left < ZSS_SLICE_NS ? left : ZSS_SLICE_NS;

        zss_enter();
        for (uint32_t i = 0; i < n; i++)
            real[i] = ZREAL(VkFence, fences[i]);
        r = dev->fn.WaitForFences(dev->real, n, real, wait_all, slice);
        if (zss_lost(dev, r)) {
            /* The fences were rebuilt, and those tied to lost work are now signalled. */
            zss_leave();
            continue;
        }
        if (r == VK_SUCCESS) {
            if (wait_all || n == 1)
                for (uint32_t i = 0; i < n; i++) {
                    ZOBJ(fences[i])->u.fence.signaled = true;
                    ZOBJ(fences[i])->u.fence.pending = false;
                }
            zss_sync_from_device(dev, NULL);
        }
        zss_leave();
        if (r != VK_TIMEOUT || left <= slice)
            break;
    }
    if (real != stack)
        free(real);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL zss_CreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo *ci,
                                                   const VkAllocationCallbacks *alloc,
                                                   VkSemaphore *out)
{
    ENTER(device);
    struct zss_obj *o = zss_obj_new(dev, ZK_SEMAPHORE);

    (void)ci;
    (void)alloc;
    return finish_create(dev, o, (uint64_t *)out);
}

/* ---- queues ------------------------------------------------------------------ */

uint32_t zss_submits;

static void *stuck_thread(void *arg)
{
    usleep((useconds_t)(intptr_t)arg * 1000);
    zss_leave();
    return NULL;
}

/*
 * Test aid. ZSS_TEST_LOSE_AT_SUBMIT=N makes the Nth submit behave as if the
 * driver had reported the device lost, without issuing it. ZSS_TEST_STUCK_MS
 * additionally keeps one thread inside the layer for that long, as a thread
 * blocked in a dead driver would be.
 */
static bool inject_loss(void)
{
    static int at = -2;
    static bool done;
    const char *stuck = getenv("ZSS_TEST_STUCK_MS");

    if (at == -2) {
        const char *e = getenv("ZSS_TEST_LOSE_AT_SUBMIT");

        at = e ? atoi(e) : -1;
    }
    if (done || at < 0 || (int)zss_submits + 1 != at)
        return false;
    done = true;
    if (stuck) {
        pthread_t tid;

        /* Entered here, on behalf of the thread that will leave later. */
        zss_enter();
        if (pthread_create(&tid, NULL, stuck_thread, (void *)(intptr_t)atoi(stuck)) == 0)
            pthread_detach(tid);
        else
            zss_leave();
    }
    return true;
}

VKAPI_ATTR VkResult VKAPI_CALL zss_QueueSubmit(VkQueue queue, uint32_t n, const VkSubmitInfo *submits,
                                               VkFence fence)
{
    struct zss_queue *q = (struct zss_queue *)queue;
    struct zss_dev *dev = q->dev;
    VkResult r;

    zss_enter();
    /* Repeated from the top after a loss: by then every real handle is new. */
    do {
        VkSubmitInfo *real = calloc(n + 1, sizeof(*real));
        void **scratch = calloc(3 * (size_t)n + 1, sizeof(*scratch));

        zss_sync_to_device(dev, NULL);
        for (uint32_t i = 0; i < n; i++) {
            const VkSubmitInfo *s = &submits[i];
            VkSemaphore *waits = malloc((s->waitSemaphoreCount + 1) * sizeof(*waits));
            VkCommandBuffer *cbs = malloc((s->commandBufferCount + 1) * sizeof(*cbs));
            VkSemaphore *signals = malloc((s->signalSemaphoreCount + 1) * sizeof(*signals));

            for (uint32_t j = 0; j < s->waitSemaphoreCount; j++)
                waits[j] = ZREAL(VkSemaphore, s->pWaitSemaphores[j]);
            for (uint32_t j = 0; j < s->commandBufferCount; j++)
                cbs[j] = (VkCommandBuffer)(uintptr_t)((struct zss_obj *)s->pCommandBuffers[j])->r.h;
            for (uint32_t j = 0; j < s->signalSemaphoreCount; j++)
                signals[j] = ZREAL(VkSemaphore, s->pSignalSemaphores[j]);
            real[i] = *s;
            real[i].pNext = NULL;
            real[i].pWaitSemaphores = waits;
            real[i].pCommandBuffers = cbs;
            real[i].pSignalSemaphores = signals;
            scratch[3 * i] = waits;
            scratch[3 * i + 1] = cbs;
            scratch[3 * i + 2] = signals;
        }
        r = inject_loss() ? VK_ERROR_DEVICE_LOST
                          : dev->fn.QueueSubmit(q->real, n, real, ZREAL(VkFence, fence));
        for (uint32_t i = 0; i < 3 * n; i++)
            free(scratch[i]);
        free(scratch);
        free(real);
    } while (zss_lost(dev, r));

    /* Bookkeeping only for work the device really accepted. */
    if (r == VK_SUCCESS) {
        zss_submits++;
        for (uint32_t i = 0; i < n; i++) {
            const VkSubmitInfo *s = &submits[i];

            for (uint32_t j = 0; j < s->waitSemaphoreCount; j++)
                ZOBJ(s->pWaitSemaphores[j])->u.sem.signaled = false;
            for (uint32_t j = 0; j < s->commandBufferCount; j++)
                zss_cmd_track_submit(dev, (struct zss_obj *)s->pCommandBuffers[j]);
            for (uint32_t j = 0; j < s->signalSemaphoreCount; j++)
                ZOBJ(s->pSignalSemaphores[j])->u.sem.signaled = true;
        }
        if (fence)
            ZOBJ(fence)->u.fence.pending = true;
    }
    zss_leave();
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL zss_QueueWaitIdle(VkQueue queue)
{
    struct zss_queue *q = (struct zss_queue *)queue;
    VkResult r;

    zss_enter();
    ZSS_RETRY(q->dev, r, q->dev->fn.QueueWaitIdle(q->real));
    zss_sync_from_device(q->dev, NULL);
    zss_leave();
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL zss_DeviceWaitIdle(VkDevice device)
{
    ENTER(device);
    VkResult r;

    ZSS_RETRY(dev, r, dev->fn.DeviceWaitIdle(dev->real));
    zss_sync_from_device(dev, NULL);
    zss_leave();
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL zss_QueueBindSparse(VkQueue queue, uint32_t n,
                                                   const VkBindSparseInfo *info, VkFence fence)
{
    (void)queue; (void)n; (void)info; (void)fence;
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

/* ---- kinds that are forwarded without tracking --------------------------------- */

#define OPAQUE_CREATE(Name, T, Info, kind, feature) \
    VKAPI_ATTR VkResult VKAPI_CALL zss_Create##Name(VkDevice device, const Info *ci, \
                                                    const VkAllocationCallbacks *alloc, T *out) \
    { \
        ENTER(device); \
        struct zss_obj *o = zss_obj_new(dev, kind); \
        Info copy = *ci; \
        T h; \
        VkResult r; \
        (void)alloc; \
        zss_dev_untracked(dev, feature); \
        OPAQUE_FIX(copy); \
        r = dev->fn.Create##Name(dev->real, &copy, NULL, &h); \
        if (r != VK_SUCCESS) { \
            zss_obj_kill(o); \
            *out = VK_NULL_HANDLE; \
        } else { \
            o->r.h = (uint64_t)(uintptr_t)h; \
            *out = ZHANDLE(T, o); \
        } \
        zss_leave(); \
        return r; \
    }

#define OPAQUE_FIX(c) (void)(c)
OPAQUE_CREATE(Event, VkEvent, VkEventCreateInfo, ZK_OPAQUE_EVENT, "events")
OPAQUE_CREATE(QueryPool, VkQueryPool, VkQueryPoolCreateInfo, ZK_OPAQUE_QUERYPOOL, "query pools")
#undef OPAQUE_FIX
#define OPAQUE_FIX(c) (c).buffer = ZREAL(VkBuffer, (c).buffer)
OPAQUE_CREATE(BufferView, VkBufferView, VkBufferViewCreateInfo, ZK_OPAQUE_BUFFERVIEW, "buffer views")
#undef OPAQUE_FIX

VKAPI_ATTR VkResult VKAPI_CALL zss_GetEventStatus(VkDevice device, VkEvent event)
{
    ENTER(device);
    VkResult r = dev->fn.GetEventStatus(dev->real, ZREAL(VkEvent, event));

    zss_leave();
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL zss_SetEvent(VkDevice device, VkEvent event)
{
    ENTER(device);
    VkResult r = dev->fn.SetEvent(dev->real, ZREAL(VkEvent, event));

    zss_leave();
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL zss_ResetEvent(VkDevice device, VkEvent event)
{
    ENTER(device);
    VkResult r = dev->fn.ResetEvent(dev->real, ZREAL(VkEvent, event));

    zss_leave();
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL zss_GetQueryPoolResults(VkDevice device, VkQueryPool pool,
                                                       uint32_t first, uint32_t count, size_t size,
                                                       void *data, VkDeviceSize stride,
                                                       VkQueryResultFlags flags)
{
    ENTER(device);
    VkResult r = dev->fn.GetQueryPoolResults(dev->real, ZREAL(VkQueryPool, pool), first, count, size,
                                             data, stride, flags);

    zss_leave();
    return r;
}
