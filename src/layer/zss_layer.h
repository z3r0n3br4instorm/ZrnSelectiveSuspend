/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ZSS graphics layer.
 *
 * Loaded by the Vulkan loader as the only driver (see zss-run). It presents
 * virtual GPUs, devices and objects to the application and drives the real
 * vendor drivers underneath, so the real device can be swapped while the
 * application keeps every handle it was given.
 */
#ifndef ZSS_LAYER_H
#define ZSS_LAYER_H

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zss_retain.h"

/* Every real-driver device entry point the layer calls. */
#define ZSS_DEV_FNS(X) \
    X(DestroyDevice) X(GetDeviceQueue) X(QueueSubmit) X(QueueWaitIdle) X(DeviceWaitIdle) \
    X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory) \
    X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges) X(GetDeviceMemoryCommitment) \
    X(BindBufferMemory) X(BindImageMemory) X(GetBufferMemoryRequirements) \
    X(GetImageMemoryRequirements) X(GetImageSparseMemoryRequirements) X(QueueBindSparse) \
    X(CreateFence) X(DestroyFence) X(ResetFences) X(GetFenceStatus) X(WaitForFences) \
    X(CreateSemaphore) X(DestroySemaphore) \
    X(CreateEvent) X(DestroyEvent) X(GetEventStatus) X(SetEvent) X(ResetEvent) \
    X(CreateQueryPool) X(DestroyQueryPool) X(GetQueryPoolResults) \
    X(CreateBuffer) X(DestroyBuffer) X(CreateBufferView) X(DestroyBufferView) \
    X(CreateImage) X(DestroyImage) X(GetImageSubresourceLayout) \
    X(CreateImageView) X(DestroyImageView) X(CreateShaderModule) X(DestroyShaderModule) \
    X(CreatePipelineCache) X(DestroyPipelineCache) X(GetPipelineCacheData) X(MergePipelineCaches) \
    X(CreateGraphicsPipelines) X(CreateComputePipelines) X(DestroyPipeline) \
    X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateSampler) X(DestroySampler) \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) X(ResetDescriptorPool) \
    X(AllocateDescriptorSets) X(FreeDescriptorSets) X(UpdateDescriptorSets) \
    X(CreateFramebuffer) X(DestroyFramebuffer) X(CreateRenderPass) X(DestroyRenderPass) \
    X(GetRenderAreaGranularity) \
    X(CreateCommandPool) X(DestroyCommandPool) X(ResetCommandPool) \
    X(AllocateCommandBuffers) X(FreeCommandBuffers) \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer) \
    X(CmdBindPipeline) X(CmdSetViewport) X(CmdSetScissor) X(CmdSetLineWidth) X(CmdSetDepthBias) \
    X(CmdSetBlendConstants) X(CmdSetDepthBounds) X(CmdSetStencilCompareMask) \
    X(CmdSetStencilWriteMask) X(CmdSetStencilReference) X(CmdBindDescriptorSets) \
    X(CmdBindIndexBuffer) X(CmdBindVertexBuffers) X(CmdDraw) X(CmdDrawIndexed) \
    X(CmdDrawIndirect) X(CmdDrawIndexedIndirect) X(CmdDispatch) X(CmdDispatchIndirect) \
    X(CmdCopyBuffer) X(CmdCopyImage) X(CmdBlitImage) X(CmdCopyBufferToImage) \
    X(CmdCopyImageToBuffer) X(CmdUpdateBuffer) X(CmdFillBuffer) X(CmdClearColorImage) \
    X(CmdClearDepthStencilImage) X(CmdClearAttachments) X(CmdResolveImage) \
    X(CmdSetEvent) X(CmdResetEvent) X(CmdWaitEvents) X(CmdPipelineBarrier) \
    X(CmdBeginQuery) X(CmdEndQuery) X(CmdResetQueryPool) X(CmdWriteTimestamp) \
    X(CmdCopyQueryPoolResults) X(CmdPushConstants) X(CmdBeginRenderPass) X(CmdNextSubpass) \
    X(CmdEndRenderPass) X(CmdExecuteCommands) \
    X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) \
    X(AcquireNextImageKHR) X(QueuePresentKHR) \
    /* VK_EXT_transform_feedback: NULL on a device that was not created with it. */ \
    X(CmdBindTransformFeedbackBuffersEXT) X(CmdBeginTransformFeedbackEXT) X(CmdEndTransformFeedbackEXT) \
    X(CmdBeginQueryIndexedEXT) X(CmdEndQueryIndexedEXT) X(CmdDrawIndirectByteCountEXT) \
    /* For Zink. Also NULL where the device lacks them; see vk11.c for what stands in. */ \
    X(CmdBeginRenderingKHR) X(CmdEndRenderingKHR) X(CmdBindIndexBuffer2KHR) X(CmdBindVertexBuffers2EXT) \
    X(CmdSetCullModeEXT) X(CmdSetFrontFaceEXT) X(CmdSetPrimitiveTopologyEXT) X(CmdSetViewportWithCountEXT) X(CmdSetScissorWithCountEXT) X(CmdSetDepthTestEnableEXT) \
    X(CmdSetDepthWriteEnableEXT) X(CmdSetDepthCompareOpEXT) X(CmdSetDepthBoundsTestEnableEXT) X(CmdSetStencilTestEnableEXT) X(CmdSetStencilOpEXT) \
    X(CmdBeginConditionalRenderingEXT) X(CmdEndConditionalRenderingEXT) X(CmdSetLineStippleEXT) \
    X(WaitSemaphoresKHR) X(SignalSemaphoreKHR) X(GetSemaphoreCounterValueKHR) \
    X(GetRenderingAreaGranularityKHR) X(GetImageSubresourceLayout2KHR) X(GetDeviceImageSubresourceLayoutKHR)

