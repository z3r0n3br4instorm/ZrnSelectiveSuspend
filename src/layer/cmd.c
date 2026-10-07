// SPDX-License-Identifier: GPL-2.0-only
/*
 * Command buffers. Every command is stored with the application's handles
 * and then executed through the same routine that replays it on another
 * device, so recording and replay cannot drift apart.
 */
#include "zss_layer.h"

#include <stdlib.h>
#include <string.h>

enum zss_op {
    OP_BIND_PIPELINE, OP_SET_VIEWPORT, OP_SET_SCISSOR, OP_SET_LINE_WIDTH, OP_SET_DEPTH_BIAS,
    OP_SET_BLEND_CONSTANTS, OP_SET_DEPTH_BOUNDS, OP_SET_STENCIL_COMPARE, OP_SET_STENCIL_WRITE,
    OP_SET_STENCIL_REF, OP_BIND_DSETS, OP_BIND_INDEX, OP_BIND_VERTEX, OP_DRAW, OP_DRAW_INDEXED,
    OP_DRAW_INDIRECT, OP_DRAW_INDEXED_INDIRECT, OP_COPY_BUFFER, OP_COPY_IMAGE, OP_BLIT_IMAGE,
    OP_COPY_BUFFER_TO_IMAGE, OP_COPY_IMAGE_TO_BUFFER, OP_UPDATE_BUFFER, OP_FILL_BUFFER,
    OP_CLEAR_COLOR, OP_CLEAR_DS, OP_CLEAR_ATTACHMENTS, OP_RESOLVE, OP_BARRIER, OP_PUSH_CONSTANTS,
    OP_BEGIN_RP, OP_NEXT_SUBPASS, OP_END_RP,
};

/* One uniform node; each op uses the fields it needs. */
struct zss_cmd {
    struct zss_cmd *next;
    enum zss_op op;
    uint32_t u[6];
    float f[4];
    struct zss_obj *h[3];
    VkDeviceSize s[3];
    uint32_t n[3];
    void *a[3];
    VkClearValue clear;
    VkRect2D area;
};

static struct zss_cmd *rec(struct zss_obj *cb, enum zss_op op)
{
    struct zss_cmd *c = calloc(1, sizeof(*c));

    c->op = op;
    if (cb->u.cb.tail)
        cb->u.cb.tail->next = c;
    else
        cb->u.cb.head = c;
    cb->u.cb.tail = c;
    return c;
}

/* Keeps an object alive for as long as the command buffer mentions it. */
static struct zss_obj *use(struct zss_obj *cb, uint64_t handle)
{
    struct zss_obj *o = (struct zss_obj *)(uintptr_t)handle;

    if (!o)
        return NULL;
    if (cb->u.cb.nrefs == cb->u.cb.caprefs) {
        cb->u.cb.caprefs = cb->u.cb.caprefs ? cb->u.cb.caprefs * 2 : 16;
        cb->u.cb.refs = realloc(cb->u.cb.refs, cb->u.cb.caprefs * sizeof(*cb->u.cb.refs));
    }
    zss_obj_ref(o);
    cb->u.cb.refs[cb->u.cb.nrefs++] = o;
    return o;
}

static void *dup(const void *src, size_t size)
{
    void *p;

    if (!src || !size)
        return NULL;
    p = malloc(size);
    memcpy(p, src, size);
    return p;
}

void zss_cmd_reset(struct zss_obj *cb)
{
    struct zss_cmd *c = cb->u.cb.head, *next;

    for (; c; c = next) {
        next = c->next;
        free(c->a[0]);
        free(c->a[1]);
        free(c->a[2]);
        free(c);
    }
    cb->u.cb.head = cb->u.cb.tail = NULL;
    cb->u.cb.version++;
    for (uint32_t i = 0; i < cb->u.cb.nrefs; i++)
        zss_obj_unref(cb->u.cb.refs[i]);
    cb->u.cb.nrefs = 0;
}

#define R(T, o) ((T)(uintptr_t)((o) ? (o)->r.h : 0))

