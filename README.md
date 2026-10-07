# ZrnSelectiveSuspend
### By Zerone Laboratories

**Power a GPU off, or lose it, while the desktop and its applications keep running.**

ZSS lets you take a graphics card out of service on a live Linux system: its
applications move to another GPU, the card is powered off, and everything
moves back when it returns. It is meant to work with any GPU and any driver.
Today some parts do, and some work only on the one machine they were built on.
The table below says which.

> **Status (October 2026).** Working on real hardware: a MacBookPro9,1 powers
> its NVIDIA GT 650M off and on under a running X session in about 0.3 s, with
> applications moved to the Intel GPU and back, through the `zss` kernel module
> or without it. Working in a virtual machine only: powering off a GPU with an
> open-source driver, hot-removal of a PCIe card, and recovery when a card
> disappears without warning. See "What is universal and what is not".

---

## What is universal and what is not

| Part | Works with | Tested on |
| :--- | :--- | :--- |
| Moving applications between GPUs (the layer) | Any Vulkan driver: NVIDIA, Mesa (Intel, AMD, Nouveau), software | NVIDIA 470, Intel `anv`, llvmpipe |
| Recovering applications when a GPU vanishes | Any Vulkan driver | QEMU, by pulling a virtual card |
| Daemon, `zssctl`, rules for who may hold a GPU, freezing, idle timer | Any GPU | Host tests and QEMU |
| Quiescing the driver before a power cut (kernel module, `quiesce=pm`) | Any driver that survives a laptop suspend: `amdgpu`, `i915`, `xe`, `nouveau`, and others | **QEMU's `bochs` driver only** |
| Saving and restoring PCI state, noticing a lost card within 0.3 s (kernel module) | Any PCI GPU | QEMU; MacBookPro9,1 |
| Cutting power: Apple classic gmux (`gmux` backend) | Apple laptops with a classic gmux | MacBookPro9,1 |
| Cutting power: firmware power resources (`acpi` backend) | Most hybrid laptops since about 2015 | **Nowhere yet: written, never run** |
| Cutting power: PCIe hot-plug slot (`pciehp-slot`, user space) | Any driver, on a hot-plug slot | QEMU only |
| Power-off under a running display server | **NVIDIA 470.256.02 only**: needs the wake-on-touch driver patch | MacBookPro9,1 |
| Hiding a switched-off GPU from new programs | Device nodes: any driver. Loader files and `/proc` files: **NVIDIA only** | MacBookPro9,1 |

What it would take to cover more hardware:

- **`amdgpu`, `i915`/`xe`, `nouveau`**: the kernel module already runs any
  driver's own sleep and wake code, which is also what re-runs the card's
  video BIOS after power returns. What is missing is a trial on real hardware
  for each driver, and a rule for the display server on those drivers (they
  wake by themselves through runtime power management, so no patch is
  expected, but that is untested).
- **Another laptop**: the `acpi` backend should cover most of them and needs
  someone with such a machine to try it. Other platforms need a backend of
  their own; one is three functions (probe, power off, power on).
- **Other NVIDIA versions**: the patch has to be checked against each one and
  added to `patches/validated-versions`.

## What runs where

- **The layer** (`libzss_vk.so`, started with `zss-run`) is a Vulkan driver
  shim in user space. It sits between an application and the real driver,
  keeps enough state to rebuild the application on another GPU, and does so
  when asked or when the GPU is lost.
- **The daemon** (`zssd`) decides and sequences: who is using the GPU, what to
  move, freeze or stop, then driver suspend and the power cut, and the reverse.
- **The kernel module** (`zss.ko`, optional) is the part that touches the
  hardware: it quiesces the driver through the driver's own sleep code, saves
  and restores PCI state, cuts and restores power through a backend, and
  watches for a card that goes silent. Loaded, it does nothing until the
  daemon hands it a device. Without it the daemon does a narrower version of
  the same from user space (NVIDIA and gmux only). See
  [`docs/kernel-module.md`](docs/kernel-module.md).
- **The NVIDIA driver patch** is separate and still needed for NVIDIA: with it,
  a caller arriving while the driver is suspended asks for a wake and sleeps
  instead of spinning for ever.

Consequences worth knowing:

- Only applications started under the layer can be moved. Anything else that
  holds the GPU is frozen while it is off, or blocks a detach.
- The layer offers Vulkan 1.0 with swapchains. Programs that need a newer
  Vulkan, and OpenGL programs, are not covered by it.
- A card that vanishes without warning is noticed by the kernel module, marked
  disconnected, and its driver told through the kernel's PCI error-recovery
  handlers if it has them. That is as far as protection goes: a driver that
  ignores the mark can still misbehave, and none of it has been tried with a
  card physically pulled from real hardware.
- The module reports whether an IOMMU confines the GPU's memory access
  (`iommu=` in `zssctl status`). It does not add confinement of its own.

[`SPEC.md`](SPEC.md) describes the long-term design and says, subsystem by
subsystem, what was built, what was built differently, and what was dropped
and why (MMIO shadowing and a DMA isolator of our own are not planned).

---

## Project Structure

