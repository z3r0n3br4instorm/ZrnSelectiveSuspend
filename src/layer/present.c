// SPDX-License-Identifier: GPL-2.0-only
/*
 * Presenting on the GPU that drives the screen.
 *
 * A program may draw on one GPU while the screen is driven by another. Mesa's
 * drivers hand their frames to the display server's GPU themselves, in order
 * (DRI3). The NVIDIA proprietary driver does that only when the X server has
 * its card; on the reference laptop the card is kept out of X so that it can
 * be lent to a virtual machine, and then the driver's frames reach the window
 * out of order now and then: an older frame flashes up after a newer one,
 * most visibly while typing or scrolling.
 *
 * So the layer does the hand-over itself. The application's swapchain images
 * are ordinary images on its GPU (the layer's stand-ins); the window's real
 * swapchain is the layer's, on the screen's GPU, on a small device of its
 * own. At each present the finished image is read back into memory, waited
 * for, written into an image of the real swapchain and presented there. One
 * thread, one frame after the other: the order the application presented in
 * is the order the screen shows.
 *
 * The cost is a copy through memory per frame (about 5 MB at 1440x900) and
 * the time to read it back. That is the price of order without the X server's
 * help.
 *
 * ZSS_PRESENT=direct turns it off; ZSS_PRESENT=copy uses it for any driver.
 */
#include "zss_layer.h"

#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

/* ---- which GPU drives the screen ------------------------------------------------------ */

static struct zss_gpu *found_display;

static void display_gpu_find(void)
{
    {
        void *xcb = dlopen("libxcb.so.1", RTLD_NOW | RTLD_GLOBAL), *dri3 = dlopen("libxcb-dri3.so.0", RTLD_NOW);
        void *(*connect)(const char *, int *);
        void (*disconnect)(void *);
        const void *(*get_setup)(void *);
        struct { void *data; int rem; int index; } (*roots)(const void *);
        unsigned (*open_req)(void *, uint32_t, uint32_t);
        void *(*open_reply)(void *, unsigned, void *);
        int *(*reply_fds)(void *, void *);
        char link[64], path[PATH_MAX];
        struct stat st;
        void *c, *rep;
        int fd;

        if (!xcb || !dri3)
            return;
        connect = (void *(*)(const char *, int *))dlsym(xcb, "xcb_connect");
        disconnect = (void (*)(void *))dlsym(xcb, "xcb_disconnect");
        get_setup = (const void *(*)(void *))dlsym(xcb, "xcb_get_setup");
        roots = (typeof(roots))dlsym(xcb, "xcb_setup_roots_iterator");
        open_req = (unsigned (*)(void *, uint32_t, uint32_t))dlsym(dri3, "xcb_dri3_open");
        open_reply = (void *(*)(void *, unsigned, void *))dlsym(dri3, "xcb_dri3_open_reply");
        reply_fds = (int *(*)(void *, void *))dlsym(dri3, "xcb_dri3_open_reply_fds");
        if (!connect || !disconnect || !get_setup || !roots || !open_req || !open_reply || !reply_fds)
            return;
        c = connect(NULL, NULL);
        if (!c)
            return;
        /* The first screen's root window: xcb_screen_t begins with it. */
        rep = open_reply(c, open_req(c, *(uint32_t *)roots(get_setup(c)).data, 0), NULL);
        fd = rep ? reply_fds(c, rep)[0] : -1;
        free(rep);
        disconnect(c);
        if (fd < 0)
            return;
        if (fstat(fd, &st) == 0) {
            ssize_t len;

            snprintf(link, sizeof(link), "/sys/dev/char/%u:%u", major(st.st_rdev), minor(st.st_rdev));
            len = readlink(link, path, sizeof(path) - 1);
            if (len > 0) {
                path[len] = '\0';
                for (int i = 0; i < zss_ngpus; i++)
                    if (zss_gpus[i]->pci[0] && strstr(path, zss_gpus[i]->pci))
                        found_display = zss_gpus[i];
            }
        }
        close(fd);
        zss_dbg("the screen is driven by %s", found_display ? found_display->props.deviceName : "a GPU the layer does not know");
    }
}

/* The X server's own GPU, asked through DRI3; NULL if it cannot be told. Asked once. */
static struct zss_gpu *display_gpu(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;

    pthread_once(&once, display_gpu_find);
    return found_display;
}

/*
 * Whether frames drawn on `gpu` are to be presented through the layer. Mesa's
 * drivers ("libvulkan_*") present across GPUs correctly on their own.
 */
