// SPDX-License-Identifier: GPL-2.0-only
/*
 * A swapchain belongs to one driver and one window, so it cannot be carried
 * across. After a migration the layer makes a new one on the target for the
 * same window, with the same number of images, and keeps the application's
 * swapchain and image handles pointing at it. The two drivers number their
 * images independently, so the layer keeps a map between the application's
 * indices and the driver's.
 *
 * Where the target cannot give the same swapchain (another image count,
 * format or size) the old way remains: the swapchain is "retired", its
 * images are replaced by ordinary stand-ins so in-flight rendering has
 * somewhere to go, and the application is told the swapchain is out of date.
 * Most windowed applications rebuild then; some (Chromium) treat it as fatal.
 */
#include "zss_layer.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SLICE_NS 20000000ull

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

VKAPI_ATTR VkResult VKAPI_CALL zss_CreateSwapchainKHR(VkDevice device,
                                                      const VkSwapchainCreateInfoKHR *ci,
                                                      const VkAllocationCallbacks *alloc,
                                                      VkSwapchainKHR *out)
{
    struct zss_dev *dev = (struct zss_dev *)device;
    VkSwapchainCreateInfoKHR real = *ci;
    uint32_t qfi[ZSS_MAX_FAMILIES], n = 0;
    struct zss_obj *o;
    VkSwapchainKHR sc;
    VkImage *images;
    VkResult r;

    (void)alloc;
    zss_enter();
    real.pNext = NULL;
    r = zss_surface_real(dev->gpu->drv, ci->surface, &real.surface);
    if (r != VK_SUCCESS) {
        zss_leave();
        return r;
    }
    /* A retired swapchain has no real counterpart to hand over. */
    real.oldSwapchain = ZREAL(VkSwapchainKHR, ci->oldSwapchain);
    if (ci->imageSharingMode == VK_SHARING_MODE_CONCURRENT) {
        for (uint32_t i = 0; i < ci->queueFamilyIndexCount && i < ZSS_MAX_FAMILIES; i++)
            qfi[i] = dev->fam_map[ci->pQueueFamilyIndices[i]] == UINT32_MAX
                         ? 0 : dev->fam_map[ci->pQueueFamilyIndices[i]];
        real.pQueueFamilyIndices = qfi;
    }
    r = dev->fn.CreateSwapchainKHR(dev->real, &real, NULL, &sc);
    if (r != VK_SUCCESS) {
        zss_leave();
        return r;
    }

    o = zss_obj_new(dev, ZK_SWAPCHAIN);
    o->r.h = (uint64_t)(uintptr_t)sc;
    o->u.sc.ci = *ci;
    o->u.sc.ci.pNext = NULL;
    o->u.sc.ci.pQueueFamilyIndices = NULL;
    o->u.sc.ci.queueFamilyIndexCount = 0;
    o->u.sc.ci.oldSwapchain = VK_NULL_HANDLE;

    dev->fn.GetSwapchainImagesKHR(dev->real, sc, &n, NULL);
    images = calloc(n ? n : 1, sizeof(*images));
    dev->fn.GetSwapchainImagesKHR(dev->real, sc, &n, images);
    o->u.sc.images = calloc(n ? n : 1, sizeof(*o->u.sc.images));
    o->u.sc.nimages = n;
    o->u.sc.real_of = calloc(n ? n : 1, sizeof(*o->u.sc.real_of));
    o->u.sc.undo_real_of = calloc(n ? n : 1, sizeof(*o->u.sc.undo_real_of));
    for (uint32_t i = 0; i < n; i++)
        o->u.sc.real_of[i] = i;
    for (uint32_t i = 0; i < n; i++) {
        struct zss_obj *img = zss_obj_new(dev, ZK_IMAGE);

        img->u.img.ci = (VkImageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = ci->imageFormat,
            .extent = { ci->imageExtent.width, ci->imageExtent.height, 1 },
            .mipLevels = 1,
            .arrayLayers = ci->imageArrayLayers,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = ci->imageUsage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        img->u.img.layout = calloc(ci->imageArrayLayers ? ci->imageArrayLayers : 1, sizeof(VkImageLayout));
        img->u.img.swapchain = o;
        img->r.h = (uint64_t)(uintptr_t)images[i];
        img->r.borrowed = true;
        o->u.sc.images[i] = img;
    }
    free(images);
    *out = ZHANDLE(VkSwapchainKHR, o);
    zss_leave();
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL zss_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                   const VkAllocationCallbacks *alloc)
{
    struct zss_dev *dev = (struct zss_dev *)device;
    struct zss_obj *o = ZOBJ(swapchain);
    struct zss_real r;

    (void)alloc;
    if (!o)
        return;
    zss_enter();
    for (uint32_t i = 0; i < o->u.sc.nimages; i++) {
        struct zss_obj *img = o->u.sc.images[i];
        struct zss_real ir = img->r;

        zss_real_destroy(dev, ZK_IMAGE, &ir);
        img->u.img.swapchain = NULL;
        zss_obj_kill(img);
    }
    o->u.sc.nimages = 0;
    free(o->u.sc.real_of);
    free(o->u.sc.undo_real_of);
    o->u.sc.real_of = o->u.sc.undo_real_of = NULL;
    r = o->r;
    zss_real_destroy(dev, ZK_SWAPCHAIN, &r);
    zss_obj_kill(o);
    zss_leave();
}

VKAPI_ATTR VkResult VKAPI_CALL zss_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                         uint32_t *count, VkImage *images)
{
    struct zss_obj *o = ZOBJ(swapchain);
    uint32_t n;

    (void)device;
    if (!images) {
        *count = o->u.sc.nimages;
        return VK_SUCCESS;
    }
    n = *count < o->u.sc.nimages ? *count : o->u.sc.nimages;
    for (uint32_t i = 0; i < n; i++)
        images[i] = ZHANDLE(VkImage, o->u.sc.images[i]);
    *count = n;
    return n < o->u.sc.nimages ? VK_INCOMPLETE : VK_SUCCESS;
}