static void exec(struct zss_dev *dev, VkCommandBuffer cb, const struct zss_cmd *c)
{
    const struct zss_dev_fns *fn = &dev->fn;

    switch (c->op) {
    case OP_BIND_PIPELINE:
        fn->CmdBindPipeline(cb, (VkPipelineBindPoint)c->u[0], R(VkPipeline, c->h[0]));
        break;
    case OP_SET_VIEWPORT:
        fn->CmdSetViewport(cb, c->u[0], c->n[0], c->a[0]);
        break;
    case OP_SET_SCISSOR:
        fn->CmdSetScissor(cb, c->u[0], c->n[0], c->a[0]);
        break;
    case OP_SET_LINE_WIDTH:
        fn->CmdSetLineWidth(cb, c->f[0]);
        break;
    case OP_SET_DEPTH_BIAS:
        fn->CmdSetDepthBias(cb, c->f[0], c->f[1], c->f[2]);
        break;
    case OP_SET_BLEND_CONSTANTS:
        fn->CmdSetBlendConstants(cb, c->f);
        break;
    case OP_SET_DEPTH_BOUNDS:
        fn->CmdSetDepthBounds(cb, c->f[0], c->f[1]);
        break;
    case OP_SET_STENCIL_COMPARE:
        fn->CmdSetStencilCompareMask(cb, c->u[0], c->u[1]);
        break;
    case OP_SET_STENCIL_WRITE:
        fn->CmdSetStencilWriteMask(cb, c->u[0], c->u[1]);
        break;
    case OP_SET_STENCIL_REF:
        fn->CmdSetStencilReference(cb, c->u[0], c->u[1]);
        break;
    case OP_BIND_DSETS: {
        struct zss_obj **sets = c->a[0];
        VkDescriptorSet real[32];

        for (uint32_t i = 0; i < c->n[0] && i < 32; i++)
            real[i] = R(VkDescriptorSet, sets[i]);
        fn->CmdBindDescriptorSets(cb, (VkPipelineBindPoint)c->u[0], R(VkPipelineLayout, c->h[0]),
                                  c->u[1], c->n[0], real, c->n[1], c->a[1]);
        break;
    }
    case OP_BIND_INDEX:
        fn->CmdBindIndexBuffer(cb, R(VkBuffer, c->h[0]), c->s[0], (VkIndexType)c->u[0]);
        break;
    case OP_BIND_VERTEX: {
        struct zss_obj **bufs = c->a[0];
        VkBuffer real[32];

        for (uint32_t i = 0; i < c->n[0] && i < 32; i++)
            real[i] = R(VkBuffer, bufs[i]);
        fn->CmdBindVertexBuffers(cb, c->u[0], c->n[0], real, c->a[1]);
        break;
    }
    case OP_DRAW:
        fn->CmdDraw(cb, c->u[0], c->u[1], c->u[2], c->u[3]);
        break;
    case OP_DRAW_INDEXED:
        fn->CmdDrawIndexed(cb, c->u[0], c->u[1], c->u[2], (int32_t)c->u[3], c->u[4]);
        break;
    case OP_DRAW_INDIRECT:
        fn->CmdDrawIndirect(cb, R(VkBuffer, c->h[0]), c->s[0], c->u[0], c->u[1]);
        break;
    case OP_DRAW_INDEXED_INDIRECT:
        fn->CmdDrawIndexedIndirect(cb, R(VkBuffer, c->h[0]), c->s[0], c->u[0], c->u[1]);
        break;
    case OP_COPY_BUFFER:
        fn->CmdCopyBuffer(cb, R(VkBuffer, c->h[0]), R(VkBuffer, c->h[1]), c->n[0], c->a[0]);
        break;
    case OP_COPY_IMAGE:
        fn->CmdCopyImage(cb, R(VkImage, c->h[0]), (VkImageLayout)c->u[0], R(VkImage, c->h[1]),
                         (VkImageLayout)c->u[1], c->n[0], c->a[0]);
        break;
    case OP_BLIT_IMAGE:
        fn->CmdBlitImage(cb, R(VkImage, c->h[0]), (VkImageLayout)c->u[0], R(VkImage, c->h[1]),
                         (VkImageLayout)c->u[1], c->n[0], c->a[0], (VkFilter)c->u[2]);
        break;
    case OP_COPY_BUFFER_TO_IMAGE:
        fn->CmdCopyBufferToImage(cb, R(VkBuffer, c->h[0]), R(VkImage, c->h[1]),
                                 (VkImageLayout)c->u[0], c->n[0], c->a[0]);
        break;
    case OP_COPY_IMAGE_TO_BUFFER:
        fn->CmdCopyImageToBuffer(cb, R(VkImage, c->h[0]), (VkImageLayout)c->u[0],
                                 R(VkBuffer, c->h[1]), c->n[0], c->a[0]);
        break;
    case OP_UPDATE_BUFFER:
        fn->CmdUpdateBuffer(cb, R(VkBuffer, c->h[0]), c->s[0], c->s[1], c->a[0]);
        break;
    case OP_FILL_BUFFER:
        fn->CmdFillBuffer(cb, R(VkBuffer, c->h[0]), c->s[0], c->s[1], c->u[0]);
        break;
    case OP_CLEAR_COLOR:
        fn->CmdClearColorImage(cb, R(VkImage, c->h[0]), (VkImageLayout)c->u[0], &c->clear.color,
                               c->n[0], c->a[0]);
        break;
    case OP_CLEAR_DS:
        fn->CmdClearDepthStencilImage(cb, R(VkImage, c->h[0]), (VkImageLayout)c->u[0],
                                      &c->clear.depthStencil, c->n[0], c->a[0]);
        break;
    case OP_CLEAR_ATTACHMENTS:
        fn->CmdClearAttachments(cb, c->n[0], c->a[0], c->n[1], c->a[1]);
        break;
    case OP_RESOLVE:
        fn->CmdResolveImage(cb, R(VkImage, c->h[0]), (VkImageLayout)c->u[0], R(VkImage, c->h[1]),
                            (VkImageLayout)c->u[1], c->n[0], c->a[0]);
        break;
    case OP_BARRIER: {
        VkBufferMemoryBarrier *bb = dup(c->a[1], c->n[1] * sizeof(*bb));
        VkImageMemoryBarrier *ib = dup(c->a[2], c->n[2] * sizeof(*ib));

        for (uint32_t i = 0; i < c->n[1]; i++)
            bb[i].buffer = ZREAL(VkBuffer, bb[i].buffer);
        for (uint32_t i = 0; i < c->n[2]; i++)
            ib[i].image = ZREAL(VkImage, ib[i].image);
        fn->CmdPipelineBarrier(cb, c->u[0], c->u[1], c->u[2], c->n[0], c->a[0], c->n[1], bb,
                               c->n[2], ib);
        free(bb);
        free(ib);
        break;
    }
    case OP_PUSH_CONSTANTS:
        fn->CmdPushConstants(cb, R(VkPipelineLayout, c->h[0]), c->u[0], c->u[1], c->u[2], c->a[0]);
        break;
    case OP_BEGIN_RP: {
        VkRenderPassBeginInfo bi = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = R(VkRenderPass, c->h[0]),
            .framebuffer = R(VkFramebuffer, c->h[1]),
            .renderArea = c->area,
            .clearValueCount = c->n[0],
            .pClearValues = c->a[0],
        };

        fn->CmdBeginRenderPass(cb, &bi, (VkSubpassContents)c->u[0]);
        break;
    }
    case OP_NEXT_SUBPASS:
        fn->CmdNextSubpass(cb, (VkSubpassContents)c->u[0]);
        break;
    case OP_END_RP:
        fn->CmdEndRenderPass(cb);
        break;
    }
}

