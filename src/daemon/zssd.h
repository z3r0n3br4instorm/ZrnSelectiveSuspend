/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ZSSD_H
#define ZSSD_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define ZSSD_MAX_GPUS 8
#define ZSSD_MAX_FUNCS 8
#define ZSSD_ERR 256

/* GS_LOST: the device left the bus without a detach. */
enum gpu_state { GS_ATTACHED, GS_DETACHING, GS_POWERED_OFF, GS_SAFE_TO_REMOVE, GS_ATTACHING, GS_LOST };

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
    /* Optional: replace the driver-specific suspend and resume (used by the test backend). */
    int (*suspend)(struct gpu *g, char *err);
    int (*resume)(struct gpu *g, char *err);
    /* Optional: whether the device is driving a connected display. */
    bool (*drives_display)(struct gpu *g, char *which, size_t n);
    /* Optional: where this device's driver reports wake requests. */
    const char *(*wake_file)(struct gpu *g);
    /* Optional: processes to treat as holding the device, besides those found in /proc. */
    int (*extra_holders)(struct gpu *g, pid_t *pids, int max);
};

struct gpu {
    char pci[16];
    const struct backend *backend;
    enum gpu_state state;
    bool dry_detached; /* applications moved away by a dry run */
    bool seen;         /* has been on the bus at some point; a test address never is */
    bool loss_pending; /* vanished or reported lost, evacuation not yet run */
    bool evacuating;
    /* Device numbers of its nodes, kept so holders can be found after it is gone. */
    dev_t nodes[32];
    int nnodes;
    char slot[300];    /* pciehp: sysfs slot directory */
    char driver[64];   /* kernel driver bound when detach began */
    char audio_driver[64];
    bool nvidia_suspended, rpm_suspended;
    bool kmod; /* the kernel module does state and power for this device */
    unsigned char config[256];
    size_t config_len;

    char name[96];          /* human-readable, for progress messages */
    char stopped[256];      /* services stopped for the power-off, comma separated */
    pid_t frozen[32];       /* processes outside the layer frozen for the power-off */
    int nfrozen;
    bool off;               /* powered off by "off": watched for wake requests */
    bool console;           /* the screen was switched to the text console for it */
    long long off_since;    /* ms */
    long wake_base;         /* wake counter when power was cut */
    int wake_fd;            /* open wake file when it can be polled, else -1 */
    int wakes;              /* times woken by a request since the daemon started */
    pid_t waiting[16];      /* callers asleep on the suspended driver that the device was not woken for */
    int nwaiting;
    bool serving;           /* off on request, but powered for a moment because the display server called */
    long long serve_until;  /* when it goes off again */
    long long serve_ended;  /* when it last went off again */
    long long serve_for;    /* ms it stays on per call; grows when calls come close together */
    int served;             /* times that has happened */
    long long idle_since;   /* ms; last moment the device was in use */
    long long idle_wait;    /* ms to wait before the next automatic power-off; 0 = configured value */
    int quick_wakes;        /* automatic power-offs in a row that ended quickly */
    bool auto_off;          /* the current power-off was the idle timer's */
    char last_refusal[160]; /* so the idle timer logs a reason once, not every second */
};

const char *state_name(enum gpu_state s);

/* Settings shared by the daemon's files (zssd.c owns them). */
struct zssd_config {
    const char *runtime_dir;   /* markers and saved PCI configuration */
    const char *stop_services; /* comma-separated units stopped around a power-off */
    const char *kmod_backend;  /* power backend the kernel module is told to use; "" lets it choose */
    const char *hide_while_off; /* "auto", "no", or extra paths hidden with the device */
    const char *service_cmd;   /* systemctl, or a stand-in for tests */
    const char *wake_file;     /* override for where wake requests are read */
    long long idle_timeout;    /* ms; 0 = never power off unasked */
    long long quick_wake;      /* ms; a power-off that ends sooner counts as woken quickly */
};
extern struct zssd_config zssd_cfg;

/* What a process holding the device means for a power-off. */
enum holder_verdict { HV_MIGRATE, HV_STOP, HV_ALLOW, HV_FREEZE, HV_BLOCK };
struct holder_facts {
    bool registered;     /* started under the ZSS layer */
    bool migratable;
    bool display_server;
    bool listed_service; /* in stop_services */
    bool wake_support;   /* the driver can signal a waiting caller */
    bool console;        /* the screen is on the text console for the duration */
    bool freezable;      /* a requested power-off in place: outsiders are frozen, not refused */
};
enum holder_verdict holder_verdict(const struct holder_facts *f, const char **why);

/* power.c */
const char *gpu_wake_path(struct gpu *g);
bool gpu_wake_supported(struct gpu *g);
bool gpu_on_bus(struct gpu *g);
bool gpu_returned(struct gpu *g);
bool gpu_driver_frozen(struct gpu *g);
int gpu_hide(struct gpu *g, char *what, size_t n);
int gpu_unhide(struct gpu *g);
long gpu_wake_count(struct gpu *g);
long gpu_wake_read(struct gpu *g, pid_t *waiters, int max, int *nwaiters);
bool gpu_drives_display(struct gpu *g, char *which, size_t n);
bool service_listed(const char *comm);
void services_stop(struct gpu *g);
void services_start(struct gpu *g);
void marker_write(const struct gpu *g);
bool marker_read(struct gpu *g);
void marker_remove(const struct gpu *g);
void gpu_describe(struct gpu *g);
int console_enter(void);
void console_leave(void);
void console_print(const char *line);
int console_input_fd(void);

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
int gpu_nodes(const char *pci, dev_t *nodes, int max);
int node_holders(const dev_t *nodes, int nn, pid_t *pids, int max);
void pid_comm(pid_t pid, char *out, size_t n);
bool is_display_server(pid_t pid, const char *comm);
int cgroup_freeze(pid_t pid);
int cgroup_thaw(pid_t pid);
int other_display_device(const char *except, char *out, size_t n);

#endif