struct zss_dev_fns {
#define X(n) PFN_vk##n n;
    ZSS_DEV_FNS(X)
#undef X
};

/* Real-driver instance entry points. Optional ones may be NULL. */
#define ZSS_INST_FNS(X) \
    X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceFeatures) \
    X(GetPhysicalDeviceFormatProperties) X(GetPhysicalDeviceImageFormatProperties) \
    X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceQueueFamilyProperties) \
    X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceSparseImageFormatProperties) \
    X(EnumerateDeviceExtensionProperties) X(CreateDevice) X(GetDeviceProcAddr) \
    X(GetPhysicalDeviceProperties2) X(GetPhysicalDeviceProperties2KHR) \
    X(GetPhysicalDeviceFeatures2) X(GetPhysicalDeviceFeatures2KHR) \
    X(DestroySurfaceKHR) X(GetPhysicalDeviceSurfaceSupportKHR) \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR) X(GetPhysicalDeviceSurfaceFormatsKHR) \
    X(GetPhysicalDeviceSurfacePresentModesKHR) \
    X(CreateXcbSurfaceKHR) X(CreateXlibSurfaceKHR) X(CreateWaylandSurfaceKHR) \
    X(GetPhysicalDeviceXcbPresentationSupportKHR) X(GetPhysicalDeviceXlibPresentationSupportKHR) \
    X(GetPhysicalDeviceWaylandPresentationSupportKHR)

struct zss_inst_fns {
#define X(n) PFN_vk##n n;
    ZSS_INST_FNS(X)
#undef X
};

#define ZSS_MAX_GPUS 16
#define ZSS_MAX_FAMILIES 16
#define ZSS_MAX_SURFACES 16
#define ZSS_PCI_LEN 16

struct zss_surface {
    VkSurfaceKHR outer; /* the loader's VkIcdSurface, as given to us */
    VkIcdWsiPlatform platform;
    uintptr_t native[2];
    VkSurfaceKHR real;
    bool owned; /* created through the driver, so we destroy it */
    void *link; /* the layer's own connection to the display server for this driver, or NULL */
};

/* One real vendor driver library. */
struct zss_driver {
    char lib[256];
    void *dl;
    PFN_vkGetInstanceProcAddr gipa;
    VkInstance inst; /* NULL while closed */
    struct zss_inst_fns fn;
    bool has_surface, has_xcb, has_xlib, has_wayland;
    struct zss_surface surfaces[ZSS_MAX_SURFACES];
    int ndevices; /* live real VkDevices */
    /* Display-device nodes the driver opened while starting, by descriptor and device number (icd.c). */
    struct { int fd; dev_t dev; } opened[16];
    int nopened;
};