static bool refs_alive(const struct zss_obj *cb)
{
    for (uint32_t i = 0; i < cb->u.cb.nrefs; i++)
        if (!cb->u.cb.refs[i]->r.h && cb->u.cb.refs[i]->kind != ZK_MEMORY)
            return false;
    return true;
}

void zss_cmd_replay(struct zss_dev *dev, struct zss_obj *cb)
{
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = cb->u.cb.usage,
    };
    VkCommandBuffer real = (VkCommandBuffer)(uintptr_t)cb->r.h;

    if (!real || (cb->u.cb.state != ZC_RECORDING && cb->u.cb.state != ZC_EXECUTABLE))
        return;
    /* A buffer that mentions something since destroyed can never be run again. */
    if (!refs_alive(cb)) {
        cb->u.cb.state = ZC_INVALID;
        return;
    }
    dev->fn.BeginCommandBuffer(real, &bi);
    for (const struct zss_cmd *c = cb->u.cb.head; c; c = c->next)
        exec(dev, real, c);
    if (cb->u.cb.state == ZC_EXECUTABLE)
        dev->fn.EndCommandBuffer(real);
}

static void set_layout(struct zss_obj *img, const VkImageSubresourceRange *r, VkImageLayout layout)
{
    const VkImageCreateInfo *ci = &img->u.img.ci;
    uint32_t mips = r->levelCount == VK_REMAINING_MIP_LEVELS ? ci->mipLevels - r->baseMipLevel : r->levelCount;
    uint32_t layers = r->layerCount == VK_REMAINING_ARRAY_LAYERS ? ci->arrayLayers - r->baseArrayLayer : r->layerCount;

    for (uint32_t m = r->baseMipLevel; m < r->baseMipLevel + mips && m < ci->mipLevels; m++)
        for (uint32_t l = r->baseArrayLayer; l < r->baseArrayLayer + layers && l < ci->arrayLayers; l++)
            img->u.img.layout[m * ci->arrayLayers + l] = layout;
}

/* ---- what a submit does to contents ------------------------------------------- */
/*
 * After a device is lost nothing can be read back from it, so the layer
 * keeps track, submit by submit, of where each buffer's and image's contents
 * came from: an upload it can retain, or the GPU itself.
 */

static uint32_t ret_index(const VkImageCreateInfo *ci, uint32_t mip, uint32_t layer, VkImageAspectFlags aspect)
{
    return (mip * ci->arrayLayers + layer) * 2 + (aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? 1 : 0);
}

/* The GPU is about to produce this image's contents; uploads no longer describe it. */
void zss_image_dirty(struct zss_obj *img, bool carried)
{
    const VkImageCreateInfo *ci = &img->u.img.ci;

    if (img->kind != ZK_IMAGE)
        return;
    if (img->u.img.ret)
        for (uint32_t i = 0; i < 2 * ci->mipLevels * ci->arrayLayers; i++)
            zss_retain_release(&img->u.img.ret[i]);
    img->u.img.unretained = false;
    if (carried)
        img->u.img.carried = true;
}

static void buffer_dirty(struct zss_obj *buf)
{
    zss_retain_release(&buf->u.buf.ret);
    buf->u.buf.gpu_written = true;
    buf->u.buf.filled = true;
}

/*
 * Bytes of a buffer as the application supplied them: from the shadow of its
 * mapped memory, or from an upload retained for it. NULL if neither exists.
 * *tmp receives an allocation the caller frees.
 */
static const uint8_t *buffer_bytes(const struct zss_obj *buf, VkDeviceSize off, VkDeviceSize size, uint8_t **tmp)
{
    const struct zss_obj *m = buf->u.buf.mem;

    *tmp = NULL;
    if (off + size > buf->u.buf.ci.size)
        return NULL;
    if (m && m->u.mem.shadow && buf->u.buf.mem_off + off + size <= m->u.mem.size)
        return m->u.mem.shadow + buf->u.buf.mem_off + off;
    if (buf->u.buf.ret.valid && off + size <= buf->u.buf.ret.size) {
        *tmp = malloc(buf->u.buf.ret.size);
        if (zss_retain_get(&buf->u.buf.ret, *tmp))
            return *tmp + off;
    }
    return NULL;
}

