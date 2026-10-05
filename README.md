# ZrnSelectiveSuspend
### By Zerone Laboratories

**Universal Linux In-Kernel GPU Runtime Power-Gating, MMIO Virtualization & Surprise-Removal Architecture**

> **Status (October 2026).** The first milestone is the orderly path: the user
> asks for a GPU to be detached, its applications move to another GPU, and the
> device is released and powered off. It is implemented in userspace (`src/`)
> and specified in `openspec/changes/zss-happy-path/`. The surprise-removal
> subsystems described below (PCIe shield, MMIO shadow, DMA isolator,
> resurrection engine) are **deferred** and not part of that milestone.

---

## Overview

`ZrnSelectiveSuspend` is an in-kernel virtualization and power-orchestration shim. It decouples the physical hardware power state and link status of a PCIe Graphics Processing Unit (GPU) from the software state of the operating system, display servers (Xorg / Wayland), and user applications.

While initially built and validated against the dual-GPU architecture of the **Apple MacBookPro9,1** (Mid-2012 15-inch Unibody, Ivy Bridge + Kepler GT 650M + Intel **Lightridge** Thunderbolt silicon), **the architecture is designed to support any GPU stack**:
* **NVIDIA Proprietary (`nvidia.ko` / NVRM):** Kepler through Blackwell
* **Open-Source Linux DRM:** `nouveau`, `amdgpu` (Radeon / ROCm), `xe` / `i915` (Intel Arc / Iris)
* **External GPUs (eGPU):** Thunderbolt 3/4, USB4, and OCuLink surprise hot-unplug protection

For the complete architectural blueprints, hardware register maps, and subsystem specifications, see [SPEC.md](file:///home/zerone/Documents/Projects/ZrnSelectiveSuspend/SPEC.md).

---

## Operating Modes

| Mode | dGPU Power | Display Server Status | Primary Screen | External Ports |
| :--- | :--- | :--- | :--- | :--- |
| **State 0: Eco / Pure Intel** | **0.00 W** | Driver frozen, MMIO in RAM | Primary iGPU | Inactive |
| **State 1: PRIME Offload** | **Dynamic** | Active for offload (`prime-run`) | Primary iGPU | Inactive |
| **State 2: Lightridge Docked** | **Active** | Secondary Provider Attached | Primary iGPU | **Active (Lightridge / DP)** |

---

## Project Structure

* [`SPEC.md`](SPEC.md) - Long-term architecture, multi-vendor design, and failure analysis.
* [`openspec/changes/zss-happy-path/`](openspec/changes/zss-happy-path/) - Proposal, design, specs and tasks for the current milestone.
* [`src/layer/`](src/layer/) - The graphics layer: a Vulkan driver shim that makes applications migratable.
* [`src/daemon/`](src/daemon/) - `zssd`, which runs detach and attach, and the power backends.
* [`src/zssctl/`](src/zssctl/) - Command-line client.
* [`tests/`](tests/) - Test application, frame comparison, and the host and QEMU harnesses.
* [`docs/`](docs/) - Wire protocol and test notes.
* [`include/`](include/) - Register maps for the reference platform.

## Building and trying it

```sh
meson setup build && ninja -C build
meson test -C build                       # host tests; nothing is powered off

# Terminal 1: manage the dGPU without touching its power
build/src/daemon/zssd --socket "$XDG_RUNTIME_DIR/zss.sock" --gpu 0000:01:00.0=dry-run --allow-software
# Terminal 2: run something under the layer, then move it away and back
export ZSS_SOCKET="$XDG_RUNTIME_DIR/zss.sock"
build/src/layer/zss-run vkcube &
build/src/zssctl/zssctl detach 0000:01:00.0
build/src/zssctl/zssctl attach 0000:01:00.0
```

Applications run under the layer see a Vulkan 1.0 device. Programs that need a
newer Vulkan version do not start under it yet.

| Variable | Effect |
| :--- | :--- |
| `ZSS_SOCKET` | Socket of the daemon to talk to (default `/run/zss/zssd.sock`) |
| `ZSS_DEBUG` | Makes the layer log what it loads and why a device is not migratable |
| `ZSS_ALLOW_SOFTWARE` | Lets a parked application resume on a software renderer |
| `ZSS_REAL_DRIVER_FILES` | Colon-separated driver manifests for the layer to use instead of the system's |
| `ZSS_BIND_PCI` | Test aid: makes the software renderer pose as the PCI device at that address |
