// SPDX-License-Identifier: GPL-2.0-only
/*
 * Moving a device's objects to another real device.
 *
 *   capture   read buffer and image contents off the source while it is alive
 *   build     create a real device on the target and recreate every object
 *   commit    destroy the source's objects and device
 *
 * Parking is capture + commit with no build; resuming is the build alone.
 * The source stays intact until the build has succeeded, so a failed
 * migration leaves the application exactly where it was.
 *
 * Callers hold the gate (zss_hold_begin), so no application thread is inside
 * the layer.
 */
#include "zss_layer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

bool zss_families_fit(const struct zss_dev *dev, const struct zss_gpu *gpu);

static const char *const feature_names[] = {
    "robustBufferAccess", "fullDrawIndexUint32", "imageCubeArray", "independentBlend",
    "geometryShader", "tessellationShader", "sampleRateShading", "dualSrcBlend", "logicOp",
    "multiDrawIndirect", "drawIndirectFirstInstance", "depthClamp", "depthBiasClamp",
    "fillModeNonSolid", "depthBounds", "wideLines", "largePoints", "alphaToOne", "multiViewport",
    "samplerAnisotropy", "textureCompressionETC2", "textureCompressionASTC_LDR",
    "textureCompressionBC", "occlusionQueryPrecise", "pipelineStatisticsQuery",
    "vertexPipelineStoresAndAtomics", "fragmentStoresAndAtomics",
    "shaderTessellationAndGeometryPointSize", "shaderImageGatherExtended",
    "shaderStorageImageExtendedFormats", "shaderStorageImageMultisample",
    "shaderStorageImageReadWithoutFormat", "shaderStorageImageWriteWithoutFormat",
    "shaderUniformBufferArrayDynamicIndexing", "shaderSampledImageArrayDynamicIndexing",
    "shaderStorageBufferArrayDynamicIndexing", "shaderStorageImageArrayDynamicIndexing",
    "shaderClipDistance", "shaderCullDistance", "shaderFloat64", "shaderInt64", "shaderInt16",
    "shaderResourceResidency", "shaderResourceMinLod", "sparseBinding", "sparseResidencyBuffer",
    "sparseResidencyImage2D", "sparseResidencyImage3D", "sparseResidency2Samples",
    "sparseResidency4Samples", "sparseResidency8Samples", "sparseResidency16Samples",
    "sparseResidencyAliased", "variableMultisampleRate", "inheritedQueries",
};

/* Everything in zss_dev that belongs to one real device. */
struct ctx {
    struct zss_gpu *gpu;
    VkDevice real;
    struct zss_dev_fns fn;
    uint32_t fam_map[ZSS_MAX_FAMILIES];
    VkQueue util_queue;
    VkCommandPool util_pool;
    VkCommandBuffer util_cmd;
    VkQueue *queues;
    bool native_dynrender, native_maint5;
    void *lower; /* stand-ins made on this device (lower.c) */
};

static void ctx_save(struct zss_dev *dev, struct ctx *c)
{
    c->gpu = dev->gpu;
    c->real = dev->real;
    c->fn = dev->fn;
    memcpy(c->fam_map, dev->fam_map, sizeof(c->fam_map));
    c->util_queue = dev->util_queue;
    c->util_pool = dev->util_pool;
    c->util_cmd = dev->util_cmd;
    c->native_dynrender = dev->native_dynrender;
    c->native_maint5 = dev->native_maint5;
    c->lower = dev->lower;
    c->queues = calloc(dev->nqueues ? dev->nqueues : 1, sizeof(*c->queues));
    for (uint32_t i = 0; i < dev->nqueues; i++)
        c->queues[i] = dev->queues[i].real;
}

static void ctx_load(struct zss_dev *dev, struct ctx *c)
{
    dev->gpu = c->gpu;
    dev->real = c->real;
    dev->fn = c->fn;
    memcpy(dev->fam_map, c->fam_map, sizeof(c->fam_map));
    dev->util_queue = c->util_queue;
    dev->util_pool = c->util_pool;
    dev->util_cmd = c->util_cmd;
    dev->native_dynrender = c->native_dynrender;
    dev->native_maint5 = c->native_maint5;
    dev->lower = c->lower;
    for (uint32_t i = 0; i < dev->nqueues; i++)
        dev->queues[i].real = c->queues[i];
    free(c->queues);
    c->queues = NULL;
}

static void ctx_clear(struct zss_dev *dev)
{
    dev->gpu = NULL;
    dev->real = VK_NULL_HANDLE;
    dev->util_queue = VK_NULL_HANDLE;
    dev->util_pool = VK_NULL_HANDLE;
    dev->util_cmd = VK_NULL_HANDLE;
    dev->lower = NULL;
}

/* ---- staging and image copies --------------------------------------------------- */

struct stage {
    VkBuffer buf;
    VkDeviceMemory mem;
    void *map;
};