* [`SPEC.md`](SPEC.md) - Long-term architecture, multi-vendor design, and failure analysis. Not a description of what exists.
* [`openspec/changes/`](openspec/changes/) - Proposal, design, specs and tasks for each milestone: `zss-happy-path` (orderly detach and attach), `zss-device-loss` (a GPU that disappears), `zss-system-integration` (service, installer, power-off under a desktop), `zss-kernel-shim` (the kernel module).
* [`src/layer/`](src/layer/) - The graphics layer: a Vulkan driver shim that makes applications migratable.
* [`src/daemon/`](src/daemon/) - `zssd`, which runs detach, attach, off and on, and the power backends.
* [`src/zssctl/`](src/zssctl/) - Command-line client.
* [`kmod/`](kmod/) - The `zss` kernel module and its DKMS files.
* [`packaging/`](packaging/) - Installer, service units, the NVIDIA patch tool, and an Arch package.
* [`patches/`](patches/) - The wake-on-touch patch to the NVIDIA driver.
* [`tests/`](tests/) - Test application, frame comparison, and the host, QEMU and hardware harnesses.
* [`docs/`](docs/) - Wire protocol, test notes, hardware results, and what happens when a GPU is lost.
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

The installer also offers the **kernel module** (`--kernel-module` to accept
without being asked, `--no-kernel-module` to decline). It is built through
DKMS, so it follows kernel updates, and it is loaded when the `zssd` service
starts, never from the initial ramdisk, so it cannot keep the machine from
booting. `rmmod zss` (with the daemon stopped) takes it out again.

Remove everything with `sudo ./packaging/uninstall.sh` (add `--purge` to drop
the configuration and the group too). On Arch, `packaging/arch/` builds a
package instead.

Power-off works today only on Apple laptops with a classic gmux and the NVIDIA
proprietary driver. On other machines the installer says so and installs
application migration only.

## Powering the GPU off and on

```sh
zssctl status                 # state, wake support, who is using the GPU
zssctl off 0000:01:00.0       # move applications away, suspend the driver, cut power
zssctl on 0000:01:00.0        # power on; add --return to bring the applications back
```

Each step is printed as it happens:

```
[ZrnSelectiveSuspend] Suspending device: 0000:01:00.0  NVIDIA Corporation GK107M [GeForce GT 650M Mac Edition]
[ZrnSelectiveSuspend]   driver nvidia, power through zss-kmod, wake on demand: yes
[ZrnSelectiveSuspend]   [1/7] In use by: Xorg (display server, idle), vkcube (application, will be moved)
[ZrnSelectiveSuspend]   [2/7] Applications: 1 moved to 0000:00:02.0, 0 parked
[ZrnSelectiveSuspend]   [3/7] Services stopped: nvidia-persistenced; processes frozen: none
[ZrnSelectiveSuspend]         Hidden from new programs (17): /dev/nvidia0, /dev/dri/card2, ...
[ZrnSelectiveSuspend]   [4/7] PCI state: saved and restored in the kernel (zss module)
[ZrnSelectiveSuspend]   [5/7] Driver nvidia suspended (0.13 s)
[ZrnSelectiveSuspend]   [6/7] Power cut through zss-kmod (0.03 s); the device has left the bus
[ZrnSelectiveSuspend]   [7/7] Watching for wake requests
[ZrnSelectiveSuspend] Device 0000:01:00.0 is powered off (0.43 s).
[ZrnSelectiveSuspend] It stays off (the display server may borrow it for a moment) until: zssctl on 0000:01:00.0
```

**A GPU you switch off stays off.** The screen stays on the desktop, and:

- Programs you start meanwhile do not see the GPU, as if it had been
  unplugged: Vulkan and OpenGL programs run on the other GPU, and `nvidia-smi`
  says it cannot reach the device.
- If the display server itself needs the GPU for a moment, it gets it, and the
  GPU switches off again a second or two later. `zssctl status` counts these
  as `served=`.
- If the GPU has started driving a display by then, it stays on.
- `hide_while_off = no` in the configuration turns the hiding off. A program
  that then reaches the driver waits until you run `zssctl on`, and
  `zssctl status` shows it as `waiting=`.

A GPU switched off by the idle timer is different: it is not hidden, and it
comes back for anyone who asks.

**Is it really off?** `lspci` keeps listing the card, because the kernel keeps
its entry for a card that cannot be removed. Ask the card itself instead:

```sh
lspci -x -s 01:00.0 | head -3     # all "ff" means no power; real bytes mean it is on
```

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
meson test -C build                       # host and QEMU tests; nothing on this machine is powered off,
                                          # and the kernel module is loaded only inside the QEMU guest

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
| `ZSS_TEST_LOSE_AFTER_SUBMIT` | Test aid: the GPU dies just after that submit was accepted, with its work in flight |
| `ZSS_TEST_STUCK_MS`, `ZSS_LOSS_GRACE_MS` | Test aids: hold a thread inside the layer during a loss; how long recovery waits for such threads (3000) |

## Licence

ZrnSelectiveSuspend is free software, Copyright (c) 2026 Zerone Laboratories,
released under the **GNU General Public License, version 2 only**. The full
text is in [`LICENSE`](LICENSE); every source file names it in an
`SPDX-License-Identifier` line. It comes with no warranty: it cuts power to
hardware and patches a kernel driver, and you use it at your own risk.

Two things are not under that licence:

- **The NVIDIA driver patch** in [`patches/`](patches/). The lines it adds are
  ours and are under the MIT licence, so that they can be built into NVIDIA's
  driver; the lines of NVIDIA's code it quotes remain NVIDIA's. See
  [`patches/LICENSE`](patches/LICENSE). ZSS does not ship or modify NVIDIA's
  driver itself: the patch is applied on your machine to the copy you
  installed.
- **Vulkan-Headers**, fetched at build time by Meson from Khronos, under its
  own licences (Apache-2.0 or MIT).

The hash in `src/common/zss_hash.c` is MurmurHash3 by Austin Appleby, which is
in the public domain.

