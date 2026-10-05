# ZrnSelectiveSuspend: Universal GPU Runtime Power-Gating & Hot-Removal Architecture

**Project Codename:** ZrnSelectiveSuspend  
**Target Class:** Universal Linux PCIe GPU Power-Gating, MMIO Virtualization & Surprise-Removal Shim  
**Reference Hardware Platform (Tier 1 Target):** Apple MacBookPro9,1 (Mid-2012 15-inch Unibody, Ivy Bridge + Kepler GT 650M + Lightridge)  
**Supported GPU Stacks:** NVIDIA Proprietary (`nvidia`), Open-Source DRM (`nouveau`, `amdgpu`, `xe`, `i915`)  
**Kernel Target:** Linux 6.x / 7.x (Current Reference: `7.2.2-arch1-1 x86_64`)  
**Document Version:** 1.1.0-UNIVERSAL  
**Author:** zerone  

> **Status (October 2026).** The first milestone is the orderly path: the user
> asks for a GPU to be detached, its applications move to another GPU, and the
> device is released and powered off. It is implemented in userspace (`src/`)
> and specified in `openspec/changes/zss-happy-path/`. The surprise-removal
> subsystems described below (PCIe shield, MMIO shadow, DMA isolator,
> resurrection engine) are **deferred** and not part of that milestone.

---

## 1. Executive Summary & Vision

`ZrnSelectiveSuspend` is an in-kernel virtualization and power-orchestration shim. It decouples the physical hardware power state and link status of a PCIe Graphics Processing Unit (GPU) from the software state of the operating system, display servers (Xorg / Wayland), and user applications.

While developed and verified against the demanding dual-GPU architecture of the **Apple MacBookPro9,1** (featuring Apple `gmux` and Intel **Lightridge** Thunderbolt silicon), **`ZrnSelectiveSuspend` is architected from the ground up to be completely vendor- and platform-agnostic**.

The core architecture enables:
1. **Universal 0W Idle Power Gating:** Cutting physical power rails to any discrete GPU at runtime without unloading kernel modules, disrupting display servers, or terminating client applications.
2. **Surprise Hot-Removal & Reconnect Immunity:** Allowing PCIe GPUs (internal discrete chips or external eGPUs over Thunderbolt 1/2/3/4, USB4, or OCuLink) to physically vanish and reappear on the bus without causing PCIe bus aborts, CPU Machine Check Exceptions (MCE), or kernel panics.
3. **Transparent Re-POST & Resurrection:** Cold-initializing resurrected or reconnected GPUs in software, restoring register states, and re-attaching live display pipelines and compute jobs seamlessly.

---

## 2. Reference Platform (Tier 1): Apple MacBookPro9,1

The initial target hardware provides the ultimate stress-test for this architecture due to its legacy hardware multiplexing, proprietary closed drivers, and first-generation Thunderbolt controllers.

```
                           ┌───────────────────────────────┐
                           │      Intel Core i7-3720QM     │
                           │       (Ivy Bridge Host)       │
                           └───────┬───────────────┬───────┘
                                   │               │
                    Internal Ring  │               │ PCIe 3.0 x8 (Bridge 00:01.0)
                                   │               │
                 ┌─────────────────▼────────┐    ┌─▼────────────────────────┐
                 │  Intel HD Graphics 4000  │    │  NVIDIA GeForce GT 650M  │
                 │   [00:02.0] (i915 DRM)   │    │  [01:00.0] (nvidia DRM)  │
                 └─────────────┬────────────┘    └──────┬─────────────┬─────┘
                               │                        │             │
                        LVDS Lines               LVDS Lines      DisplayPort 1.2
                               │                        │         (Hardwired)
                               │   ┌────────────────┐   │             │
                               └──►│   Apple gmux   │◄──┘             │
                                   │  [0x700-0x7fe] │                 │
                                   └────────┬───────┘                 │
                                            │                         │
                                            ▼                         │
                                    ┌──────────────┐                  │
                                    │ Internal LCD │                  │
                                    │ (1440x900 /  │                  │
                                    │  1680x1050)  │                  │
                                    └──────────────┘                  │
                                                                      │
                      ┌───────────────────────────────────────────────┘
                      │
                      ▼
       ┌───────────────────────────────┐
       │   Intel CV82524 Thunderbolt   │◄──── PCIe 2.0 x4 (Bridge 00:01.1)
       │    Controller ("Lightridge")  │
       │     [05:00.0 - 07:00.0]       │
       └──────────────┬────────────────┘
                      │
                      ▼
       ┌───────────────────────────────┐
       │ Physical Mini-DP / TB1 Socket │
       │     (External Display Out)    │
       └───────────────────────────────┘
```

### Component Status on Reference System

