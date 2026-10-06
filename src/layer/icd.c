// SPDX-License-Identifier: GPL-2.0
/*
 * Loader-facing side: driver discovery, virtual GPUs, instance and
 * physical-device entry points, surfaces, and the entry-point table.
 */
#include "zss_layer.h"

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ZSS_MAX_DRIVERS 16

struct zss_gpu *zss_gpus[ZSS_MAX_GPUS];
int zss_ngpus;
struct zss_dev *zss_devices;
pthread_mutex_t zss_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

static struct zss_driver drivers[ZSS_MAX_DRIVERS];
static int ndrivers;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static int debug = -1;

struct zss_instance {
    VK_LOADER_DATA ld;
};

void zss_log(const char *fmt, ...)
{
    va_list ap;

    fputs("[zss] ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void zss_dbg(const char *fmt, ...)
{
    va_list ap;

    if (debug < 0)
        debug = getenv("ZSS_DEBUG") != NULL;
    if (!debug)
        return;
    fputs("[zss] ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ---- driver/PCI map kept on disk ------------------------------------- */
/*
 * Remembers which PCI devices each driver library serves, so a driver whose
 * GPU is currently detached is not loaded (loading it would poke the absent
 * hardware).
 */

static void map_path(char *out, size_t n)
{
    const char *cache = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    char dir[400];

    if (cache && *cache)
        snprintf(dir, sizeof(dir), "%s/zss", cache);
    else
        snprintf(dir, sizeof(dir), "%s/.cache/zss", home ? home : "/tmp");
    mkdir(dir, 0700);
    snprintf(out, n, "%s/icd-map", dir);
}

static bool map_lib_is_detached(const char *lib)
{
    char path[512], line[512];
    bool hit = false;
    FILE *f;

    map_path(path, sizeof(path));
    f = fopen(path, "r");
    if (!f)
        return false;
    while (fgets(line, sizeof(line), f)) {
        char *tab = strchr(line, '\t');

        if (!tab)
            continue;
        *tab++ = '\0';
        tab[strcspn(tab, "\n")] = '\0';
        if (!strcmp(line, lib) && zss_control_detached(tab))
            hit = true;
    }
    fclose(f);
    return hit;
}

static void map_update(const char *lib)
{
    char path[512], tmp[520], line[512];
    FILE *in, *out;

    map_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.%d", path, (int)getpid());
    out = fopen(tmp, "w");
    if (!out)
        return;
    in = fopen(path, "r");
    if (in) {
        while (fgets(line, sizeof(line), in)) {
            size_t n = strlen(lib);

            if (strncmp(line, lib, n) || line[n] != '\t')
                fputs(line, out);
        }
        fclose(in);
    }
    for (int i = 0; i < zss_ngpus; i++)
        if (!strcmp(zss_gpus[i]->drv->lib, lib) && zss_gpus[i]->pci[0] && !zss_gpus[i]->test_bound)
            fprintf(out, "%s\t%s\n", lib, zss_gpus[i]->pci);
    fclose(out);
    rename(tmp, path);
}

/* ---- real drivers ----------------------------------------------------- */

static bool has_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name))
            return true;
    return false;
}

static void pci_from_sysfs(struct zss_gpu *gpu)
{
    DIR *d = opendir("/sys/bus/pci/devices");
    struct dirent *de;
    char found[ZSS_PCI_LEN] = "";
    int matches = 0;

    if (!d)
        return;
    while ((de = readdir(d))) {
        char path[300];
        unsigned vendor = 0, device = 0, class = 0;
        FILE *f;

        if (de->d_name[0] == '.')
            continue;
#define READ_HEX(file, var) \
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/" file, de->d_name); \
        f = fopen(path, "r"); \
        if (f) { if (fscanf(f, "%x", &var) != 1) var = 0; fclose(f); }
        READ_HEX("vendor", vendor)
        READ_HEX("device", device)
        READ_HEX("class", class)
#undef READ_HEX
        if ((class >> 16) == 0x03 && vendor == gpu->props.vendorID && device == gpu->props.deviceID) {
            snprintf(found, sizeof(found), "%.15s", de->d_name);
            matches++;
        }
    }
    closedir(d);
    if (matches == 1)
        snprintf(gpu->pci, sizeof(gpu->pci), "%s", found);
}

static void gpu_fill(struct zss_driver *drv, struct zss_gpu *gpu, VkPhysicalDevice pd)
{
    PFN_vkGetPhysicalDeviceProperties2 props2 =
        drv->fn.GetPhysicalDeviceProperties2 ? drv->fn.GetPhysicalDeviceProperties2
                                             : drv->fn.GetPhysicalDeviceProperties2KHR;

    gpu->drv = drv;
    gpu->real = pd;
    gpu->hold_fd = -1;
    set_loader_magic_value(gpu);
    drv->fn.GetPhysicalDeviceProperties(pd, &gpu->props);
    drv->fn.GetPhysicalDeviceFeatures(pd, &gpu->features);
    drv->fn.GetPhysicalDeviceMemoryProperties(pd, &gpu->mem);
    gpu->nfam = ZSS_MAX_FAMILIES;
    drv->fn.GetPhysicalDeviceQueueFamilyProperties(pd, &gpu->nfam, gpu->fam);
    gpu->software = gpu->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;

    drv->fn.EnumerateDeviceExtensionProperties(pd, NULL, &gpu->next, NULL);
    gpu->ext = calloc(gpu->next ? gpu->next : 1, sizeof(*gpu->ext));
    drv->fn.EnumerateDeviceExtensionProperties(pd, NULL, &gpu->next, gpu->ext);

    if (props2 && has_ext(gpu->ext, gpu->next, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME)) {
        VkPhysicalDevicePCIBusInfoPropertiesEXT pci = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT,
        };
        VkPhysicalDeviceProperties2 p2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &pci,
        };

        props2(pd, &p2);
        snprintf(gpu->pci, sizeof(gpu->pci), "%04x:%02x:%02x.%x", pci.pciDomain, pci.pciBus,
                 pci.pciDevice, pci.pciFunction);
    } else if (!gpu->software) {
        pci_from_sysfs(gpu);
    }
}

