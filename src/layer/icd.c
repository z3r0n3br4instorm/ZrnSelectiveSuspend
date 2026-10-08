// SPDX-License-Identifier: GPL-2.0-only
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
#include <sys/socket.h>
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

    fputs("[ZSS_AirLock] ", stderr);
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
    fputs("[ZSS_AirLock] ", stderr);
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
    zss_profile_cache(gpu, pd);

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

/* Drivers left out because their GPU was away when the program started (see zss_load_skipped). */
static char skipped[ZSS_MAX_DRIVERS][256];
static int nskipped;

static void driver_add_now(const char *lib, bool skip_detached)
{
    struct zss_driver *drv;

    for (int i = 0; i < ndrivers; i++)
        if (!strcmp(drivers[i].lib, lib))
            return;
    if (ndrivers == ZSS_MAX_DRIVERS || strstr(lib, "zss_airlock") || strstr(lib, "zss_vk")) /* itself, under its name and its former one */
        return;
    if (skip_detached && map_lib_is_detached(lib)) {
        zss_dbg("skipping %s: its GPU is detached", lib);
        if (nskipped < ZSS_MAX_DRIVERS)
            snprintf(skipped[nskipped++], sizeof(skipped[0]), "%s", lib);
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

static void driver_add(const char *lib)
{
    driver_add_now(lib, true);
}

/*
 * A GPU that was away when the program started is back. Its driver was not
 * loaded then, so the program has never heard of it: load it now, and its
 * GPUs join the end of the list. Called with every application thread held
 * out of the layer.
 */
void zss_load_skipped(void)
{
    int before = zss_ngpus, n = nskipped;

    nskipped = 0;
    for (int i = 0; i < n; i++)
        driver_add_now(skipped[i], false);
    for (int i = before; i < zss_ngpus; i++) {
        zss_dbg("gpu %d: %s pci=%s driver=%s (back since the program started)", i, zss_gpus[i]->props.deviceName,
                zss_gpus[i]->pci[0] ? zss_gpus[i]->pci : "-", zss_gpus[i]->drv->lib);
        zss_profile_describe(zss_gpus[i]);
    }
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
    for (int i = 0; i < zss_ngpus; i++)
        zss_profile_describe(zss_gpus[i]);
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
    void *lib = dlopen("libxcb.so.1", RTLD_NOW | RTLD_GLOBAL);
    void *(*connect)(const char *, int *) = lib ? (void *(*)(const char *, int *))dlsym(lib, "xcb_connect") : NULL;
    int (*has_error)(void *) = lib ? (int (*)(void *))dlsym(lib, "xcb_connection_has_error") : NULL;
    void (*disconnect)(void *) = lib ? (void (*)(void *))dlsym(lib, "xcb_disconnect") : NULL;
    const char *name = NULL;
    void *c;

    /* An Xlib application may be on a display other than $DISPLAY. */
    if (platform == VK_ICD_WSI_PLATFORM_XLIB) {
        void *x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_NOLOAD);
        char *(*display_string)(void *) = x11 ? (char *(*)(void *))dlsym(x11, "XDisplayString") : NULL;

        name = display_string ? display_string(native) : NULL;
    } else if (platform != VK_ICD_WSI_PLATFORM_XCB) {
        return NULL;
    }
    c = connect && has_error && disconnect ? connect(name, NULL) : NULL;
    if (c && has_error(c)) {
        disconnect(c);
        c = NULL;
    }
    return c;
}

static void x_link_close(VkIcdWsiPlatform platform, void *link)
{
    void *lib;

    (void)platform;
    if (link && (lib = dlopen("libxcb.so.1", RTLD_NOW | RTLD_NOLOAD))) {
        void (*disconnect)(void *) = (void (*)(void *))dlsym(lib, "xcb_disconnect");

        if (disconnect)
            disconnect(link);
        dlclose(lib);
    }
}

/*
 * Asks the display server to have a window repainted, as it does when the
 * window is uncovered. For when what the window's images held could not be
 * carried over (the GPU was lost, nothing could be read from it): an
 * application that redraws only what changed would leave the rest blank.
 * X11 only; elsewhere nothing is done.
 */
void zss_surface_repaint(struct zss_driver *drv, VkSurfaceKHR outer)
{
    void *lib = dlopen("libxcb.so.1", RTLD_NOW | RTLD_NOLOAD);
    unsigned (*clear_area)(void *, uint8_t, uint32_t, int16_t, int16_t, uint16_t, uint16_t) =
        lib ? (unsigned (*)(void *, uint8_t, uint32_t, int16_t, int16_t, uint16_t, uint16_t))dlsym(lib, "xcb_clear_area") : NULL;
    int (*flush)(void *) = lib ? (int (*)(void *))dlsym(lib, "xcb_flush") : NULL;

    pthread_mutex_lock(&zss_lock);
    for (int i = 0; clear_area && flush && i < ZSS_MAX_SURFACES; i++) {
        struct zss_surface *c = &drv->surfaces[i];
        void *conn = c->link ? c->link : c->platform == VK_ICD_WSI_PLATFORM_XCB ? (void *)c->native[0] : NULL;

        if (c->outer != outer || !conn ||
            (c->platform != VK_ICD_WSI_PLATFORM_XCB && c->platform != VK_ICD_WSI_PLATFORM_XLIB))
            continue;
        /* No size means the whole window; the flag asks for the expose events an uncovering would bring. */
        clear_area(conn, 1, (uint32_t)c->native[1], 0, 0, 0, 0);
        flush(conn);
    }
    pthread_mutex_unlock(&zss_lock);
    if (lib)
        dlclose(lib);
}

/*
 * The device behind this driver is gone. A thread inside the driver may be
 * waiting on the display server for something that will now never come (the
 * NVIDIA driver waits without a timeout for a present to be acknowledged).
 * Shutting the driver's connection makes that wait fail, the driver return,
 * and the thread find out that it is to carry on elsewhere. The surfaces are
 * forgotten rather than destroyed: nothing more is asked of a dead driver.
 */
void zss_driver_break_links(struct zss_driver *drv)
{
    void *lib = dlopen("libxcb.so.1", RTLD_NOW | RTLD_NOLOAD);
    int (*get_fd)(void *) = lib ? (int (*)(void *))dlsym(lib, "xcb_get_file_descriptor") : NULL;
    int broken = 0;

    pthread_mutex_lock(&zss_lock);
    for (int i = 0; i < ZSS_MAX_SURFACES; i++) {
        struct zss_surface *c = &drv->surfaces[i];

        if (!c->link || !get_fd)
            continue;
        shutdown(get_fd(c->link), SHUT_RDWR);
        memset(c, 0, sizeof(*c));
        broken++;
    }
    pthread_mutex_unlock(&zss_lock);
    if (lib)
        dlclose(lib);
    if (broken)
        zss_log("cut %d connection(s) of the lost device's driver to the display server", broken);
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
    } else if (base->platform == VK_ICD_WSI_PLATFORM_XLIB && drv->fn.CreateXcbSurfaceKHR &&
               (want.link = x_link_open(base->platform, (void *)want.native[0])) != NULL) {
        /* The same window through a connection of the layer's own, which only xcb allows. */
        VkXcbSurfaceCreateInfoKHR ci = {
            .sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
            .connection = want.link,
            .window = (xcb_window_t)want.native[1],
        };

        r = drv->fn.CreateXcbSurfaceKHR(drv->inst, &ci, NULL, &want.real);
        want.owned = true;
    } else if (base->platform == VK_ICD_WSI_PLATFORM_XLIB && drv->fn.CreateXlibSurfaceKHR) {
        VkXlibSurfaceCreateInfoKHR ci = {
            .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
            .dpy = (void *)want.native[0],
            .window = (Window)want.native[1],
        };

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
    /* Folded into Vulkan 1.1; still asked for by name. Listed last: they are left out under ZSS_VULKAN=1.0. */
    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
    VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
    VK_KHR_DEVICE_GROUP_CREATION_EXTENSION_NAME,
};
#define ZSS_INSTANCE_EXTS_1_0 4

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
    return fill_exts(instance_exts, zss_api_version() >= VK_API_VERSION_1_1
                                        ? (uint32_t)(sizeof(instance_exts) / sizeof(instance_exts[0])) : ZSS_INSTANCE_EXTS_1_0,
                     count, props);
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

/*
 * Whether a GPU is shown to the application. ZSS_START_ON says where a
 * program should start: "dedicated" (what zss-run asks for by default) is
 * the discrete GPU, the card ZSS exists to manage; otherwise a PCI address
 * or part of a GPU's name; "any" or nothing lists them all. A program that
 * would choose for itself is then left no choice (browsers prefer the
 * integrated GPU). The GPUs not listed stay places it can be moved to. If
 * what was asked for is not present, powered off for instance, everything is
 * listed: starting somewhere beats not starting.
 */
static bool start_match(const struct zss_gpu *gpu, const char *want)
{
    if (gpu->detached)
        return false;
    if (!strcmp(want, "dedicated"))
        return gpu->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    return !strcmp(gpu->pci, want) || strcasestr(gpu->props.deviceName, want) != NULL;
}

/*
 * Whether the program asked to run on this GPU (zss-run's default is the
 * dedicated one). A program started while that GPU was away began elsewhere;
 * when it is back, the program is moved to it, as one that had started there
 * would be.
 */
bool zss_start_wanted(const struct zss_gpu *gpu)
{
    const char *want = getenv("ZSS_START_ON");

    return want && *want && strcmp(want, "any") && start_match(gpu, want);
}

static bool gpu_listed(const struct zss_gpu *gpu)
{
    static bool said;
    const char *want = getenv("ZSS_START_ON");
    const struct zss_gpu *first = NULL;

    if (gpu->detached)
        return false;
    if (!want || !*want || !strcmp(want, "any"))
        return true;
    for (int i = 0; i < zss_ngpus && !first; i++)
        if (start_match(zss_gpus[i], want))
            first = zss_gpus[i];
    if (!said) {
        said = true;
        if (first)
            zss_log("starting on %s (%s)", first->props.deviceName, first->pci[0] ? first->pci : "no PCI address");
        else
            zss_log("no GPU matching \"%s\" is available; the program chooses among those that are", want);
    }
    return !first || start_match(gpu, want);
}

static VKAPI_ATTR VkResult VKAPI_CALL zss_EnumeratePhysicalDevices(VkInstance instance,
                                                                   uint32_t *count,
                                                                   VkPhysicalDevice *out)
{
    uint32_t have = 0, n = 0;

    (void)instance;
    zss_enter();
    for (int i = 0; i < zss_ngpus; i++)
        if (gpu_listed(zss_gpus[i]))
            have++;
    if (!out) {
        *count = have;
        zss_leave();
        return VK_SUCCESS;
    }
    for (int i = 0; i < zss_ngpus && n < *count; i++)
        if (gpu_listed(zss_gpus[i]))
            out[n++] = (VkPhysicalDevice)zss_gpus[i];
    *count = n;
    zss_leave();
    return n < have ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL zss_GetPhysicalDeviceProperties(VkPhysicalDevice pd,
                                                                  VkPhysicalDeviceProperties *p)
{
    *p = ((struct zss_gpu *)pd)->props;
    /* The version the layer tracks, or the lowest among the drivers of the group if that is lower. */
    p->apiVersion = zss_profile_api_version((struct zss_gpu *)pd);
    zss_profile_limits((struct zss_gpu *)pd, &p->limits);
}

VKAPI_ATTR void VKAPI_CALL zss_GetPhysicalDeviceFeatures(VkPhysicalDevice pd,
                                                                VkPhysicalDeviceFeatures *f)
{
    *f = ((struct zss_gpu *)pd)->features;
    f->sparseBinding = f->sparseResidencyBuffer = f->sparseResidencyImage2D = VK_FALSE;
    f->sparseResidencyImage3D = f->sparseResidency2Samples = f->sparseResidency4Samples = VK_FALSE;
    f->sparseResidency8Samples = f->sparseResidency16Samples = f->sparseResidencyAliased = VK_FALSE;
    zss_profile_features((struct zss_gpu *)pd, f);
}

VKAPI_ATTR void VKAPI_CALL zss_GetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *m)
{
    *m = ((struct zss_gpu *)pd)->mem;
}

VKAPI_ATTR void VKAPI_CALL zss_GetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice pd, uint32_t *count, VkQueueFamilyProperties *props)
{
    struct zss_gpu *gpu = (struct zss_gpu *)pd;
    VkQueueFamilyProperties fam[ZSS_MAX_FAMILIES];
    uint32_t have = zss_profile_families(gpu, fam), n;

    if (!props) {
        *count = have;
        return;
    }
    n = *count < have ? *count : have;
    memcpy(props, fam, n * sizeof(*props));
    *count = n;
}

