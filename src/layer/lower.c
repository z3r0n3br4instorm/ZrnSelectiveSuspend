// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dynamic rendering on a driver that has none (the NVIDIA 470 driver).
 *
 * Mesa's Zink requires VK_KHR_dynamic_rendering, so the layer offers it on
 * every GPU and, where the driver lacks it, lowers it to what every driver
 * has: a render pass and a framebuffer for each distinct rendering, made the
 * first time it is replayed and kept in a cache, and for each pipeline a
 * render pass compatible with the formats it was made for.
 *
 * Render passes are compatible when their attachments' formats and sample
 * counts match; load and store operations and layouts do not matter for
 * that. So a pipeline gets one with those alone, and a rendering gets one
 * with its own operations and layouts, and the two work together.
 *
 * The cache belongs to the real device and goes with it. A framebuffer names
 * real image views, so it goes when one of its views does.
 */
#include "zss_layer.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MAX_COLOR 8

/* What a render pass is made from; compared byte for byte, so always zeroed first. */
struct rp_key {
    uint32_t ncolor;
    bool has_depth, has_stencil, secondary;
    struct {
        VkFormat format;
        VkSampleCountFlagBits samples;
        VkAttachmentLoadOp load;
        VkAttachmentStoreOp store;
        VkImageLayout layout;
        VkFormat resolve_format;   /* VK_FORMAT_UNDEFINED: no resolve */
        VkImageLayout resolve_layout;
    } a[MAX_COLOR + 1]; /* colours, then the depth/stencil attachment */
};

struct fb_key {
    VkRenderPass rp;
    VkImageView views[2 * MAX_COLOR + 2];
    uint32_t width, height, layers;
};

struct lower_cache {
    pthread_mutex_t lock;
    struct { struct rp_key key; VkRenderPass rp; } *rps;
    uint32_t nrps;
    struct { struct fb_key key; VkFramebuffer fb; } *fbs;
    uint32_t nfbs;
};

static struct lower_cache *cache_of(struct zss_dev *dev)
{
    if (!dev->lower) {
        struct lower_cache *c = calloc(1, sizeof(*c));

        pthread_mutex_init(&c->lock, NULL);
        dev->lower = c;
    }
    return dev->lower;
}

static VkRenderPass render_pass(struct zss_dev *dev, const struct rp_key *k)
{
    struct lower_cache *c = cache_of(dev);
    VkAttachmentDescription att[2 * MAX_COLOR + 1];
    VkAttachmentReference color[MAX_COLOR], resolve[MAX_COLOR], ds;
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS };
    VkRenderPassCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    /* Dynamic rendering waits for nothing and makes nothing wait; the application's barriers do that. */
    VkRenderPass rp = VK_NULL_HANDLE;
    uint32_t n = 0;
    bool any_resolve = false;

    pthread_mutex_lock(&c->lock);
    for (uint32_t i = 0; i < c->nrps; i++)
        if (!memcmp(&c->rps[i].key, k, sizeof(*k))) {
            rp = c->rps[i].rp;
            pthread_mutex_unlock(&c->lock);
            return rp;
        }
    for (uint32_t i = 0; i < k->ncolor; i++) {
        att[n] = (VkAttachmentDescription){ 0, k->a[i].format, k->a[i].samples, k->a[i].load, k->a[i].store,
                                            VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                            k->a[i].layout, k->a[i].layout };
        /* An unused slot (VK_FORMAT_UNDEFINED) stays unused. */
        color[i] = (VkAttachmentReference){ k->a[i].format ? n++ : VK_ATTACHMENT_UNUSED, k->a[i].layout };
        resolve[i] = (VkAttachmentReference){ VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED };
    }
    for (uint32_t i = 0; i < k->ncolor; i++) {
        if (!k->a[i].resolve_format)
            continue;
        att[n] = (VkAttachmentDescription){ 0, k->a[i].resolve_format, VK_SAMPLE_COUNT_1_BIT,
                                            VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE,
                                            VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                            k->a[i].resolve_layout, k->a[i].resolve_layout };
        resolve[i] = (VkAttachmentReference){ n++, k->a[i].resolve_layout };
        any_resolve = true;
    }
    if (k->has_depth || k->has_stencil) {
        const typeof(k->a[0]) *d = &k->a[MAX_COLOR];

        att[n] = (VkAttachmentDescription){ 0, d->format, d->samples,
                                            k->has_depth ? d->load : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                            k->has_depth ? d->store : VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                            k->has_stencil ? d->load : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                            k->has_stencil ? d->store : VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                            d->layout, d->layout };
        ds = (VkAttachmentReference){ n++, d->layout };
        sub.pDepthStencilAttachment = &ds;
    }
    sub.colorAttachmentCount = k->ncolor;
    sub.pColorAttachments = color;
    sub.pResolveAttachments = any_resolve ? resolve : NULL;
    ci.attachmentCount = n;
    ci.pAttachments = att;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    if (dev->fn.CreateRenderPass(dev->real, &ci, NULL, &rp) == VK_SUCCESS) {
        c->rps = realloc(c->rps, (c->nrps + 1) * sizeof(*c->rps));
        c->rps[c->nrps].key = *k;
        c->rps[c->nrps++].rp = rp;
    }
    pthread_mutex_unlock(&c->lock);
    return rp;
}