static uint32_t app_index(const struct zss_obj *sc, uint32_t real)
{
    for (uint32_t i = 0; i < sc->u.sc.nimages; i++)
        if (sc->u.sc.real_of[i] == real)
            return i;
    return real;
}

/* Waits for an acquire the layer made for itself. */
static VkResult acquire_wait(struct zss_dev *dev, VkSwapchainKHR sc, uint64_t timeout, uint32_t *real)
{
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    VkResult r = dev->fn.CreateFence(dev->real, &fci, NULL, &fence);

    if (r != VK_SUCCESS)
        return r;
    r = dev->fn.AcquireNextImageKHR(dev->real, sc, timeout, VK_NULL_HANDLE, fence, real);
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
        VkResult w = dev->fn.WaitForFences(dev->real, 1, &fence, VK_TRUE, 2000000000ull);

        if (w != VK_SUCCESS)
            r = w < 0 ? w : VK_ERROR_DEVICE_LOST;
    }
    dev->fn.DestroyFence(dev->real, fence, NULL);
    return r;
}

/*
 * An image of a rebuilt swapchain, handed to the application for the first
 * time. The application believes it is in the layout it last left that
 * image in; the driver's new image is in none. The layer puts it there, and
 * only then lets the application's semaphore and fence fire. That takes the
 * device's queue, so it is done with every other thread out of the layer.
 */
struct first_use {
    struct zss_dev *dev;
    struct zss_obj *sc, *sem, *fence;
    uint32_t index, gen;
};