| Component | Identifier | Driver / Modules | Reference Status |
| :--- | :--- | :--- | :--- |
| **iGPU** | Intel HD 4000 (`8086:0166`) | `i915` | **Active Primary:** Drives internal LCD via `modesetting` |
| **dGPU** | NVIDIA GeForce GT 650M Mac Edition (`10de:0fd5`) | `nvidia` (470.256.02) | **Idle / D0:** Powered on in P8 state (~55 °C), unutilized |
| **Audio** | NVIDIA GK107 HDMI Audio (`10de:0e1b`) | `snd_hda_intel` | **Active / D0** |
| **Multiplexer** | Apple `gmux` (v1.9.35 classic) | `apple-gmux` | **LPC I/O 0x700–0x7fe:** Muxed to Intel via GRUB |
| **Thunderbolt** | Intel CV82524 **Lightridge** (`8086:1513`) | `pcieport`, `thunderbolt` | **Host only (0-0):** No downstream devices enumerated |

---

## 3. Why Lightridge Has No Display Output on Linux (Reference Bug)

On macOS, plugging an external monitor into the Lightridge port immediately brings up an extended desktop. On Linux, `xrandr` reports all `DP-1-x` ports as `disconnected`. This failure is the result of four intersecting constraints:

### 3.1. Physical Hardware Routing
The physical DisplayPort traces entering the Lightridge controller originate **strictly from the NVIDIA discrete GPU**. The Intel iGPU has no physical wiring to the Lightridge controller. Any external display must be driven by the dGPU.

### 3.2. The Lightridge Silicon Hotplug Bug (MSI Failure)
The 2010–2012 Intel CV82524 Lightridge controller has a silicon-level design flaw under Linux:
* **Broken MSI Signaling:** Lightridge does not emit standard PCIe Message Signaled Interrupts (MSI) upon cable hotplug under non-Apple OS environments.
* **Firmware Disconnect:** Under macOS, Apple's proprietary SMC firmware intercepts GPIO pins on the socket, wakes up the Thunderbolt controller, and notifies `AppleThunderboltNHI`. Under Linux, the native `thunderbolt.ko` software connection manager receives no interrupt when a monitor or adapter is connected post-boot.
* **Result:** Connecting a display post-boot is completely invisible to the kernel unless cold-booted or detected via custom polling.

### 3.3. Low-Level GRUB gmux DDC Hijack
In `/etc/grub.d/40_custom`, the system boots with custom LPC port writes:
```bash
outb 0x728 1   # gmux DDC lines (internal panel) = Intel
outb 0x710 2   # gmux display lines = Intel
outb 0x740 2   # gmux external port = Intel
```
Port `0x740` is the kernel's `GMUX_PORT_SWITCH_EXTERNAL`: it selects which GPU the external port is routed to. Forcing it to `2` routes the external port to Intel. Whether that alone explains the missing external display on this model has not been tested; writing `3` with a monitor connected is the experiment.

### 3.4. Xorg Artificial Invalidation
In `/etc/X11/xorg.conf.d/10-prime-intel-primary.conf`, marking the dGPU as `Inactive` with `AutoAddGPU "off"` instructs Xorg to ignore all display outputs registered by the NVIDIA driver (`DP-1-0` through `DP-1-5`).

---

## 4. Universal Multi-Vendor GPU Architecture

`ZrnSelectiveSuspend` is split into a **Universal Core Layer** and **Vendor/Platform Hardware Adapters**.

```
═════════════════════════════════════════════════════════════════════════════════
                       APPLICATION & DISPLAY SERVER LAYER
             Xorg (GLX / PRIME) / Wayland (wlroots / Mutter / KWin)
═════════════════════════════════════════════════════════════════════════════════
                                       │
                                       ▼
┌───────────────────────────────────────────────────────────────────────────────┐
│                          TARGET GPU KERNEL DRIVER                             │
│   ┌────────────────────┬────────────────────┬────────────────────┬────────┐   │
│   │   nvidia.ko        │     nouveau        │     amdgpu         │ xe/i915│   │
│   │ (Proprietary NVRM) │   (Open-Source)    │   (Radeon / ROCm)  │(Intel) │   │
│   └────────────────────┴────────────────────┴────────────────────┴────────┘   │
└──────────────────────────────────────┬────────────────────────────────────────┘
                                       │
═══════════════════════════════════════╪═════════════════════════════════════════
                   ZRNSELECTIVESUSPEND UNIVERSAL CORE
═══════════════════════════════════════╪═════════════════════════════════════════
                                       │
     ┌─────────────────────────────────┼─────────────────────────────────┐
     ▼                                 ▼                                 ▼
┌──────────────────────────┐ ┌──────────────────────────┐ ┌─────────────────────┐
│  zrn_pcie_shield         │ │  zrn_mmio_shadow         │ │  zrn_dma_isolator   │
│  - Root Port AER Masking │ │  - Dynamic PTE Swapper   │ │  - IOMMU DMA Quench │
│  - Link Down Suppression │ │  - Virtual Dummy Buffer  │ │  - Page Pin Guard   │
└──────────────────────────┘ └──────────────────────────┘ └─────────────────────┘
                                       │
═══════════════════════════════════════╪═════════════════════════════════════════
                       HARDWARE & PLATFORM ADAPTERS
═══════════════════════════════════════╪═════════════════════════════════════════
                                       │
     ┌─────────────────────────────────┼─────────────────────────────────┐
     ▼                                 ▼                                 ▼
┌──────────────────────────┐ ┌──────────────────────────┐ ┌─────────────────────┐
│  Apple gmux Adapter      │ │  Standard ACPI / PC      │ │  Thunderbolt /      │
│  - LPC 0x750 FET Gating  │ │  - ACPI _PR3 / D3cold    │ │  OCuLink eGPU       │
│  - 0x710 / 0x740 Muxing  │ │  - PCIe Slot Power Off   │ │  - Surprise Unplug  │
└──────────────────────────┘ └──────────────────────────┘ └─────────────────────┘
```