static bool gpu_matches(const struct zss_gpu *gpu, const VkPhysicalDeviceProperties *p)
{
    return gpu->props.vendorID == p->vendorID && gpu->props.deviceID == p->deviceID &&
           !memcmp(gpu->props.pipelineCacheUUID, p->pipelineCacheUUID, VK_UUID_SIZE);
}

/* Binds the driver's physical devices to our GPUs, creating them on first sight. */
static void driver_enumerate(struct zss_driver *drv, bool discover)
{
    VkPhysicalDevice pds[ZSS_MAX_GPUS];
    uint32_t n = ZSS_MAX_GPUS;
    bool used[ZSS_MAX_GPUS] = { false };

    if (drv->fn.EnumeratePhysicalDevices(drv->inst, &n, pds) < 0)
        n = 0;

    if (discover) {
        for (uint32_t i = 0; i < n && zss_ngpus < ZSS_MAX_GPUS; i++) {
            struct zss_gpu *gpu = calloc(1, sizeof(*gpu));

            gpu_fill(drv, gpu, pds[i]);
            zss_gpus[zss_ngpus++] = gpu;
        }
        return;
    }
    for (int g = 0; g < zss_ngpus; g++) {
        struct zss_gpu *gpu = zss_gpus[g];

        if (gpu->drv != drv)
            continue;
        gpu->real = VK_NULL_HANDLE;
        for (uint32_t i = 0; i < n; i++) {
            VkPhysicalDeviceProperties p;

            /* A test-bound clone shares the physical device of its original. */
            if (used[i] && !gpu->test_bound)
                continue;
            drv->fn.GetPhysicalDeviceProperties(pds[i], &p);
            if (gpu_matches(gpu, &p)) {
                gpu->real = pds[i];
                used[i] = !gpu->test_bound;
                break;
            }
        }
    }
}

/*
 * Some drivers (NVIDIA's for one) keep their device files open for as long
 * as the library is loaded, whatever happens to the instance. Releasing a
 * GPU therefore means unloading its driver, and using it again means loading
 * it afresh.
 */
static bool driver_load(struct zss_driver *drv)
{
    VkResult (*negotiate)(uint32_t *);
    uint32_t version = 5;

    if (drv->dl)
        return true;
    drv->dl = dlopen(drv->lib, RTLD_NOW | RTLD_LOCAL);
    if (!drv->dl) {
        zss_dbg("cannot load %s: %s", drv->lib, dlerror());
        return false;
    }
    negotiate = (VkResult (*)(uint32_t *))dlsym(drv->dl, "vk_icdNegotiateLoaderICDInterfaceVersion");
    drv->gipa = (PFN_vkGetInstanceProcAddr)dlsym(drv->dl, "vk_icdGetInstanceProcAddr");
    if (!drv->gipa)
        drv->gipa = (PFN_vkGetInstanceProcAddr)dlsym(drv->dl, "vkGetInstanceProcAddr");
    if (!drv->gipa || (negotiate && negotiate(&version) != VK_SUCCESS)) {
        dlclose(drv->dl);
        drv->dl = NULL;
        drv->gipa = NULL;
        return false;
    }
    return true;
}