bool zss_present_needed(struct zss_gpu *gpu)
{
    const char *mode = getenv("ZSS_PRESENT");
    struct zss_gpu *screen;

    if (!gpu || gpu->software || (mode && !strcmp(mode, "direct")))
        return false;
    screen = display_gpu();
    if (!screen || screen == gpu)
        return false;
    if (mode && !strcmp(mode, "copy"))
        return true;
    return !strstr(gpu->drv->lib, "libvulkan_");
}

/* The driver whose swapchain holds the window when drawing happens on `gpu`. */
struct zss_driver *zss_present_driver(struct zss_gpu *gpu)
{
    return zss_present_needed(gpu) ? display_gpu()->drv : gpu->drv;
}

/* ---- the presenter ------------------------------------------------------------------- */

#define PFNS(X) \
    X(DestroyDevice) X(GetDeviceQueue) X(QueueSubmit) X(DeviceWaitIdle) X(CreateSwapchainKHR) \
    X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) X(AcquireNextImageKHR) X(QueuePresentKHR) \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) \
    X(EndCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyBufferToImage) X(CreateFence) X(DestroyFence) \
    X(WaitForFences) X(ResetFences) X(CreateSemaphore) X(DestroySemaphore) X(CreateBuffer) X(DestroyBuffer) \
    X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(MapMemory) X(BindBufferMemory)

#define MAXI 8

struct zss_presenter {
    /* The screen's side: a device of the layer's own. */
    struct zss_gpu *gpu;
    VkDevice dev;
    struct {
#define X(n) PFN_vk##n n;
        PFNS(X)
#undef X
    } fn;
    VkQueue queue;
    uint32_t family;
    VkSurfaceKHR surface;
    VkSwapchainCreateInfoKHR ci; /* as the screen's swapchain is made */
    VkSwapchainKHR sc;
    VkImage images[MAXI];
    uint32_t nimages;
    VkCommandPool pool;
    VkCommandBuffer cb[MAXI];
    VkFence done[MAXI];
    VkSemaphore ready[MAXI];
    VkFence acquired;
    VkBuffer up;
    VkDeviceMemory upmem;
    void *upmap;
    VkFence uploaded; /* the last copy out of `up`; it is written again only after */
    bool given_up;    /* its window was handed to a newer swapchain's presenter */

    /* The drawing side: on the application's device. */
    struct zss_dev *owner;
    VkDevice rdev;
    uint32_t rfamily;
    VkCommandPool rpool;
    VkCommandBuffer rcb;
    VkFence rfence;
    VkBuffer down;
    VkDeviceMemory downmem;
    void *downmap;
    bool down_coherent;
    VkDeviceSize size;
    VkExtent2D extent; /* the application's images */
};

static uint32_t mem_type(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

/* A format whose bytes the screen's swapchain shows the same: itself, or its sRGB or linear twin. */
static VkFormat twin(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    default: return f;
    }
}

static bool four_bytes(VkFormat f)
{
    return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_UNORM ||
           f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
           f == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
}

/* (Re)makes the screen's swapchain for the window's present size. */
static VkResult screen_swapchain(struct zss_presenter *p)
{
    const struct zss_inst_fns *ifn = &p->gpu->drv->fn;
    VkPhysicalDevice pd = zss_gpu_real(p->gpu);
    VkSurfaceCapabilitiesKHR caps;
    VkSwapchainKHR old = p->sc;
    VkResult r;
    uint32_t n = MAXI;

    if (ifn->GetPhysicalDeviceSurfaceCapabilitiesKHR(pd, p->surface, &caps) != VK_SUCCESS)
        return VK_ERROR_SURFACE_LOST_KHR;
    p->ci.imageExtent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : p->extent;
    if (!p->ci.imageExtent.width || !p->ci.imageExtent.height)
        return VK_ERROR_OUT_OF_DATE_KHR; /* minimised */
    p->ci.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && p->ci.minImageCount > caps.maxImageCount)
        p->ci.minImageCount = caps.maxImageCount;
    if (p->ci.minImageCount > MAXI)
        p->ci.minImageCount = MAXI;
    p->ci.preTransform = caps.currentTransform;
    if (!(caps.supportedCompositeAlpha & p->ci.compositeAlpha))
        p->ci.compositeAlpha = caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                   ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                   : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    p->ci.oldSwapchain = old;
    r = p->fn.CreateSwapchainKHR(p->dev, &p->ci, NULL, &p->sc);
    if (old) {
        p->fn.DeviceWaitIdle(p->dev);
        p->fn.DestroySwapchainKHR(p->dev, old, NULL);
    }
    if (r != VK_SUCCESS) {
        p->sc = VK_NULL_HANDLE;
        return r;
    }
    p->fn.GetSwapchainImagesKHR(p->dev, p->sc, &n, p->images);
    p->nimages = n;
    return VK_SUCCESS;
}