static VkResult stage_new(struct zss_dev *dev, VkDeviceSize size, struct stage *s)
{
    VkBufferCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    VkMemoryRequirements req;
    VkResult r;

    memset(s, 0, sizeof(*s));
    r = dev->fn.CreateBuffer(dev->real, &ci, NULL, &s->buf);
    if (r != VK_SUCCESS)
        return r;
    dev->fn.GetBufferMemoryRequirements(dev->real, s->buf, &req);
    r = zss_backing_alloc(dev, &req, true, &s->mem, &s->map);
    if (r == VK_SUCCESS)
        r = dev->fn.BindBufferMemory(dev->real, s->buf, s->mem, 0);
    return r;
}

static void stage_free(struct zss_dev *dev, struct stage *s)
{
    if (s->buf)
        dev->fn.DestroyBuffer(dev->real, s->buf, NULL);
    if (s->mem)
        dev->fn.FreeMemory(dev->real, s->mem, NULL);
}

static uint32_t mip_dim(uint32_t v, uint32_t mip)
{
    v >>= mip;
    return v ? v : 1;
}

VkDeviceSize zss_sub_size(const VkImageCreateInfo *ci, VkImageAspectFlags aspect, uint32_t mip)
{
    struct zss_format_info fi;
    uint32_t w = mip_dim(ci->extent.width, mip), h = mip_dim(ci->extent.height, mip);

    if (!zss_format_info(ci->format, aspect, &fi))
        return 0;
    return (VkDeviceSize)((w + fi.block_w - 1) / fi.block_w) * ((h + fi.block_h - 1) / fi.block_h) *
           mip_dim(ci->extent.depth, mip) * fi.block_bytes;
}

static VkDeviceSize blob_size(const struct zss_obj *o)
{
    const VkImageCreateInfo *ci = &o->u.img.ci;
    VkImageAspectFlags aspects = zss_format_aspects(ci->format);
    VkDeviceSize total = 0;

    for (uint32_t m = 0; m < ci->mipLevels; m++)
        for (VkImageAspectFlags bit = 1; bit <= VK_IMAGE_ASPECT_STENCIL_BIT; bit <<= 1)
            if (aspects & bit)
                total += zss_sub_size(ci, bit, m) * ci->arrayLayers;
    return total;
}