static void retain_image_upload(struct zss_obj *src, struct zss_obj *img, const VkBufferImageCopy *regions,
                                uint32_t n)
{
    const VkImageCreateInfo *ci = &img->u.img.ci;

    if (!img->u.img.ret)
        return;
    for (uint32_t i = 0; i < n; i++) {
        const VkBufferImageCopy *r = &regions[i];
        VkImageAspectFlags aspect = r->imageSubresource.aspectMask;
        uint32_t mip = r->imageSubresource.mipLevel, layers = r->imageSubresource.layerCount;
        uint32_t w = ci->extent.width >> mip, h = ci->extent.height >> mip, d = ci->extent.depth >> mip;
        VkDeviceSize sub = zss_sub_size(ci, aspect, mip);
        const uint8_t *bytes = NULL;
        uint8_t *tmp = NULL;
        /* Only whole, tightly packed subresources are kept; anything else is rare and fiddly. */
        bool whole = sub && !r->imageOffset.x && !r->imageOffset.y && !r->imageOffset.z &&
                     r->imageExtent.width == (w ? w : 1) && r->imageExtent.height == (h ? h : 1) &&
                     r->imageExtent.depth == (d ? d : 1) &&
                     (!r->bufferRowLength || r->bufferRowLength == r->imageExtent.width) &&
                     (!r->bufferImageHeight || r->bufferImageHeight == r->imageExtent.height);

        if (mip >= ci->mipLevels)
            continue;
        if (whole)
            bytes = buffer_bytes(src, r->bufferOffset, sub * layers, &tmp);
        for (uint32_t l = 0; l < layers && r->imageSubresource.baseArrayLayer + l < ci->arrayLayers; l++) {
            struct zss_ret *ret = &img->u.img.ret[ret_index(ci, mip, r->imageSubresource.baseArrayLayer + l, aspect)];

            zss_retain_release(ret);
            if (!bytes || !zss_retain_put(bytes + l * sub, sub, ret))
                img->u.img.unretained = true;
        }
        free(tmp);
    }
}

/* A copy into a buffer the layer cannot read: keep the bytes if it replaces the whole buffer. */
static void retain_buffer_copy(struct zss_obj *src, struct zss_obj *dst, const VkBufferCopy *regions, uint32_t n)
{
    const uint8_t *bytes;
    uint8_t *tmp = NULL;

    dst->u.buf.gpu_written = true;
    if (dst->u.buf.mem && dst->u.buf.mem->u.mem.shadow)
        return; /* its shadow is refreshed from the device after the next wait */
    zss_retain_release(&dst->u.buf.ret);
    dst->u.buf.filled = true;
    if (n != 1 || regions[0].dstOffset != 0 || regions[0].size != dst->u.buf.ci.size)
        return;
    bytes = buffer_bytes(src, regions[0].srcOffset, regions[0].size, &tmp);
    if (bytes && zss_retain_put(bytes, regions[0].size, &dst->u.buf.ret))
        dst->u.buf.filled = false;
    free(tmp);
}

/*
 * Called once a submit has been accepted: the command buffer's effects on
 * image layouts and on where contents come from are now real.
 */
void zss_cmd_track_submit(struct zss_dev *dev, struct zss_obj *cb)
{
    (void)dev;
    for (const struct zss_cmd *c = cb->u.cb.head; c; c = c->next) {
        switch (c->op) {
        case OP_BARRIER: {
            const VkImageMemoryBarrier *ib = c->a[2];

            for (uint32_t i = 0; i < c->n[2]; i++)
                if (ib[i].image)
                    set_layout(ZOBJ(ib[i].image), &ib[i].subresourceRange, ib[i].newLayout);
            break;
        }
        case OP_BEGIN_RP: {
            const VkRenderPassCreateInfo *rp = &c->h[0]->u.rp.ci;
            const VkFramebufferCreateInfo *fb = &c->h[1]->u.fb.ci;

            for (uint32_t i = 0; i < fb->attachmentCount && i < rp->attachmentCount; i++) {
                struct zss_obj *view = ZOBJ(fb->pAttachments[i]);
                const VkAttachmentDescription *att = &rp->pAttachments[i];
                struct zss_obj *img;

                if (!view || !view->u.view.ci.image)
                    continue;
                img = ZOBJ(view->u.view.ci.image);
                set_layout(img, &view->u.view.ci.subresourceRange, att->finalLayout);
                /* Loading means later frames build on what is there now. */
                zss_image_dirty(img, att->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD ||
                                         att->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
            }
            break;
        }
        case OP_COPY_BUFFER_TO_IMAGE:
            if (c->h[0] && c->h[1])
                retain_image_upload(c->h[0], c->h[1], c->a[0], c->n[0]);
            break;
        case OP_COPY_BUFFER:
            if (c->h[0] && c->h[1])
                retain_buffer_copy(c->h[0], c->h[1], c->a[0], c->n[0]);
            break;
        case OP_COPY_IMAGE_TO_BUFFER:
            if (c->h[1])
                buffer_dirty(c->h[1]);
            break;
        case OP_UPDATE_BUFFER:
            if (c->h[0]) {
                struct zss_obj *dst = c->h[0];

                buffer_dirty(dst);
                if (c->s[0] == 0 && c->s[1] == dst->u.buf.ci.size &&
                    zss_retain_put(c->a[0], c->s[1], &dst->u.buf.ret))
                    dst->u.buf.filled = false;
            }
            break;
        case OP_FILL_BUFFER:
            if (c->h[0])
                buffer_dirty(c->h[0]);
            break;
        case OP_COPY_IMAGE:
        case OP_BLIT_IMAGE:
        case OP_RESOLVE:
            if (c->h[1])
                zss_image_dirty(c->h[1], true);
            break;
        case OP_CLEAR_COLOR:
        case OP_CLEAR_DS:
            if (c->h[0])
                zss_image_dirty(c->h[0], true);
            break;
        default:
            break;
        }
    }
    /* Submitted once and never again: no reason to keep the recording. */
    if (cb->u.cb.usage & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT)
        cb->u.cb.state = ZC_INVALID;
}

/* ---- entry points ----------------------------------------------------------- */

#define CB(cmd) struct zss_obj *cb = (struct zss_obj *)(cmd); struct zss_dev *dev = cb->dev; zss_enter()
#define RUN(c) do { exec(dev, (VkCommandBuffer)(uintptr_t)cb->r.h, (c)); zss_leave(); } while (0)
#define REAL_CB ((VkCommandBuffer)(uintptr_t)cb->r.h)
#define H(x) ((uint64_t)(uintptr_t)(x))

VKAPI_ATTR VkResult VKAPI_CALL zss_BeginCommandBuffer(VkCommandBuffer cmd,
                                                      const VkCommandBufferBeginInfo *info)
{
    CB(cmd);
    VkCommandBufferBeginInfo bi = *info;
    VkCommandBufferInheritanceInfo inh;
    VkResult r;

    zss_cmd_reset(cb);
    cb->u.cb.usage = info->flags;
    /* Not "recording" until the driver agrees: a rebuild must not begin it a second time. */
    cb->u.cb.state = ZC_INITIAL;
    bi.pNext = NULL;
    if (cb->u.cb.level == VK_COMMAND_BUFFER_LEVEL_SECONDARY && info->pInheritanceInfo) {
        inh = *info->pInheritanceInfo;
        inh.renderPass = ZREAL(VkRenderPass, inh.renderPass);
        inh.framebuffer = ZREAL(VkFramebuffer, inh.framebuffer);
        bi.pInheritanceInfo = &inh;
    } else {
        bi.pInheritanceInfo = NULL;
    }
    ZSS_RETRY(dev, r, dev->fn.BeginCommandBuffer(REAL_CB, &bi));
    if (r == VK_SUCCESS)
        cb->u.cb.state = ZC_RECORDING;
    zss_leave();
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL zss_EndCommandBuffer(VkCommandBuffer cmd)
{
    CB(cmd);
    VkResult r;

    /* After a loss the rebuild has replayed the recording; only the end is repeated. */
    ZSS_RETRY(dev, r, dev->fn.EndCommandBuffer(REAL_CB));
    cb->u.cb.state = ZC_EXECUTABLE;
    zss_leave();
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL zss_ResetCommandBuffer(VkCommandBuffer cmd,
                                                      VkCommandBufferResetFlags flags)
{
    CB(cmd);
    VkResult r;

    ZSS_RETRY(dev, r, dev->fn.ResetCommandBuffer(REAL_CB, flags));
    zss_cmd_reset(cb);
    cb->u.cb.state = ZC_INITIAL;
    zss_leave();
    return r;
}

VKAPI_ATTR void VKAPI_CALL zss_CmdBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint bp,
                                               VkPipeline pipeline)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_BIND_PIPELINE);

    c->u[0] = bp;
    c->h[0] = use(cb, H(pipeline));
    RUN(c);
}

