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
* [`packaging/`](packaging/) - Installer, service units, the NVIDIA patch tool, and an Arch package.
* [`patches/`](patches/) - The wake-on-touch patch to the NVIDIA driver.
* [`tests/`](tests/) - Test application, frame comparison, and the host and QEMU harnesses.
* [`docs/`](docs/) - Wire protocol, test notes, and what happens when a GPU is lost.
* [`include/`](include/) - Register maps for the reference platform.

## Installing

```sh
meson setup build && ninja -C build
./packaging/install.sh --check      # what this machine supports; changes nothing
sudo ./packaging/install.sh         # installs; asks before touching the GPU driver
```

The installer puts `zssd`, `zssctl` and `zss-run` under `/usr/local`, creates a
`zss` group, and sets up `zssd.service` with its settings in
`/etc/zss/zssd.conf`. It never edits the bootloader's configuration.

To power a GPU off under a running desktop, the NVIDIA driver needs the
wake-on-touch patch in [`patches/`](patches/). The installer offers to apply it
through DKMS when the driver version is one the patch has been validated
against (470.256.02 so far); it then survives kernel updates, and a hook
re-applies it after a driver update. The stock modules are kept and restored at
boot if the patched driver does not load. `zss-nvidia-patch status` shows where
things stand, and `zss-nvidia-patch remove` puts the stock driver back.

Remove everything with `sudo ./packaging/uninstall.sh` (add `--purge` to drop
the configuration and the group too). On Arch, `packaging/arch/` builds a
package instead.

Power-off works today only on Apple laptops with a classic gmux. On other
machines the installer says so and installs application migration only.

## Powering the GPU off and on

```sh
zssctl status                 # state, wake support, who is using the GPU
zssctl off 0000:01:00.0       # move applications away, suspend the driver, cut power
zssctl on 0000:01:00.0        # power on; add --return to bring the applications back
```

Each step is printed as it happens:

```
[ZrnSelectiveSuspend] Suspending device: 0000:01:00.0  NVIDIA Corporation GK107M [GeForce GT 650M Mac Edition]
[ZrnSelectiveSuspend]   driver nvidia, power through apple-gmux, wake on demand: yes
[ZrnSelectiveSuspend]   [1/7] In use by: Xorg (display server, idle), nvidia-persiste (service, will be stopped)
[ZrnSelectiveSuspend]   [2/7] Applications: 0 moved to 0000:00:02.0, 0 parked
  ...
[ZrnSelectiveSuspend] Device 0000:01:00.0 is powered off (0.42 s).
[ZrnSelectiveSuspend] It powers on by itself when needed, or with: zssctl on 0000:01:00.0
```

With the patched driver the screen stays on the desktop. A GPU you switched off
stays off, and is hidden from programs you start meanwhile, as if it had been
unplugged: Vulkan and OpenGL programs run on the other GPU, and `nvidia-smi`
says it cannot reach the device. If X itself needs the GPU for a moment it gets
it, and the GPU switches off again a second or two later; `zssctl status`
counts these as `served=`. (`hide_while_off = no` in the configuration
turns the hiding off; a program that then reaches the driver waits until you
run `zssctl on`, and `zssctl status` shows `waiting=`.) The one exception is the display server: if X itself needs the GPU
(you ask it about displays, for instance) the GPU comes back, because the
desktop would otherwise stand still. A GPU switched off by the idle timer comes
back for anyone.
`zssctl off --console` shows the same report on a text console instead and
keeps the screen there while the GPU is off; press a key to power it on.

A program that uses the GPU but was not started with `zss-run` (a monitor such
as `btop`, say) cannot be moved, so `zssctl off` freezes it and `zssctl on`
lets it carry on; the report names it. The terminal you type `zssctl off` into
is never frozen: if it uses the GPU the command says so and stops. If a frozen
program is one you need in order to get back (a window manager), switch to
another virtual terminal and run `zssctl on` there.

Set `idle_timeout` in `/etc/zss/zssd.conf` to have an unused GPU powered off
automatically. A GPU that is driving a display is never powered off,
and the timer never freezes anything: a program outside the layer keeps the GPU on.

A monitor plugged in while the GPU is off is not noticed until something asks
about displays or you run `zssctl on`.

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
| `ZSS_RETAIN` | Where uploaded textures are kept so they survive a lost GPU: `disk` (default), `ram`, or `off` |
| `ZSS_RETAIN_LIMIT_MB`, `ZSS_RETAIN_QUEUE_MB` | Size cap of the store (4096) and of data waiting to be written (256) |
| `ZSS_BIND_PCI` | Test aid: makes the software renderer pose as the PCI device at that address |
| `ZSS_TEST_LOSE_AT_SUBMIT` | Test aid: the layer behaves as if the GPU died at that submit |
| `ZSS_TEST_STUCK_MS`, `ZSS_LOSS_GRACE_MS` | Test aids: hold a thread inside the layer during a loss; how long recovery waits for such threads (3000) |