### 4.1. Supported Driver Stacks

#### A. NVIDIA Proprietary Driver (`nvidia.ko` / NVRM)
* **Target:** Kepler, Maxwell, Pascal, Turing, Ampere, Ada Lovelace, Blackwell.
* **Mechanism:** Integrates with `/proc/driver/nvidia/suspend` to freeze the command dispatch engine, captures BAR0 (`16MB`) and BAR1 (`256MB`) memory spaces, and shadows them to a RAM scratchpad.

#### B. Open-Source Linux DRM/KMS (`nouveau`, `amdgpu`, `xe`, `i915`)
* **Target:** AMD Radeon RX series, Intel Arc Alchemist/Battlemage, open NVIDIA.
* **Mechanism:** Intercepts DRM runtime PM hooks. Automatically signals TTM (Translation Table Manager) or GEM to evacuate in-flight VRAM allocations to system RAM (`ttm_bo_evict_mm`), quenching GPU DMA activity before the physical link is severed.

#### C. External GPUs (eGPU over Thunderbolt 3/4, USB4, OCuLink)
* **Target:** Any PCIe GPU housed in an external chassis.
* **Mechanism:** Instantly traps the PCIe root port interrupt upon physical disconnection, swaps in the dummy MMIO buffer, and holds client render pipelines until the cable is re-inserted.

---

## 5. The Core Subsystems

### 5.1. Subsystem 1: Root Port AER & Link Shield (`zrn_pcie_shield`)
* **Target:** Upstream PCIe Root Port hosting the GPU (e.g. `0000:00:01.0` on Intel Ivy Bridge, or generic root bridges on AMD/Intel PCs).
* **Operation:**
  1. Accesses the Root Port's PCI Express Extended Capabilities (AER - Advanced Error Reporting).
  2. Sets bit 5 (`Surprise Down`) in `PCI_ERR_UNCOR_MASK` (Offset `0x08`).
  3. Disables Link Down and Bandwidth Change interrupt generation in `PCI_EXP_LNKCTL` (`0x10`).
  4. Masks Machine Check Exceptions (MCE) on the host CPU. When power drops or the cable is unplugged, the CPU treats the link loss as an authorized silent event.

### 5.2. Subsystem 2: MMIO Shadowing Engine (`zrn_mmio_shadow`)
* **Target:** Target GPU MMIO BARs (e.g. BAR0 and BAR1).
* **Operation:**
  1. Allocates a contiguous shadow scratchpad buffer in kernel RAM matching the primary BAR size.
  2. Traverses the kernel page tables (PGD -> P4D -> PUD -> PMD -> PTE) for the virtual address returned by `ioremap` inside the GPU driver.
  3. **Swap-Out (Powering Down / Unplugged):**
     * Updates PTE flags and re-points physical frame addresses to the RAM scratchpad.
     * Any read by driver background timers returns shadow values (preventing `0xFFFFFFFF` aborts).
     * Any write is safely absorbed by RAM.
  4. **Swap-In (Powering Up / Reconnected):**
     * Re-points PTEs to the physical PCIe BAR aperture on the bus.

### 5.3. Subsystem 3: DMA & Interrupt Quencher (`zrn_dma_isolator`)
* **Target:** Host Memory and Device Ring Buffers.
* **Operation:**
  1. Commands the Linux IOMMU (Intel VT-d / AMD-Vi) to revoke DMA write permissions for the GPU's BDF (Bus/Device/Function).
  2. Prevents stale in-flight transactions or un-notified memory controllers from corrupting system memory while the card is being disconnected.
  3. Mask and quiesce device MSI-X / MSI interrupt vectors.