VKAPI_ATTR void VKAPI_CALL zss_CmdSetViewport(VkCommandBuffer cmd, uint32_t first, uint32_t n,
                                              const VkViewport *v)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_SET_VIEWPORT);

    c->u[0] = first;
    c->n[0] = n;
    c->a[0] = dup(v, n * sizeof(*v));
    RUN(c);
}

VKAPI_ATTR void VKAPI_CALL zss_CmdSetScissor(VkCommandBuffer cmd, uint32_t first, uint32_t n,
                                             const VkRect2D *s)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_SET_SCISSOR);

    c->u[0] = first;
    c->n[0] = n;
    c->a[0] = dup(s, n * sizeof(*s));
    RUN(c);
}

VKAPI_ATTR void VKAPI_CALL zss_CmdSetLineWidth(VkCommandBuffer cmd, float width)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_SET_LINE_WIDTH);

    c->f[0] = width;
    RUN(c);
}

VKAPI_ATTR void VKAPI_CALL zss_CmdSetDepthBias(VkCommandBuffer cmd, float constant, float clamp,
                                               float slope)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_SET_DEPTH_BIAS);

    c->f[0] = constant;
    c->f[1] = clamp;
    c->f[2] = slope;
    RUN(c);
}

VKAPI_ATTR void VKAPI_CALL zss_CmdSetBlendConstants(VkCommandBuffer cmd, const float k[4])
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_SET_BLEND_CONSTANTS);

    memcpy(c->f, k, sizeof(c->f));
    RUN(c);
}

VKAPI_ATTR void VKAPI_CALL zss_CmdSetDepthBounds(VkCommandBuffer cmd, float min, float max)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_SET_DEPTH_BOUNDS);

    c->f[0] = min;
    c->f[1] = max;
    RUN(c);
}

#define STENCIL_FN(Name, op) \
    VKAPI_ATTR void VKAPI_CALL zss_CmdSetStencil##Name(VkCommandBuffer cmd, \
                                                       VkStencilFaceFlags face, uint32_t value) \
    { \
        CB(cmd); \
        struct zss_cmd *c = rec(cb, op); \
        c->u[0] = face; \
        c->u[1] = value; \
        RUN(c); \
    }
STENCIL_FN(CompareMask, OP_SET_STENCIL_COMPARE)
STENCIL_FN(WriteMask, OP_SET_STENCIL_WRITE)
STENCIL_FN(Reference, OP_SET_STENCIL_REF)

VKAPI_ATTR void VKAPI_CALL zss_CmdBindDescriptorSets(VkCommandBuffer cmd, VkPipelineBindPoint bp,
                                                     VkPipelineLayout layout, uint32_t first,
                                                     uint32_t n, const VkDescriptorSet *sets,
                                                     uint32_t ndyn, const uint32_t *dyn)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_BIND_DSETS);
    struct zss_obj **list = calloc(n ? n : 1, sizeof(*list));

    c->u[0] = bp;
    c->u[1] = first;
    c->h[0] = use(cb, H(layout));
    c->n[0] = n;
    for (uint32_t i = 0; i < n; i++)
        list[i] = use(cb, H(sets[i]));
    c->a[0] = list;
    c->n[1] = ndyn;
    c->a[1] = dup(dyn, ndyn * sizeof(*dyn));
    RUN(c);
}

