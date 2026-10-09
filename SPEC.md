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

### Names

The two shims the system is built around have names:

| Name | What it is | Where |
| :--- | :--- | :--- |
| **ZSS_AirLock** | The user-space shim for Vulkan and (through Mesa's Zink) OpenGL. Applications see it as their Vulkan driver; it loads the real drivers underneath and can rebuild an application on another GPU. | `src/layer/`, installed as `libzss_airlock.so` |
| **ZSS_Interceptor** | The kernel shim: quiesces the GPU's driver, saves and restores PCI state, switches power through a backend, and notices a card that has gone. | `kmod/`, the module `zss.ko` (its file, `/sys` entries and kernel messages keep the short name `zss`) |

The daemon (`zssd`), the control tool (`zssctl`) and the launcher (`zss-run`)
keep their plain names. The subsystem names in section 5 (`zrn_pcie_shield`
and the rest) are the original design's and are kept there as history; what
was built of them lives inside ZSS_Interceptor.

### What "universal" means today

Vendor-neutral by construction, proven on one machine: see the table in
`README.md`. The pieces written for any GPU are ZSS_AirLock, the daemon, and
ZSS_Interceptor's sequence and guard. The pieces tied to hardware are the power
backends and the NVIDIA patch.

---

## 1. Executive Summary & Vision

`ZrnSelectiveSuspend` decouples a PCIe GPU's power and presence from the programs that use it. A user-space shim (ZSS_AirLock) sits between programs and the real graphics drivers and can rebuild a program on another GPU at any moment; a kernel shim (ZSS_Interceptor) quiesces the GPU's driver, cuts and restores power, and notices a card that vanishes; a daemon decides and sequences.

It was developed and verified on the **Apple MacBookPro9,1** (Apple `gmux`, NVIDIA GT 650M, Intel HD 4000, Lightridge Thunderbolt), and is designed to be vendor- and platform-agnostic; the table in `README.md` says how far that has been proven.

What it does:
1. **Power gating under a running desktop:** a discrete GPU is switched off while the display server and the programs keep running; programs are moved to another GPU first and back afterwards.
2. **Surviving a card that vanishes:** a GPU that loses power or leaves the bus is noticed, its driver is kept from hanging the machine, and its programs are rebuilt on another GPU.
3. **Lending a GPU to a virtual machine:** the card is handed to `vfio-pci` and taken back, with the host's programs moved away and returned, and no reboot or log-out.

---

## 2. Reference Platform (Tier 1): Apple MacBookPro9,1

The initial target hardware provides the ultimate stress-test for this architecture due to its legacy hardware multiplexing, proprietary closed drivers, and first-generation Thunderbolt controllers.

```
                           ┌───────────────────────────────┐
                           │      Intel Core i7-3615QM     │
                           │       (Ivy Bridge, VT-d)      │
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
| **iGPU** | Intel HD 4000 (`8086:0166`) | `i915` | **Drives the internal panel and runs the X server alone** (`modesetting`) |
| **dGPU** | NVIDIA GeForce GT 650M Mac Edition (`10de:0fd5`) | `nvidia` 470.256.02 with the ZSS wake-on-touch patch | **Render only:** not in the X configuration, its display nodes on a seat of their own (for lending); programs draw on it through ZSS_AirLock |
| **Audio** | NVIDIA GK107 HDMI Audio (`10de:0e1b`) | `snd_hda_intel` | Second function of the dGPU; lent with it |
| **Multiplexer** | Apple `gmux` (v1.9.35 classic) | `apple-gmux` | **LPC I/O 0x700–0x7fe:** muxed to Intel by the boot menu; power port `0x750` used by ZSS_Interceptor |
| **Thunderbolt** | Intel CV82524 **Lightridge** (`8086:1513`) | `pcieport`, `thunderbolt` | Host only; **shares IOMMU group 2 with the dGPU** (both root ports lack ACS), so it goes with the card when it is lent |
| **IOMMU** | VT-d (DMAR) | `intel_iommu=on iommu=pt` | On in an added boot entry; **no interrupt remapping** (firmware bug: "ioapic 2 has no mapping iommu") |

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

### 3.4. Xorg and the dGPU
Until 9 October 2026, `/etc/X11/xorg.conf.d/10-prime-intel-primary.conf` listed the dGPU as an `Inactive` device with `AutoAddGPU "off"`, which kept the NVIDIA outputs (`DP-1-0` to `DP-1-5`) out of the screen. For lending the card to virtual machines it was then taken out of the configuration altogether, and a udev rule puts its display nodes on a seat of their own: X and the login manager open every display node of their seat at start-up, used or not, and a card they hold cannot be handed over. The external port therefore stays dark on Linux until there is a way to route it.

Frames drawn on the dGPU still reach the Intel-driven screen, but the NVIDIA driver no longer hands them across in order without X's help; ZSS_AirLock presents them itself on the Intel GPU (see 4.2).

---

## 4. Architecture as Built

ZSS is three parts around the GPU's own driver: a user-space shim the
applications draw through, a daemon that decides and sequences, and a kernel
shim that touches the hardware. The design this replaced (an MMIO shadow and a
DMA isolator in the kernel) is kept in section 5 as history.

```
 USER SPACE
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │  Programs: Vulkan · OpenGL through Zink · Chromium/Electron (ANGLE→Vulkan)   │
 │  started by zss-run, or by the launch router (ZrnLaunchRouter, separate)     │
 │        │                                                                     │
 │        ▼                                                                     │
 │  ┌───────────────────── ZSS_AirLock (libzss_airlock.so) ──────────────────┐  │
 │  │  owns every handle · records command buffers · shadows host memory     │  │
 │  │  portable feature profile · stands in for what a driver lacks          │  │
 │  │  rebuilds the program on another GPU (asked, or when a GPU is lost)    │  │
 │  │  presents on the screen's GPU when the drawing GPU cannot (4.2)        │  │
 │  └───────┬─────────────────────────┬─────────────────────────┬───────────┘  │
 │          ▼                         ▼                         ▼              │
 │   NVIDIA Vulkan driver      Mesa (Intel, AMD, nouveau)   llvmpipe (CPU)     │
 │                                                                              │
 │  zssd ◄── control socket ── ZSS_AirLock in each program                      │
 │   ▲   who holds the GPU · move / freeze / stop · off / on · lend / reclaim   │
 │   └── zssctl · zss-power-event (charger) · zrn_perfd                          │
 └───┬──────────────────────────────────────────────────────────────────────────┘
     │ /sys/kernel/zss/<pci>/power   (off · on · lend · unlend · reclaim)
 KERNEL
 ┌───▼──────────────────────────────────────────────────────────────────────────┐
 │  ZSS_Interceptor (zss.ko)                                                    │
 │   quiesce the driver through its own sleep code · save / restore PCI state  │
 │   loss guard (silence → disconnected mark, error handlers or driver freeze) │
 │   pause across system sleep · "lent": hands off, reset by power cycle back  │
 │   power backends:  gmux (Apple)  │  acpi (_PR3)  │  test                      │
 ├──────────────────────────────────────────────────────────────────────────────┤
 │  GPU driver: nvidia (+ wake-on-touch / freeze patch) · amdgpu · i915 · xe ·  │
 │  nouveau          ── or, while lent ──          vfio-pci (to a VM)           │
 └──────────────────────────────────────────────────────────────────────────────┘
```

### 4.1. Driver Stacks

#### A. NVIDIA Proprietary Driver (`nvidia.ko`)
* **Tested:** 470.256.02 (Kepler) on the reference laptop.
* **Mechanism:** the driver is suspended through `/proc/driver/nvidia/suspend` and resumed the same way. A patch adds two things: a caller arriving while the driver is suspended asks for a wake and sleeps instead of spinning, and the driver can be frozen (shut to every caller, nothing asked of the card) when the card vanishes. For lending, the modules stacked on the driver are unloaded first and the driver must have no user left.

#### B. Open-Source DRM/KMS Drivers (`nouveau`, `amdgpu`, `xe`, `i915`)
* **Mechanism:** ZSS_Interceptor runs the driver's own runtime sleep and wake callbacks (`quiesce=pm`), which also re-run the card's firmware after power returns.
* **Tested:** in QEMU only.

#### C. External GPUs and Hot-Plug Slots
* **Mechanism:** the slot's power is switched by `pciehp` from user space; a card that leaves the bus is reported by ZSS_Interceptor's bus notifier and its programs are rebuilt elsewhere.
* **Tested:** in QEMU only.

### 4.2. Presenting Across GPUs
A program may draw on one GPU while another drives the screen. Mesa's drivers hand frames across in order themselves (DRI3). The NVIDIA proprietary driver does that only when the X server has its card. Without it, ZSS_AirLock gives the program ordinary images on its own GPU, keeps the window's real swapchain on a small device of its own on the screen's GPU, and at each present reads the frame back, waits for it, copies it in and presents it there. Moves switch between this and direct presenting.

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

## 6. Device States

The states `zssctl status` reports for a managed GPU. The original three
operating modes (Eco, Offload, Docked) became the first two of these; Docked
was dropped with the display daemon.

```
                        zssctl off · idle timer · charger unplugged
          ┌──────────┐ ──────────────── detaching ───────────────► ┌─────────────┐
          │          │                                              │ powered-off │◄──┐ display server
          │ attached │ ◄─────────────── attaching ──────────────── │   (0 W)     │   │ calls: powered
          │          │       zssctl on · wake request · charger      └──────┬──────┘   │ for a moment,
          └─┬──▲───┬─┘                                                      └──────────┘ then off again
            │  │   │
     lend   │  │   │ card stops answering, or leaves the bus
            │  │   ▼
            │  │ ┌──────┐   zssctl on (power restored, driver thawed),
            │  │ │ lost │ ──or the card answers again──────────────► attached
            │  │ └──────┘
            ▼  │ reclaim: power cycle, host drivers back, programs returned
          ┌────┴────┐
          │  lent   │   card on vfio-pci, owned by a virtual machine;
          └─────────┘   ZSS does not watch, wake or power it
```

| State | Card power | Host driver | Programs that were on it | Display server |
| :--- | :--- | :--- | :--- | :--- |
| **attached** | on | bound | running on it | may use it |
| **powered-off** | **off (0 W)** | suspended | moved to another GPU, or frozen | served on demand (patched NVIDIA driver) |
| **lost** | unknown | frozen, or told through its error handlers | rebuilt on another GPU, or parked | carries on |
| **lent** | on | **none** (`vfio-pci`) | moved to another GPU | must not hold the card |
| detaching / attaching | changing | changing | being moved | — |

## 7. Roadmap

### Done

* **Orderly detach and attach** in user space: applications moved between GPUs, driver released, power cut (`zss-happy-path`).
* **Device loss**: applications rebuilt from memory on another GPU, or parked (`zss-device-loss`).
* **System integration**: service, installer, the NVIDIA wake-on-touch patch through DKMS, power-off under a running X session, freezing, hiding, serving the display server (`zss-system-integration`).
* **Kernel module**: the power sequence, state save and restore, loss guard, `gmux` backend verified on the reference laptop (`zss-kernel-shim`).
* **Unannounced loss of an idle card**: noticed in about 50 ms, the NVIDIA driver frozen before it finds out, card and driver brought back by `zssctl on` without a reboot (driver patch revision 4).
* **A wider Vulkan surface and OpenGL** (`zss-vulkan-12-and-opengl`, in progress): Vulkan 1.1, the portable profile, in-place swapchain rebuild, secondary command buffers, OpenGL through Zink with dynamic rendering and `VK_KHR_maintenance5` stood in for on NVIDIA 470, and frames presented across GPUs in order. Chromium, Electron apps, VS Code and a Unity game (IFSCL, both renderers) move between the reference laptop's GPUs.
* **Lending a GPU to a virtual machine** (`zss-vm-passthrough`): lent, used by a QEMU guest and reclaimed on the reference laptop with the desktop and its programs running throughout.

### Next, in the order they unblock real users

1. **Unannounced loss of a card in use.** Decide what a frozen driver does with callers other than the display server (sleeping keeps an application from being moved), then try closing the stuck programs before the driver is resumed.
2. **Try the module on an open driver on real hardware** (`amdgpu`, `i915`/`xe` or `nouveau`), and settle how the display server is handled there.
3. **Try the `acpi` backend** on a hybrid laptop that has firmware power resources.
4. **Vulkan 1.2 and OpenGL 3.3** in ZSS_AirLock, so that more programs start under it.
5. **Testers on other hardware**, through the tester kit (`tester/`): the report and the moving tests on machines with other GPUs and drivers.
6. **Programs register again with a restarted daemon**; today they have to be restarted to be moved.
7. **External displays with the dGPU out of X** on the reference laptop.

### Open, with no test bed yet

* AER and machine-check handling for a card that is physically pulled (5.1). One reset below the kernel has now been seen on the reference laptop.
* Anything for a driver that ignores the disconnected mark.

### Dropped

* MMIO shadowing (5.2) and a DMA isolator of our own (5.3). See "Scope".
* Executing a card's video BIOS from ZSS (part of 5.5): drivers do it.
* The display-switching daemon `zrn-dockd` (old phase 5): outside what ZSS is for.
