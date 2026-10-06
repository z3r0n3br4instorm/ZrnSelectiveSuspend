// SPDX-License-Identifier: GPL-2.0
/*
 * Swapchains cannot be moved: the image count and formats belong to one
 * driver. After a migration the old swapchain is "retired": its images are
 * replaced by ordinary stand-ins so in-flight rendering has somewhere to go,
 * and the application is told the swapchain is out of date, which every
 * windowed application already handles by building a new one.
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

        zss_enter();
        if (o->u.sc.retired) {
            zss_leave();
            return VK_ERROR_OUT_OF_DATE_KHR;
        }
        r = dev->fn.AcquireNextImageKHR(dev->real, ZREAL(VkSwapchainKHR, swapchain), slice,
                                        ZREAL(VkSemaphore, semaphore), ZREAL(VkFence, fence), index);
        if (zss_lost(dev, r)) {
            /* The swapchain went with the device; the application builds a new one. */
            zss_leave();
            return VK_ERROR_OUT_OF_DATE_KHR;
        }
        if ((r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) && semaphore)
            ZOBJ(semaphore)->u.sem.signaled = true;
        zss_leave();
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
    bool retired = false;
    VkResult r;

    zss_enter();
    waits = malloc((info->waitSemaphoreCount + 1) * sizeof(*waits));
    chains = malloc((info->swapchainCount + 1) * sizeof(*chains));
    for (uint32_t i = 0; i < info->waitSemaphoreCount; i++) {
        waits[i] = ZREAL(VkSemaphore, info->pWaitSemaphores[i]);
        ZOBJ(info->pWaitSemaphores[i])->u.sem.signaled = false;
    }
    for (uint32_t i = 0; i < info->swapchainCount; i++) {
        chains[i] = ZREAL(VkSwapchainKHR, info->pSwapchains[i]);
        if (ZOBJ(info->pSwapchains[i])->u.sc.retired)
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
        r = dev->fn.QueuePresentKHR(q->real, &real);
        if (zss_lost(dev, r))
            r = VK_ERROR_OUT_OF_DATE_KHR;
    }
    free(waits);
    free(chains);
    zss_leave();
    return r;
}

void zss_swapchain_retire(struct zss_dev *dev, struct zss_obj *sc)
{
    (void)dev;
    sc->u.sc.retired = true;
    sc->r = (struct zss_real){ 0 };
}