VKAPI_ATTR void VKAPI_CALL zss_CmdBindIndexBuffer(VkCommandBuffer cmd, VkBuffer buffer,
                                                  VkDeviceSize offset, VkIndexType type)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_BIND_INDEX);

    c->h[0] = use(cb, H(buffer));
    c->s[0] = offset;
    c->u[0] = type;
    RUN(c);
}

VKAPI_ATTR void VKAPI_CALL zss_CmdBindVertexBuffers(VkCommandBuffer cmd, uint32_t first, uint32_t n,
                                                    const VkBuffer *bufs, const VkDeviceSize *offs)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_BIND_VERTEX);
    struct zss_obj **list = calloc(n ? n : 1, sizeof(*list));

    c->u[0] = first;
    c->n[0] = n;
    for (uint32_t i = 0; i < n; i++)
        list[i] = use(cb, H(bufs[i]));
    c->a[0] = list;
    c->a[1] = dup(offs, n * sizeof(*offs));
    RUN(c);
}

VKAPI_ATTR void VKAPI_CALL zss_CmdDraw(VkCommandBuffer cmd, uint32_t vc, uint32_t ic, uint32_t fv,
                                       uint32_t fi)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_DRAW);

    c->u[0] = vc;
    c->u[1] = ic;
    c->u[2] = fv;
    c->u[3] = fi;
    RUN(c);
}

VKAPI_ATTR void VKAPI_CALL zss_CmdDrawIndexed(VkCommandBuffer cmd, uint32_t ic, uint32_t inst,
                                              uint32_t first, int32_t voff, uint32_t finst)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_DRAW_INDEXED);

    c->u[0] = ic;
    c->u[1] = inst;
    c->u[2] = first;
    c->u[3] = (uint32_t)voff;
    c->u[4] = finst;
    RUN(c);
}

#define INDIRECT_FN(Name, op) \
    VKAPI_ATTR void VKAPI_CALL zss_Cmd##Name(VkCommandBuffer cmd, VkBuffer buffer, \
                                             VkDeviceSize offset, uint32_t count, uint32_t stride) \
    { \
        CB(cmd); \
        struct zss_cmd *c = rec(cb, op); \
        c->h[0] = use(cb, H(buffer)); \
        c->s[0] = offset; \
        c->u[0] = count; \
        c->u[1] = stride; \
        RUN(c); \
    }
INDIRECT_FN(DrawIndirect, OP_DRAW_INDIRECT)
INDIRECT_FN(DrawIndexedIndirect, OP_DRAW_INDEXED_INDIRECT)

VKAPI_ATTR void VKAPI_CALL zss_CmdCopyBuffer(VkCommandBuffer cmd, VkBuffer src, VkBuffer dst,
                                             uint32_t n, const VkBufferCopy *regions)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_COPY_BUFFER);

    c->h[0] = use(cb, H(src));
    c->h[1] = use(cb, H(dst));
    c->n[0] = n;
    c->a[0] = dup(regions, n * sizeof(*regions));
    RUN(c);
}

#define IMAGE_PAIR_FN(Name, op, Region) \
    VKAPI_ATTR void VKAPI_CALL zss_Cmd##Name(VkCommandBuffer cmd, VkImage src, VkImageLayout sl, \
                                             VkImage dst, VkImageLayout dl, uint32_t n, \
                                             const Region *regions) \
    { \
        CB(cmd); \
        struct zss_cmd *c = rec(cb, op); \
        c->h[0] = use(cb, H(src)); \
        c->h[1] = use(cb, H(dst)); \
        c->u[0] = sl; \
        c->u[1] = dl; \
        c->n[0] = n; \
        c->a[0] = dup(regions, n * sizeof(*regions)); \
        RUN(c); \
    }
IMAGE_PAIR_FN(CopyImage, OP_COPY_IMAGE, VkImageCopy)
IMAGE_PAIR_FN(ResolveImage, OP_RESOLVE, VkImageResolve)

VKAPI_ATTR void VKAPI_CALL zss_CmdBlitImage(VkCommandBuffer cmd, VkImage src, VkImageLayout sl,
                                            VkImage dst, VkImageLayout dl, uint32_t n,
                                            const VkImageBlit *regions, VkFilter filter)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_BLIT_IMAGE);

    c->h[0] = use(cb, H(src));
    c->h[1] = use(cb, H(dst));
    c->u[0] = sl;
    c->u[1] = dl;
    c->u[2] = filter;
    c->n[0] = n;
    c->a[0] = dup(regions, n * sizeof(*regions));
    RUN(c);
}

VKAPI_ATTR void VKAPI_CALL zss_CmdCopyBufferToImage(VkCommandBuffer cmd, VkBuffer src, VkImage dst,
                                                    VkImageLayout layout, uint32_t n,
                                                    const VkBufferImageCopy *regions)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_COPY_BUFFER_TO_IMAGE);

    c->h[0] = use(cb, H(src));
    c->h[1] = use(cb, H(dst));
    c->u[0] = layout;
    c->n[0] = n;
    c->a[0] = dup(regions, n * sizeof(*regions));
    RUN(c);
}

