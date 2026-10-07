# ZrnSelectiveSuspend: Universal GPU Runtime Power-Gating & Hot-Removal Architecture

**Project Codename:** ZrnSelectiveSuspend  
**Target Class:** Universal Linux PCIe GPU Power-Gating, MMIO Virtualization & Surprise-Removal Shim  
**Reference Hardware Platform (Tier 1 Target):** Apple MacBookPro9,1 (Mid-2012 15-inch Unibody, Ivy Bridge + Kepler GT 650M + Lightridge)  
**Supported GPU Stacks:** NVIDIA Proprietary (`nvidia`), Open-Source DRM (`nouveau`, `amdgpu`, `xe`, `i915`)  
**Kernel Target:** Linux 6.x / 7.x (Current Reference: `7.2.2-arch1-1 x86_64`)  
**Document Version:** 1.3.0  
**Author:** zerone  

> **Status (October 2026).** This document is the original design. Part of it
> has been built, part was built differently, and part has been dropped. The
> section "Scope" below says which, and each subsystem in section 5 carries a
> status line. For what exists and how to use it, read `README.md`.

## Scope: what was built, what changed, what was dropped

The original design put everything in the kernel and protected a running
driver from a vanishing card by standing between the driver and its hardware.
What was built instead moves applications off the card first, then powers it
off with the driver's own cooperation. That needs far less from the kernel,
and it works on the reference laptop today.

| Original subsystem | Outcome | Where it lives now |
| :--- | :--- | :--- |
| 5.1 Root-port shield | **Not built.** The kernel's own hot-plug handling covers ports that report removal. For the rest, the `zss` module's loss guard notices a silent card within 0.3 s, marks it disconnected and tells its driver. Masking AER and machine checks is not done and has no test bed. | `kmod/zss.c` (guard) |
| 5.2 MMIO shadowing | **Dropped.** See below. | - |
| 5.3 DMA isolator | **Dropped as a subsystem.** Confinement is the IOMMU's job and the kernel already does it when one is enabled. The module switches bus mastering off before a power cut and reports whether an IOMMU is in force. | `kmod/zss.c` |
| 5.4 Power adapters | **Built.** `gmux` (verified on the laptop), `acpi` (written, never run), a hot-plug slot backend in user space (QEMU). | `kmod/zss.c`, `src/daemon/backend.c` |
| Surprise removal as a whole | **Partly working.** On the reference laptop a card that loses power while idle comes back, driver included, without a reboot. A card lost while a program renders on it does not: the desktop survives, the program is stuck, and the card needs a reboot. | `kmod/zss.c`, `patches/`, `src/layer/` |
| 5.5 Resurrection engine | **Built differently.** Power, a bounded wait for the card to answer, and the PCI core's state restore are in the module. The card's video BIOS is not executed by ZSS: every real GPU driver re-initialises its card on resume, and the module calls that. A card with no driver is therefore refused. | `kmod/zss.c` |
| Not in the original design | **Built.** Moving applications between GPUs and rebuilding them after a loss (the Vulkan layer), the daemon and its rules, freezing what cannot be moved, hiding a switched-off card, and the wake-on-touch patch to NVIDIA's driver. | `src/`, `patches/` |

### Why MMIO shadowing was dropped

A driver reaches its card with ordinary memory instructions, so there is
nothing to hook. Getting in between means one of:

- **Trapping every access by page fault**, as the kernel's `mmiotrace` does.
  Each register access becomes a fault and a single-step, and that mechanism
  restricts the machine to one CPU while active. Unusable for a working GPU.
- **Re-pointing the driver's page tables at RAM** (the design in 5.2). The
  kernel exports no way to find another driver's mappings; it would mean
  walking private structures that change between kernel versions.
- **Running the driver in a virtual machine.** A different project.

Beyond the mechanism, a shadow has to answer reads on the dead card's behalf.
For a closed driver nobody outside the vendor knows what thousands of
registers should say, and plausible wrong answers can be worse than the
all-ones a missing device returns by itself.

And none of it can be tested here: the reference laptop's card is soldered,
and a virtual card does not die the way hardware does.

If this is taken up again it needs hardware that can really lose a card (an
eGPU), an open driver to try it on first, and its own design document.

### What four real power cuts showed (October 2026)

The first version of this document assumed the hard part of surprise removal
was keeping the kernel alive, and proposed to do it by shadowing the card's
registers. On the reference laptop, with the proprietary NVIDIA 470 driver,
that turned out not to be where the difficulty lies:

- **The machine survives a dead card without any shadowing.** The driver reads
  all-ones, logs that the GPU has fallen off the bus, and stops.
- **The difficulty is the driver's state afterwards.** Having declared the GPU
  lost it will not look at it again, and its own suspend path hangs in that
  state. The answer that works is to *freeze* it first: shut every caller out
  without asking the card anything, and run its ordinary resume when the card
  is back. On an idle card that brings card and driver back with no reboot.
- **A program rendering on the card defeats it.** It calls into the driver
  before the loss can be noticed, and what it had on the card cannot be put
  back by a resume that follows no suspend. Resuming the driver under it hung
  the machine.
- **One reset came from below the kernel**, with no panic message, as the
  original design feared. It followed a rescan of the whole PCI bus during a
  return, which has been removed; whether that was the cause is not proven.