static void drawing_side_free(struct zss_presenter *p)
{
    struct zss_dev *d = p->owner;

    if (!p->rdev)
        return;
    if (p->rfence)
        d->fn.DestroyFence(p->rdev, p->rfence, NULL);
    if (p->rpool)
        d->fn.DestroyCommandPool(p->rdev, p->rpool, NULL);
    if (p->down)
        d->fn.DestroyBuffer(p->rdev, p->down, NULL);
    if (p->downmem)
        d->fn.FreeMemory(p->rdev, p->downmem, NULL);
    p->rfence = VK_NULL_HANDLE;
    p->rpool = VK_NULL_HANDLE;
    p->down = VK_NULL_HANDLE;
    p->downmem = VK_NULL_HANDLE;
    p->rdev = VK_NULL_HANDLE;
}

/* The readback buffer and the copy command on the application's device, for the queue family presenting. */
static VkResult drawing_side(struct zss_presenter *p, struct zss_dev *dev, uint32_t real_family)
{
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = p->size,
                               .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = real_family };
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkMemoryRequirements req;
    uint32_t t;
    VkResult r;

    if (p->rdev == dev->real && p->rfamily == real_family)
        return VK_SUCCESS;
    drawing_side_free(p);
    p->owner = dev;
    p->rdev = dev->real;
    p->rfamily = real_family;
    if ((r = dev->fn.CreateBuffer(p->rdev, &bci, NULL, &p->down)) != VK_SUCCESS)
        return r;
    dev->fn.GetBufferMemoryRequirements(p->rdev, p->down, &req);
    /* Read by the CPU: cached memory if there is any, coherent or not. */
    t = mem_type(&dev->gpu->mem, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (t == UINT32_MAX)
        t = mem_type(&dev->gpu->mem, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (t == UINT32_MAX)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    p->down_coherent = dev->gpu->mem.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    {
        VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                    .memoryTypeIndex = t };

        if ((r = dev->fn.AllocateMemory(p->rdev, &ai, NULL, &p->downmem)) != VK_SUCCESS ||
            (r = dev->fn.BindBufferMemory(p->rdev, p->down, p->downmem, 0)) != VK_SUCCESS ||
            (r = dev->fn.MapMemory(p->rdev, p->downmem, 0, VK_WHOLE_SIZE, 0, &p->downmap)) != VK_SUCCESS)
            return r;
    }
    if ((r = dev->fn.CreateCommandPool(p->rdev, &pci, NULL, &p->rpool)) != VK_SUCCESS)
        return r;
    cai.commandPool = p->rpool;
    if ((r = dev->fn.AllocateCommandBuffers(p->rdev, &cai, &p->rcb)) != VK_SUCCESS)
        return r;
    /* Dispatchable: the driver's loader data is set by whoever allocates for it, here the layer. */
    *(void **)p->rcb = *(void **)p->rdev;
    return dev->fn.CreateFence(p->rdev, &fci, NULL, &p->rfence);
}

void zss_presenter_destroy(struct zss_presenter *p, bool drawing_side_gone)
{
    if (!p)
        return;
    if (p->dev) {
        p->fn.DeviceWaitIdle(p->dev);
        for (uint32_t i = 0; i < MAXI; i++) {
            if (p->done[i])
                p->fn.DestroyFence(p->dev, p->done[i], NULL);
            if (p->ready[i])
                p->fn.DestroySemaphore(p->dev, p->ready[i], NULL);
        }
        if (p->acquired)
            p->fn.DestroyFence(p->dev, p->acquired, NULL);
        if (p->uploaded)
            p->fn.DestroyFence(p->dev, p->uploaded, NULL);
        if (p->pool)
            p->fn.DestroyCommandPool(p->dev, p->pool, NULL);
        if (p->up)
            p->fn.DestroyBuffer(p->dev, p->up, NULL);
        if (p->upmem)
            p->fn.FreeMemory(p->dev, p->upmem, NULL);
        if (p->sc)
            p->fn.DestroySwapchainKHR(p->dev, p->sc, NULL);
        p->fn.DestroyDevice(p->dev, NULL);
        p->gpu->drv->ndevices--;
    }
    if (!drawing_side_gone)
        drawing_side_free(p);
    zss_dbg("presenter on %s closed", p->gpu ? p->gpu->props.deviceName : "?");
    free(p);
}