VKAPI_ATTR void VKAPI_CALL zss_CmdCopyImageToBuffer(VkCommandBuffer cmd, VkImage src,
                                                    VkImageLayout layout, VkBuffer dst, uint32_t n,
                                                    const VkBufferImageCopy *regions)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_COPY_IMAGE_TO_BUFFER);

    c->h[0] = use(cb, H(src));
    c->h[1] = use(cb, H(dst));
    c->u[0] = layout;
    c->n[0] = n;
    c->a[0] = dup(regions, n * sizeof(*regions));
    RUN(c);
}

VKAPI_ATTR void VKAPI_CALL zss_CmdUpdateBuffer(VkCommandBuffer cmd, VkBuffer dst, VkDeviceSize off,
                                               VkDeviceSize size, const void *data)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_UPDATE_BUFFER);

    c->h[0] = use(cb, H(dst));
    c->s[0] = off;
    c->s[1] = size;
    c->a[0] = dup(data, size);
    RUN(c);
}

VKAPI_ATTR void VKAPI_CALL zss_CmdFillBuffer(VkCommandBuffer cmd, VkBuffer dst, VkDeviceSize off,
                                             VkDeviceSize size, uint32_t data)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_FILL_BUFFER);

    c->h[0] = use(cb, H(dst));
    c->s[0] = off;
    c->s[1] = size;
    c->u[0] = data;
    RUN(c);
}

VKAPI_ATTR void VKAPI_CALL zss_CmdClearColorImage(VkCommandBuffer cmd, VkImage image,
                                                  VkImageLayout layout,
                                                  const VkClearColorValue *color, uint32_t n,
                                                  const VkImageSubresourceRange *ranges)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_CLEAR_COLOR);

    c->h[0] = use(cb, H(image));
    c->u[0] = layout;
    c->clear.color = *color;
    c->n[0] = n;
    c->a[0] = dup(ranges, n * sizeof(*ranges));
    RUN(c);
}

VKAPI_ATTR void VKAPI_CALL zss_CmdClearDepthStencilImage(VkCommandBuffer cmd, VkImage image,
                                                         VkImageLayout layout,
                                                         const VkClearDepthStencilValue *value,
                                                         uint32_t n,
                                                         const VkImageSubresourceRange *ranges)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_CLEAR_DS);

    c->h[0] = use(cb, H(image));
    c->u[0] = layout;
    c->clear.depthStencil = *value;
    c->n[0] = n;
    c->a[0] = dup(ranges, n * sizeof(*ranges));
    RUN(c);
}

VKAPI_ATTR void VKAPI_CALL zss_CmdClearAttachments(VkCommandBuffer cmd, uint32_t na,
                                                   const VkClearAttachment *att, uint32_t nr,
                                                   const VkClearRect *rects)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_CLEAR_ATTACHMENTS);

    c->n[0] = na;
    c->a[0] = dup(att, na * sizeof(*att));
    c->n[1] = nr;
    c->a[1] = dup(rects, nr * sizeof(*rects));
    RUN(c);
}

VKAPI_ATTR void VKAPI_CALL zss_CmdPipelineBarrier(VkCommandBuffer cmd, VkPipelineStageFlags src,
                                                  VkPipelineStageFlags dst, VkDependencyFlags deps,
                                                  uint32_t nm, const VkMemoryBarrier *mb,
                                                  uint32_t nb, const VkBufferMemoryBarrier *bb,
                                                  uint32_t ni, const VkImageMemoryBarrier *ib)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_BARRIER);

    c->u[0] = src;
    c->u[1] = dst;
    c->u[2] = deps;
    c->n[0] = nm;
    c->a[0] = dup(mb, nm * sizeof(*mb));
    c->n[1] = nb;
    c->a[1] = dup(bb, nb * sizeof(*bb));
    c->n[2] = ni;
    c->a[2] = dup(ib, ni * sizeof(*ib));
    for (uint32_t i = 0; i < nb; i++)
        use(cb, H(bb[i].buffer));
    for (uint32_t i = 0; i < ni; i++)
        use(cb, H(ib[i].image));
    RUN(c);
}

VKAPI_ATTR void VKAPI_CALL zss_CmdPushConstants(VkCommandBuffer cmd, VkPipelineLayout layout,
                                                VkShaderStageFlags stages, uint32_t offset,
                                                uint32_t size, const void *values)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_PUSH_CONSTANTS);

    c->h[0] = use(cb, H(layout));
    c->u[0] = stages;
    c->u[1] = offset;
    c->u[2] = size;
    c->a[0] = dup(values, size);
    RUN(c);
}

VKAPI_ATTR void VKAPI_CALL zss_CmdBeginRenderPass(VkCommandBuffer cmd,
                                                  const VkRenderPassBeginInfo *info,
                                                  VkSubpassContents contents)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_BEGIN_RP);

    c->h[0] = use(cb, H(info->renderPass));
    c->h[1] = use(cb, H(info->framebuffer));
    c->area = info->renderArea;
    c->n[0] = info->clearValueCount;
    c->a[0] = dup(info->pClearValues, info->clearValueCount * sizeof(VkClearValue));
    c->u[0] = contents;
    RUN(c);
}

VKAPI_ATTR void VKAPI_CALL zss_CmdNextSubpass(VkCommandBuffer cmd, VkSubpassContents contents)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_NEXT_SUBPASS);

    c->u[0] = contents;
    RUN(c);
}

VKAPI_ATTR void VKAPI_CALL zss_CmdEndRenderPass(VkCommandBuffer cmd)
{
    CB(cmd);
    struct zss_cmd *c = rec(cb, OP_END_RP);

    RUN(c);
}

