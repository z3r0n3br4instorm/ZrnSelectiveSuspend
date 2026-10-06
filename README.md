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
> applications moved to the Intel GPU and back. Working in a virtual machine
> only: hot-removal of a PCIe card, and recovery when a card disappears without
> warning. There is no ZSS kernel module; see "What runs where".

---

## What is universal and what is not

| Part | Works with | Tested on |
| :--- | :--- | :--- |
| Moving applications between GPUs (the layer) | Any Vulkan driver: NVIDIA, Mesa (Intel, AMD, Nouveau), software | NVIDIA 470, Intel `anv`, llvmpipe |
| Recovering applications when a GPU vanishes | Any Vulkan driver | QEMU, by pulling a virtual card |
| Daemon, `zssctl`, rules for who may hold a GPU, freezing, idle timer | Any GPU | Host tests and QEMU |
| Detach by unbinding the driver and cutting slot power | Any driver, on a PCIe hot-plug slot (`pciehp-slot` backend) | QEMU only |
| Power-off with the driver suspended in place | **NVIDIA proprietary driver on an Apple classic gmux only** (`apple-gmux` backend) | MacBookPro9,1 |
| Power-off under a running display server | **NVIDIA 470.256.02 only**: needs the wake-on-touch driver patch | MacBookPro9,1 |
| Hiding a switched-off GPU from new programs | Device nodes: any driver. Loader files and `/proc` files: **NVIDIA only** | MacBookPro9,1 |

What it would take to cover more hardware:

- **Another laptop or desktop**: a power backend for its platform (ACPI
  `_PR3`, another mux, a Thunderbolt or OCuLink slot). The backend interface
  is small: probe, power off, power on, is it powered.
- **`amdgpu`, `i915`/`xe`, `nouveau`**: these are open drivers with runtime
  power management, so suspend-in-place should need no driver patch, only a
  backend that asks the kernel to do it. Not written yet.
- **Other NVIDIA versions**: the patch has to be checked against each one and
  added to `patches/validated-versions`.

## What runs where

- **The layer** (`libzss_vk.so`, started with `zss-run`) is a Vulkan driver
  shim in user space. It sits between an application and the real driver,
  keeps enough state to rebuild the application on another GPU, and does so
  when asked or when the GPU is lost.
- **The daemon** (`zssd`) decides and sequences: who is using the GPU, what to
  move, freeze or stop, then driver suspend and the power cut, and the reverse.
- **In the kernel there is no ZSS module.** The only kernel change is a small
  patch to NVIDIA's own driver, so that a caller arriving while the driver is
  suspended asks for a wake and sleeps instead of spinning for ever.

Consequences worth knowing:

- Only applications started under the layer can be moved. Anything else that
  holds the GPU is frozen while it is off, or blocks a detach.
- The layer offers Vulkan 1.0 with swapchains. Programs that need a newer
  Vulkan, and OpenGL programs, are not covered by it.
- A card that vanishes without warning is handled in user space only. Nothing
  protects the kernel driver of the vanished card; that has never been tried
  on real hardware.

[`SPEC.md`](SPEC.md) describes the long-term design, including in-kernel
protection against surprise removal (PCIe shield, MMIO shadow, DMA isolation).
None of that is implemented.

---

## Project Structure

* [`SPEC.md`](SPEC.md) - Long-term architecture, multi-vendor design, and failure analysis. Not a description of what exists.
* [`openspec/changes/`](openspec/changes/) - Proposal, design, specs and tasks for each milestone: `zss-happy-path` (orderly detach and attach), `zss-device-loss` (a GPU that disappears), `zss-system-integration` (service, installer, power-off under a desktop).
* [`src/layer/`](src/layer/) - The graphics layer: a Vulkan driver shim that makes applications migratable.
* [`src/daemon/`](src/daemon/) - `zssd`, which runs detach, attach, off and on, and the power backends.
* [`src/zssctl/`](src/zssctl/) - Command-line client.
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
[ZrnSelectiveSuspend]   driver nvidia, power through apple-gmux, wake on demand: yes
[ZrnSelectiveSuspend]   [1/7] In use by: Xorg (display server, idle), vkcube (application, will be moved)
[ZrnSelectiveSuspend]   [2/7] Applications: 1 moved to 0000:00:02.0, 0 parked
[ZrnSelectiveSuspend]   [3/7] Services stopped: nvidia-persistenced; processes frozen: none
[ZrnSelectiveSuspend]         Hidden from new programs (17): /dev/nvidia0, /dev/dri/card2, ...
[ZrnSelectiveSuspend]   [4/7] PCI configuration saved: 256 bytes
[ZrnSelectiveSuspend]   [5/7] Driver nvidia suspended (0.12 s)
[ZrnSelectiveSuspend]   [6/7] Power cut through apple-gmux (0.12 s); the device has left the bus
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
