/* SPDX-License-Identifier: GPL-2.0-only */
/**
 * @file zrn_selective_suspend.h
 * @brief Core definitions for ZrnSelectiveSuspend Universal GPU Shim
 * Supports: NVIDIA (Proprietary/Nouveau), AMD (amdgpu), Intel (Xe/i915)
 * Tier 1 Reference Platform: Apple MacBookPro9,1 (Kepler GT 650M + Lightridge)
 */

#ifndef _ZRN_SELECTIVE_SUSPEND_H_
#define _ZRN_SELECTIVE_SUSPEND_H_

#include <linux/types.h>

/* Target GPU Architecture Stacks */
enum zrn_gpu_vendor {
    ZRN_GPU_VENDOR_NVIDIA_PROPRIETARY = 0, /* nvidia.ko (NVRM) */
    ZRN_GPU_VENDOR_NVIDIA_NOUVEAU     = 1, /* nouveau DRM */
    ZRN_GPU_VENDOR_AMD_ROCM           = 2, /* amdgpu DRM */
    ZRN_GPU_VENDOR_INTEL_ARC          = 3, /* xe / i915 DRM */
    ZRN_GPU_VENDOR_GENERIC_PCI        = 4, /* Universal Fallback */
};

/* Platform Power Control Backends */
enum zrn_power_backend_type {
    ZRN_BACKEND_APPLE_GMUX_CLASSIC    = 0, /* LPC I/O 0x700-0x7fe */
    ZRN_BACKEND_ACPI_PR3_D3COLD       = 1, /* Standard PC ACPI _PR3 */
    ZRN_BACKEND_EGPU_HOTPLUG_PASSIVE  = 2, /* Thunderbolt / OCuLink */
};

/* Universal PCIe AER & Extended Capability Registers */
#define ZRN_PCI_EXT_CAP_AER_ID          0x0001
#define ZRN_PCI_ERR_UNCOR_MASK          0x08
#define ZRN_AER_SURPRISE_DOWN_MASK      (1 << 5)
#define ZRN_AER_COMPLETION_TIMEOUT_MASK (1 << 14)

/*
 * Apple gmux Classic LPC I/O Ports (Reference Platform)
 * Names and values follow the kernel's include/linux/apple-gmux.h and
 * drivers/platform/x86/apple-gmux.c.
 */
#define GMUX_PORT_BASE                  0x700
#define GMUX_PORT_SWITCH_DISPLAY        0x710   /* 2 = iGPU, 3 = dGPU */
#define GMUX_PORT_SWITCH_DDC            0x728   /* 1 = iGPU, 2 = dGPU */
#define GMUX_PORT_SWITCH_EXTERNAL       0x740   /* 2 = iGPU, 3 = dGPU: AUX and hotplug of the external port */
#define GMUX_PORT_DISCRETE_POWER        0x750   /* write 1 then 0 = off, 1 then 3 = on */

/* Reference Platform PCI Identifiers */
#define ZRN_REF_ROOT_PORT_BDF           "0000:00:01.0"
#define ZRN_REF_DGPU_BDF                "0000:01:00.0"
#define ZRN_REF_HDA_BDF                 "0000:01:00.1"
#define ZRN_REF_LIGHTRIDGE_BDF          "0000:05:00.0"

/* Reference MMIO BAR Specifications for GK107M */
#define ZRN_GK107_BAR0_PHYS             0xc2000000
#define ZRN_GK107_BAR0_SIZE             (16 * 1024 * 1024)   /* 16 MB */
#define ZRN_GK107_BAR1_PHYS             0xd0000000
#define ZRN_GK107_BAR1_SIZE             (256 * 1024 * 1024)  /* 256 MB */

/**
 * enum zrn_power_state - ZrnSelectiveSuspend State Machine
 */
enum zrn_power_state {
    ZRN_STATE_ECO_OFF       = 0,  /* 0W, MMIO Shadowed to RAM, Link Masked */
    ZRN_STATE_PRIME_OFFLOAD = 1,  /* dGPU Powered, Internal LCD on Intel */
    ZRN_STATE_LIGHTRIDGE_DP = 2,  /* dGPU Powered, External Display Active */
};

/**
 * struct zrn_device_context - Main runtime context for ZrnSelectiveSuspend
 */
struct zrn_device_context {
    enum zrn_gpu_vendor vendor;
    enum zrn_power_backend_type backend;
    enum zrn_power_state state;
    
    /* MMIO Shadow Virtualization */
    void __iomem *shadow_bar0_buffer;
    unsigned long shadow_bar0_phys;
    void __iomem *real_bar0_virt;
    size_t bar0_size;
    
    /* Saved Bus State */
    uint32_t saved_config_space[64];
    bool aer_masked;
};

#endif /* _ZRN_SELECTIVE_SUSPEND_H_ */