VkResult zss_driver_open(struct zss_driver *drv)
{
    PFN_vkEnumerateInstanceExtensionProperties enum_ext;
    PFN_vkEnumerateInstanceVersion enum_ver;
    PFN_vkCreateInstance create;
    VkExtensionProperties ext[128];
    uint32_t next = 128, napi = VK_API_VERSION_1_0, nenabled = 0;
    const char *enabled[8];
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "zss" };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkResult r;

    if (drv->inst)
        return VK_SUCCESS;
    if (!driver_load(drv))
        return VK_ERROR_INCOMPATIBLE_DRIVER;

    enum_ext = (PFN_vkEnumerateInstanceExtensionProperties)drv->gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    enum_ver = (PFN_vkEnumerateInstanceVersion)drv->gipa(NULL, "vkEnumerateInstanceVersion");
    create = (PFN_vkCreateInstance)drv->gipa(NULL, "vkCreateInstance");
    if (!create)
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    if (!enum_ext || enum_ext(NULL, &next, ext) < 0)
        next = 0;
    if (enum_ver && enum_ver(&napi) == VK_SUCCESS && napi >= VK_API_VERSION_1_1)
        app.apiVersion = VK_API_VERSION_1_1;
    else
        app.apiVersion = VK_API_VERSION_1_0;

    drv->has_surface = has_ext(ext, next, VK_KHR_SURFACE_EXTENSION_NAME);
    drv->has_xcb = has_ext(ext, next, VK_KHR_XCB_SURFACE_EXTENSION_NAME);
    drv->has_xlib = has_ext(ext, next, VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
    drv->has_wayland = has_ext(ext, next, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
    if (drv->has_surface)
        enabled[nenabled++] = VK_KHR_SURFACE_EXTENSION_NAME;
    if (drv->has_xcb)
        enabled[nenabled++] = VK_KHR_XCB_SURFACE_EXTENSION_NAME;
    if (drv->has_xlib)
        enabled[nenabled++] = VK_KHR_XLIB_SURFACE_EXTENSION_NAME;
    if (drv->has_wayland)
        enabled[nenabled++] = VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
    if (app.apiVersion == VK_API_VERSION_1_0 &&
        has_ext(ext, next, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
        enabled[nenabled++] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    ci.enabledExtensionCount = nenabled;
    ci.ppEnabledExtensionNames = enabled;

    r = create(&ci, NULL, &drv->inst);
    if (r != VK_SUCCESS) {
        drv->inst = VK_NULL_HANDLE;
        return r;
    }
#define X(n) drv->fn.n = (PFN_vk##n)drv->gipa(drv->inst, "vk" #n);
    ZSS_INST_FNS(X)
#undef X
    memset(drv->surfaces, 0, sizeof(drv->surfaces));
    driver_enumerate(drv, false);
    zss_dbg("opened driver %s", drv->lib);
    return VK_SUCCESS;
}

static void x_link_close(VkIcdWsiPlatform platform, void *link);

void zss_driver_close(struct zss_driver *drv)
{
    if (!drv->inst || drv->ndevices > 0)
        return;
    for (int i = 0; i < ZSS_MAX_SURFACES; i++)
        if (drv->surfaces[i].owned && drv->fn.DestroySurfaceKHR)
            drv->fn.DestroySurfaceKHR(drv->inst, drv->surfaces[i].real, NULL);
    drv->fn.DestroyInstance(drv->inst, NULL);
    /* Only now, with the driver done, are its connections to the display server closed. */
    for (int i = 0; i < ZSS_MAX_SURFACES; i++)
        x_link_close(drv->surfaces[i].platform, drv->surfaces[i].link);
    memset(drv->surfaces, 0, sizeof(drv->surfaces));
    drv->inst = VK_NULL_HANDLE;
    for (int g = 0; g < zss_ngpus; g++)
        if (zss_gpus[g]->drv == drv)
            zss_gpus[g]->real = VK_NULL_HANDLE;
    dlclose(drv->dl);
    drv->dl = NULL;
    drv->gipa = NULL;
    zss_dbg("closed driver %s", drv->lib);
}

void zss_gpu_hold(struct zss_gpu *gpu, bool hold)
{
    char path[300], node[300];
    struct dirent *de;
    DIR *d;

    if (!gpu->test_bound)
        return;
    if (!hold) {
        if (gpu->hold_fd >= 0)
            close(gpu->hold_fd);
        gpu->hold_fd = -1;
        return;
    }
    if (gpu->hold_fd >= 0)
        return;
    /* Stand in for the fds a real driver would keep on its device. */
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/drm", gpu->pci);
    d = opendir(path);
    if (!d)
        return;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "card", 4))
            continue;
        snprintf(node, sizeof(node), "/dev/dri/%s", de->d_name);
        gpu->hold_fd = open(node, O_RDWR | O_CLOEXEC);
        break;
    }
    closedir(d);
}

bool zss_gpu_held(const struct zss_gpu *gpu)
{
    if (gpu->test_bound)
        return gpu->hold_fd >= 0;
    return gpu->drv->inst != VK_NULL_HANDLE && !gpu->software;
}