/* One virtual physical device, as the application sees it. */
struct zss_gpu {
    VK_LOADER_DATA ld;
    struct zss_driver *drv;
    VkPhysicalDevice real; /* valid only while drv->inst is open */
    char pci[ZSS_PCI_LEN]; /* "" when the device has no PCI address */
    bool software;
    bool test_bound; /* software device posing as a PCI card (ZSS_BIND_PCI) */
    bool detached;
    int hold_fd;

    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceFeatures features;
    VkPhysicalDeviceMemoryProperties mem;
    uint32_t nfam;
    VkQueueFamilyProperties fam[ZSS_MAX_FAMILIES];
    uint32_t next;
    VkExtensionProperties *ext;
    /* Read once when the GPU was found, for the portable profile (profile.c). */
    VkFormatProperties *fmt; /* by core format number */
    uint64_t *featbits;      /* two words per feature structure the layer knows (vk11.c) */
};

enum zss_kind {
    ZK_MEMORY, ZK_BUFFER, ZK_IMAGE, ZK_VIEW, ZK_SAMPLER, ZK_SHADER, ZK_RENDERPASS,
    ZK_FRAMEBUFFER, ZK_DSL, ZK_DPOOL, ZK_DSET, ZK_PLAYOUT, ZK_PCACHE, ZK_PIPELINE,
    ZK_CPOOL, ZK_CMDBUF, ZK_FENCE, ZK_SEMAPHORE, ZK_SWAPCHAIN, ZK_YCBCR,
    /* Forwarded without tracking; using one makes the device non-migratable. */
    ZK_OPAQUE_EVENT, ZK_OPAQUE_QUERYPOOL, ZK_OPAQUE_BUFFERVIEW, ZK_OPAQUE_PIPELINE,
    ZK_COUNT
};

struct zss_dev;
struct zss_cmd;

/* The part of an object that belongs to the current real device. */
struct zss_real {
    uint64_t h;
    VkDeviceMemory backing;
    void *map;
    bool standin; /* image standing in for a swapchain image; on a swapchain: h is a presenter (present.c) */
    bool borrowed; /* image owned by a real swapchain: never destroyed directly */
};

struct zss_slot {
    VkDescriptorType type;
    uint32_t count;
    struct zss_slot_entry {
        struct zss_obj *sampler, *view, *buffer;
        VkImageLayout layout;
        VkDeviceSize offset, range;
        bool written;
    } *e;
};

/*
 * Every handle the application holds is a pointer to one of these. The
 * loader word comes first because command buffers are dispatchable.
 */
struct zss_obj {
    VK_LOADER_DATA ld;
    enum zss_kind kind;
    uint32_t refs;
    bool dead; /* destroyed by the application, kept while referenced */
    struct zss_real r;
    struct zss_dev *dev;
    struct zss_obj *prev, *next; /* creation order */
    struct zss_obj **deps;
    uint32_t ndeps;
    void **allocs; /* deep-copied creation data */
    uint32_t nallocs;