VKAPI_ATTR void VKAPI_CALL zss_GetPhysicalDeviceFormatProperties(VkPhysicalDevice pd,
                                                                        VkFormat format,
                                                                        VkFormatProperties *p)
{
    struct zss_gpu *gpu;
    VkPhysicalDevice real;

    zss_enter();
    gpu = gpu_effective((struct zss_gpu *)pd);
    real = zss_gpu_real(gpu);
    memset(p, 0, sizeof(*p));
    if (real && !zss_format_planar(format))
        gpu->drv->fn.GetPhysicalDeviceFormatProperties(real, format, p);
    zss_profile_format((struct zss_gpu *)pd, format, p);
    zss_leave();
}

VKAPI_ATTR VkResult VKAPI_CALL zss_GetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice pd, VkFormat format, VkImageType type, VkImageTiling tiling,
    VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties *p)
{
    struct zss_gpu *gpu;
    VkPhysicalDevice real;
    VkResult r = VK_ERROR_FORMAT_NOT_SUPPORTED;

    zss_enter();
    gpu = gpu_effective((struct zss_gpu *)pd);
    real = zss_gpu_real(gpu);
    if (real && !zss_format_planar(format) && zss_profile_format_usable((struct zss_gpu *)pd, format, tiling))
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
    const char *names[64];
    struct zss_gpu *gpu = (struct zss_gpu *)pd;
    uint32_t n = 0;

    if (layer)
        return VK_ERROR_LAYER_NOT_PRESENT;
    /* The swapchain, and whatever else the layer offers that this GPU's driver has too. */
    if (has_ext(gpu->ext, gpu->next, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        names[n++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    for (uint32_t i = 0; i < zss_offered_ext_count() && n < 64; i++)
        if (zss_profile_has_ext(gpu, zss_offered_ext(i)))
            names[n++] = zss_offered_ext(i);
    return fill_exts(names, n, count, props);
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

/*
 * A window's surface, as the other GPUs of the group see it. What a swapchain
 * may be like is each driver's own answer, and a swapchain made to one
 * driver's answer has to be made again on another after a move. So these
 * queries, too, give what all of them can do. A member whose driver cannot be
 * reached now (its GPU is powered off) is left out: moving there later may
 * then need the application to rebuild its swapchain.
 */
static bool surface_peer(struct zss_gpu *shown, struct zss_gpu *eff, int i, VkSurfaceKHR surface,
                         struct zss_gpu **peer, VkPhysicalDevice *real, VkSurfaceKHR *rsurf)
{
    *peer = zss_gpus[i];
    if (*peer == eff || !zss_profile_member(shown, *peer))
        return false;
    *real = zss_gpu_real(*peer);
    return *real && (*peer)->drv->fn.GetPhysicalDeviceSurfaceCapabilitiesKHR &&
           zss_surface_real((*peer)->drv, surface, rsurf) == VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL zss_GetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice pd, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *caps)
{
    struct zss_gpu *gpu, *peer;
    VkPhysicalDevice real;
    VkSurfaceKHR rsurf;
    VkResult r;

    zss_enter();
    r = surface_target(pd, surface, &gpu, &real, &rsurf);
    if (r == VK_SUCCESS)
        r = gpu->drv->fn.GetPhysicalDeviceSurfaceCapabilitiesKHR(real, rsurf, caps);
    for (int i = 0; r == VK_SUCCESS && i < zss_ngpus; i++) {
        VkSurfaceCapabilitiesKHR c;

        if (!surface_peer((struct zss_gpu *)pd, gpu, i, surface, &peer, &real, &rsurf) ||
            peer->drv->fn.GetPhysicalDeviceSurfaceCapabilitiesKHR(real, rsurf, &c) != VK_SUCCESS)
            continue;
        if (c.minImageCount > caps->minImageCount)
            caps->minImageCount = c.minImageCount;
        /* Zero means no upper limit. */
        if (c.maxImageCount && (!caps->maxImageCount || c.maxImageCount < caps->maxImageCount))
            caps->maxImageCount = c.maxImageCount;
        if (c.maxImageArrayLayers < caps->maxImageArrayLayers)
            caps->maxImageArrayLayers = c.maxImageArrayLayers;
        caps->supportedUsageFlags &= c.supportedUsageFlags;
        /* Drivers may have no alpha mode in common for a window; one has to be reported, so then it is this GPU's. */
        if (caps->supportedCompositeAlpha & c.supportedCompositeAlpha)
            caps->supportedCompositeAlpha &= c.supportedCompositeAlpha;
        caps->supportedTransforms = (caps->supportedTransforms & c.supportedTransforms) | caps->currentTransform;
    }
    zss_leave();
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL zss_GetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice pd, VkSurfaceKHR surface, uint32_t *count, VkSurfaceFormatKHR *formats)
{
    struct zss_gpu *gpu, *peer;
    VkPhysicalDevice real;
    VkSurfaceKHR rsurf;
    VkSurfaceFormatKHR all[128], theirs[128];
    uint32_t n = 128, kept = 0;
    VkResult r;

    zss_enter();
    r = surface_target(pd, surface, &gpu, &real, &rsurf);
    if (r == VK_SUCCESS)
        r = gpu->drv->fn.GetPhysicalDeviceSurfaceFormatsKHR(real, rsurf, &n, all);
    if (r < 0) {
        zss_leave();
        return r;
    }
    for (int i = 0; i < zss_ngpus; i++) {
        uint32_t nt = 128;

        if (!surface_peer((struct zss_gpu *)pd, gpu, i, surface, &peer, &real, &rsurf) ||
            peer->drv->fn.GetPhysicalDeviceSurfaceFormatsKHR(real, rsurf, &nt, theirs) < 0)
            continue;
        kept = 0;
        for (uint32_t k = 0; k < n; k++)
            for (uint32_t t = 0; t < nt; t++)
                if (all[k].format == theirs[t].format && all[k].colorSpace == theirs[t].colorSpace) {
                    all[kept++] = all[k];
                    break;
                }
        n = kept;
    }
    zss_leave();
    if (!formats) {
        *count = n;
        return VK_SUCCESS;
    }
    kept = *count < n ? *count : n;
    memcpy(formats, all, kept * sizeof(*formats));
    *count = kept;
    return kept < n ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL zss_GetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice pd, VkSurfaceKHR surface, uint32_t *count, VkPresentModeKHR *modes)
{
    struct zss_gpu *gpu, *peer;
    VkPhysicalDevice real;
    VkSurfaceKHR rsurf;
    VkPresentModeKHR all[16], theirs[16];
    uint32_t n = 16, kept = 0;
    VkResult r;

    zss_enter();
    r = surface_target(pd, surface, &gpu, &real, &rsurf);
    if (r == VK_SUCCESS)
        r = gpu->drv->fn.GetPhysicalDeviceSurfacePresentModesKHR(real, rsurf, &n, all);
    if (r < 0) {
        zss_leave();
        return r;
    }
    for (int i = 0; i < zss_ngpus; i++) {
        uint32_t nt = 16;

        if (!surface_peer((struct zss_gpu *)pd, gpu, i, surface, &peer, &real, &rsurf) ||
            peer->drv->fn.GetPhysicalDeviceSurfacePresentModesKHR(real, rsurf, &nt, theirs) < 0)
            continue;
        kept = 0;
        for (uint32_t k = 0; k < n; k++)
            for (uint32_t t = 0; t < nt; t++)
                if (all[k] == theirs[t]) {
                    all[kept++] = all[k];
                    break;
                }
        n = kept;
    }
    zss_leave();
    if (!modes) {
        *count = n;
        return VK_SUCCESS;
    }
    kept = *count < n ? *count : n;
    memcpy(modes, all, kept * sizeof(*modes));
    *count = kept;
    return kept < n ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL zss_PresentationSupport(void)
{
    return VK_TRUE;
}

/* ---- entry-point table ------------------------------------------------- */


static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL zss_GetDeviceProcAddr(VkDevice device,
                                                                      const char *name);

/*
 * Debugging aid, on with ZSS_TRAP=1: a command the layer does not provide is
 * answered with a stand-in that names the command and stops the program, so
 * that an application which calls one regardless (instead of checking for
 * NULL) says which.
 */
static const char *trap_names[768];
static int ntraps;

static void zss_trap_hit(int i)
{
    zss_log("the application called %s, which the layer does not provide; stopping it here", trap_names[i]);
    abort();
}

static void zss_trap_0(void) { zss_trap_hit(0); }
static void zss_trap_1(void) { zss_trap_hit(1); }
static void zss_trap_2(void) { zss_trap_hit(2); }
static void zss_trap_3(void) { zss_trap_hit(3); }
static void zss_trap_4(void) { zss_trap_hit(4); }
static void zss_trap_5(void) { zss_trap_hit(5); }
static void zss_trap_6(void) { zss_trap_hit(6); }
static void zss_trap_7(void) { zss_trap_hit(7); }
static void zss_trap_8(void) { zss_trap_hit(8); }
static void zss_trap_9(void) { zss_trap_hit(9); }
static void zss_trap_10(void) { zss_trap_hit(10); }
static void zss_trap_11(void) { zss_trap_hit(11); }
static void zss_trap_12(void) { zss_trap_hit(12); }
static void zss_trap_13(void) { zss_trap_hit(13); }
static void zss_trap_14(void) { zss_trap_hit(14); }
static void zss_trap_15(void) { zss_trap_hit(15); }
static void zss_trap_16(void) { zss_trap_hit(16); }
static void zss_trap_17(void) { zss_trap_hit(17); }
static void zss_trap_18(void) { zss_trap_hit(18); }
static void zss_trap_19(void) { zss_trap_hit(19); }
static void zss_trap_20(void) { zss_trap_hit(20); }
static void zss_trap_21(void) { zss_trap_hit(21); }
static void zss_trap_22(void) { zss_trap_hit(22); }
static void zss_trap_23(void) { zss_trap_hit(23); }
static void zss_trap_24(void) { zss_trap_hit(24); }
static void zss_trap_25(void) { zss_trap_hit(25); }
static void zss_trap_26(void) { zss_trap_hit(26); }
static void zss_trap_27(void) { zss_trap_hit(27); }
static void zss_trap_28(void) { zss_trap_hit(28); }
static void zss_trap_29(void) { zss_trap_hit(29); }
static void zss_trap_30(void) { zss_trap_hit(30); }
static void zss_trap_31(void) { zss_trap_hit(31); }
static void zss_trap_32(void) { zss_trap_hit(32); }
static void zss_trap_33(void) { zss_trap_hit(33); }
static void zss_trap_34(void) { zss_trap_hit(34); }
static void zss_trap_35(void) { zss_trap_hit(35); }
static void zss_trap_36(void) { zss_trap_hit(36); }
static void zss_trap_37(void) { zss_trap_hit(37); }
static void zss_trap_38(void) { zss_trap_hit(38); }
static void zss_trap_39(void) { zss_trap_hit(39); }
static void zss_trap_40(void) { zss_trap_hit(40); }
static void zss_trap_41(void) { zss_trap_hit(41); }
static void zss_trap_42(void) { zss_trap_hit(42); }
static void zss_trap_43(void) { zss_trap_hit(43); }
static void zss_trap_44(void) { zss_trap_hit(44); }
static void zss_trap_45(void) { zss_trap_hit(45); }
static void zss_trap_46(void) { zss_trap_hit(46); }
static void zss_trap_47(void) { zss_trap_hit(47); }
static void zss_trap_48(void) { zss_trap_hit(48); }
static void zss_trap_49(void) { zss_trap_hit(49); }
static void zss_trap_50(void) { zss_trap_hit(50); }
static void zss_trap_51(void) { zss_trap_hit(51); }
static void zss_trap_52(void) { zss_trap_hit(52); }
static void zss_trap_53(void) { zss_trap_hit(53); }
static void zss_trap_54(void) { zss_trap_hit(54); }
static void zss_trap_55(void) { zss_trap_hit(55); }
static void zss_trap_56(void) { zss_trap_hit(56); }
static void zss_trap_57(void) { zss_trap_hit(57); }
static void zss_trap_58(void) { zss_trap_hit(58); }
static void zss_trap_59(void) { zss_trap_hit(59); }
static void zss_trap_60(void) { zss_trap_hit(60); }
static void zss_trap_61(void) { zss_trap_hit(61); }
static void zss_trap_62(void) { zss_trap_hit(62); }
static void zss_trap_63(void) { zss_trap_hit(63); }
static void zss_trap_64(void) { zss_trap_hit(64); }
static void zss_trap_65(void) { zss_trap_hit(65); }
static void zss_trap_66(void) { zss_trap_hit(66); }
static void zss_trap_67(void) { zss_trap_hit(67); }
static void zss_trap_68(void) { zss_trap_hit(68); }
static void zss_trap_69(void) { zss_trap_hit(69); }
static void zss_trap_70(void) { zss_trap_hit(70); }
static void zss_trap_71(void) { zss_trap_hit(71); }
static void zss_trap_72(void) { zss_trap_hit(72); }
static void zss_trap_73(void) { zss_trap_hit(73); }
static void zss_trap_74(void) { zss_trap_hit(74); }
static void zss_trap_75(void) { zss_trap_hit(75); }
static void zss_trap_76(void) { zss_trap_hit(76); }
static void zss_trap_77(void) { zss_trap_hit(77); }
static void zss_trap_78(void) { zss_trap_hit(78); }
static void zss_trap_79(void) { zss_trap_hit(79); }
static void zss_trap_80(void) { zss_trap_hit(80); }
static void zss_trap_81(void) { zss_trap_hit(81); }
static void zss_trap_82(void) { zss_trap_hit(82); }
static void zss_trap_83(void) { zss_trap_hit(83); }
static void zss_trap_84(void) { zss_trap_hit(84); }
static void zss_trap_85(void) { zss_trap_hit(85); }
static void zss_trap_86(void) { zss_trap_hit(86); }
static void zss_trap_87(void) { zss_trap_hit(87); }
static void zss_trap_88(void) { zss_trap_hit(88); }
static void zss_trap_89(void) { zss_trap_hit(89); }
static void zss_trap_90(void) { zss_trap_hit(90); }
static void zss_trap_91(void) { zss_trap_hit(91); }
static void zss_trap_92(void) { zss_trap_hit(92); }
static void zss_trap_93(void) { zss_trap_hit(93); }
static void zss_trap_94(void) { zss_trap_hit(94); }
static void zss_trap_95(void) { zss_trap_hit(95); }
static void zss_trap_96(void) { zss_trap_hit(96); }
static void zss_trap_97(void) { zss_trap_hit(97); }
static void zss_trap_98(void) { zss_trap_hit(98); }
static void zss_trap_99(void) { zss_trap_hit(99); }
static void zss_trap_100(void) { zss_trap_hit(100); }
static void zss_trap_101(void) { zss_trap_hit(101); }
static void zss_trap_102(void) { zss_trap_hit(102); }
static void zss_trap_103(void) { zss_trap_hit(103); }
static void zss_trap_104(void) { zss_trap_hit(104); }
static void zss_trap_105(void) { zss_trap_hit(105); }
static void zss_trap_106(void) { zss_trap_hit(106); }
static void zss_trap_107(void) { zss_trap_hit(107); }
static void zss_trap_108(void) { zss_trap_hit(108); }
static void zss_trap_109(void) { zss_trap_hit(109); }
static void zss_trap_110(void) { zss_trap_hit(110); }
static void zss_trap_111(void) { zss_trap_hit(111); }
static void zss_trap_112(void) { zss_trap_hit(112); }
static void zss_trap_113(void) { zss_trap_hit(113); }
static void zss_trap_114(void) { zss_trap_hit(114); }
static void zss_trap_115(void) { zss_trap_hit(115); }
static void zss_trap_116(void) { zss_trap_hit(116); }
static void zss_trap_117(void) { zss_trap_hit(117); }
static void zss_trap_118(void) { zss_trap_hit(118); }
static void zss_trap_119(void) { zss_trap_hit(119); }
static void zss_trap_120(void) { zss_trap_hit(120); }
static void zss_trap_121(void) { zss_trap_hit(121); }
static void zss_trap_122(void) { zss_trap_hit(122); }
static void zss_trap_123(void) { zss_trap_hit(123); }
static void zss_trap_124(void) { zss_trap_hit(124); }
static void zss_trap_125(void) { zss_trap_hit(125); }
static void zss_trap_126(void) { zss_trap_hit(126); }
static void zss_trap_127(void) { zss_trap_hit(127); }
static void zss_trap_128(void) { zss_trap_hit(128); }
static void zss_trap_129(void) { zss_trap_hit(129); }
static void zss_trap_130(void) { zss_trap_hit(130); }
static void zss_trap_131(void) { zss_trap_hit(131); }
static void zss_trap_132(void) { zss_trap_hit(132); }
static void zss_trap_133(void) { zss_trap_hit(133); }
static void zss_trap_134(void) { zss_trap_hit(134); }
static void zss_trap_135(void) { zss_trap_hit(135); }
static void zss_trap_136(void) { zss_trap_hit(136); }
static void zss_trap_137(void) { zss_trap_hit(137); }
static void zss_trap_138(void) { zss_trap_hit(138); }
static void zss_trap_139(void) { zss_trap_hit(139); }
static void zss_trap_140(void) { zss_trap_hit(140); }
static void zss_trap_141(void) { zss_trap_hit(141); }
static void zss_trap_142(void) { zss_trap_hit(142); }
static void zss_trap_143(void) { zss_trap_hit(143); }
static void zss_trap_144(void) { zss_trap_hit(144); }
static void zss_trap_145(void) { zss_trap_hit(145); }
static void zss_trap_146(void) { zss_trap_hit(146); }
static void zss_trap_147(void) { zss_trap_hit(147); }
static void zss_trap_148(void) { zss_trap_hit(148); }
static void zss_trap_149(void) { zss_trap_hit(149); }
static void zss_trap_150(void) { zss_trap_hit(150); }
static void zss_trap_151(void) { zss_trap_hit(151); }
static void zss_trap_152(void) { zss_trap_hit(152); }
static void zss_trap_153(void) { zss_trap_hit(153); }
static void zss_trap_154(void) { zss_trap_hit(154); }
static void zss_trap_155(void) { zss_trap_hit(155); }
static void zss_trap_156(void) { zss_trap_hit(156); }
static void zss_trap_157(void) { zss_trap_hit(157); }
static void zss_trap_158(void) { zss_trap_hit(158); }
static void zss_trap_159(void) { zss_trap_hit(159); }
static void zss_trap_160(void) { zss_trap_hit(160); }
static void zss_trap_161(void) { zss_trap_hit(161); }
static void zss_trap_162(void) { zss_trap_hit(162); }
static void zss_trap_163(void) { zss_trap_hit(163); }
static void zss_trap_164(void) { zss_trap_hit(164); }
static void zss_trap_165(void) { zss_trap_hit(165); }
static void zss_trap_166(void) { zss_trap_hit(166); }
static void zss_trap_167(void) { zss_trap_hit(167); }
static void zss_trap_168(void) { zss_trap_hit(168); }
static void zss_trap_169(void) { zss_trap_hit(169); }
static void zss_trap_170(void) { zss_trap_hit(170); }
static void zss_trap_171(void) { zss_trap_hit(171); }
static void zss_trap_172(void) { zss_trap_hit(172); }
static void zss_trap_173(void) { zss_trap_hit(173); }
static void zss_trap_174(void) { zss_trap_hit(174); }
static void zss_trap_175(void) { zss_trap_hit(175); }
static void zss_trap_176(void) { zss_trap_hit(176); }
static void zss_trap_177(void) { zss_trap_hit(177); }
static void zss_trap_178(void) { zss_trap_hit(178); }
static void zss_trap_179(void) { zss_trap_hit(179); }
static void zss_trap_180(void) { zss_trap_hit(180); }
static void zss_trap_181(void) { zss_trap_hit(181); }
static void zss_trap_182(void) { zss_trap_hit(182); }
static void zss_trap_183(void) { zss_trap_hit(183); }
static void zss_trap_184(void) { zss_trap_hit(184); }
static void zss_trap_185(void) { zss_trap_hit(185); }
static void zss_trap_186(void) { zss_trap_hit(186); }
static void zss_trap_187(void) { zss_trap_hit(187); }
static void zss_trap_188(void) { zss_trap_hit(188); }
static void zss_trap_189(void) { zss_trap_hit(189); }
static void zss_trap_190(void) { zss_trap_hit(190); }
static void zss_trap_191(void) { zss_trap_hit(191); }
static void zss_trap_192(void) { zss_trap_hit(192); }
static void zss_trap_193(void) { zss_trap_hit(193); }
static void zss_trap_194(void) { zss_trap_hit(194); }
static void zss_trap_195(void) { zss_trap_hit(195); }
static void zss_trap_196(void) { zss_trap_hit(196); }
static void zss_trap_197(void) { zss_trap_hit(197); }
static void zss_trap_198(void) { zss_trap_hit(198); }
static void zss_trap_199(void) { zss_trap_hit(199); }
static void zss_trap_200(void) { zss_trap_hit(200); }
static void zss_trap_201(void) { zss_trap_hit(201); }
static void zss_trap_202(void) { zss_trap_hit(202); }
static void zss_trap_203(void) { zss_trap_hit(203); }
static void zss_trap_204(void) { zss_trap_hit(204); }
static void zss_trap_205(void) { zss_trap_hit(205); }
static void zss_trap_206(void) { zss_trap_hit(206); }
static void zss_trap_207(void) { zss_trap_hit(207); }
static void zss_trap_208(void) { zss_trap_hit(208); }
static void zss_trap_209(void) { zss_trap_hit(209); }
static void zss_trap_210(void) { zss_trap_hit(210); }
static void zss_trap_211(void) { zss_trap_hit(211); }
static void zss_trap_212(void) { zss_trap_hit(212); }
static void zss_trap_213(void) { zss_trap_hit(213); }
static void zss_trap_214(void) { zss_trap_hit(214); }
static void zss_trap_215(void) { zss_trap_hit(215); }
static void zss_trap_216(void) { zss_trap_hit(216); }
static void zss_trap_217(void) { zss_trap_hit(217); }
static void zss_trap_218(void) { zss_trap_hit(218); }
static void zss_trap_219(void) { zss_trap_hit(219); }
static void zss_trap_220(void) { zss_trap_hit(220); }
static void zss_trap_221(void) { zss_trap_hit(221); }
static void zss_trap_222(void) { zss_trap_hit(222); }
static void zss_trap_223(void) { zss_trap_hit(223); }
static void zss_trap_224(void) { zss_trap_hit(224); }
static void zss_trap_225(void) { zss_trap_hit(225); }
static void zss_trap_226(void) { zss_trap_hit(226); }
static void zss_trap_227(void) { zss_trap_hit(227); }
static void zss_trap_228(void) { zss_trap_hit(228); }
static void zss_trap_229(void) { zss_trap_hit(229); }
static void zss_trap_230(void) { zss_trap_hit(230); }
static void zss_trap_231(void) { zss_trap_hit(231); }
static void zss_trap_232(void) { zss_trap_hit(232); }
static void zss_trap_233(void) { zss_trap_hit(233); }
static void zss_trap_234(void) { zss_trap_hit(234); }
static void zss_trap_235(void) { zss_trap_hit(235); }
static void zss_trap_236(void) { zss_trap_hit(236); }
static void zss_trap_237(void) { zss_trap_hit(237); }
static void zss_trap_238(void) { zss_trap_hit(238); }
static void zss_trap_239(void) { zss_trap_hit(239); }
static void zss_trap_240(void) { zss_trap_hit(240); }
static void zss_trap_241(void) { zss_trap_hit(241); }
static void zss_trap_242(void) { zss_trap_hit(242); }
static void zss_trap_243(void) { zss_trap_hit(243); }
static void zss_trap_244(void) { zss_trap_hit(244); }
static void zss_trap_245(void) { zss_trap_hit(245); }
static void zss_trap_246(void) { zss_trap_hit(246); }
static void zss_trap_247(void) { zss_trap_hit(247); }
static void zss_trap_248(void) { zss_trap_hit(248); }
static void zss_trap_249(void) { zss_trap_hit(249); }
static void zss_trap_250(void) { zss_trap_hit(250); }
static void zss_trap_251(void) { zss_trap_hit(251); }
static void zss_trap_252(void) { zss_trap_hit(252); }
static void zss_trap_253(void) { zss_trap_hit(253); }
static void zss_trap_254(void) { zss_trap_hit(254); }
static void zss_trap_255(void) { zss_trap_hit(255); }
static void zss_trap_256(void) { zss_trap_hit(256); }
static void zss_trap_257(void) { zss_trap_hit(257); }
static void zss_trap_258(void) { zss_trap_hit(258); }
static void zss_trap_259(void) { zss_trap_hit(259); }
static void zss_trap_260(void) { zss_trap_hit(260); }
static void zss_trap_261(void) { zss_trap_hit(261); }
static void zss_trap_262(void) { zss_trap_hit(262); }
static void zss_trap_263(void) { zss_trap_hit(263); }
static void zss_trap_264(void) { zss_trap_hit(264); }
static void zss_trap_265(void) { zss_trap_hit(265); }
static void zss_trap_266(void) { zss_trap_hit(266); }
static void zss_trap_267(void) { zss_trap_hit(267); }
static void zss_trap_268(void) { zss_trap_hit(268); }
static void zss_trap_269(void) { zss_trap_hit(269); }
static void zss_trap_270(void) { zss_trap_hit(270); }
static void zss_trap_271(void) { zss_trap_hit(271); }
static void zss_trap_272(void) { zss_trap_hit(272); }
static void zss_trap_273(void) { zss_trap_hit(273); }
static void zss_trap_274(void) { zss_trap_hit(274); }
static void zss_trap_275(void) { zss_trap_hit(275); }
static void zss_trap_276(void) { zss_trap_hit(276); }
static void zss_trap_277(void) { zss_trap_hit(277); }
static void zss_trap_278(void) { zss_trap_hit(278); }
static void zss_trap_279(void) { zss_trap_hit(279); }
static void zss_trap_280(void) { zss_trap_hit(280); }
static void zss_trap_281(void) { zss_trap_hit(281); }
static void zss_trap_282(void) { zss_trap_hit(282); }
static void zss_trap_283(void) { zss_trap_hit(283); }
static void zss_trap_284(void) { zss_trap_hit(284); }
static void zss_trap_285(void) { zss_trap_hit(285); }
static void zss_trap_286(void) { zss_trap_hit(286); }
static void zss_trap_287(void) { zss_trap_hit(287); }
static void zss_trap_288(void) { zss_trap_hit(288); }
static void zss_trap_289(void) { zss_trap_hit(289); }
static void zss_trap_290(void) { zss_trap_hit(290); }
static void zss_trap_291(void) { zss_trap_hit(291); }
static void zss_trap_292(void) { zss_trap_hit(292); }
static void zss_trap_293(void) { zss_trap_hit(293); }
static void zss_trap_294(void) { zss_trap_hit(294); }
static void zss_trap_295(void) { zss_trap_hit(295); }
static void zss_trap_296(void) { zss_trap_hit(296); }
static void zss_trap_297(void) { zss_trap_hit(297); }
static void zss_trap_298(void) { zss_trap_hit(298); }
static void zss_trap_299(void) { zss_trap_hit(299); }
static void zss_trap_300(void) { zss_trap_hit(300); }
static void zss_trap_301(void) { zss_trap_hit(301); }
static void zss_trap_302(void) { zss_trap_hit(302); }
static void zss_trap_303(void) { zss_trap_hit(303); }
static void zss_trap_304(void) { zss_trap_hit(304); }
static void zss_trap_305(void) { zss_trap_hit(305); }
static void zss_trap_306(void) { zss_trap_hit(306); }
static void zss_trap_307(void) { zss_trap_hit(307); }
static void zss_trap_308(void) { zss_trap_hit(308); }
static void zss_trap_309(void) { zss_trap_hit(309); }
static void zss_trap_310(void) { zss_trap_hit(310); }
static void zss_trap_311(void) { zss_trap_hit(311); }
static void zss_trap_312(void) { zss_trap_hit(312); }
static void zss_trap_313(void) { zss_trap_hit(313); }
static void zss_trap_314(void) { zss_trap_hit(314); }
static void zss_trap_315(void) { zss_trap_hit(315); }
static void zss_trap_316(void) { zss_trap_hit(316); }
static void zss_trap_317(void) { zss_trap_hit(317); }
static void zss_trap_318(void) { zss_trap_hit(318); }
static void zss_trap_319(void) { zss_trap_hit(319); }
static void zss_trap_320(void) { zss_trap_hit(320); }
static void zss_trap_321(void) { zss_trap_hit(321); }
static void zss_trap_322(void) { zss_trap_hit(322); }
static void zss_trap_323(void) { zss_trap_hit(323); }
static void zss_trap_324(void) { zss_trap_hit(324); }
static void zss_trap_325(void) { zss_trap_hit(325); }
static void zss_trap_326(void) { zss_trap_hit(326); }
static void zss_trap_327(void) { zss_trap_hit(327); }
static void zss_trap_328(void) { zss_trap_hit(328); }
static void zss_trap_329(void) { zss_trap_hit(329); }
static void zss_trap_330(void) { zss_trap_hit(330); }
static void zss_trap_331(void) { zss_trap_hit(331); }
static void zss_trap_332(void) { zss_trap_hit(332); }
static void zss_trap_333(void) { zss_trap_hit(333); }
static void zss_trap_334(void) { zss_trap_hit(334); }
static void zss_trap_335(void) { zss_trap_hit(335); }
static void zss_trap_336(void) { zss_trap_hit(336); }
static void zss_trap_337(void) { zss_trap_hit(337); }
static void zss_trap_338(void) { zss_trap_hit(338); }
static void zss_trap_339(void) { zss_trap_hit(339); }
static void zss_trap_340(void) { zss_trap_hit(340); }
static void zss_trap_341(void) { zss_trap_hit(341); }
static void zss_trap_342(void) { zss_trap_hit(342); }
static void zss_trap_343(void) { zss_trap_hit(343); }
static void zss_trap_344(void) { zss_trap_hit(344); }
static void zss_trap_345(void) { zss_trap_hit(345); }
static void zss_trap_346(void) { zss_trap_hit(346); }
static void zss_trap_347(void) { zss_trap_hit(347); }
static void zss_trap_348(void) { zss_trap_hit(348); }
static void zss_trap_349(void) { zss_trap_hit(349); }
static void zss_trap_350(void) { zss_trap_hit(350); }
static void zss_trap_351(void) { zss_trap_hit(351); }
static void zss_trap_352(void) { zss_trap_hit(352); }
static void zss_trap_353(void) { zss_trap_hit(353); }
static void zss_trap_354(void) { zss_trap_hit(354); }
static void zss_trap_355(void) { zss_trap_hit(355); }
static void zss_trap_356(void) { zss_trap_hit(356); }
static void zss_trap_357(void) { zss_trap_hit(357); }
static void zss_trap_358(void) { zss_trap_hit(358); }
static void zss_trap_359(void) { zss_trap_hit(359); }
static void zss_trap_360(void) { zss_trap_hit(360); }
static void zss_trap_361(void) { zss_trap_hit(361); }
static void zss_trap_362(void) { zss_trap_hit(362); }
static void zss_trap_363(void) { zss_trap_hit(363); }
static void zss_trap_364(void) { zss_trap_hit(364); }
static void zss_trap_365(void) { zss_trap_hit(365); }
static void zss_trap_366(void) { zss_trap_hit(366); }
static void zss_trap_367(void) { zss_trap_hit(367); }
static void zss_trap_368(void) { zss_trap_hit(368); }
static void zss_trap_369(void) { zss_trap_hit(369); }
static void zss_trap_370(void) { zss_trap_hit(370); }
static void zss_trap_371(void) { zss_trap_hit(371); }
static void zss_trap_372(void) { zss_trap_hit(372); }
static void zss_trap_373(void) { zss_trap_hit(373); }
static void zss_trap_374(void) { zss_trap_hit(374); }
static void zss_trap_375(void) { zss_trap_hit(375); }
static void zss_trap_376(void) { zss_trap_hit(376); }
static void zss_trap_377(void) { zss_trap_hit(377); }
static void zss_trap_378(void) { zss_trap_hit(378); }
static void zss_trap_379(void) { zss_trap_hit(379); }
static void zss_trap_380(void) { zss_trap_hit(380); }
static void zss_trap_381(void) { zss_trap_hit(381); }
static void zss_trap_382(void) { zss_trap_hit(382); }
static void zss_trap_383(void) { zss_trap_hit(383); }
static void zss_trap_384(void) { zss_trap_hit(384); }
static void zss_trap_385(void) { zss_trap_hit(385); }
static void zss_trap_386(void) { zss_trap_hit(386); }
static void zss_trap_387(void) { zss_trap_hit(387); }
static void zss_trap_388(void) { zss_trap_hit(388); }
static void zss_trap_389(void) { zss_trap_hit(389); }
static void zss_trap_390(void) { zss_trap_hit(390); }
static void zss_trap_391(void) { zss_trap_hit(391); }
static void zss_trap_392(void) { zss_trap_hit(392); }
static void zss_trap_393(void) { zss_trap_hit(393); }
static void zss_trap_394(void) { zss_trap_hit(394); }
static void zss_trap_395(void) { zss_trap_hit(395); }
static void zss_trap_396(void) { zss_trap_hit(396); }
static void zss_trap_397(void) { zss_trap_hit(397); }
static void zss_trap_398(void) { zss_trap_hit(398); }
static void zss_trap_399(void) { zss_trap_hit(399); }
static void zss_trap_400(void) { zss_trap_hit(400); }
static void zss_trap_401(void) { zss_trap_hit(401); }
static void zss_trap_402(void) { zss_trap_hit(402); }
static void zss_trap_403(void) { zss_trap_hit(403); }
static void zss_trap_404(void) { zss_trap_hit(404); }
static void zss_trap_405(void) { zss_trap_hit(405); }
static void zss_trap_406(void) { zss_trap_hit(406); }
static void zss_trap_407(void) { zss_trap_hit(407); }
static void zss_trap_408(void) { zss_trap_hit(408); }
static void zss_trap_409(void) { zss_trap_hit(409); }
static void zss_trap_410(void) { zss_trap_hit(410); }
static void zss_trap_411(void) { zss_trap_hit(411); }
static void zss_trap_412(void) { zss_trap_hit(412); }
static void zss_trap_413(void) { zss_trap_hit(413); }
static void zss_trap_414(void) { zss_trap_hit(414); }
static void zss_trap_415(void) { zss_trap_hit(415); }
static void zss_trap_416(void) { zss_trap_hit(416); }
static void zss_trap_417(void) { zss_trap_hit(417); }
static void zss_trap_418(void) { zss_trap_hit(418); }
static void zss_trap_419(void) { zss_trap_hit(419); }
static void zss_trap_420(void) { zss_trap_hit(420); }
static void zss_trap_421(void) { zss_trap_hit(421); }
static void zss_trap_422(void) { zss_trap_hit(422); }
static void zss_trap_423(void) { zss_trap_hit(423); }
static void zss_trap_424(void) { zss_trap_hit(424); }
static void zss_trap_425(void) { zss_trap_hit(425); }
static void zss_trap_426(void) { zss_trap_hit(426); }
static void zss_trap_427(void) { zss_trap_hit(427); }
static void zss_trap_428(void) { zss_trap_hit(428); }
static void zss_trap_429(void) { zss_trap_hit(429); }
static void zss_trap_430(void) { zss_trap_hit(430); }
static void zss_trap_431(void) { zss_trap_hit(431); }
static void zss_trap_432(void) { zss_trap_hit(432); }
static void zss_trap_433(void) { zss_trap_hit(433); }
static void zss_trap_434(void) { zss_trap_hit(434); }
static void zss_trap_435(void) { zss_trap_hit(435); }
static void zss_trap_436(void) { zss_trap_hit(436); }
static void zss_trap_437(void) { zss_trap_hit(437); }
static void zss_trap_438(void) { zss_trap_hit(438); }
static void zss_trap_439(void) { zss_trap_hit(439); }
static void zss_trap_440(void) { zss_trap_hit(440); }
static void zss_trap_441(void) { zss_trap_hit(441); }
static void zss_trap_442(void) { zss_trap_hit(442); }
static void zss_trap_443(void) { zss_trap_hit(443); }
static void zss_trap_444(void) { zss_trap_hit(444); }
static void zss_trap_445(void) { zss_trap_hit(445); }
static void zss_trap_446(void) { zss_trap_hit(446); }
static void zss_trap_447(void) { zss_trap_hit(447); }
static void zss_trap_448(void) { zss_trap_hit(448); }
static void zss_trap_449(void) { zss_trap_hit(449); }
static void zss_trap_450(void) { zss_trap_hit(450); }
static void zss_trap_451(void) { zss_trap_hit(451); }
static void zss_trap_452(void) { zss_trap_hit(452); }
static void zss_trap_453(void) { zss_trap_hit(453); }
static void zss_trap_454(void) { zss_trap_hit(454); }
static void zss_trap_455(void) { zss_trap_hit(455); }
static void zss_trap_456(void) { zss_trap_hit(456); }
static void zss_trap_457(void) { zss_trap_hit(457); }
static void zss_trap_458(void) { zss_trap_hit(458); }
static void zss_trap_459(void) { zss_trap_hit(459); }
static void zss_trap_460(void) { zss_trap_hit(460); }
static void zss_trap_461(void) { zss_trap_hit(461); }
static void zss_trap_462(void) { zss_trap_hit(462); }
static void zss_trap_463(void) { zss_trap_hit(463); }
static void zss_trap_464(void) { zss_trap_hit(464); }
static void zss_trap_465(void) { zss_trap_hit(465); }
static void zss_trap_466(void) { zss_trap_hit(466); }
static void zss_trap_467(void) { zss_trap_hit(467); }
static void zss_trap_468(void) { zss_trap_hit(468); }
static void zss_trap_469(void) { zss_trap_hit(469); }
static void zss_trap_470(void) { zss_trap_hit(470); }
static void zss_trap_471(void) { zss_trap_hit(471); }
static void zss_trap_472(void) { zss_trap_hit(472); }
static void zss_trap_473(void) { zss_trap_hit(473); }
static void zss_trap_474(void) { zss_trap_hit(474); }
static void zss_trap_475(void) { zss_trap_hit(475); }
static void zss_trap_476(void) { zss_trap_hit(476); }
static void zss_trap_477(void) { zss_trap_hit(477); }
static void zss_trap_478(void) { zss_trap_hit(478); }
static void zss_trap_479(void) { zss_trap_hit(479); }
static void zss_trap_480(void) { zss_trap_hit(480); }
static void zss_trap_481(void) { zss_trap_hit(481); }
static void zss_trap_482(void) { zss_trap_hit(482); }
static void zss_trap_483(void) { zss_trap_hit(483); }
static void zss_trap_484(void) { zss_trap_hit(484); }
static void zss_trap_485(void) { zss_trap_hit(485); }
static void zss_trap_486(void) { zss_trap_hit(486); }
static void zss_trap_487(void) { zss_trap_hit(487); }
static void zss_trap_488(void) { zss_trap_hit(488); }
static void zss_trap_489(void) { zss_trap_hit(489); }
static void zss_trap_490(void) { zss_trap_hit(490); }
static void zss_trap_491(void) { zss_trap_hit(491); }
static void zss_trap_492(void) { zss_trap_hit(492); }
static void zss_trap_493(void) { zss_trap_hit(493); }
static void zss_trap_494(void) { zss_trap_hit(494); }
static void zss_trap_495(void) { zss_trap_hit(495); }
static void zss_trap_496(void) { zss_trap_hit(496); }
static void zss_trap_497(void) { zss_trap_hit(497); }
static void zss_trap_498(void) { zss_trap_hit(498); }
static void zss_trap_499(void) { zss_trap_hit(499); }
static void zss_trap_500(void) { zss_trap_hit(500); }
static void zss_trap_501(void) { zss_trap_hit(501); }
static void zss_trap_502(void) { zss_trap_hit(502); }
static void zss_trap_503(void) { zss_trap_hit(503); }
static void zss_trap_504(void) { zss_trap_hit(504); }
static void zss_trap_505(void) { zss_trap_hit(505); }
static void zss_trap_506(void) { zss_trap_hit(506); }
static void zss_trap_507(void) { zss_trap_hit(507); }
static void zss_trap_508(void) { zss_trap_hit(508); }
static void zss_trap_509(void) { zss_trap_hit(509); }
static void zss_trap_510(void) { zss_trap_hit(510); }
static void zss_trap_511(void) { zss_trap_hit(511); }
static void zss_trap_512(void) { zss_trap_hit(512); }
static void zss_trap_513(void) { zss_trap_hit(513); }
static void zss_trap_514(void) { zss_trap_hit(514); }
static void zss_trap_515(void) { zss_trap_hit(515); }
static void zss_trap_516(void) { zss_trap_hit(516); }
static void zss_trap_517(void) { zss_trap_hit(517); }
static void zss_trap_518(void) { zss_trap_hit(518); }
static void zss_trap_519(void) { zss_trap_hit(519); }
static void zss_trap_520(void) { zss_trap_hit(520); }
static void zss_trap_521(void) { zss_trap_hit(521); }
static void zss_trap_522(void) { zss_trap_hit(522); }
static void zss_trap_523(void) { zss_trap_hit(523); }
static void zss_trap_524(void) { zss_trap_hit(524); }
static void zss_trap_525(void) { zss_trap_hit(525); }
static void zss_trap_526(void) { zss_trap_hit(526); }
static void zss_trap_527(void) { zss_trap_hit(527); }
static void zss_trap_528(void) { zss_trap_hit(528); }
static void zss_trap_529(void) { zss_trap_hit(529); }
static void zss_trap_530(void) { zss_trap_hit(530); }
static void zss_trap_531(void) { zss_trap_hit(531); }
static void zss_trap_532(void) { zss_trap_hit(532); }
static void zss_trap_533(void) { zss_trap_hit(533); }
static void zss_trap_534(void) { zss_trap_hit(534); }
static void zss_trap_535(void) { zss_trap_hit(535); }
static void zss_trap_536(void) { zss_trap_hit(536); }
static void zss_trap_537(void) { zss_trap_hit(537); }
static void zss_trap_538(void) { zss_trap_hit(538); }
static void zss_trap_539(void) { zss_trap_hit(539); }
static void zss_trap_540(void) { zss_trap_hit(540); }
static void zss_trap_541(void) { zss_trap_hit(541); }
static void zss_trap_542(void) { zss_trap_hit(542); }
static void zss_trap_543(void) { zss_trap_hit(543); }
static void zss_trap_544(void) { zss_trap_hit(544); }
static void zss_trap_545(void) { zss_trap_hit(545); }
static void zss_trap_546(void) { zss_trap_hit(546); }
static void zss_trap_547(void) { zss_trap_hit(547); }
static void zss_trap_548(void) { zss_trap_hit(548); }
static void zss_trap_549(void) { zss_trap_hit(549); }
static void zss_trap_550(void) { zss_trap_hit(550); }
static void zss_trap_551(void) { zss_trap_hit(551); }
static void zss_trap_552(void) { zss_trap_hit(552); }
static void zss_trap_553(void) { zss_trap_hit(553); }
static void zss_trap_554(void) { zss_trap_hit(554); }
static void zss_trap_555(void) { zss_trap_hit(555); }
static void zss_trap_556(void) { zss_trap_hit(556); }
static void zss_trap_557(void) { zss_trap_hit(557); }
static void zss_trap_558(void) { zss_trap_hit(558); }
static void zss_trap_559(void) { zss_trap_hit(559); }
static void zss_trap_560(void) { zss_trap_hit(560); }
static void zss_trap_561(void) { zss_trap_hit(561); }
static void zss_trap_562(void) { zss_trap_hit(562); }
static void zss_trap_563(void) { zss_trap_hit(563); }
static void zss_trap_564(void) { zss_trap_hit(564); }
static void zss_trap_565(void) { zss_trap_hit(565); }
static void zss_trap_566(void) { zss_trap_hit(566); }
static void zss_trap_567(void) { zss_trap_hit(567); }
static void zss_trap_568(void) { zss_trap_hit(568); }
static void zss_trap_569(void) { zss_trap_hit(569); }
static void zss_trap_570(void) { zss_trap_hit(570); }
static void zss_trap_571(void) { zss_trap_hit(571); }
static void zss_trap_572(void) { zss_trap_hit(572); }
static void zss_trap_573(void) { zss_trap_hit(573); }
static void zss_trap_574(void) { zss_trap_hit(574); }
static void zss_trap_575(void) { zss_trap_hit(575); }
static void zss_trap_576(void) { zss_trap_hit(576); }
static void zss_trap_577(void) { zss_trap_hit(577); }
static void zss_trap_578(void) { zss_trap_hit(578); }
static void zss_trap_579(void) { zss_trap_hit(579); }
static void zss_trap_580(void) { zss_trap_hit(580); }
static void zss_trap_581(void) { zss_trap_hit(581); }
static void zss_trap_582(void) { zss_trap_hit(582); }
static void zss_trap_583(void) { zss_trap_hit(583); }
static void zss_trap_584(void) { zss_trap_hit(584); }
static void zss_trap_585(void) { zss_trap_hit(585); }
static void zss_trap_586(void) { zss_trap_hit(586); }
static void zss_trap_587(void) { zss_trap_hit(587); }
static void zss_trap_588(void) { zss_trap_hit(588); }
static void zss_trap_589(void) { zss_trap_hit(589); }
static void zss_trap_590(void) { zss_trap_hit(590); }
static void zss_trap_591(void) { zss_trap_hit(591); }
static void zss_trap_592(void) { zss_trap_hit(592); }
static void zss_trap_593(void) { zss_trap_hit(593); }
static void zss_trap_594(void) { zss_trap_hit(594); }
static void zss_trap_595(void) { zss_trap_hit(595); }
static void zss_trap_596(void) { zss_trap_hit(596); }
static void zss_trap_597(void) { zss_trap_hit(597); }
static void zss_trap_598(void) { zss_trap_hit(598); }
static void zss_trap_599(void) { zss_trap_hit(599); }
static void zss_trap_600(void) { zss_trap_hit(600); }
static void zss_trap_601(void) { zss_trap_hit(601); }
static void zss_trap_602(void) { zss_trap_hit(602); }
static void zss_trap_603(void) { zss_trap_hit(603); }
static void zss_trap_604(void) { zss_trap_hit(604); }
static void zss_trap_605(void) { zss_trap_hit(605); }
static void zss_trap_606(void) { zss_trap_hit(606); }
static void zss_trap_607(void) { zss_trap_hit(607); }
static void zss_trap_608(void) { zss_trap_hit(608); }
static void zss_trap_609(void) { zss_trap_hit(609); }
static void zss_trap_610(void) { zss_trap_hit(610); }
static void zss_trap_611(void) { zss_trap_hit(611); }
static void zss_trap_612(void) { zss_trap_hit(612); }
static void zss_trap_613(void) { zss_trap_hit(613); }
static void zss_trap_614(void) { zss_trap_hit(614); }
static void zss_trap_615(void) { zss_trap_hit(615); }
static void zss_trap_616(void) { zss_trap_hit(616); }
static void zss_trap_617(void) { zss_trap_hit(617); }
static void zss_trap_618(void) { zss_trap_hit(618); }
static void zss_trap_619(void) { zss_trap_hit(619); }
static void zss_trap_620(void) { zss_trap_hit(620); }
static void zss_trap_621(void) { zss_trap_hit(621); }
static void zss_trap_622(void) { zss_trap_hit(622); }
static void zss_trap_623(void) { zss_trap_hit(623); }
static void zss_trap_624(void) { zss_trap_hit(624); }
static void zss_trap_625(void) { zss_trap_hit(625); }
static void zss_trap_626(void) { zss_trap_hit(626); }
static void zss_trap_627(void) { zss_trap_hit(627); }
static void zss_trap_628(void) { zss_trap_hit(628); }
static void zss_trap_629(void) { zss_trap_hit(629); }
static void zss_trap_630(void) { zss_trap_hit(630); }
static void zss_trap_631(void) { zss_trap_hit(631); }
static void zss_trap_632(void) { zss_trap_hit(632); }
static void zss_trap_633(void) { zss_trap_hit(633); }
static void zss_trap_634(void) { zss_trap_hit(634); }
static void zss_trap_635(void) { zss_trap_hit(635); }
static void zss_trap_636(void) { zss_trap_hit(636); }
static void zss_trap_637(void) { zss_trap_hit(637); }
static void zss_trap_638(void) { zss_trap_hit(638); }
static void zss_trap_639(void) { zss_trap_hit(639); }
static void zss_trap_640(void) { zss_trap_hit(640); }
static void zss_trap_641(void) { zss_trap_hit(641); }
static void zss_trap_642(void) { zss_trap_hit(642); }
static void zss_trap_643(void) { zss_trap_hit(643); }
static void zss_trap_644(void) { zss_trap_hit(644); }
static void zss_trap_645(void) { zss_trap_hit(645); }
static void zss_trap_646(void) { zss_trap_hit(646); }
static void zss_trap_647(void) { zss_trap_hit(647); }
static void zss_trap_648(void) { zss_trap_hit(648); }
static void zss_trap_649(void) { zss_trap_hit(649); }
static void zss_trap_650(void) { zss_trap_hit(650); }
static void zss_trap_651(void) { zss_trap_hit(651); }
static void zss_trap_652(void) { zss_trap_hit(652); }
static void zss_trap_653(void) { zss_trap_hit(653); }
static void zss_trap_654(void) { zss_trap_hit(654); }
static void zss_trap_655(void) { zss_trap_hit(655); }
static void zss_trap_656(void) { zss_trap_hit(656); }
static void zss_trap_657(void) { zss_trap_hit(657); }
static void zss_trap_658(void) { zss_trap_hit(658); }
static void zss_trap_659(void) { zss_trap_hit(659); }
static void zss_trap_660(void) { zss_trap_hit(660); }
static void zss_trap_661(void) { zss_trap_hit(661); }
static void zss_trap_662(void) { zss_trap_hit(662); }
static void zss_trap_663(void) { zss_trap_hit(663); }
static void zss_trap_664(void) { zss_trap_hit(664); }
static void zss_trap_665(void) { zss_trap_hit(665); }
static void zss_trap_666(void) { zss_trap_hit(666); }
static void zss_trap_667(void) { zss_trap_hit(667); }
static void zss_trap_668(void) { zss_trap_hit(668); }
static void zss_trap_669(void) { zss_trap_hit(669); }
static void zss_trap_670(void) { zss_trap_hit(670); }
static void zss_trap_671(void) { zss_trap_hit(671); }
static void zss_trap_672(void) { zss_trap_hit(672); }
static void zss_trap_673(void) { zss_trap_hit(673); }
static void zss_trap_674(void) { zss_trap_hit(674); }
static void zss_trap_675(void) { zss_trap_hit(675); }
static void zss_trap_676(void) { zss_trap_hit(676); }
static void zss_trap_677(void) { zss_trap_hit(677); }
static void zss_trap_678(void) { zss_trap_hit(678); }
static void zss_trap_679(void) { zss_trap_hit(679); }
static void zss_trap_680(void) { zss_trap_hit(680); }
static void zss_trap_681(void) { zss_trap_hit(681); }
static void zss_trap_682(void) { zss_trap_hit(682); }
static void zss_trap_683(void) { zss_trap_hit(683); }
static void zss_trap_684(void) { zss_trap_hit(684); }
static void zss_trap_685(void) { zss_trap_hit(685); }
static void zss_trap_686(void) { zss_trap_hit(686); }
static void zss_trap_687(void) { zss_trap_hit(687); }
static void zss_trap_688(void) { zss_trap_hit(688); }
static void zss_trap_689(void) { zss_trap_hit(689); }
static void zss_trap_690(void) { zss_trap_hit(690); }
static void zss_trap_691(void) { zss_trap_hit(691); }
static void zss_trap_692(void) { zss_trap_hit(692); }
static void zss_trap_693(void) { zss_trap_hit(693); }
static void zss_trap_694(void) { zss_trap_hit(694); }
static void zss_trap_695(void) { zss_trap_hit(695); }
static void zss_trap_696(void) { zss_trap_hit(696); }
static void zss_trap_697(void) { zss_trap_hit(697); }
static void zss_trap_698(void) { zss_trap_hit(698); }
static void zss_trap_699(void) { zss_trap_hit(699); }
static void zss_trap_700(void) { zss_trap_hit(700); }
static void zss_trap_701(void) { zss_trap_hit(701); }
static void zss_trap_702(void) { zss_trap_hit(702); }
static void zss_trap_703(void) { zss_trap_hit(703); }
static void zss_trap_704(void) { zss_trap_hit(704); }
static void zss_trap_705(void) { zss_trap_hit(705); }
static void zss_trap_706(void) { zss_trap_hit(706); }
static void zss_trap_707(void) { zss_trap_hit(707); }
static void zss_trap_708(void) { zss_trap_hit(708); }
static void zss_trap_709(void) { zss_trap_hit(709); }
static void zss_trap_710(void) { zss_trap_hit(710); }
static void zss_trap_711(void) { zss_trap_hit(711); }
static void zss_trap_712(void) { zss_trap_hit(712); }
static void zss_trap_713(void) { zss_trap_hit(713); }
static void zss_trap_714(void) { zss_trap_hit(714); }
static void zss_trap_715(void) { zss_trap_hit(715); }
static void zss_trap_716(void) { zss_trap_hit(716); }
static void zss_trap_717(void) { zss_trap_hit(717); }
static void zss_trap_718(void) { zss_trap_hit(718); }
static void zss_trap_719(void) { zss_trap_hit(719); }
static void zss_trap_720(void) { zss_trap_hit(720); }
static void zss_trap_721(void) { zss_trap_hit(721); }
static void zss_trap_722(void) { zss_trap_hit(722); }
static void zss_trap_723(void) { zss_trap_hit(723); }
static void zss_trap_724(void) { zss_trap_hit(724); }
static void zss_trap_725(void) { zss_trap_hit(725); }
static void zss_trap_726(void) { zss_trap_hit(726); }
static void zss_trap_727(void) { zss_trap_hit(727); }
static void zss_trap_728(void) { zss_trap_hit(728); }
static void zss_trap_729(void) { zss_trap_hit(729); }
static void zss_trap_730(void) { zss_trap_hit(730); }
static void zss_trap_731(void) { zss_trap_hit(731); }
static void zss_trap_732(void) { zss_trap_hit(732); }
static void zss_trap_733(void) { zss_trap_hit(733); }
static void zss_trap_734(void) { zss_trap_hit(734); }
static void zss_trap_735(void) { zss_trap_hit(735); }
static void zss_trap_736(void) { zss_trap_hit(736); }
static void zss_trap_737(void) { zss_trap_hit(737); }
static void zss_trap_738(void) { zss_trap_hit(738); }
static void zss_trap_739(void) { zss_trap_hit(739); }
static void zss_trap_740(void) { zss_trap_hit(740); }
static void zss_trap_741(void) { zss_trap_hit(741); }
static void zss_trap_742(void) { zss_trap_hit(742); }
static void zss_trap_743(void) { zss_trap_hit(743); }
static void zss_trap_744(void) { zss_trap_hit(744); }
static void zss_trap_745(void) { zss_trap_hit(745); }
static void zss_trap_746(void) { zss_trap_hit(746); }
static void zss_trap_747(void) { zss_trap_hit(747); }
static void zss_trap_748(void) { zss_trap_hit(748); }
static void zss_trap_749(void) { zss_trap_hit(749); }
static void zss_trap_750(void) { zss_trap_hit(750); }
static void zss_trap_751(void) { zss_trap_hit(751); }
static void zss_trap_752(void) { zss_trap_hit(752); }
static void zss_trap_753(void) { zss_trap_hit(753); }
static void zss_trap_754(void) { zss_trap_hit(754); }
static void zss_trap_755(void) { zss_trap_hit(755); }
static void zss_trap_756(void) { zss_trap_hit(756); }
static void zss_trap_757(void) { zss_trap_hit(757); }
static void zss_trap_758(void) { zss_trap_hit(758); }
static void zss_trap_759(void) { zss_trap_hit(759); }
static void zss_trap_760(void) { zss_trap_hit(760); }
static void zss_trap_761(void) { zss_trap_hit(761); }
static void zss_trap_762(void) { zss_trap_hit(762); }
static void zss_trap_763(void) { zss_trap_hit(763); }
static void zss_trap_764(void) { zss_trap_hit(764); }
static void zss_trap_765(void) { zss_trap_hit(765); }
static void zss_trap_766(void) { zss_trap_hit(766); }
static void zss_trap_767(void) { zss_trap_hit(767); }

static void (*const trap_fns[768])(void) = { zss_trap_0, zss_trap_1, zss_trap_2, zss_trap_3, zss_trap_4, zss_trap_5, zss_trap_6, zss_trap_7, zss_trap_8, zss_trap_9, zss_trap_10, zss_trap_11, zss_trap_12, zss_trap_13, zss_trap_14, zss_trap_15, zss_trap_16, zss_trap_17, zss_trap_18, zss_trap_19, zss_trap_20, zss_trap_21, zss_trap_22, zss_trap_23, zss_trap_24, zss_trap_25, zss_trap_26, zss_trap_27, zss_trap_28, zss_trap_29, zss_trap_30, zss_trap_31, zss_trap_32, zss_trap_33, zss_trap_34, zss_trap_35, zss_trap_36, zss_trap_37, zss_trap_38, zss_trap_39, zss_trap_40, zss_trap_41, zss_trap_42, zss_trap_43, zss_trap_44, zss_trap_45, zss_trap_46, zss_trap_47, zss_trap_48, zss_trap_49, zss_trap_50, zss_trap_51, zss_trap_52, zss_trap_53, zss_trap_54, zss_trap_55, zss_trap_56, zss_trap_57, zss_trap_58, zss_trap_59, zss_trap_60, zss_trap_61, zss_trap_62, zss_trap_63, zss_trap_64, zss_trap_65, zss_trap_66, zss_trap_67, zss_trap_68, zss_trap_69, zss_trap_70, zss_trap_71, zss_trap_72, zss_trap_73, zss_trap_74, zss_trap_75, zss_trap_76, zss_trap_77, zss_trap_78, zss_trap_79, zss_trap_80, zss_trap_81, zss_trap_82, zss_trap_83, zss_trap_84, zss_trap_85, zss_trap_86, zss_trap_87, zss_trap_88, zss_trap_89, zss_trap_90, zss_trap_91, zss_trap_92, zss_trap_93, zss_trap_94, zss_trap_95, zss_trap_96, zss_trap_97, zss_trap_98, zss_trap_99, zss_trap_100, zss_trap_101, zss_trap_102, zss_trap_103, zss_trap_104, zss_trap_105, zss_trap_106, zss_trap_107, zss_trap_108, zss_trap_109, zss_trap_110, zss_trap_111, zss_trap_112, zss_trap_113, zss_trap_114, zss_trap_115, zss_trap_116, zss_trap_117, zss_trap_118, zss_trap_119, zss_trap_120, zss_trap_121, zss_trap_122, zss_trap_123, zss_trap_124, zss_trap_125, zss_trap_126, zss_trap_127, zss_trap_128, zss_trap_129, zss_trap_130, zss_trap_131, zss_trap_132, zss_trap_133, zss_trap_134, zss_trap_135, zss_trap_136, zss_trap_137, zss_trap_138, zss_trap_139, zss_trap_140, zss_trap_141, zss_trap_142, zss_trap_143, zss_trap_144, zss_trap_145, zss_trap_146, zss_trap_147, zss_trap_148, zss_trap_149, zss_trap_150, zss_trap_151, zss_trap_152, zss_trap_153, zss_trap_154, zss_trap_155, zss_trap_156, zss_trap_157, zss_trap_158, zss_trap_159, zss_trap_160, zss_trap_161, zss_trap_162, zss_trap_163, zss_trap_164, zss_trap_165, zss_trap_166, zss_trap_167, zss_trap_168, zss_trap_169, zss_trap_170, zss_trap_171, zss_trap_172, zss_trap_173, zss_trap_174, zss_trap_175, zss_trap_176, zss_trap_177, zss_trap_178, zss_trap_179, zss_trap_180, zss_trap_181, zss_trap_182, zss_trap_183, zss_trap_184, zss_trap_185, zss_trap_186, zss_trap_187, zss_trap_188, zss_trap_189, zss_trap_190, zss_trap_191, zss_trap_192, zss_trap_193, zss_trap_194, zss_trap_195, zss_trap_196, zss_trap_197, zss_trap_198, zss_trap_199, zss_trap_200, zss_trap_201, zss_trap_202, zss_trap_203, zss_trap_204, zss_trap_205, zss_trap_206, zss_trap_207, zss_trap_208, zss_trap_209, zss_trap_210, zss_trap_211, zss_trap_212, zss_trap_213, zss_trap_214, zss_trap_215, zss_trap_216, zss_trap_217, zss_trap_218, zss_trap_219, zss_trap_220, zss_trap_221, zss_trap_222, zss_trap_223, zss_trap_224, zss_trap_225, zss_trap_226, zss_trap_227, zss_trap_228, zss_trap_229, zss_trap_230, zss_trap_231, zss_trap_232, zss_trap_233, zss_trap_234, zss_trap_235, zss_trap_236, zss_trap_237, zss_trap_238, zss_trap_239, zss_trap_240, zss_trap_241, zss_trap_242, zss_trap_243, zss_trap_244, zss_trap_245, zss_trap_246, zss_trap_247, zss_trap_248, zss_trap_249, zss_trap_250, zss_trap_251, zss_trap_252, zss_trap_253, zss_trap_254, zss_trap_255, zss_trap_256, zss_trap_257, zss_trap_258, zss_trap_259, zss_trap_260, zss_trap_261, zss_trap_262, zss_trap_263, zss_trap_264, zss_trap_265, zss_trap_266, zss_trap_267, zss_trap_268, zss_trap_269, zss_trap_270, zss_trap_271, zss_trap_272, zss_trap_273, zss_trap_274, zss_trap_275, zss_trap_276, zss_trap_277, zss_trap_278, zss_trap_279, zss_trap_280, zss_trap_281, zss_trap_282, zss_trap_283, zss_trap_284, zss_trap_285, zss_trap_286, zss_trap_287, zss_trap_288, zss_trap_289, zss_trap_290, zss_trap_291, zss_trap_292, zss_trap_293, zss_trap_294, zss_trap_295, zss_trap_296, zss_trap_297, zss_trap_298, zss_trap_299, zss_trap_300, zss_trap_301, zss_trap_302, zss_trap_303, zss_trap_304, zss_trap_305, zss_trap_306, zss_trap_307, zss_trap_308, zss_trap_309, zss_trap_310, zss_trap_311, zss_trap_312, zss_trap_313, zss_trap_314, zss_trap_315, zss_trap_316, zss_trap_317, zss_trap_318, zss_trap_319, zss_trap_320, zss_trap_321, zss_trap_322, zss_trap_323, zss_trap_324, zss_trap_325, zss_trap_326, zss_trap_327, zss_trap_328, zss_trap_329, zss_trap_330, zss_trap_331, zss_trap_332, zss_trap_333, zss_trap_334, zss_trap_335, zss_trap_336, zss_trap_337, zss_trap_338, zss_trap_339, zss_trap_340, zss_trap_341, zss_trap_342, zss_trap_343, zss_trap_344, zss_trap_345, zss_trap_346, zss_trap_347, zss_trap_348, zss_trap_349, zss_trap_350, zss_trap_351, zss_trap_352, zss_trap_353, zss_trap_354, zss_trap_355, zss_trap_356, zss_trap_357, zss_trap_358, zss_trap_359, zss_trap_360, zss_trap_361, zss_trap_362, zss_trap_363, zss_trap_364, zss_trap_365, zss_trap_366, zss_trap_367, zss_trap_368, zss_trap_369, zss_trap_370, zss_trap_371, zss_trap_372, zss_trap_373, zss_trap_374, zss_trap_375, zss_trap_376, zss_trap_377, zss_trap_378, zss_trap_379, zss_trap_380, zss_trap_381, zss_trap_382, zss_trap_383, zss_trap_384, zss_trap_385, zss_trap_386, zss_trap_387, zss_trap_388, zss_trap_389, zss_trap_390, zss_trap_391, zss_trap_392, zss_trap_393, zss_trap_394, zss_trap_395, zss_trap_396, zss_trap_397, zss_trap_398, zss_trap_399, zss_trap_400, zss_trap_401, zss_trap_402, zss_trap_403, zss_trap_404, zss_trap_405, zss_trap_406, zss_trap_407, zss_trap_408, zss_trap_409, zss_trap_410, zss_trap_411, zss_trap_412, zss_trap_413, zss_trap_414, zss_trap_415, zss_trap_416, zss_trap_417, zss_trap_418, zss_trap_419, zss_trap_420, zss_trap_421, zss_trap_422, zss_trap_423, zss_trap_424, zss_trap_425, zss_trap_426, zss_trap_427, zss_trap_428, zss_trap_429, zss_trap_430, zss_trap_431, zss_trap_432, zss_trap_433, zss_trap_434, zss_trap_435, zss_trap_436, zss_trap_437, zss_trap_438, zss_trap_439, zss_trap_440, zss_trap_441, zss_trap_442, zss_trap_443, zss_trap_444, zss_trap_445, zss_trap_446, zss_trap_447, zss_trap_448, zss_trap_449, zss_trap_450, zss_trap_451, zss_trap_452, zss_trap_453, zss_trap_454, zss_trap_455, zss_trap_456, zss_trap_457, zss_trap_458, zss_trap_459, zss_trap_460, zss_trap_461, zss_trap_462, zss_trap_463, zss_trap_464, zss_trap_465, zss_trap_466, zss_trap_467, zss_trap_468, zss_trap_469, zss_trap_470, zss_trap_471, zss_trap_472, zss_trap_473, zss_trap_474, zss_trap_475, zss_trap_476, zss_trap_477, zss_trap_478, zss_trap_479, zss_trap_480, zss_trap_481, zss_trap_482, zss_trap_483, zss_trap_484, zss_trap_485, zss_trap_486, zss_trap_487, zss_trap_488, zss_trap_489, zss_trap_490, zss_trap_491, zss_trap_492, zss_trap_493, zss_trap_494, zss_trap_495, zss_trap_496, zss_trap_497, zss_trap_498, zss_trap_499, zss_trap_500, zss_trap_501, zss_trap_502, zss_trap_503, zss_trap_504, zss_trap_505, zss_trap_506, zss_trap_507, zss_trap_508, zss_trap_509, zss_trap_510, zss_trap_511, zss_trap_512, zss_trap_513, zss_trap_514, zss_trap_515, zss_trap_516, zss_trap_517, zss_trap_518, zss_trap_519, zss_trap_520, zss_trap_521, zss_trap_522, zss_trap_523, zss_trap_524, zss_trap_525, zss_trap_526, zss_trap_527, zss_trap_528, zss_trap_529, zss_trap_530, zss_trap_531, zss_trap_532, zss_trap_533, zss_trap_534, zss_trap_535, zss_trap_536, zss_trap_537, zss_trap_538, zss_trap_539, zss_trap_540, zss_trap_541, zss_trap_542, zss_trap_543, zss_trap_544, zss_trap_545, zss_trap_546, zss_trap_547, zss_trap_548, zss_trap_549, zss_trap_550, zss_trap_551, zss_trap_552, zss_trap_553, zss_trap_554, zss_trap_555, zss_trap_556, zss_trap_557, zss_trap_558, zss_trap_559, zss_trap_560, zss_trap_561, zss_trap_562, zss_trap_563, zss_trap_564, zss_trap_565, zss_trap_566, zss_trap_567, zss_trap_568, zss_trap_569, zss_trap_570, zss_trap_571, zss_trap_572, zss_trap_573, zss_trap_574, zss_trap_575, zss_trap_576, zss_trap_577, zss_trap_578, zss_trap_579, zss_trap_580, zss_trap_581, zss_trap_582, zss_trap_583, zss_trap_584, zss_trap_585, zss_trap_586, zss_trap_587, zss_trap_588, zss_trap_589, zss_trap_590, zss_trap_591, zss_trap_592, zss_trap_593, zss_trap_594, zss_trap_595, zss_trap_596, zss_trap_597, zss_trap_598, zss_trap_599, zss_trap_600, zss_trap_601, zss_trap_602, zss_trap_603, zss_trap_604, zss_trap_605, zss_trap_606, zss_trap_607, zss_trap_608, zss_trap_609, zss_trap_610, zss_trap_611, zss_trap_612, zss_trap_613, zss_trap_614, zss_trap_615, zss_trap_616, zss_trap_617, zss_trap_618, zss_trap_619, zss_trap_620, zss_trap_621, zss_trap_622, zss_trap_623, zss_trap_624, zss_trap_625, zss_trap_626, zss_trap_627, zss_trap_628, zss_trap_629, zss_trap_630, zss_trap_631, zss_trap_632, zss_trap_633, zss_trap_634, zss_trap_635, zss_trap_636, zss_trap_637, zss_trap_638, zss_trap_639, zss_trap_640, zss_trap_641, zss_trap_642, zss_trap_643, zss_trap_644, zss_trap_645, zss_trap_646, zss_trap_647, zss_trap_648, zss_trap_649, zss_trap_650, zss_trap_651, zss_trap_652, zss_trap_653, zss_trap_654, zss_trap_655, zss_trap_656, zss_trap_657, zss_trap_658, zss_trap_659, zss_trap_660, zss_trap_661, zss_trap_662, zss_trap_663, zss_trap_664, zss_trap_665, zss_trap_666, zss_trap_667, zss_trap_668, zss_trap_669, zss_trap_670, zss_trap_671, zss_trap_672, zss_trap_673, zss_trap_674, zss_trap_675, zss_trap_676, zss_trap_677, zss_trap_678, zss_trap_679, zss_trap_680, zss_trap_681, zss_trap_682, zss_trap_683, zss_trap_684, zss_trap_685, zss_trap_686, zss_trap_687, zss_trap_688, zss_trap_689, zss_trap_690, zss_trap_691, zss_trap_692, zss_trap_693, zss_trap_694, zss_trap_695, zss_trap_696, zss_trap_697, zss_trap_698, zss_trap_699, zss_trap_700, zss_trap_701, zss_trap_702, zss_trap_703, zss_trap_704, zss_trap_705, zss_trap_706, zss_trap_707, zss_trap_708, zss_trap_709, zss_trap_710, zss_trap_711, zss_trap_712, zss_trap_713, zss_trap_714, zss_trap_715, zss_trap_716, zss_trap_717, zss_trap_718, zss_trap_719, zss_trap_720, zss_trap_721, zss_trap_722, zss_trap_723, zss_trap_724, zss_trap_725, zss_trap_726, zss_trap_727, zss_trap_728, zss_trap_729, zss_trap_730, zss_trap_731, zss_trap_732, zss_trap_733, zss_trap_734, zss_trap_735, zss_trap_736, zss_trap_737, zss_trap_738, zss_trap_739, zss_trap_740, zss_trap_741, zss_trap_742, zss_trap_743, zss_trap_744, zss_trap_745, zss_trap_746, zss_trap_747, zss_trap_748, zss_trap_749, zss_trap_750, zss_trap_751, zss_trap_752, zss_trap_753, zss_trap_754, zss_trap_755, zss_trap_756, zss_trap_757, zss_trap_758, zss_trap_759, zss_trap_760, zss_trap_761, zss_trap_762, zss_trap_763, zss_trap_764, zss_trap_765, zss_trap_766, zss_trap_767 };

static PFN_vkVoidFunction zss_trap_for(const char *name)
{
    static int on = -1;

    if (on < 0)
        on = getenv("ZSS_TRAP") != NULL;
    if (!on || ntraps >= 768)
        return NULL;
    for (int i = 0; i < ntraps; i++)
        if (!strcmp(trap_names[i], name))
            return (PFN_vkVoidFunction)trap_fns[i];
    trap_names[ntraps] = strdup(name);
    return (PFN_vkVoidFunction)trap_fns[ntraps++];
}

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
    return zss_vk11_device_proc(name);
}

PFN_vkVoidFunction zss_instance_proc(const char *name)
{
    for (size_t i = 0; i < sizeof(instance_entries) / sizeof(instance_entries[0]); i++)
        if (!strcmp(instance_entries[i].name, name))
            return instance_entries[i].fn;
    return zss_vk11_instance_proc(name);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL zss_GetDeviceProcAddr(VkDevice device,
                                                                      const char *name)
{
    PFN_vkVoidFunction fn = zss_device_proc(name);

    (void)device;
    /* What an application asked for and did not get: the first thing to look at when one misbehaves. */
    if (!fn) {
        zss_dbg("an application asked for %s, which the layer does not provide", name);
        fn = zss_trap_for(name);
    }
    return fn;
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
    PFN_vkVoidFunction fn = zss_instance_proc(name);

    (void)instance;
    return fn ? fn : zss_device_proc(name);
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