VkPhysicalDevice zss_gpu_real(struct zss_gpu *gpu)
{
    if (gpu->detached)
        return VK_NULL_HANDLE;
    if (!gpu->drv->inst) {
        if (zss_driver_open(gpu->drv) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        zss_control_state_changed();
    }
    if (gpu->test_bound && gpu->hold_fd < 0) {
        zss_gpu_hold(gpu, true);
        zss_control_state_changed();
    }
    return gpu->real;
}

struct zss_gpu *zss_gpu_by_pci(const char *pci)
{
    for (int i = 0; i < zss_ngpus; i++)
        if (!strcmp(zss_gpus[i]->pci, pci))
            return zss_gpus[i];
    return NULL;
}

/*
 * After a migration the application still holds the GPU it started on, but
 * questions about formats and surfaces must be answered by the GPU it is
 * actually rendering on.
 */
static struct zss_gpu *gpu_effective(struct zss_gpu *gpu)
{
    struct zss_gpu *eff = gpu;

    pthread_mutex_lock(&zss_lock);
    for (struct zss_dev *d = zss_devices; d; d = d->next_dev)
        if (d->origin == gpu && d->gpu && d->gpu != gpu)
            eff = d->gpu;
    pthread_mutex_unlock(&zss_lock);
    return eff;
}

static void driver_add(const char *lib)
{
    struct zss_driver *drv;

    for (int i = 0; i < ndrivers; i++)
        if (!strcmp(drivers[i].lib, lib))
            return;
    if (ndrivers == ZSS_MAX_DRIVERS || strstr(lib, "zss_vk"))
        return;
    if (map_lib_is_detached(lib)) {
        zss_dbg("skipping %s: its GPU is detached", lib);
        return;
    }

    drv = &drivers[ndrivers];
    memset(drv, 0, sizeof(*drv));
    snprintf(drv->lib, sizeof(drv->lib), "%s", lib);
    if (!driver_load(drv))
        return;
    ndrivers++;

    if (zss_driver_open(drv) != VK_SUCCESS) {
        zss_dbg("driver %s has no usable instance", lib);
        ndrivers--;
        return;
    }
    driver_enumerate(drv, true);
    map_update(lib);
    /* Opened only to look; reopened when a GPU of this driver is used. */
    zss_driver_close(drv);
}

static void manifest_add(const char *path)
{
    char text[4096], lib[512];
    const char *p, *q;
    size_t n;
    FILE *f = fopen(path, "r");

    if (!f)
        return;
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';

    p = strstr(text, "\"library_path\"");
    if (!p || !(p = strchr(p + 14, ':')) || !(p = strchr(p, '"')))
        return;
    q = ++p;
    n = 0;
    while (*q && *q != '"' && n < sizeof(lib) - 1) {
        if (*q == '\\' && q[1])
            q++;
        lib[n++] = *q++;
    }
    lib[n] = '\0';

    if (lib[0] != '/' && strchr(lib, '/')) {
        char full[800];
        const char *slash = strrchr(path, '/');

        snprintf(full, sizeof(full), "%.*s/%s", (int)(slash ? slash - path : 0), path, lib);
        driver_add(full);
    } else {
        driver_add(lib);
    }
}

static void manifests_scan(const char *dir)
{
    struct dirent **list;
    int n = scandir(dir, &list, NULL, alphasort);

    for (int i = 0; i < n; i++) {
        size_t len = strlen(list[i]->d_name);
        char path[800];

        if (len > 5 && !strcmp(list[i]->d_name + len - 5, ".json")) {
            snprintf(path, sizeof(path), "%s/%s", dir, list[i]->d_name);
            manifest_add(path);
        }
        free(list[i]);
    }
    if (n >= 0)
        free(list);
}

static int gpu_rank(const struct zss_gpu *g)
{
    switch (g->props.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 0;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 1;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
    default: return g->test_bound ? 3 : 4;
    }
}

static void zss_init(void)
{
    const char *list = getenv("ZSS_REAL_DRIVER_FILES");
    const char *bind = getenv("ZSS_BIND_PCI");

    zss_control_start();

    if (list && *list) {
        char *copy = strdup(list), *save = NULL;

        for (char *tok = strtok_r(copy, ":", &save); tok; tok = strtok_r(NULL, ":", &save))
            manifest_add(tok);
        free(copy);
    } else {
        const char *home = getenv("HOME");
        char user[512];

        manifests_scan("/etc/vulkan/icd.d");
        manifests_scan("/usr/local/share/vulkan/icd.d");
        manifests_scan("/usr/share/vulkan/icd.d");
        if (home) {
            snprintf(user, sizeof(user), "%s/.local/share/vulkan/icd.d", home);
            manifests_scan(user);
        }
    }

    /* Test aid: let the software renderer pose as the given PCI device. */
    if (bind && *bind) {
        for (int i = 0; i < zss_ngpus && zss_ngpus < ZSS_MAX_GPUS; i++) {
            struct zss_gpu *sw = zss_gpus[i], *clone;
            size_t len;

            if (!sw->software)
                continue;
            clone = calloc(1, sizeof(*clone));
            *clone = *sw;
            clone->ext = calloc(sw->next ? sw->next : 1, sizeof(*sw->ext));
            memcpy(clone->ext, sw->ext, sw->next * sizeof(*sw->ext));
            clone->test_bound = true;
            clone->software = false;
            clone->detached = zss_control_detached(bind);
            snprintf(clone->pci, sizeof(clone->pci), "%s", bind);
            len = strlen(clone->props.deviceName);
            snprintf(clone->props.deviceName + len, sizeof(clone->props.deviceName) - len,
                     " [ZSS %s]", bind);
            zss_gpus[zss_ngpus++] = clone;
            break;
        }
    }

    for (int i = 1; i < zss_ngpus; i++) {
        struct zss_gpu *g = zss_gpus[i];
        int j = i;

        for (; j > 0 && gpu_rank(zss_gpus[j - 1]) > gpu_rank(g); j--)
            zss_gpus[j] = zss_gpus[j - 1];
        zss_gpus[j] = g;
    }
    for (int i = 0; i < zss_ngpus; i++)
        zss_dbg("gpu %d: %s pci=%s driver=%s", i, zss_gpus[i]->props.deviceName,
                zss_gpus[i]->pci[0] ? zss_gpus[i]->pci : "-", zss_gpus[i]->drv->lib);
    zss_control_state_changed();
}

/* ---- surfaces --------------------------------------------------------- */

/*
 * A driver is not given the application's connection to the X server but one
 * of the layer's own, to the same server. The server keeps state for every
 * client of a driver and tears it down, calling into that driver, when the
 * client disconnects. With the application's connection that happens when the
 * application exits, which may be long after its GPU was powered off, and the
 * call then wakes the GPU. With a connection of its own the layer disconnects
 * as it releases the driver, while the device is still there.
 *
 * libxcb and libX11 are looked up at run time: whichever the application uses
 * is already loaded, and the layer must not require either.
 */
static void *x_link_open(VkIcdWsiPlatform platform, void *native)
{
    if (platform == VK_ICD_WSI_PLATFORM_XCB) {
        void *lib = dlopen("libxcb.so.1", RTLD_NOW | RTLD_GLOBAL);
        void *(*connect)(const char *, int *) = lib ? (void *(*)(const char *, int *))dlsym(lib, "xcb_connect") : NULL;
        int (*has_error)(void *) = lib ? (int (*)(void *))dlsym(lib, "xcb_connection_has_error") : NULL;
        void (*disconnect)(void *) = lib ? (void (*)(void *))dlsym(lib, "xcb_disconnect") : NULL;
        void *c = connect && has_error && disconnect ? connect(NULL, NULL) : NULL;

        if (c && has_error(c)) {
            disconnect(c);
            c = NULL;
        }
        return c;
    }
    if (platform == VK_ICD_WSI_PLATFORM_XLIB) {
        void *lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
        void *(*open_display)(const char *) = lib ? (void *(*)(const char *))dlsym(lib, "XOpenDisplay") : NULL;
        char *(*display_string)(void *) = lib ? (char *(*)(void *))dlsym(lib, "XDisplayString") : NULL;

        return open_display && display_string ? open_display(display_string(native)) : NULL;
    }
    return NULL;
}

static void x_link_close(VkIcdWsiPlatform platform, void *link)
{
    void *lib;

    if (!link)
        return;
    if (platform == VK_ICD_WSI_PLATFORM_XCB && (lib = dlopen("libxcb.so.1", RTLD_NOW | RTLD_NOLOAD))) {
        void (*disconnect)(void *) = (void (*)(void *))dlsym(lib, "xcb_disconnect");

        if (disconnect)
            disconnect(link);
        dlclose(lib);
    } else if (platform == VK_ICD_WSI_PLATFORM_XLIB && (lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_NOLOAD))) {
        int (*close_display)(void *) = (int (*)(void *))dlsym(lib, "XCloseDisplay");

        if (close_display)
            close_display(link);
        dlclose(lib);
    }
}