    union {
        struct {
            VkDeviceSize size;
            VkMemoryPropertyFlags flags;
            uint8_t *shadow;
            bool mapped;
        } mem;
        struct {
            VkBufferCreateInfo ci;
            struct zss_obj *mem;
            VkDeviceSize mem_off;
            bool gpu_written;
            bool filled; /* the GPU has written it, and no copy of that exists outside */
            struct zss_ret ret; /* retained upload covering the whole buffer */
            uint8_t *saved;
        } buf;
        struct {
            VkImageCreateInfo ci;
            struct zss_obj *mem;
            VkDeviceSize mem_off, map_size; /* linear image in mappable memory */
            struct zss_obj *swapchain;
            VkImageLayout *layout; /* [mip * arrayLayers + layer] */
            uint8_t *saved;
            VkDeviceSize saved_size;
            /* A rebuilt swapchain's image: contents to put in it when the application first gets it (swapchain.c). */
            uint8_t *pending;
            VkDeviceSize pending_size;
            /* Retained uploads, indexed [(mip * arrayLayers + layer) * 2 + (stencil ? 1 : 0)]. */
            struct zss_ret *ret;
            bool carried;    /* holds GPU-generated contents that later frames depend on */
            bool unretained; /* received an upload that could not be retained */
        } img;
        struct { VkImageViewCreateInfo ci; } view;
        struct { VkSamplerCreateInfo ci; } sampler;
        struct { VkSamplerYcbcrConversionCreateInfo ci; } ycbcr;
        struct { VkShaderModuleCreateInfo ci; } shader;
        struct { VkRenderPassCreateInfo ci; } rp;
        struct { VkFramebufferCreateInfo ci; } fb;
        struct { VkDescriptorSetLayoutCreateInfo ci; } dsl;
        struct { VkDescriptorPoolCreateInfo ci; } dpool;
        struct {
            struct zss_obj *pool, *layout;
            uint32_t nslots;
            struct zss_slot *slot; /* indexed by binding number */
        } dset;
        struct { VkPipelineLayoutCreateInfo ci; } playout;
        struct { VkGraphicsPipelineCreateInfo ci; } pipe;
        struct { VkCommandPoolCreateInfo ci; } cpool;
        struct {
            struct zss_obj *pool;
            VkCommandBufferLevel level;
            enum { ZC_INITIAL, ZC_RECORDING, ZC_EXECUTABLE, ZC_INVALID } state;
            VkCommandBufferUsageFlags usage;
            struct zss_cmd *head, *tail;
            struct zss_obj **refs;
            uint32_t nrefs, caprefs;
            uint32_t version; /* bumped whenever the recorded commands are thrown away */
            /* A secondary buffer's inheritance, kept for replay; the render pass and framebuffer are in refs. */
            bool has_inh;
            VkCommandBufferInheritanceInfo inh;
        } cb;
        struct { bool signaled, pending; } fence;
        struct {
            bool signaled;
            bool timeline;   /* VK_KHR_timeline_semaphore: a counter instead of a flag */
            bool virtual_acquire; /* "signalled" by an acquire of a presented-through swapchain: not waited on for real (present.c) */
            uint64_t value;  /* the counter as last known: read from the device, or the highest signalled */
        } sem;
        struct {
            VkSwapchainCreateInfoKHR ci;
            struct zss_obj **images;
            uint32_t nimages;
            bool retired;
            bool superseded; /* the application made a newer swapchain from this one (oldSwapchain) */
            /* Kept across a rebuild on another GPU (swapchain.c). Bit and index are the application's. */
            uint32_t *real_of, *undo_real_of; /* application's image index -> the driver's */
            uint64_t acquired;                /* acquired and not yet presented */
            uint64_t fresh, undo_fresh;       /* not yet put in the layout the application believes */
            uint32_t gen;                     /* counts rebuilds */
            bool undo_valid;                  /* the build in progress rebuilt this swapchain */
            uint64_t undo_pending;            /* images whose contents that rebuild took over */
        } sc;
    } u;
};

struct zss_queue {
    VK_LOADER_DATA ld;
    struct zss_dev *dev;
    uint32_t family, index;
    VkQueue real;
};

/*
 * A submission the device accepted and that has not been seen to finish. If
 * the device is lost first, its work is gone with it, and is issued again on
 * the device the application is rebuilt on.
 */
#define ZSS_MAX_INFLIGHT 16

struct zss_inflight {
    struct zss_queue *q;
    struct zss_obj *fence; /* or NULL */
    struct zss_obj **cbs;
    uint32_t *versions;    /* of each command buffer when it was submitted */
    uint32_t ncbs;
};

struct zss_dev {
    VK_LOADER_DATA ld;
    struct zss_dev *next_dev;
    struct zss_gpu *origin;
    struct zss_gpu *gpu; /* NULL while parked */
    VkDevice real;
    struct zss_dev_fns fn;

    /* What the application asked for, in origin terms. */
    VkPhysicalDeviceFeatures features;
    bool want_swapchain;
    uint32_t nreq;
    struct { uint32_t family, count; } req[ZSS_MAX_FAMILIES];
    uint32_t fam_map[ZSS_MAX_FAMILIES]; /* origin family -> current real family */

    struct zss_queue *queues;
    uint32_t nqueues;
    struct zss_obj *head, *tail;

    bool migratable;
    char reason[128];