static void first_use(void *arg)
{
    struct first_use *u = arg;
    struct zss_dev *dev = u->dev;
    VkSemaphore sem = u->sem ? (VkSemaphore)(uintptr_t)u->sem->r.h : VK_NULL_HANDLE;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .signalSemaphoreCount = sem ? 1 : 0,
        .pSignalSemaphores = &sem,
    };

    /* Moved again, or parked, in the meantime: the rebuild that did it has dealt with this image. */
    if (!dev->gpu || u->sc->u.sc.gen != u->gen || !((u->sc->u.sc.fresh >> u->index) & 1))
        return;
    u->sc->u.sc.fresh &= ~(1ull << u->index);
    {
        struct zss_obj *img = u->sc->u.sc.images[u->index];

        zss_image_bring_up(dev, img, img->u.img.pending, img->u.img.pending_size);
        free(img->u.img.pending);
        img->u.img.pending = NULL;
    }
    if (sem || u->fence) {
        dev->fn.QueueSubmit(dev->util_queue, 1, &si, u->fence ? (VkFence)(uintptr_t)u->fence->r.h : VK_NULL_HANDLE);
        dev->fn.QueueWaitIdle(dev->util_queue);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL zss_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                       uint64_t timeout, VkSemaphore semaphore,
                                                       VkFence fence, uint32_t *index)
{
    struct zss_dev *dev = (struct zss_dev *)device;
    struct zss_obj *o = ZOBJ(swapchain);
    uint64_t start = now_ns();
    VkResult r;

    for (;;) {
        uint64_t elapsed = now_ns() - start;
        uint64_t left = timeout == UINT64_MAX ? UINT64_MAX : (elapsed >= timeout ? 0 : timeout - elapsed);
        uint64_t slice = left < SLICE_NS ? left : SLICE_NS;
        struct first_use first = { .dev = dev, .sc = o };
        bool fresh;
        uint32_t real = 0;

        zss_enter();
        if (o->u.sc.retired) {
            zss_leave();
            return VK_ERROR_OUT_OF_DATE_KHR;
        }
        /* While any image is still new, the layer has to see which one comes before the application does. */
        fresh = o->u.sc.fresh != 0;
        if (fresh)
            r = acquire_wait(dev, ZREAL(VkSwapchainKHR, swapchain), slice, &real);
        else
            r = dev->fn.AcquireNextImageKHR(dev->real, ZREAL(VkSwapchainKHR, swapchain), slice,
                                            ZREAL(VkSemaphore, semaphore), ZREAL(VkFence, fence), &real);
        if (zss_lost(dev, r)) {
            /*
             * The device was lost and has been rebuilt while this thread
             * waited. If its swapchain was rebuilt with it, the application
             * need not know: ask again. Otherwise it builds a new one.
             */
            bool gone = o->u.sc.retired;

            zss_leave();
            if (gone)
                return VK_ERROR_OUT_OF_DATE_KHR;
            continue;
        }
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            *index = app_index(o, real);
            if (*index < 64)
                o->u.sc.acquired |= 1ull << *index;
            /* Recorded as fired, so that a rebuild from here on makes them fired again. */
            if (semaphore)
                ZOBJ(semaphore)->u.sem.signaled = true;
            if (fence && fresh) {
                ZOBJ(fence)->u.fence.signaled = true;
                ZOBJ(fence)->u.fence.pending = false;
            }
            first.sem = semaphore ? ZOBJ(semaphore) : NULL;
            first.fence = fence ? ZOBJ(fence) : NULL;
            first.index = *index;
            first.gen = o->u.sc.gen;
        }
        zss_leave();
        if (fresh && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR))
            zss_control_exclusive(first_use, &first);
        if ((r != VK_TIMEOUT && r != VK_NOT_READY) || left <= slice)
            return r;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL zss_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info)
{
    struct zss_queue *q = (struct zss_queue *)queue;
    struct zss_dev *dev = q->dev;
    VkPresentInfoKHR real = *info;
    VkSemaphore *waits;
    VkSwapchainKHR *chains;
    uint32_t *indices;
    bool retired = false;
    VkResult r;

    zss_enter();
    waits = malloc((info->waitSemaphoreCount + 1) * sizeof(*waits));
    chains = malloc((info->swapchainCount + 1) * sizeof(*chains));
    for (uint32_t i = 0; i < info->waitSemaphoreCount; i++) {
        waits[i] = ZREAL(VkSemaphore, info->pWaitSemaphores[i]);
        ZOBJ(info->pWaitSemaphores[i])->u.sem.signaled = false;
    }
    indices = malloc((info->swapchainCount + 1) * sizeof(*indices));
    for (uint32_t i = 0; i < info->swapchainCount; i++) {
        struct zss_obj *sc = ZOBJ(info->pSwapchains[i]);
        uint32_t a = info->pImageIndices[i];

        chains[i] = ZREAL(VkSwapchainKHR, info->pSwapchains[i]);
        indices[i] = a < sc->u.sc.nimages && sc->u.sc.real_of ? sc->u.sc.real_of[a] : a;
        if (a < 64)
            sc->u.sc.acquired &= ~(1ull << a);
        if (sc->u.sc.retired)
            retired = true;
    }

    if (retired) {
        /* Nothing to present to. Consume the semaphores so they can be reused. */
        VkPipelineStageFlags *stages = malloc((info->waitSemaphoreCount + 1) * sizeof(*stages));
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = info->waitSemaphoreCount,
            .pWaitSemaphores = waits,
            .pWaitDstStageMask = stages,
        };

        for (uint32_t i = 0; i < info->waitSemaphoreCount; i++)
            stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        if (info->waitSemaphoreCount)
            dev->fn.QueueSubmit(q->real, 1, &si, VK_NULL_HANDLE);
        for (uint32_t i = 0; info->pResults && i < info->swapchainCount; i++)
            info->pResults[i] = VK_ERROR_OUT_OF_DATE_KHR;
        free(stages);
        r = VK_ERROR_OUT_OF_DATE_KHR;
    } else {
        real.pNext = NULL;
        real.pWaitSemaphores = waits;
        real.pSwapchains = chains;
        real.pImageIndices = indices;
        r = dev->fn.QueuePresentKHR(q->real, &real);
        if (zss_lost(dev, r)) {
            /*
             * The frame went with the device. Where the swapchain was rebuilt
             * in place that is one frame not shown, which is no error; where
             * it was not, the application has to build a new one.
             */
            r = VK_SUCCESS;
            for (uint32_t i = 0; i < info->swapchainCount; i++) {
                bool gone = ZOBJ(info->pSwapchains[i])->u.sc.retired;

                if (gone)
                    r = VK_ERROR_OUT_OF_DATE_KHR;
                if (info->pResults)
                    info->pResults[i] = gone ? VK_ERROR_OUT_OF_DATE_KHR : VK_SUCCESS;
            }
        }
    }
    free(waits);
    free(chains);
    free(indices);
    zss_leave();
    return r;
}