/*
 * A presenter for the application's swapchain `ci` (its surface is the
 * loader's), drawing on `dev`. NULL if the screen's GPU cannot take it; the
 * caller then presents directly.
 */
struct zss_presenter *zss_presenter_new(struct zss_dev *dev, const VkSwapchainCreateInfoKHR *app)
{
    struct zss_presenter *p;
    struct zss_gpu *g = display_gpu();
    const struct zss_inst_fns *ifn;
    VkPhysicalDevice pd;
    VkQueueFamilyProperties fam[ZSS_MAX_FAMILIES];
    VkSurfaceFormatKHR formats[64];
    uint32_t nfam = ZSS_MAX_FAMILIES, nf = 64, family = UINT32_MAX;
    const float prio = 1;
    const char *ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkFormat want;
    bool format_ok = false;
    VkResult r;

    if (!g || !four_bytes(app->imageFormat) || app->imageArrayLayers != 1 || zss_driver_open(g->drv) != VK_SUCCESS)
        return NULL;
    ifn = &g->drv->fn;
    pd = zss_gpu_real(g);
    if (!pd)
        return NULL;
    p = calloc(1, sizeof(*p));
    p->gpu = g;
    p->extent = app->imageExtent;
    p->size = (VkDeviceSize)app->imageExtent.width * app->imageExtent.height * 4;
    if (zss_surface_real(g->drv, app->surface, &p->surface) != VK_SUCCESS)
        goto fail;
    ifn->GetPhysicalDeviceQueueFamilyProperties(pd, &nfam, fam);
    for (uint32_t i = 0; i < nfam && family == UINT32_MAX; i++) {
        VkBool32 can = VK_FALSE;

        ifn->GetPhysicalDeviceSurfaceSupportKHR(pd, i, p->surface, &can);
        if (can && (fam[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT)))
            family = i;
    }
    if (family == UINT32_MAX)
        goto fail;
    want = app->imageFormat;
    if (ifn->GetPhysicalDeviceSurfaceFormatsKHR(pd, p->surface, &nf, formats) >= 0) {
        for (uint32_t i = 0; i < nf && !format_ok; i++)
            if (formats[i].format == want || formats[i].format == twin(want)) {
                p->ci.imageFormat = formats[i].format;
                p->ci.imageColorSpace = formats[i].colorSpace;
                format_ok = true;
            }
    }
    if (!format_ok) {
        zss_dbg("the screen's GPU does not show format %d; presenting directly", app->imageFormat);
        goto fail;
    }
    {
        VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family,
                                        .queueCount = 1, .pQueuePriorities = &prio };
        VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                   .pQueueCreateInfos = &qci, .enabledExtensionCount = 1,
                                   .ppEnabledExtensionNames = &ext };

        if (ifn->CreateDevice(pd, &dci, NULL, &p->dev) != VK_SUCCESS) {
            p->dev = VK_NULL_HANDLE;
            goto fail;
        }
        g->drv->ndevices++;
    }
#define X(n) p->fn.n = (PFN_vk##n)ifn->GetDeviceProcAddr(p->dev, "vk" #n);
    PFNS(X)