/* ---- commands that are forwarded but not recorded ------------------------------ */

VKAPI_ATTR void VKAPI_CALL zss_CmdDispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z)
{
    CB(cmd);
    zss_dev_untracked(dev, "compute dispatch");
    dev->fn.CmdDispatch(REAL_CB, x, y, z);
    zss_leave();
}

VKAPI_ATTR void VKAPI_CALL zss_CmdDispatchIndirect(VkCommandBuffer cmd, VkBuffer buffer,
                                                   VkDeviceSize offset)
{
    CB(cmd);
    zss_dev_untracked(dev, "compute dispatch");
    dev->fn.CmdDispatchIndirect(REAL_CB, ZREAL(VkBuffer, buffer), offset);
    zss_leave();
}

VKAPI_ATTR void VKAPI_CALL zss_CmdSetEvent(VkCommandBuffer cmd, VkEvent event,
                                           VkPipelineStageFlags stage)
{
    CB(cmd);
    dev->fn.CmdSetEvent(REAL_CB, ZREAL(VkEvent, event), stage);
    zss_leave();
}

VKAPI_ATTR void VKAPI_CALL zss_CmdResetEvent(VkCommandBuffer cmd, VkEvent event,
                                             VkPipelineStageFlags stage)
{
    CB(cmd);
    dev->fn.CmdResetEvent(REAL_CB, ZREAL(VkEvent, event), stage);
    zss_leave();
}

VKAPI_ATTR void VKAPI_CALL zss_CmdWaitEvents(VkCommandBuffer cmd, uint32_t ne, const VkEvent *events,
                                             VkPipelineStageFlags src, VkPipelineStageFlags dst,
                                             uint32_t nm, const VkMemoryBarrier *mb, uint32_t nb,
                                             const VkBufferMemoryBarrier *bb, uint32_t ni,
                                             const VkImageMemoryBarrier *ib)
{
    CB(cmd);
    VkEvent *re = malloc((ne + 1) * sizeof(*re));
    VkBufferMemoryBarrier *rb = malloc((nb + 1) * sizeof(*rb));
    VkImageMemoryBarrier *ri = malloc((ni + 1) * sizeof(*ri));

    for (uint32_t i = 0; i < ne; i++)
        re[i] = ZREAL(VkEvent, events[i]);
    for (uint32_t i = 0; i < nb; i++) {
        rb[i] = bb[i];
        rb[i].buffer = ZREAL(VkBuffer, bb[i].buffer);
    }
    for (uint32_t i = 0; i < ni; i++) {
        ri[i] = ib[i];
        ri[i].image = ZREAL(VkImage, ib[i].image);
    }
    dev->fn.CmdWaitEvents(REAL_CB, ne, re, src, dst, nm, mb, nb, rb, ni, ri);
    free(re);
    free(rb);
    free(ri);
    zss_leave();
}

VKAPI_ATTR void VKAPI_CALL zss_CmdBeginQuery(VkCommandBuffer cmd, VkQueryPool pool, uint32_t query,
                                             VkQueryControlFlags flags)
{
    CB(cmd);
    dev->fn.CmdBeginQuery(REAL_CB, ZREAL(VkQueryPool, pool), query, flags);
    zss_leave();
}

VKAPI_ATTR void VKAPI_CALL zss_CmdEndQuery(VkCommandBuffer cmd, VkQueryPool pool, uint32_t query)
{
    CB(cmd);
    dev->fn.CmdEndQuery(REAL_CB, ZREAL(VkQueryPool, pool), query);
    zss_leave();
}

VKAPI_ATTR void VKAPI_CALL zss_CmdResetQueryPool(VkCommandBuffer cmd, VkQueryPool pool,
                                                 uint32_t first, uint32_t count)
{
    CB(cmd);
    dev->fn.CmdResetQueryPool(REAL_CB, ZREAL(VkQueryPool, pool), first, count);
    zss_leave();
}

VKAPI_ATTR void VKAPI_CALL zss_CmdWriteTimestamp(VkCommandBuffer cmd, VkPipelineStageFlagBits stage,
                                                 VkQueryPool pool, uint32_t query)
{
    CB(cmd);
    dev->fn.CmdWriteTimestamp(REAL_CB, stage, ZREAL(VkQueryPool, pool), query);
    zss_leave();
}

VKAPI_ATTR void VKAPI_CALL zss_CmdCopyQueryPoolResults(VkCommandBuffer cmd, VkQueryPool pool,
                                                       uint32_t first, uint32_t count, VkBuffer dst,
                                                       VkDeviceSize offset, VkDeviceSize stride,
                                                       VkQueryResultFlags flags)
{
    CB(cmd);
    dev->fn.CmdCopyQueryPoolResults(REAL_CB, ZREAL(VkQueryPool, pool), first, count,
                                    ZREAL(VkBuffer, dst), offset, stride, flags);
    zss_leave();
}

VKAPI_ATTR void VKAPI_CALL zss_CmdExecuteCommands(VkCommandBuffer cmd, uint32_t n,
                                                  const VkCommandBuffer *bufs)
{
    CB(cmd);
    VkCommandBuffer *real = malloc((n + 1) * sizeof(*real));

    zss_dev_untracked(dev, "secondary command buffers");
    for (uint32_t i = 0; i < n; i++)
        real[i] = (VkCommandBuffer)(uintptr_t)((struct zss_obj *)bufs[i])->r.h;
    dev->fn.CmdExecuteCommands(REAL_CB, n, real);
    free(real);
    zss_leave();
}