void zss_swapchain_retire(struct zss_dev *dev, struct zss_obj *sc)
{
    (void)dev;
    sc->u.sc.retired = true;
    sc->u.sc.gen++;
    sc->r = (struct zss_real){ 0 };
}

/* Whether the target can present this swapchain's images to its window exactly as the application made them. */
static bool why_not(const char *why)
{
    zss_dbg("swapchain not rebuilt in place: %s", why);
    return false;
}

static bool rebuild_fits(struct zss_dev *dev, const struct zss_obj *sc, VkSwapchainCreateInfoKHR *ci)
{
    const struct zss_inst_fns *fn = &dev->gpu->drv->fn;
    VkPhysicalDevice pd = zss_gpu_real(dev->gpu);
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR formats[128];
    VkPresentModeKHR modes[16];
    uint32_t nf = 128, nm = 16, n = sc->u.sc.nimages;
    bool found = false;

    if (!pd || !n || n > 64 || ci->imageSharingMode != VK_SHARING_MODE_EXCLUSIVE)
        return why_not("its images are shared between queue families, or there are too many");
    if (zss_surface_real(dev->gpu->drv, sc->u.sc.ci.surface, &ci->surface) != VK_SUCCESS)
        return why_not("the target has no surface for the window");
    if (fn->GetPhysicalDeviceSurfaceCapabilitiesKHR(pd, ci->surface, &caps) != VK_SUCCESS)
        return why_not("the target does not answer for the window");
    if (n < caps.minImageCount || (caps.maxImageCount && n > caps.maxImageCount))
        return why_not("the target cannot make a swapchain of as many images");
    if (caps.currentExtent.width != UINT32_MAX &&
        (caps.currentExtent.width != ci->imageExtent.width || caps.currentExtent.height != ci->imageExtent.height))
        return why_not("the window has changed size"); /* the window has changed size: out of date is the truth */
    if ((caps.supportedUsageFlags & ci->imageUsage) != ci->imageUsage || ci->imageArrayLayers > caps.maxImageArrayLayers) {
        zss_dbg("swapchain uses %#x layers %u; the target offers uses %#x layers %u", (unsigned)ci->imageUsage,
                ci->imageArrayLayers, (unsigned)caps.supportedUsageFlags, caps.maxImageArrayLayers);
        return why_not("the target lacks an image use the swapchain has");
    }
    if (!(caps.supportedCompositeAlpha & ci->compositeAlpha)) {
        /*
         * Drivers disagree on how a window's alpha may be treated (for a
         * window with an alpha channel one offers "opaque", another only
         * "as the window system decides"). Leaving it to the window system
         * is what an application falls back to itself; where the target has
         * not even that, its first mode is taken. The picture can differ
         * only where the application leaves alpha below one.
         */
        VkCompositeAlphaFlagsKHR have = caps.supportedCompositeAlpha;
        VkCompositeAlphaFlagBitsKHR was = ci->compositeAlpha;

        ci->compositeAlpha = have & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
                           : have & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                           : (VkCompositeAlphaFlagBitsKHR)(have & -have);
        if (!ci->compositeAlpha)
            return why_not("the target has no alpha mode for the window");
        zss_dbg("swapchain alpha mode %#x is not on the target; using %#x", (unsigned)was, (unsigned)ci->compositeAlpha);
    }
    if (!(caps.supportedTransforms & ci->preTransform))
        ci->preTransform = caps.currentTransform;
    if (fn->GetPhysicalDeviceSurfaceFormatsKHR(pd, ci->surface, &nf, formats) < 0)
        return false;
    for (uint32_t i = 0; i < nf; i++)
        found = found || (formats[i].format == ci->imageFormat && formats[i].colorSpace == ci->imageColorSpace);
    if (!found)
        return why_not("the target does not present the swapchain's format");
    found = false;
    if (fn->GetPhysicalDeviceSurfacePresentModesKHR(pd, ci->surface, &nm, modes) < 0)
        return false;
    for (uint32_t i = 0; i < nm; i++)
        found = found || modes[i] == ci->presentMode;
    if (!found)
        ci->presentMode = VK_PRESENT_MODE_FIFO_KHR; /* the one mode every driver has */
    ci->minImageCount = n;
    ci->oldSwapchain = VK_NULL_HANDLE;
    return true;
}