static void barrier(struct zss_dev *dev, const struct zss_obj *o, uint32_t mip, uint32_t layer,
                    VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = (VkImage)(uintptr_t)o->r.h,
        .subresourceRange = { zss_format_aspects(o->u.img.ci.format), mip, 1, layer, 1 },
    };

    dev->fn.CmdPipelineBarrier(dev->util_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static bool settable(VkImageLayout l)
{
    return l != VK_IMAGE_LAYOUT_UNDEFINED && l != VK_IMAGE_LAYOUT_PREINITIALIZED;
}

/*
 * Copies every subresource that has contents between the image and a staging
 * buffer. The walk order fixes the blob layout, so capture and restore agree.
 */
static void copy_subresources(struct zss_dev *dev, struct zss_obj *o, VkBuffer staging, bool to_image)
{
    const VkImageCreateInfo *ci = &o->u.img.ci;
    VkImageAspectFlags aspects = zss_format_aspects(ci->format);
    VkImage img = (VkImage)(uintptr_t)o->r.h;
    VkDeviceSize off = 0;

    for (uint32_t m = 0; m < ci->mipLevels; m++) {
        for (uint32_t l = 0; l < ci->arrayLayers; l++) {
            VkImageLayout layout = o->u.img.layout[m * ci->arrayLayers + l];
            VkImageLayout xfer = to_image ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                          : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            bool live = settable(layout);

            if (live)
                barrier(dev, o, m, l, to_image ? VK_IMAGE_LAYOUT_UNDEFINED : layout, xfer);
            for (VkImageAspectFlags bit = 1; bit <= VK_IMAGE_ASPECT_STENCIL_BIT; bit <<= 1) {
                VkBufferImageCopy region = {
                    .bufferOffset = off,
                    .imageSubresource = { bit, m, l, 1 },
                    .imageExtent = { mip_dim(ci->extent.width, m), mip_dim(ci->extent.height, m),
                                     mip_dim(ci->extent.depth, m) },
                };

                if (!(aspects & bit))
                    continue;
                off += zss_sub_size(ci, bit, m);
                if (!live)
                    continue;
                if (to_image)
                    dev->fn.CmdCopyBufferToImage(dev->util_cmd, staging, img, xfer, 1, &region);
                else
                    dev->fn.CmdCopyImageToBuffer(dev->util_cmd, img, xfer, staging, 1, &region);
            }
            if (live)
                barrier(dev, o, m, l, xfer, layout);
        }
    }
}

static bool has_contents(const struct zss_obj *o)
{
    uint32_t n = o->u.img.ci.mipLevels * o->u.img.ci.arrayLayers;

    for (uint32_t i = 0; i < n; i++)
        if (settable(o->u.img.layout[i]))
            return true;
    return false;
}

/* ---- capture ---------------------------------------------------------------- */

static VkResult capture_buffer(struct zss_dev *dev, struct zss_obj *o)
{
    VkDeviceSize size = o->u.buf.ci.size;
    VkBufferCopy region = { 0, 0, size };
    struct stage s;
    VkResult r;

    o->u.buf.saved = malloc(size ? size : 1);
    if (!o->u.buf.saved)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (o->r.map) {
        memcpy(o->u.buf.saved, o->r.map, size);
        return VK_SUCCESS;
    }
    r = stage_new(dev, size, &s);
    if (r == VK_SUCCESS)
        r = zss_util_begin(dev);
    if (r == VK_SUCCESS) {
        dev->fn.CmdCopyBuffer(dev->util_cmd, (VkBuffer)(uintptr_t)o->r.h, s.buf, 1, &region);
        r = zss_util_run(dev);
    }
    if (r == VK_SUCCESS)
        memcpy(o->u.buf.saved, s.map, size);
    stage_free(dev, &s);
    return r;
}

static VkResult capture_image(struct zss_dev *dev, struct zss_obj *o)
{
    VkDeviceSize size = blob_size(o);
    struct stage s;
    VkResult r;

    /* Multisampled images cannot be copied out; they are redrawn every frame. */
    if (!size || !has_contents(o) || o->u.img.ci.samples != VK_SAMPLE_COUNT_1_BIT)
        return VK_SUCCESS;
    o->u.img.saved = malloc(size);
    if (!o->u.img.saved)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    o->u.img.saved_size = size;
    r = stage_new(dev, size, &s);
    if (r == VK_SUCCESS)
        r = zss_util_begin(dev);
    if (r == VK_SUCCESS) {
        copy_subresources(dev, o, s.buf, false);
        r = zss_util_run(dev);
    }
    if (r == VK_SUCCESS)
        memcpy(o->u.img.saved, s.map, size);
    stage_free(dev, &s);
    return r;
}

static VkResult capture(struct zss_dev *dev)
{
    VkResult r = VK_SUCCESS;

    for (struct zss_obj *o = dev->head; o && r == VK_SUCCESS; o = o->next) {
        if (o->dead || !o->r.h)
            continue;
        if (o->kind == ZK_BUFFER && o->r.backing)
            r = capture_buffer(dev, o);
        else if (o->kind == ZK_IMAGE && o->r.backing && !o->u.img.swapchain)
            r = capture_image(dev, o);
        else if (o->kind == ZK_IMAGE && o->u.img.swapchain && !o->u.img.pending &&
                 (o->r.standin || ((o->u.img.ci.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
                                   (o->u.img.ci.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT))))
            /*
             * A window's images are not required to keep what was presented
             * from them, but drivers do, and applications that redraw only
             * what changed (browsers) count on it. So they are carried over
             * like any other image, where the swapchain allows copying. One
             * still waiting for contents from an earlier move keeps those.
             */
            r = capture_image(dev, o);
        else if (o->kind == ZK_FENCE)
            o->u.fence.signaled = dev->fn.GetFenceStatus(dev->real, (VkFence)(uintptr_t)o->r.h) == VK_SUCCESS;
        else if (o->kind == ZK_SEMAPHORE && o->u.sem.timeline && dev->fn.GetSemaphoreCounterValueKHR) {
            /* Where the counter really is, so that the new semaphore starts there. */
            uint64_t v = 0;

            if (dev->fn.GetSemaphoreCounterValueKHR(dev->real, (VkSemaphore)(uintptr_t)o->r.h, &v) == VK_SUCCESS)
                o->u.sem.value = v;
        }
    }
    return r;
}

static void free_saved(struct zss_dev *dev)
{
    for (struct zss_obj *o = dev->head; o; o = o->next) {
        if (o->kind == ZK_BUFFER) {
            free(o->u.buf.saved);
            o->u.buf.saved = NULL;
        } else if (o->kind == ZK_IMAGE) {
            free(o->u.img.saved);
            o->u.img.saved = NULL;
        }
    }
}

/* ---- build ------------------------------------------------------------------ */

static VkResult restore_buffer(struct zss_dev *dev, struct zss_obj *o)
{
    VkDeviceSize size = o->u.buf.ci.size;
    VkBufferCopy region = { 0, 0, size };
    struct stage s;
    VkResult r;

    if (!o->u.buf.saved || !o->r.backing)
        return VK_SUCCESS;
    if (o->r.map) {
        memcpy(o->r.map, o->u.buf.saved, size);
        return VK_SUCCESS;
    }
    r = stage_new(dev, size, &s);
    if (r == VK_SUCCESS) {
        memcpy(s.map, o->u.buf.saved, size);
        r = zss_util_begin(dev);
    }
    if (r == VK_SUCCESS) {
        dev->fn.CmdCopyBuffer(dev->util_cmd, s.buf, (VkBuffer)(uintptr_t)o->r.h, 1, &region);
        r = zss_util_run(dev);
    }
    stage_free(dev, &s);
    return r;
}

static VkResult restore_image(struct zss_dev *dev, struct zss_obj *o)
{
    const VkImageCreateInfo *ci = &o->u.img.ci;
    struct stage s = { 0 };
    VkResult r;

    if (!o->r.backing || !has_contents(o))
        return VK_SUCCESS;
    if (o->u.img.saved) {
        r = stage_new(dev, o->u.img.saved_size, &s);
        if (r != VK_SUCCESS) {
            stage_free(dev, &s);
            return r;
        }
        memcpy(s.map, o->u.img.saved, o->u.img.saved_size);
    }
    r = zss_util_begin(dev);
    if (r == VK_SUCCESS) {
        if (o->u.img.saved) {
            copy_subresources(dev, o, s.buf, true);
        } else {
            /* No contents to bring over; only put the image in the layout the application expects. */
            for (uint32_t m = 0; m < ci->mipLevels; m++) {
                for (uint32_t l = 0; l < ci->arrayLayers; l++) {
                    VkImageLayout layout = o->u.img.layout[m * ci->arrayLayers + l];

                    if (settable(layout) && !(o->r.standin && layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR))
                        barrier(dev, o, m, l, VK_IMAGE_LAYOUT_UNDEFINED, layout);
                }
            }
        }
        r = zss_util_run(dev);
    }
    stage_free(dev, &s);
    return r;
}

static bool temp_kind(enum zss_kind k)
{
    return k == ZK_SHADER || k == ZK_RENDERPASS || k == ZK_DSL || k == ZK_PLAYOUT || k == ZK_SAMPLER ||
           k == ZK_YCBCR;
}

static bool wanted(const struct zss_obj *o)
{
    if (o->kind >= ZK_OPAQUE_EVENT)
        return false;
    /* Destroyed by the application but still needed to rebuild what refers to it. */
    if (o->dead && !temp_kind(o->kind))
        return false;
    for (uint32_t i = 0; i < o->ndeps; i++)
        if (!o->deps[i]->r.h && o->deps[i]->kind != ZK_MEMORY && o->deps[i]->kind != ZK_SWAPCHAIN)
            return false;
    return true;
}

/*
 * Brings a new image to where the application believes its old one is: the
 * saved contents if there are any, and the layout it last gave it.
 */
VkResult zss_image_bring_up(struct zss_dev *dev, struct zss_obj *img, const uint8_t *bytes, VkDeviceSize size)
{
    const VkImageCreateInfo *ci = &img->u.img.ci;
    struct stage s = { 0 };
    VkResult r = VK_SUCCESS;

    if (bytes && size == blob_size(img)) {
        r = stage_new(dev, size, &s);
        if (r == VK_SUCCESS)
            memcpy(s.map, bytes, size);
        else
            bytes = NULL;
    } else {
        bytes = NULL;
    }
    r = zss_util_begin(dev);
    if (r == VK_SUCCESS) {
        if (bytes) {
            copy_subresources(dev, img, s.buf, true);
        } else {
            for (uint32_t m = 0; m < ci->mipLevels; m++)
                for (uint32_t l = 0; l < ci->arrayLayers; l++)
                    if (settable(img->u.img.layout[m * ci->arrayLayers + l]))
                        barrier(dev, img, m, l, VK_IMAGE_LAYOUT_UNDEFINED, img->u.img.layout[m * ci->arrayLayers + l]);
        }
        r = zss_util_run(dev);
    }
    stage_free(dev, &s);
    return r;
}

static VkResult signal_semaphore(struct zss_dev *dev, struct zss_obj *o)
{
    VkSemaphore sem = (VkSemaphore)(uintptr_t)o->r.h;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &sem,
    };
    VkResult r = dev->fn.QueueSubmit(dev->util_queue, 1, &si, VK_NULL_HANDLE);

    if (r == VK_SUCCESS)
        r = dev->fn.QueueWaitIdle(dev->util_queue);
    return r;
}

static VkResult build(struct zss_dev *dev, struct zss_gpu *target)
{
    VkResult r;

    for (struct zss_obj *o = dev->head; o; o = o->next)
        if (o->kind == ZK_SWAPCHAIN)
            o->u.sc.undo_valid = false;
    r = zss_dev_create_real(dev, target);
    if (r != VK_SUCCESS)
        return r;

    for (struct zss_obj *o = dev->head; o; o = o->next) {
        if (!wanted(o))
            continue;
        if (o->kind == ZK_SWAPCHAIN) {
            /* Rebuilt in place where the target allows it; otherwise the application is told to make a new one. */
            /* One the application has replaced shares its window with the newer one, which is the one to rebuild. */
            if (o->u.sc.superseded || !zss_swapchain_rebuild(dev, o))
                zss_swapchain_retire(dev, o);
            continue;
        }
        /* An image of a swapchain that was just rebuilt is the driver's, already in place. */
        if (o->kind == ZK_IMAGE && o->u.img.swapchain && o->r.h)
            continue;
        r = zss_real_create(dev, o);
        if (r == VK_SUCCESS && o->kind == ZK_BUFFER)
            r = restore_buffer(dev, o);
        if (r == VK_SUCCESS && o->kind == ZK_IMAGE)
            r = restore_image(dev, o);
        /* A timeline semaphore was made at its value; only a binary one has to be signalled again. */
        if (r == VK_SUCCESS && o->kind == ZK_SEMAPHORE && o->u.sem.signaled && !o->u.sem.timeline)
            r = signal_semaphore(dev, o);
        if (r != VK_SUCCESS) {
            zss_log("recreating object kind %d on %s failed (VkResult %d)", o->kind,
                    target->props.deviceName, r);
            return r;
        }
    }
    /* Sets and command buffers refer to objects created after them. */
    for (struct zss_obj *o = dev->head; o; o = o->next)
        if (o->kind == ZK_DSET && !o->dead && o->r.h)
            zss_dset_apply(dev, o);
    /* Secondary buffers first: a primary can only record running one that is already recorded. */
    for (struct zss_obj *o = dev->head; o; o = o->next)
        if (o->kind == ZK_CMDBUF && !o->dead && o->r.h && o->u.cb.level == VK_COMMAND_BUFFER_LEVEL_SECONDARY)
            zss_cmd_replay(dev, o);
    for (struct zss_obj *o = dev->head; o; o = o->next)
        if (o->kind == ZK_CMDBUF && !o->dead && o->r.h && o->u.cb.level != VK_COMMAND_BUFFER_LEVEL_SECONDARY)
            zss_cmd_replay(dev, o);
    for (struct zss_obj *o = dev->head; o; o = o->next)
        if (o->dead && o->r.h)
            zss_real_destroy(dev, o->kind, &o->r);
    return VK_SUCCESS;
}

/* Destroys whatever real objects currently hang off the device, then the device. */
static void teardown(struct zss_dev *dev)
{
    if (!dev->real)
        return;
    dev->fn.DeviceWaitIdle(dev->real);
    for (struct zss_obj *o = dev->tail; o; o = o->prev)
        zss_real_destroy(dev, o->kind, &o->r);
    zss_dev_destroy_real(dev);
}

/* ---- compatibility ------------------------------------------------------------ */

const char *zss_feature_name(size_t k)
{
    return k < sizeof(feature_names) / sizeof(feature_names[0]) ? feature_names[k] : "?";
}

bool zss_compatible(struct zss_dev *dev, struct zss_gpu *target, char *reason, size_t rlen)
{
    const VkBool32 *want = (const VkBool32 *)&dev->features;
    const VkBool32 *have = (const VkBool32 *)&target->features;
    VkPhysicalDevice pd = zss_gpu_real(target);
    bool swapchain = false;

    if (!pd) {
        snprintf(reason, rlen, "%s is not available", target->props.deviceName);
        return false;
    }
    for (size_t i = 0; i < sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32); i++) {
        if (want[i] && !have[i]) {
            snprintf(reason, rlen, "%s lacks the %s feature", target->props.deviceName, feature_names[i]);
            return false;
        }
    }
    for (uint32_t i = 0; i < target->next; i++)
        if (!strcmp(target->ext[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
            swapchain = true;
    if (!zss_feats_supported(target, dev->feat_chain, reason, rlen))
        return false;
    for (uint32_t k = 0; k < dev->nexts; k++) {
        bool has = zss_ext_emulated(dev->exts[k]); /* the layer stands in where the driver lacks it */

        for (uint32_t i = 0; i < target->next; i++)
            has = has || !strcmp(target->ext[i].extensionName, dev->exts[k]);
        if (!has) {
            snprintf(reason, rlen, "%s lacks the %s extension", target->props.deviceName, dev->exts[k]);
            return false;
        }
    }
    if (dev->want_swapchain && !swapchain) {
        snprintf(reason, rlen, "%s cannot present", target->props.deviceName);
        return false;
    }
    if (!zss_families_fit(dev, target)) {
        snprintf(reason, rlen, "%s lacks a matching queue family", target->props.deviceName);
        return false;
    }
    for (struct zss_obj *o = dev->head; o; o = o->next) {
        const VkImageCreateInfo *ci = &o->u.img.ci;
        VkImageFormatProperties p;

        if (o->kind != ZK_IMAGE || o->dead)
            continue;
        if (target->drv->fn.GetPhysicalDeviceImageFormatProperties(
                pd, ci->format, ci->imageType, ci->tiling,
                ci->usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                ci->flags, &p) != VK_SUCCESS ||
            p.maxExtent.width < ci->extent.width || p.maxExtent.height < ci->extent.height ||
            p.maxExtent.depth < ci->extent.depth || p.maxMipLevels < ci->mipLevels ||
            p.maxArrayLayers < ci->arrayLayers || !(p.sampleCounts & ci->samples)) {
            snprintf(reason, rlen, "%s cannot hold an image the application uses (format %d, %ux%u)",
                     target->props.deviceName, (int)ci->format, ci->extent.width, ci->extent.height);
            return false;
        }
    }
    return true;
}

/* ---- contents after a loss ------------------------------------------------------- */

/*
 * With the device gone, nothing can be read back. Fills each object's saved
 * contents from what is held outside the device instead: the shadow of
 * mapped memory, then retained uploads. Contents that only the GPU had are
 * replaced by zeros and counted. Afterwards the normal build restores from
 * the saved contents as it does after a capture.
 */
static int prepare_from_memory(struct zss_dev *dev)
{
    int lost = 0;

    for (struct zss_obj *o = dev->head; o; o = o->next) {
        if (o->dead)
            continue;
        if (o->kind == ZK_BUFFER && o->r.backing) {
            const struct zss_obj *m = o->u.buf.mem;
            VkDeviceSize size = o->u.buf.ci.size;

            free(o->u.buf.saved);
            o->u.buf.saved = NULL;
            if (m && m->u.mem.shadow && o->u.buf.mem_off + size <= m->u.mem.size) {
                o->u.buf.saved = malloc(size ? size : 1);
                memcpy(o->u.buf.saved, m->u.mem.shadow + o->u.buf.mem_off, size);
            } else if (o->u.buf.ret.valid && o->u.buf.ret.size == size) {
                o->u.buf.saved = calloc(1, size ? size : 1);
                if (!zss_retain_get(&o->u.buf.ret, o->u.buf.saved))
                    lost++;
            } else if (o->u.buf.filled) {
                o->u.buf.saved = calloc(1, size ? size : 1);
                lost++;
            }
        } else if (o->kind == ZK_FENCE) {
            /* Whatever it was waiting for will never finish; do not let the application wait for it. */
            o->u.fence.signaled = o->u.fence.signaled || o->u.fence.pending;
            o->u.fence.pending = false;
        } else if (o->kind == ZK_IMAGE && o->r.backing && !o->u.img.swapchain) {
            const VkImageCreateInfo *ci = &o->u.img.ci;
            VkImageAspectFlags aspects = zss_format_aspects(ci->format);
            VkDeviceSize size = blob_size(o), off = 0;
            bool missing = false;

            free(o->u.img.saved);
            o->u.img.saved = NULL;
            if (!size || !has_contents(o) || ci->samples != VK_SAMPLE_COUNT_1_BIT)
                continue;
            o->u.img.saved = calloc(1, size);
            o->u.img.saved_size = size;
            /* Same walk as copy_subresources, so offsets agree. */
            for (uint32_t m = 0; m < ci->mipLevels; m++) {
                for (uint32_t l = 0; l < ci->arrayLayers; l++) {
                    for (VkImageAspectFlags bit = 1; bit <= VK_IMAGE_ASPECT_STENCIL_BIT; bit <<= 1) {
                        struct zss_ret *ret;
                        VkDeviceSize sub;

                        if (!(aspects & bit))
                            continue;
                        sub = zss_sub_size(ci, bit, m);
                        ret = &o->u.img.ret[(m * ci->arrayLayers + l) * 2 + (bit == VK_IMAGE_ASPECT_STENCIL_BIT)];
                        if (ret->valid && ret->size == sub && !zss_retain_get(ret, o->u.img.saved + off))
                            missing = true;
                        off += sub;
                    }
                }
            }
            /* An image that is cleared and redrawn every frame has nothing worth counting. */
            if (missing || o->u.img.carried || o->u.img.unretained)
                lost++;
        }
    }
    return lost;
}

/* ---- waiting for a recovery ------------------------------------------------------ */

static pthread_mutex_t rec_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rec_cv = PTHREAD_COND_INITIALIZER;

/* The device has a new real device behind it: wake everyone waiting on the old one. */
/* Before anyone is let back in: what the lost device never finished is issued again. */
static void reissue(struct zss_dev *dev)
{
    uint32_t n = zss_inflight_reissue(dev);

    if (n)
        zss_log("%u submission(s) in flight were issued again", n);
}

static void rebuilt(struct zss_dev *dev)
{
    pthread_mutex_lock(&rec_lock);
    dev->generation++;
    zss_epoch++;
    dev->lost = false;
    pthread_cond_broadcast(&rec_cv);
    pthread_mutex_unlock(&rec_lock);
}

void zss_dev_mark_lost(struct zss_dev *dev)
{
    pthread_mutex_lock(&rec_lock);
    dev->lost = true;
    pthread_mutex_unlock(&rec_lock);
}

void zss_dev_unrecoverable(struct zss_dev *dev)
{
    pthread_mutex_lock(&rec_lock);
    dev->dead = true;
    pthread_cond_broadcast(&rec_cv);
    pthread_mutex_unlock(&rec_lock);
}

#define ZSS_EVACUATE_WAIT_S 5

/* Whether the GPU has stopped answering on the bus: a device without power reads as all ones. */
static bool gpu_gone(const struct zss_gpu *gpu)
{
    unsigned char id[2] = { 0, 0 };
    char path[96];
    int fd;

    if (!gpu || !gpu->pci[0] || gpu->software || gpu->test_bound)
        return false;
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/config", gpu->pci);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT;
    if (pread(fd, id, 2, 0) != 2)
        id[0] = id[1] = 0;
    close(fd);
    return id[0] == 0xff && id[1] == 0xff;
}

bool zss_lost(struct zss_dev *dev, VkResult r)
{
    struct timespec until;
    bool first, asked_locally = false;
    uint32_t gen;

    if (dev->dead || !dev->migratable)
        return false;
    /*
     * A thread that was left behind in the dead driver and has only now come
     * back: the recovery it needs has already happened. Whatever the dead
     * driver answered, even success, was about a device that no longer
     * exists. Rejoin and repeat.
     */
    if (zss_stale()) {
        zss_leave();
        zss_enter();
        return true;
    }
    /*
     * A device already known to be lost fails in whatever way its driver
     * happens to. So does one that has only just gone, before anyone has said
     * so: the proprietary NVIDIA driver answers a present with an error of its
     * own, and an application acting on that error does itself in. Any failure
     * is therefore checked against the bus before it is passed on.
     */
    if (r != VK_ERROR_DEVICE_LOST && !(r < 0 && (dev->lost || gpu_gone(dev->gpu))))
        return false;

    pthread_mutex_lock(&rec_lock);
    gen = dev->generation;
    first = !dev->lost;
    dev->lost = true;
    pthread_mutex_unlock(&rec_lock);

    /* Out of the gate, so the recovery can close it. */
    zss_leave();
    if (first) {
        zss_log("the device on %s was lost; waiting to be rebuilt", dev->gpu ? dev->gpu->props.deviceName : "?");
        zss_control_report_lost(dev);
    }

    /* Give the daemon a moment to say where to go; without one, decide at once. */
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += zss_control_connected() ? ZSS_EVACUATE_WAIT_S : 0;
    pthread_mutex_lock(&rec_lock);
    while (dev->generation == gen && !dev->dead) {
        if (asked_locally) {
            pthread_cond_wait(&rec_cv, &rec_lock);
        } else if (pthread_cond_timedwait(&rec_cv, &rec_lock, &until) != 0) {
            /* No daemon, or it did not answer: decide here. A parked result keeps us waiting. */
            asked_locally = true;
            pthread_mutex_unlock(&rec_lock);
            zss_control_recover_local(dev);
            pthread_mutex_lock(&rec_lock);
        }
    }
    pthread_mutex_unlock(&rec_lock);

    zss_enter();
    return !dev->dead;
}

/* ---- entry points ------------------------------------------------------------- */

enum zss_outcome zss_resume(struct zss_dev *dev, struct zss_gpu *target, char *reason, size_t rlen)
{
    VkResult r;

    if (dev->gpu)
        return ZO_MIGRATED;
    if (!zss_compatible(dev, target, reason, rlen))
        return ZO_FAILED;
    r = build(dev, target);
    if (r != VK_SUCCESS) {
        teardown(dev);
        ctx_clear(dev);
        snprintf(reason, rlen, "%s rejected a resource the application needs (VkResult %d)",
                 target->props.deviceName, r);
        return ZO_FAILED;
    }
    free_saved(dev);
    reissue(dev);
    rebuilt(dev);
    return ZO_MIGRATED;
}

/*
 * Moves a device to `target`, or parks it when target is NULL or unsuitable.
 * `lost` means the source cannot be read or trusted: contents come from
 * memory, a failed build cannot fall back to the source, and its teardown
 * is best effort. `abandon` skips that teardown altogether.
 */
static enum zss_outcome relocate(struct zss_dev *dev, struct zss_gpu *target, bool lost, bool abandon,
                                 char *reason, size_t rlen)
{
    struct zss_real *snap;
    struct ctx old, new;
    bool have_new = false;
    uint32_t n = 0, i;
    VkResult r;

    if (!dev->gpu)
        return target ? zss_resume(dev, target, reason, rlen) : ZO_PARKED;
    if (target == dev->gpu && !lost)
        return ZO_MIGRATED;
    if (!dev->migratable) {
        snprintf(reason, rlen, "%s", dev->reason);
        if (lost)
            zss_dev_unrecoverable(dev);
        return ZO_FAILED;
    }

    if (!lost) {
        /*
         * A linear image the application fills through a mapping is laid out by
         * one driver; its bytes mean nothing to another. Once uploaded it moves
         * like any image, but not while the upload is still in progress.
         */
        for (struct zss_obj *o = dev->head; o; o = o->next) {
            if (o->kind != ZK_IMAGE || o->dead || !o->r.map || !o->u.img.mem)
                continue;
            if (o->u.img.mem->u.mem.mapped || o->u.img.layout[0] == VK_IMAGE_LAYOUT_PREINITIALIZED) {
                snprintf(reason, rlen, "an image is being filled through mapped memory; retry in a moment");
                return ZO_FAILED;
            }
        }
        dev->fn.DeviceWaitIdle(dev->real);
        zss_inflight_done(dev, NULL);
        zss_sync_from_device(dev, NULL);
    }
    /* An unsuitable target is not an error: the application waits for a better one. */
    if (target && !zss_compatible(dev, target, reason, rlen))
        target = NULL;

    dev->lost_contents = 0;
    if (lost) {
        dev->lost_contents = prepare_from_memory(dev);
    } else {
        r = capture(dev);
        if (r != VK_SUCCESS) {
            free_saved(dev);
            snprintf(reason, rlen, "could not read state back from %s (VkResult %d)",
                     dev->gpu->props.deviceName, r);
            return ZO_FAILED;
        }
    }

    for (struct zss_obj *o = dev->head; o; o = o->next)
        n++;
    snap = calloc(n ? n : 1, sizeof(*snap));
    i = 0;
    for (struct zss_obj *o = dev->head; o; o = o->next) {
        /*
         * A driver gives a window one swapchain at a time. When the device is
         * rebuilt on the driver it is already on (after a loss, a reset) the
         * old swapchain has to go before the new one can be made. Should the
         * rebuild then fail, the application is told to make a new one.
         */
        if (o->kind == ZK_SWAPCHAIN && o->r.h && o->r.standin) {
            /* Presented through the screen's GPU: that presenter holds the window; it goes now (present.c). */
            zss_presenter_destroy((struct zss_presenter *)(uintptr_t)o->r.h, abandon);
            o->r = (struct zss_real){ 0 };
        }
        if (o->kind == ZK_SWAPCHAIN && o->r.h && target && dev->gpu && zss_present_driver(target) == dev->gpu->drv && !abandon) {
            dev->fn.DestroySwapchainKHR(dev->real, (VkSwapchainKHR)(uintptr_t)o->r.h, NULL);
            o->r.h = 0;
        }
        snap[i++] = o->r;
        o->r = (struct zss_real){ 0 };
    }
    ctx_save(dev, &old);

    if (target) {
        ctx_clear(dev);
        r = build(dev, target);
        if (r == VK_SUCCESS) {
            ctx_save(dev, &new);
            have_new = true;
        } else {
            teardown(dev);
            snprintf(reason, rlen, "%s rejected a resource the application needs (VkResult %d)",
                     target->props.deviceName, r);
            if (!lost) {
                /* The source is intact: put everything back as it was. */
                ctx_load(dev, &old);
                i = 0;
                for (struct zss_obj *o = dev->head; o; o = o->next) {
                    o->r = snap[i++];
                    o->r.synced = NULL; /* written pages may have been counted against the new mappings */
                    if (o->kind == ZK_SWAPCHAIN) {
                        zss_swapchain_rollback(o);
                        o->u.sc.retired = o->r.h == 0;
                    }
                }
                free(snap);
                free_saved(dev);
                return ZO_FAILED;
            }
            /* There is no source to go back to. Park, and try again when a GPU turns up. */
            for (struct zss_obj *o = dev->head; o; o = o->next)
                o->r = (struct zss_real){ 0 };
            target = NULL;
        }
    }

    /* Commit: the source's objects go in reverse creation order, then its device. */
    ctx_load(dev, &old);
    if (abandon) {
        /*
         * A thread is still inside the old driver. Destroying the device
         * under it would be worse than leaking it. Its driver stays loaded.
         */
        zss_log("abandoning the lost device on %s: a thread is still inside its driver",
                dev->gpu->props.deviceName);
        zss_driver_break_links(dev->gpu->drv);
        zss_lower_abandon(dev); /* its stand-ins go with the device; the driver is not called */
    } else {
        i = n;
        for (struct zss_obj *o = dev->tail; o; o = o->prev) {
            i--;
            zss_real_destroy(dev, o->kind, &snap[i]);
        }
        zss_dev_destroy_real(dev);
    }
    free(snap);

    if (!have_new) {
        for (struct zss_obj *o = dev->head; o; o = o->next)
            if (o->kind == ZK_SWAPCHAIN)
                o->u.sc.retired = true;
        ctx_clear(dev);
        return ZO_PARKED;
    }
    ctx_load(dev, &new);
    free_saved(dev);
    reissue(dev);
    rebuilt(dev);
    return ZO_MIGRATED;
}

enum zss_outcome zss_migrate(struct zss_dev *dev, struct zss_gpu *target, char *reason, size_t rlen)
{
    return relocate(dev, target, false, false, reason, rlen);
}

enum zss_outcome zss_recover(struct zss_dev *dev, struct zss_gpu *target, bool abandon, char *reason,
                             size_t rlen)
{
    struct timespec t0, t1;
    enum zss_outcome out;
    long ms;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    out = relocate(dev, target, true, abandon, reason, rlen);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;

    if (out == ZO_MIGRATED)
        zss_log("recovered after submit %u onto %s; %d object(s) lost their contents; rebuild took %ld ms",
                zss_submits - dev->ninflight, dev->gpu->props.deviceName, dev->lost_contents, ms);
    else if (out == ZO_PARKED)
        zss_log("parked after submit %u with nowhere to recover to; %d object(s) lost their contents",
                zss_submits - dev->ninflight, dev->lost_contents);
    return out;
}