static VkFramebuffer framebuffer(struct zss_dev *dev, const struct fb_key *k, uint32_t nviews)
{
    struct lower_cache *c = cache_of(dev);
    VkFramebufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = k->rp,
                                   .attachmentCount = nviews, .pAttachments = k->views,
                                   .width = k->width, .height = k->height, .layers = k->layers };
    VkFramebuffer fb = VK_NULL_HANDLE;

    pthread_mutex_lock(&c->lock);
    for (uint32_t i = 0; i < c->nfbs; i++)
        if (!memcmp(&c->fbs[i].key, k, sizeof(*k))) {
            fb = c->fbs[i].fb;
            pthread_mutex_unlock(&c->lock);
            return fb;
        }
    if (dev->fn.CreateFramebuffer(dev->real, &ci, NULL, &fb) == VK_SUCCESS) {
        c->fbs = realloc(c->fbs, (c->nfbs + 1) * sizeof(*c->fbs));
        c->fbs[c->nfbs].key = *k;
        c->fbs[c->nfbs++].fb = fb;
    }
    pthread_mutex_unlock(&c->lock);
    return fb;
}

/* A layer image view's format and sample count. */
static void view_info(VkImageView v, VkFormat *format, VkSampleCountFlagBits *samples)
{
    struct zss_obj *view = ZOBJ(v);

    *format = view->u.view.ci.format;
    *samples = ZOBJ(view->u.view.ci.image)->u.img.ci.samples;
}

/*
 * vkCmdBeginRendering, lowered. `ri` holds the layer's handles (as kept by
 * cmd.c); the real ones are looked up here.
 */
void zss_lower_begin_rendering(struct zss_dev *dev, VkCommandBuffer cb, const VkRenderingInfo *ri)
{
    struct rp_key k;
    struct fb_key f;
    VkClearValue clears[2 * MAX_COLOR + 1];
    VkRenderPassBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderArea = ri->renderArea };
    const VkRenderingAttachmentInfo *dsa = ri->pDepthAttachment && ri->pDepthAttachment->imageView ? ri->pDepthAttachment
                                         : ri->pStencilAttachment && ri->pStencilAttachment->imageView ? ri->pStencilAttachment
                                         : NULL;
    uint32_t nviews = 0, nclear = 0;

    memset(&k, 0, sizeof(k));
    memset(&f, 0, sizeof(f));
    k.ncolor = ri->colorAttachmentCount < MAX_COLOR ? ri->colorAttachmentCount : MAX_COLOR;
    k.secondary = (ri->flags & VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT) != 0;
    /* Attachment numbers follow render_pass(): colours, then resolves, then depth/stencil. */
    for (uint32_t i = 0; i < k.ncolor; i++) {
        const VkRenderingAttachmentInfo *a = &ri->pColorAttachments[i];

        if (!a->imageView)
            continue;
        view_info(a->imageView, &k.a[i].format, &k.a[i].samples);
        k.a[i].load = a->loadOp;
        k.a[i].store = a->storeOp;
        k.a[i].layout = a->imageLayout;
        f.views[nviews] = (VkImageView)(uintptr_t)ZOBJ(a->imageView)->r.h;
        clears[nviews++] = a->clearValue;
    }
    nclear = nviews;
    for (uint32_t i = 0; i < k.ncolor; i++) {
        const VkRenderingAttachmentInfo *a = &ri->pColorAttachments[i];
        VkSampleCountFlagBits s;

        if (!a->imageView || a->resolveMode == VK_RESOLVE_MODE_NONE || !a->resolveImageView)
            continue;
        view_info(a->resolveImageView, &k.a[i].resolve_format, &s);
        k.a[i].resolve_layout = a->resolveImageLayout;
        f.views[nviews] = (VkImageView)(uintptr_t)ZOBJ(a->resolveImageView)->r.h;
        clears[nviews++] = (VkClearValue){ 0 };
    }
    if (dsa) {
        k.has_depth = ri->pDepthAttachment && ri->pDepthAttachment->imageView;
        k.has_stencil = ri->pStencilAttachment && ri->pStencilAttachment->imageView;
        view_info(dsa->imageView, &k.a[MAX_COLOR].format, &k.a[MAX_COLOR].samples);
        k.a[MAX_COLOR].load = dsa->loadOp;
        k.a[MAX_COLOR].store = dsa->storeOp;
        k.a[MAX_COLOR].layout = dsa->imageLayout;
        f.views[nviews] = (VkImageView)(uintptr_t)ZOBJ(dsa->imageView)->r.h;
        /* Depth and stencil share one attachment here; each keeps its own clear value. */
        clears[nviews] = dsa->clearValue;
        if (k.has_depth && k.has_stencil)
            clears[nviews].depthStencil.stencil = ri->pStencilAttachment->clearValue.depthStencil.stencil;
        nviews++;
    }
    nclear = nviews;
    f.rp = render_pass(dev, &k);
    f.width = (uint32_t)ri->renderArea.offset.x + ri->renderArea.extent.width;
    f.height = (uint32_t)ri->renderArea.offset.y + ri->renderArea.extent.height;
    f.layers = ri->layerCount ? ri->layerCount : 1;
    bi.renderPass = f.rp;
    bi.framebuffer = f.rp ? framebuffer(dev, &f, nviews) : VK_NULL_HANDLE;
    bi.clearValueCount = nclear;
    bi.pClearValues = clears;
    if (!bi.renderPass || !bi.framebuffer) {
        zss_log("could not stand in for a dynamic render pass (%u attachment(s)); it is skipped", nviews);
        return;
    }
    dev->fn.CmdBeginRenderPass(cb, &bi, k.secondary ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS
                                                     : VK_SUBPASS_CONTENTS_INLINE);
}