### 5.4. Subsystem 4: Platform Power Adapters (`zrn_power_backend`)
Modular power backends allow the same core to trigger power gating across different physical platforms:
* **Apple Classic gmux Adapter:** Controls LPC I/O ports `0x750` (power), `0x710` (display), and `0x740` (DDC).
* **Standard PC / ACPI Adapter:** Invokes ACPI `_PR3` / `_PS4` power resource methods on modern laptops.
* **eGPU / Hot-Plug Adapter:** Passive mode; detects physical insertion/removal without local power FET control.

### 5.5. Subsystem 5: Cold-Resurrection Engine (`zrn_resurrect`)
When re-energizing a GPU from a 0W cold state or reconnecting an eGPU:
1. **Power Stabilization:** Assert power rails; enforce platform-specific stabilization delays (50ms on Apple gmux).
2. **Link Retraining:** Force physical link retraining via `PCI_EXP_LNKCTL_RETRAIN`. Poll `PCI_EXP_LNKSTA_TRAIN` until hardware link synchronization is achieved.
3. **PCI Configuration Space Restoration:** Blast saved PCI configuration space registers back into the endpoint (`pci_restore_state()`), restoring BARs, Command registers, Latency Timers, and Bus Mastering.
4. **VBIOS / Firmware Re-POST:** Execute platform microcode restoration:
   * Re-POST the GPU core (via saved VBIOS shadow or Falcon PMU microcode loader).
   * Initialize GPU GDDR PLLs and memory calibration registers.
5. **Page Table Flip & Unfreeze:** Remap MMIO PTEs from the shadow RAM buffer back to physical PCIe address space, and thaw the driver state machine.

---

## 6. Operating Modes & State Machine

```
                   ┌──────────────────────────────┐
                   │         STATE 0: ECO         │
                   │   Power Gate: 0.00 W         │
                   │   MMIO: Shadowed to RAM      │
                   │   PTEs: Virtual Dummy        │
                   │   Root Port: AER Masked      │
                   └──────┬────────────────▲──────┘
                          │                │
            On-Demand     │                │ Inactivity
            Wake Request  │                │ Timeout
                          ▼                │
                   ┌───────────────────────┴──────┐
                   │       STATE 1: OFFLOAD       │
                   │   Power Gate: De-asserted    │
                   │   PCIe Link: Full Link Speed │
                   │   MMIO: Real Hardware Map    │
                   │   Panel: Driven by Primary   │
                   │   Task: prime-run rendering  │
                   └──────┬────────────────▲──────┘
                          │                │
            External      │                │ External
            Display In    │                │ Display Out
                          ▼                │
                   ┌───────────────────────┴──────┐
                   │       STATE 2: DOCKED        │
                   │   Power: Active (Full)       │
                   │   External Port: Active Link │
                   │   Server: Output Sink Bound  │
                   │   Outputs: Multi-Mon Active  │
                   └──────────────────────────────┘
```

### State Definitions

| State | dGPU Power | Display Server Status | Primary Screen | External Ports |
| :--- | :--- | :--- | :--- | :--- |
| **State 0: Eco** | **0.00 W** | Driver frozen, MMIO in RAM | Primary iGPU | Inactive |
| **State 1: Offload** | **Active (Scaled)** | Active for offload (`prime-run`) | Primary iGPU | Inactive |
| **State 2: Docked** | **Active (Full)** | Secondary Provider Attached | Primary iGPU | **Active (Lightridge / DP)** |

---

## 7. Implementation Roadmap & Phases

### Phase 1: Reference Platform Diagnostic Bridge (Completed)
* Author formal specification and hardware topology mapping.
* Define Apple gmux classic registers and PCIe capability offsets in [`include/zrn_selective_suspend.h`](file:///home/zerone/Documents/Projects/ZrnSelectiveSuspend/include/zrn_selective_suspend.h).

### Phase 2: PCIe Root Port Shield (`zrn_pcie_shield.ko`)
* Implement universal AER Surprise Down masking on PCIe root ports.
* Test that cutting power via gmux (`outb 0x750 0`) does not panic the Linux host CPU when the card is unmanaged.

### Phase 3: MMIO Page Table Swapper (`zrn_mmio_shadow.ko`)
* Implement dynamic kernel page-table walking and shadow buffer allocation for target BAR0 addresses.
* Verify that read/write accesses to shadowed space are absorbed without generating bus errors.

### Phase 4: Cold-Resurrection Engine (`zrn_resurrect.ko`)
* Implement PCI link retraining and configuration space replay.
* Implement Kepler GK107M / generic GPU microcode initialization sequence on bare metal.

### Phase 5: Display Server & Hotplug Daemon (`zrn-dockd`)
* Build userspace daemon to listen to SMC/Lightridge hotplug events and automate `xrandr` / Wayland multi-output binding.
* Generalize platform adapters for modern PC laptops and eGPUs.