    /* Loss handling (see migrate.c). */
    bool lost;           /* the real device is gone; recovery is due */
    bool dead;           /* recovery is impossible: the application gets the error */
    uint32_t generation; /* bumped every time the real device is replaced */
    int lost_contents;   /* objects zero-filled by the last recovery */
    void *feat_chain;    /* feature structures enabled at creation, kept for every rebuild (vk11.c) */
    /* Whether the current real device has these itself; if not, the layer stands in (lower.c, vk11.c). */
    bool native_dynrender, native_maint5;
    void *lower;         /* render passes and framebuffers standing in for dynamic rendering (lower.c) */
    const char *exts[64]; /* offered extensions the application enabled */
    uint32_t nexts;
    struct zss_inflight inflight[ZSS_MAX_INFLIGHT]; /* oldest first */
    uint32_t ninflight;

    /* Helpers on the current real device for copies during migration. */
    VkQueue util_queue;
    VkCommandPool util_pool;
    VkCommandBuffer util_cmd;
};

/* The layer's own entry points, type-checked against the Vulkan prototypes. */
#define X(n) extern __typeof__(*(PFN_vk##n)0) zss_##n;
ZSS_DEV_FNS(X)
#undef X
extern __typeof__(*(PFN_vkCreateDevice)0) zss_CreateDevice;

/* Unwrapping. VK_NULL_HANDLE stays null. */
/* Vulkan 1.1 (vk11.c). */
struct zss_entry {
    const char *name;
    PFN_vkVoidFunction fn;
};
uint32_t zss_api_version(void);
uint32_t zss_gpu_api_version(const struct zss_gpu *gpu);
void *zss_feats_keep(const void *pnext, VkPhysicalDeviceFeatures *base, bool *had_base);
void zss_feats_free(void *chain);
bool zss_feats_supported(struct zss_gpu *gpu, const void *chain, char *reason, size_t rlen);
uint32_t zss_offered_ext_count(void);
const char *zss_offered_ext(uint32_t i);
const char *zss_offered_ext_named(const char *name);
void *zss_chain_keep(struct zss_obj *o, const void *pnext);
void *zss_chain_real(const void *kept, void *scratch, size_t room);
bool zss_format_planar(VkFormat f);
extern __typeof__(*(PFN_vkCreateSamplerYcbcrConversion)0) zss_CreateSamplerYcbcrConversion;
extern __typeof__(*(PFN_vkDestroySamplerYcbcrConversion)0) zss_DestroySamplerYcbcrConversion;
void zss_feats_cache(struct zss_gpu *gpu, VkPhysicalDevice pd);
bool zss_feats_in_profile(const struct zss_gpu *gpu, const void *chain, char *reason, size_t rlen);
/* The portable profile (profile.c). */
bool zss_profile_member(const struct zss_gpu *self, const struct zss_gpu *g);
void zss_profile_cache(struct zss_gpu *gpu, VkPhysicalDevice pd);
uint32_t zss_profile_api_version(const struct zss_gpu *self);
void zss_profile_limits(const struct zss_gpu *self, VkPhysicalDeviceLimits *lim);
void zss_profile_features(const struct zss_gpu *self, VkPhysicalDeviceFeatures *f);
const struct zss_gpu *zss_profile_feature_lacking(const struct zss_gpu *self, size_t k);
bool zss_profile_has_ext(const struct zss_gpu *self, const char *name);
void zss_profile_format(const struct zss_gpu *self, VkFormat format, VkFormatProperties *p);
bool zss_profile_format_usable(const struct zss_gpu *self, VkFormat format, VkImageTiling tiling);
uint32_t zss_profile_families(const struct zss_gpu *self, VkQueueFamilyProperties *out);
void zss_profile_describe(const struct zss_gpu *self);
bool zss_control_software_allowed(void);
bool zss_start_wanted(const struct zss_gpu *gpu);
void zss_load_skipped(void);
const char *zss_feature_name(size_t k);
PFN_vkVoidFunction zss_vk11_instance_proc(const char *name);
PFN_vkVoidFunction zss_vk11_device_proc(const char *name);
PFN_vkVoidFunction zss_instance_proc(const char *name);
extern __typeof__(*(PFN_vkCmdSetDeviceMask)0) zss_CmdSetDeviceMask;
extern __typeof__(*(PFN_vkCmdDispatchBase)0) zss_CmdDispatchBase;