Register shadowing would not have changed any of these outcomes. What would
help the busy case is a driver that can be detached and attached again while
the desktop runs, which the open drivers offer and this one does not.

### What "universal" means today

Vendor-neutral by construction, proven on one machine: see the table in
`README.md`. The pieces written for any GPU are the layer, the daemon, and
the module's sequence and guard. The pieces tied to hardware are the power
backends and the NVIDIA patch.

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

> These are the subsystems as originally designed. The **Status** line under
> each says what became of it; the "Scope" section at the top has the reasons.

### 5.1. Subsystem 1: Root Port AER & Link Shield (`zrn_pcie_shield`)
* **Status:** not built. Loss detection and the disconnected mark are in the `zss` module; AER and machine-check masking are open.
* **Target:** Upstream PCIe Root Port hosting the GPU (e.g. `0000:00:01.0` on Intel Ivy Bridge, or generic root bridges on AMD/Intel PCs).
* **Operation:**
  1. Accesses the Root Port's PCI Express Extended Capabilities (AER - Advanced Error Reporting).
  2. Sets bit 5 (`Surprise Down`) in `PCI_ERR_UNCOR_MASK` (Offset `0x08`).
  3. Disables Link Down and Bandwidth Change interrupt generation in `PCI_EXP_LNKCTL` (`0x10`).
  4. Masks Machine Check Exceptions (MCE) on the host CPU. When power drops or the cable is unplugged, the CPU treats the link loss as an authorized silent event.

### 5.2. Subsystem 2: MMIO Shadowing Engine (`zrn_mmio_shadow`)
* **Status:** dropped. Not planned.
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
* **Status:** dropped as a subsystem. The module disables bus mastering before a power cut and reports the IOMMU state.
* **Target:** Host Memory and Device Ring Buffers.
* **Operation:**
  1. Commands the Linux IOMMU (Intel VT-d / AMD-Vi) to revoke DMA write permissions for the GPU's BDF (Bus/Device/Function).
  2. Prevents stale in-flight transactions or un-notified memory controllers from corrupting system memory while the card is being disconnected.
  3. Mask and quiesce device MSI-X / MSI interrupt vectors.

### 5.4. Subsystem 4: Platform Power Adapters (`zrn_power_backend`)
**Status:** built. `gmux` runs on the reference laptop; `acpi` is written and has never run; the hot-plug adapter exists in user space and is tested in QEMU. The gmux adapter drives the power port only, not the display or DDC ports.

Modular power backends allow the same core to trigger power gating across different physical platforms:
* **Apple Classic gmux Adapter:** Controls LPC I/O ports `0x750` (power), `0x710` (display), and `0x740` (DDC).
* **Standard PC / ACPI Adapter:** Invokes ACPI `_PR3` / `_PS4` power resource methods on modern laptops.
* **eGPU / Hot-Plug Adapter:** Passive mode; detects physical insertion/removal without local power FET control.

### 5.5. Subsystem 5: Cold-Resurrection Engine (`zrn_resurrect`)
**Status:** built differently. Steps 1 and 3 are in the `zss` module. Step 2 is left to the hardware and checked by waiting for the card to answer. Step 4 is the driver's own resume code, not ZSS's. Step 5 does not exist, since there is no shadow.

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

## 7. Roadmap

### Done

* **Orderly detach and attach** in user space: applications moved between GPUs, driver released, power cut (`zss-happy-path`).
* **Device loss**: applications rebuilt from memory on another GPU, or parked (`zss-device-loss`).
* **System integration**: service, installer, the NVIDIA wake-on-touch patch through DKMS, power-off under a running X session, freezing, hiding, serving the display server (`zss-system-integration`).
* **Kernel module**: the power sequence, state save and restore, loss guard, `gmux` backend verified on the reference laptop (`zss-kernel-shim`).
* **Unannounced loss of an idle card**: noticed in about 50 ms, the NVIDIA driver frozen before it finds out, card and driver brought back by `zssctl on` without a reboot (driver patch revision 4).

### Next, in the order they unblock real users

1. **Unannounced loss of a card in use.** Decide what a frozen driver does with callers other than the display server (sleeping keeps an application from being moved), then try closing the stuck programs before the driver is resumed.
2. **Try the module on an open driver on real hardware** (`amdgpu`, `i915`/`xe` or `nouveau`), and settle how the display server is handled there.
3. **Try the `acpi` backend** on a hybrid laptop that has firmware power resources.
4. **A wider Vulkan surface in the layer** (beyond 1.0), and an OpenGL path, so that more applications can be moved rather than frozen.
5. **Notice a monitor plugged in while the card is off** (the gmux hot-plug interrupt on the reference laptop).

### Open, with no test bed yet

* AER and machine-check handling for a card that is physically pulled (5.1). One reset below the kernel has now been seen on the reference laptop.
* Anything for a driver that ignores the disconnected mark.

### Dropped

* MMIO shadowing (5.2) and a DMA isolator of our own (5.3). See "Scope".
* Executing a card's video BIOS from ZSS (part of 5.5): drivers do it.
* The display-switching daemon `zrn-dockd` (old phase 5): outside what ZSS is for.