/*
 * Makes the swapchain again on the device's new GPU. Returns false, with
 * nothing changed, if the target cannot give one the application's handles
 * still fit; the caller retires it then.
 */
bool zss_swapchain_rebuild(struct zss_dev *dev, struct zss_obj *sc)
{
    VkSwapchainCreateInfoKHR ci = sc->u.sc.ci;
    uint32_t n = sc->u.sc.nimages, got = 0, map[64];
    VkImage images[64];
    VkSwapchainKHR real;
    bool blank = false;

    if (!sc->u.sc.real_of || !rebuild_fits(dev, sc, &ci))
        return false;
    if (dev->fn.CreateSwapchainKHR(dev->real, &ci, NULL, &real) != VK_SUCCESS)
        return why_not("the target refused to create it");
    dev->fn.GetSwapchainImagesKHR(dev->real, real, &got, NULL);
    if (got != n || dev->fn.GetSwapchainImagesKHR(dev->real, real, &got, images) != VK_SUCCESS) {
        zss_dbg("swapchain not rebuilt in place: the target made %u images where the application has %u", got, n);
        goto no;
    }

    /*
     * An image the application holds must be one the layer holds of the new
     * swapchain. Which one the driver gives cannot be chosen, so the map is
     * arranged around whatever comes.
     */
    for (uint32_t i = 0; i < n; i++)
        map[i] = i;
    for (uint32_t a = 0; a < n; a++) {
        uint32_t k, b;

        if (!((sc->u.sc.acquired >> a) & 1))
            continue;
        if (acquire_wait(dev, real, 2000000000ull, &k) < 0 || k >= n) {
            zss_dbg("swapchain not rebuilt in place: no image of the new one could be acquired");
            goto no;
        }
        for (b = 0; b < n && map[b] != k; b++)
            ;
        map[b] = map[a];
        map[a] = k;
    }

    memcpy(sc->u.sc.undo_real_of, sc->u.sc.real_of, n * sizeof(*sc->u.sc.real_of));
    sc->u.sc.undo_fresh = sc->u.sc.fresh;
    sc->u.sc.undo_valid = true;
    sc->u.sc.undo_pending = 0;
    memcpy(sc->u.sc.real_of, map, n * sizeof(*sc->u.sc.real_of));
    sc->u.sc.fresh = n == 64 ? ~0ull : (1ull << n) - 1;
    sc->u.sc.gen++;
    sc->u.sc.retired = false;
    sc->r = (struct zss_real){ .h = (uint64_t)(uintptr_t)real };
    for (uint32_t i = 0; i < n; i++) {
        struct zss_obj *img = sc->u.sc.images[i];

        img->r = (struct zss_real){ .h = (uint64_t)(uintptr_t)images[map[i]], .borrowed = true };
        /* What was read out of the old image waits here until the new one may be written to. */
        if (img->u.img.saved) {
            free(img->u.img.pending);
            img->u.img.pending = img->u.img.saved;
            img->u.img.pending_size = img->u.img.saved_size;
            img->u.img.saved = NULL;
            sc->u.sc.undo_pending |= 1ull << i;
        } else if (!img->u.img.pending && img->u.img.layout[0] != VK_IMAGE_LAYOUT_UNDEFINED) {
            blank = true; /* it had been drawn to, and what it held is gone */
        }
    }
    if (blank)
        zss_surface_repaint(dev->gpu->drv, sc->u.sc.ci.surface);
    /* Those the application already holds are in use from the next command on. */
    for (uint32_t a = 0; a < n; a++) {
        struct zss_obj *img = sc->u.sc.images[a];

        if (!((sc->u.sc.acquired >> a) & 1))
            continue;
        sc->u.sc.fresh &= ~(1ull << a);
        zss_image_bring_up(dev, img, img->u.img.pending, img->u.img.pending_size);
        free(img->u.img.pending);
        img->u.img.pending = NULL;
    }
    return true;
no:
    dev->fn.DestroySwapchainKHR(dev->real, real, NULL);
    return false;
}

/* The rebuild's device was given up; the application's indices mean what they meant before it. */
void zss_swapchain_rollback(struct zss_obj *sc)
{
    if (!sc->u.sc.real_of || !sc->u.sc.undo_valid)
        return;
    sc->u.sc.undo_valid = false;
    /* The old images still hold what was read out of them. */
    for (uint32_t i = 0; i < sc->u.sc.nimages; i++)
        if ((sc->u.sc.undo_pending >> i) & 1) {
            free(sc->u.sc.images[i]->u.img.pending);
            sc->u.sc.images[i]->u.img.pending = NULL;
        }
    memcpy(sc->u.sc.real_of, sc->u.sc.undo_real_of, sc->u.sc.nimages * sizeof(*sc->u.sc.real_of));
    sc->u.sc.fresh = sc->u.sc.undo_fresh;
    sc->u.sc.gen++;
}