#undef X
    p->family = family;
    p->fn.GetDeviceQueue(p->dev, family, 0, &p->queue);
    *(void **)p->queue = *(void **)p->dev;

    p->ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    p->ci.surface = p->surface;
    p->ci.imageArrayLayers = 1;
    p->ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    p->ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    p->ci.compositeAlpha = app->compositeAlpha;
    p->ci.presentMode = VK_PRESENT_MODE_FIFO_KHR; /* every driver has it; the application's own pace is kept by its waits */
    p->ci.clipped = VK_TRUE;
    if ((r = screen_swapchain(p)) != VK_SUCCESS) {
        zss_dbg("the screen's GPU did not make a swapchain for the window (VkResult %d); presenting directly", r);
        goto fail;
    }
    {
        VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family };
        VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = MAXI };
        VkFenceCreateInfo signaled = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
        VkFenceCreateInfo plain = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = p->size,
                                   .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
        VkPhysicalDeviceMemoryProperties mp;
        VkMemoryRequirements req;
        uint32_t t;

        if (p->fn.CreateCommandPool(p->dev, &pci, NULL, &p->pool) != VK_SUCCESS)
            goto fail;
        cai.commandPool = p->pool;
        if (p->fn.AllocateCommandBuffers(p->dev, &cai, p->cb) != VK_SUCCESS)
            goto fail;
        for (uint32_t i = 0; i < MAXI; i++) {
            *(void **)p->cb[i] = *(void **)p->dev;
            if (p->fn.CreateFence(p->dev, &signaled, NULL, &p->done[i]) != VK_SUCCESS ||
                p->fn.CreateSemaphore(p->dev, &sci, NULL, &p->ready[i]) != VK_SUCCESS)
                goto fail;
        }
        if (p->fn.CreateFence(p->dev, &plain, NULL, &p->acquired) != VK_SUCCESS ||
            p->fn.CreateFence(p->dev, &signaled, NULL, &p->uploaded) != VK_SUCCESS ||
            p->fn.CreateBuffer(p->dev, &bci, NULL, &p->up) != VK_SUCCESS)
            goto fail;
        p->fn.GetBufferMemoryRequirements(p->dev, p->up, &req);
        ifn->GetPhysicalDeviceMemoryProperties(pd, &mp);
        t = mem_type(&mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (t == UINT32_MAX)
            goto fail;
        {
            VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                        .memoryTypeIndex = t };

            if (p->fn.AllocateMemory(p->dev, &ai, NULL, &p->upmem) != VK_SUCCESS ||
                p->fn.BindBufferMemory(p->dev, p->up, p->upmem, 0) != VK_SUCCESS ||
                p->fn.MapMemory(p->dev, p->upmem, 0, VK_WHOLE_SIZE, 0, &p->upmap) != VK_SUCCESS)
                goto fail;
        }
    }
    p->owner = dev;
    zss_dbg("presenting a %ux%u window through %s (drawn on %s)", app->imageExtent.width, app->imageExtent.height,
            g->props.deviceName, dev->gpu->props.deviceName);
    return p;
fail:
    zss_presenter_destroy(p, true);
    return NULL;
}

/*
 * Shows the application's image `img` (a stand-in on its device, left by the
 * application in the present layout) once `waits` have fired. `q` is the
 * queue the application presents on. VK_SUBOPTIMAL_KHR when the window is no
 * longer the swapchain's size.
 */
