/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ZSSD_H
#define ZSSD_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define ZSSD_MAX_GPUS 8
#define ZSSD_MAX_FUNCS 8
#define ZSSD_ERR 256

enum gpu_state { GS_ATTACHED, GS_DETACHING, GS_POWERED_OFF, GS_SAFE_TO_REMOVE, GS_ATTACHING };

/* How the kernel driver lets go of the device before power is cut. */
enum release_strategy { RS_UNBIND, RS_SUSPEND };

struct gpu;

struct backend {
    const char *name;
    bool removal_safe;
    bool dry_run;
    enum release_strategy strategy;
    /* Returns 0 if this backend can drive the device, else -1 with err set. */
    int (*probe)(struct gpu *g, char *err);
    int (*power_off)(struct gpu *g, char *err);
    int (*power_on)(struct gpu *g, char *err);
    /* 1 powered, 0 not powered, -1 unknown. */
    int (*is_powered)(struct gpu *g);
};

struct gpu {
    char pci[16];
    const struct backend *backend;
    enum gpu_state state;
    bool dry_detached; /* applications moved away by a dry run */
    char slot[300];    /* pciehp: sysfs slot directory */
    char driver[64];   /* kernel driver bound when detach began */
    char audio_driver[64];
    bool nvidia_suspended, rpm_suspended;
    unsigned char config[256];
    size_t config_len;
};

const char *state_name(enum gpu_state s);

/* backend.c */
const struct backend *backend_by_name(const char *name);
const struct backend *backend_detect(struct gpu *g);
int driver_suspend(struct gpu *g, char *err);
int driver_resume(struct gpu *g, char *err);

/* sysfs.c */
int read_text(const char *path, char *buf, size_t n);
int write_text(const char *path, const char *text);
bool path_exists(const char *path);
int pci_functions(const char *pci, char funcs[][16], int max);
void pci_driver(const char *pci, char *out, size_t n);
int pci_remove(const char *pci, char *err);
int pci_rescan(void);
bool pci_present(const char *pci);
bool pci_any_driver(const char *pci);
int gpu_holders(const char *pci, pid_t *pids, int max);
void pid_comm(pid_t pid, char *out, size_t n);
bool is_display_server(pid_t pid, const char *comm);
int cgroup_freeze(pid_t pid);
int cgroup_thaw(pid_t pid);
int other_display_device(const char *except, char *out, size_t n);

#endif