VkResult zss_surface_real(struct zss_driver *drv, VkSurfaceKHR outer, VkSurfaceKHR *real)
{
    VkIcdSurfaceBase *base = (VkIcdSurfaceBase *)(uintptr_t)outer;
    struct zss_surface want = { .outer = outer, .platform = base->platform }, *s = NULL;
    VkResult r = VK_SUCCESS;

    switch (base->platform) {
    case VK_ICD_WSI_PLATFORM_XCB:
        want.native[0] = (uintptr_t)((VkIcdSurfaceXcb *)base)->connection;
        want.native[1] = (uintptr_t)((VkIcdSurfaceXcb *)base)->window;
        break;
    case VK_ICD_WSI_PLATFORM_XLIB:
        want.native[0] = (uintptr_t)((VkIcdSurfaceXlib *)base)->dpy;
        want.native[1] = (uintptr_t)((VkIcdSurfaceXlib *)base)->window;
        break;
    case VK_ICD_WSI_PLATFORM_WAYLAND:
        want.native[0] = (uintptr_t)((VkIcdSurfaceWayland *)base)->display;
        want.native[1] = (uintptr_t)((VkIcdSurfaceWayland *)base)->surface;
        break;
    default:
        return VK_ERROR_SURFACE_LOST_KHR;
    }

    pthread_mutex_lock(&zss_lock);
    for (int i = 0; i < ZSS_MAX_SURFACES; i++) {
        struct zss_surface *c = &drv->surfaces[i];

        if (c->outer == outer) {
            if (c->platform == want.platform && c->native[0] == want.native[0] &&
                c->native[1] == want.native[1]) {
                *real = c->real;
                goto out;
            }
            /* The loader reused the address for a different window. */
            if (c->owned && drv->fn.DestroySurfaceKHR)
                drv->fn.DestroySurfaceKHR(drv->inst, c->real, NULL);
            x_link_close(c->platform, c->link);
            memset(c, 0, sizeof(*c));
        }
        if (!s && !c->outer)
            s = c;
    }
    if (!s) {
        r = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto out;
    }

    want.real = outer; /* drivers without their own constructor take the loader's struct */
    if (base->platform == VK_ICD_WSI_PLATFORM_XCB && drv->fn.CreateXcbSurfaceKHR) {
        VkXcbSurfaceCreateInfoKHR ci = {
            .sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
            .connection = (void *)want.native[0],
            .window = (xcb_window_t)want.native[1],
        };

        want.link = x_link_open(base->platform, (void *)want.native[0]);
        if (want.link)
            ci.connection = want.link;

        r = drv->fn.CreateXcbSurfaceKHR(drv->inst, &ci, NULL, &want.real);
        want.owned = true;
    } else if (base->platform == VK_ICD_WSI_PLATFORM_XLIB && drv->fn.CreateXlibSurfaceKHR) {
        VkXlibSurfaceCreateInfoKHR ci = {
            .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
            .dpy = (void *)want.native[0],
            .window = (Window)want.native[1],
        };

        want.link = x_link_open(base->platform, (void *)want.native[0]);
        if (want.link)
            ci.dpy = want.link;

        r = drv->fn.CreateXlibSurfaceKHR(drv->inst, &ci, NULL, &want.real);
        want.owned = true;
    } else if (base->platform == VK_ICD_WSI_PLATFORM_WAYLAND && drv->fn.CreateWaylandSurfaceKHR) {
        VkWaylandSurfaceCreateInfoKHR ci = {
            .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
            .display = (void *)want.native[0],
            .surface = (void *)want.native[1],
        };

        r = drv->fn.CreateWaylandSurfaceKHR(drv->inst, &ci, NULL, &want.real);
        want.owned = true;
    }
    if (r == VK_SUCCESS) {
        *s = want;
        *real = want.real;
    } else {
        x_link_close(want.platform, want.link);
    }
out:
    pthread_mutex_unlock(&zss_lock);
    return r;
}