/* A render pass compatible with what a pipeline (or a secondary buffer) was made for. */
VkRenderPass zss_lower_compatible(struct zss_dev *dev, uint32_t ncolor, const VkFormat *colors, VkFormat depth,
                                  VkFormat stencil, VkSampleCountFlagBits samples)
{
    struct rp_key k;

    memset(&k, 0, sizeof(k));
    k.ncolor = ncolor < MAX_COLOR ? ncolor : MAX_COLOR;
    for (uint32_t i = 0; i < k.ncolor; i++) {
        k.a[i].format = colors[i];
        k.a[i].samples = samples;
        k.a[i].load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        k.a[i].store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        k.a[i].layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    if (depth || stencil) {
        k.has_depth = depth != VK_FORMAT_UNDEFINED;
        k.has_stencil = stencil != VK_FORMAT_UNDEFINED;
        k.a[MAX_COLOR].format = depth ? depth : stencil;
        k.a[MAX_COLOR].samples = samples;
        k.a[MAX_COLOR].load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        k.a[MAX_COLOR].store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        k.a[MAX_COLOR].layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    return render_pass(dev, &k);
}

/* A real image view is going: framebuffers naming it go first. */
void zss_lower_forget_view(struct zss_dev *dev, VkImageView view)
{
    struct lower_cache *c = dev->lower;

    if (!c || !view)
        return;
    pthread_mutex_lock(&c->lock);
    for (uint32_t i = 0; i < c->nfbs;) {
        bool hit = false;

        for (uint32_t j = 0; j < 2 * MAX_COLOR + 2; j++)
            hit = hit || c->fbs[i].key.views[j] == view;
        if (hit) {
            dev->fn.DestroyFramebuffer(dev->real, c->fbs[i].fb, NULL);
            c->fbs[i] = c->fbs[--c->nfbs];
        } else {
            i++;
        }
    }
    pthread_mutex_unlock(&c->lock);
}

/* The real device is abandoned without a call into its driver: only the layer's records go. */
void zss_lower_abandon(struct zss_dev *dev)
{
    struct lower_cache *c = dev->lower;

    if (!c)
        return;
    free(c->fbs);
    free(c->rps);
    pthread_mutex_destroy(&c->lock);
    free(c);
    dev->lower = NULL;
}

/* The real device is going: so does everything made for it. */
void zss_lower_drop(struct zss_dev *dev)
{
    struct lower_cache *c = dev->lower;

    if (!c)
        return;
    for (uint32_t i = 0; i < c->nfbs; i++)
        dev->fn.DestroyFramebuffer(dev->real, c->fbs[i].fb, NULL);
    for (uint32_t i = 0; i < c->nrps; i++)
        dev->fn.DestroyRenderPass(dev->real, c->rps[i].rp, NULL);
    free(c->fbs);
    free(c->rps);
    pthread_mutex_destroy(&c->lock);
    free(c);
    dev->lower = NULL;
}