#define ZOBJ(handle) ((struct zss_obj *)(uintptr_t)(handle))
#define ZREAL(T, handle) ((T)(uintptr_t)((handle) ? ZOBJ(handle)->r.h : 0))
#define ZHANDLE(T, obj) ((T)(uintptr_t)(obj))

/* icd.c */
extern struct zss_gpu *zss_gpus[ZSS_MAX_GPUS];
extern int zss_ngpus;
extern struct zss_dev *zss_devices;
extern pthread_mutex_t zss_lock; /* object lists, refcounts, device list */

void zss_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void zss_dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
VkResult zss_driver_open(struct zss_driver *drv);
void zss_driver_close(struct zss_driver *drv);
void zss_driver_break_links(struct zss_driver *drv);
void zss_dev_mark_lost(struct zss_dev *dev);
VkPhysicalDevice zss_gpu_real(struct zss_gpu *gpu);
struct zss_gpu *zss_gpu_by_pci(const char *pci);
void zss_gpu_hold(struct zss_gpu *gpu, bool hold);
bool zss_gpu_held(const struct zss_gpu *gpu);
VkResult zss_surface_real(struct zss_driver *drv, VkSurfaceKHR outer, VkSurfaceKHR *real);
PFN_vkVoidFunction zss_device_proc(const char *name);

/* gate.c: stops the application at the API boundary during a migration. */
void zss_enter(void);
void zss_leave(void);
void zss_hold_begin(void);
/* Gives up waiting after ms; the gate is closed either way. False if threads are still inside. */
bool zss_hold_begin_timed(int ms);
void zss_hold_end(void);
extern uint32_t zss_epoch;
bool zss_stale(void);

/* device.c */
VkResult zss_dev_create_real(struct zss_dev *dev, struct zss_gpu *gpu);
void zss_dev_destroy_real(struct zss_dev *dev);
void zss_dev_untracked(struct zss_dev *dev, const char *feature);
struct zss_obj *zss_obj_new(struct zss_dev *dev, enum zss_kind kind);
void zss_obj_ref(struct zss_obj *o);
void zss_obj_unref(struct zss_obj *o);
void zss_obj_dep(struct zss_obj *o, struct zss_obj *dep);
void *zss_obj_dup(struct zss_obj *o, const void *src, size_t size);
void zss_obj_kill(struct zss_obj *o);
VkResult zss_real_create(struct zss_dev *dev, struct zss_obj *o);
void zss_real_destroy(struct zss_dev *dev, enum zss_kind kind, struct zss_real *r);
VkResult zss_backing_alloc(struct zss_dev *dev, const VkMemoryRequirements *req, bool host,
                           VkDeviceMemory *mem, void **map);
void zss_dset_apply(struct zss_dev *dev, struct zss_obj *set);
void zss_sync_to_device(struct zss_dev *dev, struct zss_obj *only_mem);
void zss_sync_from_device(struct zss_dev *dev, struct zss_obj *only_mem);
void zss_surface_repaint(struct zss_driver *drv, VkSurfaceKHR outer);
void zss_cmd_track_submit_effects(struct zss_dev *dev, struct zss_obj *cb);
bool zss_swapchain_rebuild(struct zss_dev *dev, struct zss_obj *sc);
void zss_swapchain_rollback(struct zss_obj *sc);
VkResult zss_image_bring_up(struct zss_dev *dev, struct zss_obj *img, const uint8_t *bytes, VkDeviceSize size);
void zss_control_exclusive(void (*fn)(void *), void *arg);
VkResult zss_util_begin(struct zss_dev *dev);
VkResult zss_util_run(struct zss_dev *dev);
VkImageAspectFlags zss_format_aspects(VkFormat f);

/* cmd.c */
void zss_cmd_reset(struct zss_obj *cb);
void zss_cmd_replay(struct zss_dev *dev, struct zss_obj *cb);
void zss_cmd_track_submit(struct zss_dev *dev, struct zss_obj *cb);
void zss_image_dirty(struct zss_obj *img, bool carried);
struct zss_presenter;
struct zss_queue;
bool zss_present_needed(struct zss_gpu *gpu);
struct zss_driver *zss_present_driver(struct zss_gpu *gpu);
struct zss_presenter *zss_presenter_new(struct zss_dev *dev, const VkSwapchainCreateInfoKHR *app);
void zss_presenter_destroy(struct zss_presenter *p, bool drawing_side_gone);
void zss_presenter_give_up(struct zss_presenter *p);
VkResult zss_presenter_show(struct zss_presenter *p, struct zss_dev *dev, struct zss_queue *q, struct zss_obj *img,
                            const VkSemaphore *waits, uint32_t nwaits);
