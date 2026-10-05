/* SPDX-License-Identifier: GPL-2.0 */
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
    X(AcquireNextImageKHR) X(QueuePresentKHR)

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
};

enum zss_kind {
    ZK_MEMORY, ZK_BUFFER, ZK_IMAGE, ZK_VIEW, ZK_SAMPLER, ZK_SHADER, ZK_RENDERPASS,
    ZK_FRAMEBUFFER, ZK_DSL, ZK_DPOOL, ZK_DSET, ZK_PLAYOUT, ZK_PCACHE, ZK_PIPELINE,
    ZK_CPOOL, ZK_CMDBUF, ZK_FENCE, ZK_SEMAPHORE, ZK_SWAPCHAIN,
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
    bool standin; /* image standing in for a retired swapchain image */
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
        } img;
        struct { VkImageViewCreateInfo ci; } view;
        struct { VkSamplerCreateInfo ci; } sampler;
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
        } cb;
        struct { bool signaled; } fence;
        struct { bool signaled; } sem;
        struct {
            VkSwapchainCreateInfoKHR ci;
            struct zss_obj **images;
            uint32_t nimages;
            bool retired;
        } sc;
    } u;
};

struct zss_queue {
    VK_LOADER_DATA ld;
    struct zss_dev *dev;
    uint32_t family, index;
    VkQueue real;
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
void zss_hold_end(void);

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
VkResult zss_util_begin(struct zss_dev *dev);
VkResult zss_util_run(struct zss_dev *dev);
VkImageAspectFlags zss_format_aspects(VkFormat f);

/* cmd.c */
void zss_cmd_reset(struct zss_obj *cb);
void zss_cmd_replay(struct zss_dev *dev, struct zss_obj *cb);
void zss_cmd_track_submit(struct zss_dev *dev, struct zss_obj *cb);

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

/* control.c */
void zss_control_start(void);
void zss_control_state_changed(void);
bool zss_control_detached(const char *pci);

#endif