VkResult zss_presenter_show(struct zss_presenter *p, struct zss_dev *dev, struct zss_queue *q, struct zss_obj *img,
                            const VkSemaphore *waits, uint32_t nwaits)
{
    VkPipelineStageFlags stages[16];
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .subresourceRange = all };
    VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 } };
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    uint32_t real_family = dev->fam_map[q->family], idx = 0, w, h;
    bool suboptimal = false;
    VkResult r;

    if (nwaits > 16)
        nwaits = 16;
    if (p->given_up)
        return VK_ERROR_OUT_OF_DATE_KHR;
    if ((r = drawing_side(p, dev, real_family)) != VK_SUCCESS)
        return r;

    /* 1. Read the finished frame back, on the application's GPU, after what it waits for. */
    dev->fn.ResetFences(p->rdev, 1, &p->rfence);
    dev->fn.BeginCommandBuffer(p->rcb, &bi);
    b.image = (VkImage)(uintptr_t)img->r.h;
    b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    dev->fn.CmdPipelineBarrier(p->rcb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                               NULL, 1, &b);
    region.imageExtent = (VkExtent3D){ p->extent.width, p->extent.height, 1 };
    dev->fn.CmdCopyImageToBuffer(p->rcb, b.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p->down, 1, &region);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = 0;
    dev->fn.CmdPipelineBarrier(p->rcb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL,
                               0, NULL, 1, &b);
    {
        VkBufferMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                     .dstAccessMask = VK_ACCESS_HOST_READ_BIT, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = p->down, .size = VK_WHOLE_SIZE };

        dev->fn.CmdPipelineBarrier(p->rcb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &hb,
                                   0, NULL);
    }
    dev->fn.EndCommandBuffer(p->rcb);
    for (uint32_t i = 0; i < nwaits; i++)
        stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    si.waitSemaphoreCount = nwaits;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = stages;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &p->rcb;
    if ((r = dev->fn.QueueSubmit(q->real, 1, &si, p->rfence)) != VK_SUCCESS)
        return r;
    if ((r = dev->fn.WaitForFences(p->rdev, 1, &p->rfence, VK_TRUE, 5000000000ull)) != VK_SUCCESS)
        return r == VK_TIMEOUT ? VK_ERROR_DEVICE_LOST : r;
    if (!p->down_coherent) {
        VkMappedMemoryRange mr = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = p->downmem, .size = VK_WHOLE_SIZE };

        dev->fn.InvalidateMappedMemoryRanges(p->rdev, 1, &mr);
    }

    /* 2. Into an image of the screen's swapchain, and shown. */
    for (int tries = 0;; tries++) {
        p->fn.ResetFences(p->dev, 1, &p->acquired);
        r = p->sc ? p->fn.AcquireNextImageKHR(p->dev, p->sc, 1000000000ull, VK_NULL_HANDLE, p->acquired, &idx)
                  : VK_ERROR_OUT_OF_DATE_KHR;
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            p->fn.WaitForFences(p->dev, 1, &p->acquired, VK_TRUE, 1000000000ull);
            break;
        }
        zss_dbg("the screen's GPU gave no image to present into (VkResult %d)", r);
        /* The window changed size: the screen's swapchain follows it; the application is told to, too. */
        if (r != VK_ERROR_OUT_OF_DATE_KHR || tries > 1 || screen_swapchain(p) != VK_SUCCESS)
            return VK_SUCCESS; /* a frame not shown is no error the application could act on */
        suboptimal = true;
    }
    p->fn.WaitForFences(p->dev, 1, &p->uploaded, VK_TRUE, 1000000000ull);
    p->fn.WaitForFences(p->dev, 1, &p->done[idx], VK_TRUE, 1000000000ull);
    memcpy(p->upmap, p->downmap, (size_t)p->size);

    w = p->extent.width < p->ci.imageExtent.width ? p->extent.width : p->ci.imageExtent.width;
    h = p->extent.height < p->ci.imageExtent.height ? p->extent.height : p->ci.imageExtent.height;
    if (w != p->ci.imageExtent.width || h != p->ci.imageExtent.height || w != p->extent.width || h != p->extent.height)
        suboptimal = true;
    p->fn.BeginCommandBuffer(p->cb[idx], &bi);
    b.image = p->images[idx];
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    p->fn.CmdPipelineBarrier(p->cb[idx], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &b);
    region.bufferRowLength = p->extent.width;
    region.imageExtent = (VkExtent3D){ w, h, 1 };
    p->fn.CmdCopyBufferToImage(p->cb[idx], p->up, b.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    p->fn.CmdPipelineBarrier(p->cb[idx], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL,
                             0, NULL, 1, &b);
    p->fn.EndCommandBuffer(p->cb[idx]);
    {
        VkSubmitInfo s2 = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &p->cb[idx],
                            .signalSemaphoreCount = 1, .pSignalSemaphores = &p->ready[idx] };
        VkFence both[2] = { p->done[idx], p->uploaded };
        VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                .pWaitSemaphores = &p->ready[idx], .swapchainCount = 1, .pSwapchains = &p->sc,
                                .pImageIndices = &idx };

        p->fn.ResetFences(p->dev, 2, both);
        if (p->fn.QueueSubmit(p->queue, 1, &s2, p->done[idx]) != VK_SUCCESS)
            return VK_SUCCESS;
        /* `uploaded` follows the same work: an empty submission after it, on the same queue. */
        {
            VkSubmitInfo s3 = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };

            p->fn.QueueSubmit(p->queue, 1, &s3, p->uploaded);
        }
        r = p->fn.QueuePresentKHR(p->queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
            suboptimal = true;
        else if (r != VK_SUCCESS)
            zss_dbg("the screen's GPU did not present (VkResult %d)", r);
    }
    return suboptimal ? VK_SUBOPTIMAL_KHR : VK_SUCCESS;
}

/*
 * A newer swapchain is being made for the same window: this one lets go of
 * the window first (a window has one swapchain at a time on a device, and the
 * newer presenter is on a device of its own).
 */
void zss_presenter_give_up(struct zss_presenter *p)
{
    if (!p || !p->dev || !p->sc)
        return;
    p->fn.DeviceWaitIdle(p->dev);
    p->fn.DestroySwapchainKHR(p->dev, p->sc, NULL);
    p->sc = VK_NULL_HANDLE;
    p->given_up = true;
}