void zss_cmd_set_layout(struct zss_obj *img, const VkImageSubresourceRange *r, VkImageLayout layout);
void zss_cmd_track_rendering(const void *rendering);
void zss_cmd_exec_begin_rendering(struct zss_dev *dev, VkCommandBuffer cb, void *rendering);
void zss_cmd_exec_end_rendering(struct zss_dev *dev, VkCommandBuffer cb);
void zss_lower_begin_rendering(struct zss_dev *dev, VkCommandBuffer cb, const VkRenderingInfo *ri);
VkRenderPass zss_lower_compatible(struct zss_dev *dev, uint32_t ncolor, const VkFormat *colors, VkFormat depth,
                                  VkFormat stencil, VkSampleCountFlagBits samples);
void zss_lower_forget_view(struct zss_dev *dev, VkImageView view);
void zss_lower_drop(struct zss_dev *dev);
void zss_lower_abandon(struct zss_dev *dev);
bool zss_ext_emulated(const char *name);
bool zss_gpu_has_ext(const struct zss_gpu *gpu, const char *name);
void *zss_chain_without(const void *chain, VkStructureType drop1, VkStructureType drop2, void *scratch, size_t room);
void *zss_feats_for_gpu(const struct zss_gpu *gpu, const void *chain);
bool zss_inheritance_real(struct zss_dev *dev, VkCommandBufferInheritanceInfo *inh, void *scratch, size_t room);

/* swapchain.c */
void zss_swapchain_retire(struct zss_dev *dev, struct zss_obj *sc);

/* format.c: bytes of one texel block for one aspect, 0 if unknown. */
struct zss_format_info {
    uint32_t block_bytes, block_w, block_h;
};
bool zss_format_info(VkFormat f, VkImageAspectFlags aspect, struct zss_format_info *out);

/* migrate.c */
enum zss_outcome { ZO_MIGRATED, ZO_PARKED, ZO_FAILED };
/* target == NULL parks. reason receives a human-readable explanation. */
enum zss_outcome zss_migrate(struct zss_dev *dev, struct zss_gpu *target, char *reason, size_t rlen);
enum zss_outcome zss_resume(struct zss_dev *dev, struct zss_gpu *target, char *reason, size_t rlen);
bool zss_compatible(struct zss_dev *dev, struct zss_gpu *target, char *reason, size_t rlen);
/*
 * The real device is lost: rebuild on target from what is held in memory.
 * `abandon` leaves the old device alone because a thread is stuck inside it.
 * Returns ZO_MIGRATED, ZO_PARKED, or ZO_FAILED for a device that cannot be recovered.
 */
enum zss_outcome zss_recover(struct zss_dev *dev, struct zss_gpu *target, bool abandon, char *reason,
                             size_t rlen);
/*
 * For entry points: given the result of a real call, decides whether the
 * call must be repeated because the device was lost and has been replaced.
 * Called and returns inside the gate.
 */
bool zss_lost(struct zss_dev *dev, VkResult r);
void zss_dev_unrecoverable(struct zss_dev *dev);
VkDeviceSize zss_sub_size(const VkImageCreateInfo *ci, VkImageAspectFlags aspect, uint32_t mip);
void zss_inflight_done(struct zss_dev *dev, const struct zss_obj *fence);
uint32_t zss_inflight_reissue(struct zss_dev *dev);
extern uint32_t zss_submits; /* submits completed by this process, for logs and tests */
/* Repeats a real call for as long as the device it was made on turns out to be lost. */
#define ZSS_RETRY(dev, r, call) do { (r) = (call); } while (zss_lost((dev), (r)))

/* control.c */
void zss_control_start(void);
void zss_control_state_changed(void);
bool zss_control_detached(const char *pci);
bool zss_control_connected(void);
void zss_control_report_lost(struct zss_dev *dev);
/* No answer from a daemon: pick a target here. */
void zss_control_recover_local(struct zss_dev *dev);

#endif