/*
 * The layer creates surfaces itself rather than relying on the loader's
 * built-in ones: each is one of the loader's own VkIcdSurface structs, so
 * zss_surface_real can read the window back out of it for any driver.
 */
static VKAPI_ATTR VkResult VKAPI_CALL zss_CreateXcbSurfaceKHR(VkInstance instance,
                                                              const VkXcbSurfaceCreateInfoKHR *ci,
                                                              const VkAllocationCallbacks *alloc,
                                                              VkSurfaceKHR *out)
{
    VkIcdSurfaceXcb *s = calloc(1, sizeof(*s));

    (void)instance;
    (void)alloc;
    if (!s)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->base.platform = VK_ICD_WSI_PLATFORM_XCB;
    s->connection = ci->connection;
    s->window = ci->window;
    *out = (VkSurfaceKHR)(uintptr_t)s;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL zss_CreateXlibSurfaceKHR(VkInstance instance,
                                                               const VkXlibSurfaceCreateInfoKHR *ci,
                                                               const VkAllocationCallbacks *alloc,
                                                               VkSurfaceKHR *out)
{
    VkIcdSurfaceXlib *s = calloc(1, sizeof(*s));

    (void)instance;
    (void)alloc;
    if (!s)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->base.platform = VK_ICD_WSI_PLATFORM_XLIB;
    s->dpy = ci->dpy;
    s->window = ci->window;
    *out = (VkSurfaceKHR)(uintptr_t)s;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL zss_CreateWaylandSurfaceKHR(
    VkInstance instance, const VkWaylandSurfaceCreateInfoKHR *ci, const VkAllocationCallbacks *alloc,
    VkSurfaceKHR *out)
{
    VkIcdSurfaceWayland *s = calloc(1, sizeof(*s));

    (void)instance;
    (void)alloc;
    if (!s)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->base.platform = VK_ICD_WSI_PLATFORM_WAYLAND;
    s->display = ci->display;
    s->surface = ci->surface;
    *out = (VkSurfaceKHR)(uintptr_t)s;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL zss_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                                                        const VkAllocationCallbacks *alloc)
{
    (void)instance;
    (void)alloc;
    if (!surface)
        return;
    zss_enter();
    pthread_mutex_lock(&zss_lock);
    for (int d = 0; d < ndrivers; d++) {
        for (int i = 0; i < ZSS_MAX_SURFACES; i++) {
            struct zss_surface *c = &drivers[d].surfaces[i];

            if (c->outer != surface)
                continue;
            if (c->owned && drivers[d].inst && drivers[d].fn.DestroySurfaceKHR)
                drivers[d].fn.DestroySurfaceKHR(drivers[d].inst, c->real, NULL);
            x_link_close(c->platform, c->link);
            memset(c, 0, sizeof(*c));
        }
    }
    pthread_mutex_unlock(&zss_lock);
    free((void *)(uintptr_t)surface);
    zss_leave();
}

/* ---- instance and physical-device entry points ------------------------- */

static const char *const instance_exts[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
    VK_KHR_XCB_SURFACE_EXTENSION_NAME,
    VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
    VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
};

static VkResult fill_exts(const char *const *names, uint32_t have, uint32_t *count,
                          VkExtensionProperties *props)
{
    uint32_t n;

    if (!props) {
        *count = have;
        return VK_SUCCESS;
    }
    n = *count < have ? *count : have;
    for (uint32_t i = 0; i < n; i++) {
        memset(&props[i], 0, sizeof(props[i]));
        snprintf(props[i].extensionName, sizeof(props[i].extensionName), "%s", names[i]);
        props[i].specVersion = 1;
    }
    *count = n;
    return n < have ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL zss_EnumerateInstanceExtensionProperties(
    const char *layer, uint32_t *count, VkExtensionProperties *props)
{
    if (layer)
        return VK_ERROR_LAYER_NOT_PRESENT;
    return fill_exts(instance_exts, 4, count, props);
}

static VKAPI_ATTR VkResult VKAPI_CALL zss_CreateInstance(const VkInstanceCreateInfo *ci,
                                                         const VkAllocationCallbacks *alloc,
                                                         VkInstance *out)
{
    struct zss_instance *inst = calloc(1, sizeof(*inst));

    (void)ci;
    (void)alloc;
    if (!inst)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    set_loader_magic_value(inst);
    pthread_once(&init_once, zss_init);
    *out = (VkInstance)inst;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL zss_DestroyInstance(VkInstance instance,
                                                      const VkAllocationCallbacks *alloc)
{
    (void)alloc;
    free(instance);
}

static VKAPI_ATTR VkResult VKAPI_CALL zss_EnumeratePhysicalDevices(VkInstance instance,
                                                                   uint32_t *count,
                                                                   VkPhysicalDevice *out)
{
    uint32_t have = 0, n = 0;

    (void)instance;
    zss_enter();
    for (int i = 0; i < zss_ngpus; i++)
        if (!zss_gpus[i]->detached)
            have++;
    if (!out) {
        *count = have;
        zss_leave();
        return VK_SUCCESS;
    }
    for (int i = 0; i < zss_ngpus && n < *count; i++)
        if (!zss_gpus[i]->detached)
            out[n++] = (VkPhysicalDevice)zss_gpus[i];
    *count = n;
    zss_leave();
    return n < have ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL zss_GetPhysicalDeviceProperties(VkPhysicalDevice pd,
                                                                  VkPhysicalDeviceProperties *p)
{
    *p = ((struct zss_gpu *)pd)->props;
    /* Only the Vulkan 1.0 core is tracked, so that is what the device claims. */
    p->apiVersion = VK_API_VERSION_1_0;
}

static VKAPI_ATTR void VKAPI_CALL zss_GetPhysicalDeviceFeatures(VkPhysicalDevice pd,
                                                                VkPhysicalDeviceFeatures *f)
{
    *f = ((struct zss_gpu *)pd)->features;
    f->sparseBinding = f->sparseResidencyBuffer = f->sparseResidencyImage2D = VK_FALSE;
    f->sparseResidencyImage3D = f->sparseResidency2Samples = f->sparseResidency4Samples = VK_FALSE;
    f->sparseResidency8Samples = f->sparseResidency16Samples = f->sparseResidencyAliased = VK_FALSE;
}

static VKAPI_ATTR void VKAPI_CALL zss_GetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *m)
{
    *m = ((struct zss_gpu *)pd)->mem;
}

static VKAPI_ATTR void VKAPI_CALL zss_GetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice pd, uint32_t *count, VkQueueFamilyProperties *props)
{
    struct zss_gpu *gpu = (struct zss_gpu *)pd;
    uint32_t n;

    if (!props) {
        *count = gpu->nfam;
        return;
    }
    n = *count < gpu->nfam ? *count : gpu->nfam;
    for (uint32_t i = 0; i < n; i++) {
        props[i] = gpu->fam[i];
        props[i].queueFlags &= VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    }
    *count = n;
}

static VKAPI_ATTR void VKAPI_CALL zss_GetPhysicalDeviceFormatProperties(VkPhysicalDevice pd,
                                                                        VkFormat format,
                                                                        VkFormatProperties *p)
{
    struct zss_gpu *gpu;
    VkPhysicalDevice real;

    zss_enter();
    gpu = gpu_effective((struct zss_gpu *)pd);
    real = zss_gpu_real(gpu);
    memset(p, 0, sizeof(*p));
    if (real)
        gpu->drv->fn.GetPhysicalDeviceFormatProperties(real, format, p);
    zss_leave();
}

static VKAPI_ATTR VkResult VKAPI_CALL zss_GetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice pd, VkFormat format, VkImageType type, VkImageTiling tiling,
    VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties *p)
{
    struct zss_gpu *gpu;
    VkPhysicalDevice real;
    VkResult r = VK_ERROR_FORMAT_NOT_SUPPORTED;

    zss_enter();
    gpu = gpu_effective((struct zss_gpu *)pd);
    real = zss_gpu_real(gpu);
    if (real)
        r = gpu->drv->fn.GetPhysicalDeviceImageFormatProperties(real, format, type, tiling, usage,
                                                                 flags, p);
    zss_leave();
    return r;
}

static VKAPI_ATTR void VKAPI_CALL zss_GetPhysicalDeviceSparseImageFormatProperties(
    VkPhysicalDevice pd, VkFormat format, VkImageType type, VkSampleCountFlagBits samples,
    VkImageUsageFlags usage, VkImageTiling tiling, uint32_t *count,
    VkSparseImageFormatProperties *props)
{
    (void)pd; (void)format; (void)type; (void)samples; (void)usage; (void)tiling; (void)props;
    *count = 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL zss_EnumerateDeviceExtensionProperties(
    VkPhysicalDevice pd, const char *layer, uint32_t *count, VkExtensionProperties *props)
{
    static const char *const names[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    struct zss_gpu *gpu = (struct zss_gpu *)pd;

    if (layer)
        return VK_ERROR_LAYER_NOT_PRESENT;
    return fill_exts(names, has_ext(gpu->ext, gpu->next, VK_KHR_SWAPCHAIN_EXTENSION_NAME) ? 1 : 0,
                     count, props);
}

/* Resolves the GPU and real surface a surface query should go to. */
static VkResult surface_target(VkPhysicalDevice pd, VkSurfaceKHR surface, struct zss_gpu **gpu,
                               VkPhysicalDevice *real, VkSurfaceKHR *rsurf)
{
    VkResult r;

    *gpu = gpu_effective((struct zss_gpu *)pd);
    *real = zss_gpu_real(*gpu);
    if (!*real)
        return VK_ERROR_SURFACE_LOST_KHR;
    r = zss_surface_real((*gpu)->drv, surface, rsurf);
    if (r != VK_SUCCESS)
        zss_dbg("no real surface on %s (VkResult %d, platform %d)", (*gpu)->props.deviceName, r,
                (int)((VkIcdSurfaceBase *)(uintptr_t)surface)->platform);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL zss_GetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice pd, uint32_t family, VkSurfaceKHR surface, VkBool32 *supported)
{
    struct zss_gpu *gpu;
    VkPhysicalDevice real;
    VkSurfaceKHR rsurf;
    VkResult r;

    zss_enter();
    r = surface_target(pd, surface, &gpu, &real, &rsurf);
    if (r == VK_SUCCESS && gpu != (struct zss_gpu *)pd) {
        /* Family numbers belong to the origin GPU; presentation follows the device. */
        *supported = VK_TRUE;
    } else if (r == VK_SUCCESS) {
        r = gpu->drv->fn.GetPhysicalDeviceSurfaceSupportKHR(real, family, rsurf, supported);
    }
    zss_leave();
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL zss_GetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice pd, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *caps)
{
    struct zss_gpu *gpu;
    VkPhysicalDevice real;
    VkSurfaceKHR rsurf;
    VkResult r;

    zss_enter();
    r = surface_target(pd, surface, &gpu, &real, &rsurf);
    if (r == VK_SUCCESS)
        r = gpu->drv->fn.GetPhysicalDeviceSurfaceCapabilitiesKHR(real, rsurf, caps);
    zss_leave();
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL zss_GetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice pd, VkSurfaceKHR surface, uint32_t *count, VkSurfaceFormatKHR *formats)
{
    struct zss_gpu *gpu;
    VkPhysicalDevice real;
    VkSurfaceKHR rsurf;
    VkResult r;

    zss_enter();
    r = surface_target(pd, surface, &gpu, &real, &rsurf);
    if (r == VK_SUCCESS)
        r = gpu->drv->fn.GetPhysicalDeviceSurfaceFormatsKHR(real, rsurf, count, formats);
    zss_leave();
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL zss_GetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice pd, VkSurfaceKHR surface, uint32_t *count, VkPresentModeKHR *modes)
{
    struct zss_gpu *gpu;
    VkPhysicalDevice real;
    VkSurfaceKHR rsurf;
    VkResult r;

    zss_enter();
    r = surface_target(pd, surface, &gpu, &real, &rsurf);
    if (r == VK_SUCCESS)
        r = gpu->drv->fn.GetPhysicalDeviceSurfacePresentModesKHR(real, rsurf, count, modes);
    zss_leave();
    return r;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL zss_PresentationSupport(void)
{
    return VK_TRUE;
}

/* ---- entry-point table ------------------------------------------------- */

struct zss_entry {
    const char *name;
    PFN_vkVoidFunction fn;
};

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL zss_GetDeviceProcAddr(VkDevice device,
                                                                      const char *name);

static const struct zss_entry device_entries[] = {
#define X(n) { "vk" #n, (PFN_vkVoidFunction)zss_##n },
    ZSS_DEV_FNS(X)
#undef X
    { "vkGetDeviceProcAddr", (PFN_vkVoidFunction)zss_GetDeviceProcAddr },
};

static const struct zss_entry instance_entries[] = {
#define E(n) { "vk" #n, (PFN_vkVoidFunction)zss_##n }
    E(CreateInstance), E(DestroyInstance), E(EnumerateInstanceExtensionProperties),
    E(EnumeratePhysicalDevices), E(GetPhysicalDeviceProperties), E(GetPhysicalDeviceFeatures),
    E(GetPhysicalDeviceMemoryProperties), E(GetPhysicalDeviceQueueFamilyProperties),
    E(GetPhysicalDeviceFormatProperties), E(GetPhysicalDeviceImageFormatProperties),
    E(GetPhysicalDeviceSparseImageFormatProperties), E(EnumerateDeviceExtensionProperties),
    E(CreateDevice), E(GetPhysicalDeviceSurfaceSupportKHR),
    E(GetPhysicalDeviceSurfaceCapabilitiesKHR), E(GetPhysicalDeviceSurfaceFormatsKHR),
    E(GetPhysicalDeviceSurfacePresentModesKHR), E(CreateXcbSurfaceKHR), E(CreateXlibSurfaceKHR),
    E(CreateWaylandSurfaceKHR), E(DestroySurfaceKHR),
#undef E
    { "vkGetPhysicalDeviceXcbPresentationSupportKHR", (PFN_vkVoidFunction)zss_PresentationSupport },
    { "vkGetPhysicalDeviceXlibPresentationSupportKHR", (PFN_vkVoidFunction)zss_PresentationSupport },
    { "vkGetPhysicalDeviceWaylandPresentationSupportKHR", (PFN_vkVoidFunction)zss_PresentationSupport },
};

PFN_vkVoidFunction zss_device_proc(const char *name)
{
    for (size_t i = 0; i < sizeof(device_entries) / sizeof(device_entries[0]); i++)
        if (!strcmp(device_entries[i].name, name))
            return device_entries[i].fn;
    return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL zss_GetDeviceProcAddr(VkDevice device,
                                                                      const char *name)
{
    (void)device;
    return zss_device_proc(name);
}

#define ZSS_EXPORT __attribute__((visibility("default")))
ZSS_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,
                                                                              const char *name);
ZSS_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(
    VkInstance instance, const char *name);
ZSS_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *version);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,
                                                                   const char *name)
{
    (void)instance;
    for (size_t i = 0; i < sizeof(instance_entries) / sizeof(instance_entries[0]); i++)
        if (!strcmp(instance_entries[i].name, name))
            return instance_entries[i].fn;
    return zss_device_proc(name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance,
                                                                         const char *name)
{
    (void)instance;
    (void)name;
    return NULL;
}

VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *version)
{
    /* Version 3 and up lets a driver own its surfaces, which the layer does. */
    if (*version < 3)
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    if (*version > 5)
        *version = 5;
    return VK_SUCCESS;
}
