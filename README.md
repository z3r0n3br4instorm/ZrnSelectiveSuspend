# ZrnSelectiveSuspend
### By Zerone Laboratories

**Power a GPU off, or lose it, while the desktop and its applications keep running.**

ZSS lets you take a graphics card out of service on a live Linux system: its
applications move to another GPU, the card is powered off, and everything
moves back when it returns. It is meant to work with any GPU and any driver.
Today some parts do, and some work only on the one machine they were built on.
The table below says which.

> **Status (October 2026).** Working on real hardware, a MacBookPro9,1 with an
> NVIDIA GT 650M and an Intel HD 4000:
>
> - **Power off and on under a running X session** in about 0.3 s, with
>   applications moved to the Intel GPU and back.
> - **Lending the card to a virtual machine** and taking it back, with no
>   reboot and no log-out: programs moved away, the card handed to `vfio-pci`, a
>   QEMU guest using it, then the card reset and returned with its programs.
> - **Moving Vulkan programs, OpenGL programs (through Zink), browsers and
>   Electron apps, and Unity games** between the two GPUs while they draw.
> - **A card that loses power without warning while idle** is brought back,
>   driver included, with no reboot.
>
> Not working yet: a card lost **while a program is rendering on it** (the
> desktop survives, the program does not move, the card needs a reboot).
> Working in a virtual machine only: powering off a GPU with an open-source
> driver, and hot-removal of a PCIe card. See "What is universal and what is
> not" and "When a card is lost without warning".

---

## What is universal and what is not

| Part | Works with | Tested on |
| :--- | :--- | :--- |
| Moving applications between GPUs (ZSS_AirLock) | Any Vulkan driver: NVIDIA, Mesa (Intel, AMD, Nouveau), software | NVIDIA 470, Intel `hasvk`, llvmpipe |
| Moving OpenGL applications (through Mesa's Zink) | Any Vulkan driver; ZSS_AirLock stands in for dynamic rendering and `VK_KHR_maintenance5` where a driver lacks them | NVIDIA 470, Intel `hasvk` (OpenGL 3.2) |
| Showing frames drawn on one GPU on a screen driven by another, in order | Drivers that do not hand frames across themselves (the NVIDIA proprietary driver without its card in X) | NVIDIA 470 to Intel |
| Lending a GPU to a virtual machine and taking it back (`zssctl lend`, `reclaim`) | Any PCI GPU behind an IOMMU, with ZSS_Interceptor; the display server must not hold the card | MacBookPro9,1 with a QEMU guest; QEMU with an emulated IOMMU |
| Recovering applications when a GPU vanishes | Any Vulkan driver | QEMU, by pulling a virtual card |
| Daemon, `zssctl`, rules for who may hold a GPU, freezing, idle timer | Any GPU | Host tests and QEMU |
| Quiescing the driver before a power cut (kernel module, `quiesce=pm`) | Any driver that survives a laptop suspend: `amdgpu`, `i915`, `xe`, `nouveau`, and others | **QEMU's `bochs` driver only** |
| Saving and restoring PCI state, noticing a lost card within 0.3 s (kernel module) | Any PCI GPU | QEMU; MacBookPro9,1 |
| Cutting power: Apple classic gmux (`gmux` backend) | Apple laptops with a classic gmux | MacBookPro9,1 |
| Cutting power: firmware power resources (`acpi` backend) | Most hybrid laptops since about 2015 | **Nowhere yet: written, never run** |
| Cutting power: PCIe hot-plug slot (`pciehp-slot`, user space) | Any driver, on a hot-plug slot | QEMU only |
| Power-off under a running display server | **NVIDIA 470.256.02 only**: needs the wake-on-touch driver patch | MacBookPro9,1 |
| Recovering card and driver after an unannounced power loss, card idle | **NVIDIA 470.256.02 with the patch** (freeze and thaw); drivers with PCI error handlers in principle | MacBookPro9,1 |
| The same, with a program rendering on the card | **Nothing yet** | Failed on MacBookPro9,1 |
| Hiding a switched-off GPU from new programs | Device nodes: any driver. Loader files and `/proc` files: **NVIDIA only** | MacBookPro9,1 |

What it would take to cover more hardware:

- **`amdgpu`, `i915`/`xe`, `nouveau`**: ZSS_Interceptor already runs any
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

Two parts carry names of their own, because they are the two shims the rest
is built around:

- **ZSS_AirLock** (`libzss_airlock.so`, started with `zss-run`) is the
  user-space shim for Vulkan and, through Mesa's Zink, OpenGL. To an
  application it is the Vulkan driver; underneath it loads the real drivers.
  It keeps enough state to rebuild the application on another GPU, and does so
  when asked or when the GPU is lost. Its messages begin `[ZSS_AirLock]`.
  When a program draws on a GPU that does not drive the screen and its driver
  cannot hand frames across in order, ZSS_AirLock presents them itself on the
  screen's GPU (`ZSS_PRESENT=direct` turns that off).
- **The daemon** (`zssd`) decides and sequences: who is using the GPU, what to
  move, freeze or stop, then driver suspend and the power cut, and the reverse;
  and for lending, the hand-over of the card to `vfio-pci` and back.
- **ZSS_Interceptor** (the kernel module `zss.ko`, optional) is the kernel
  shim, the part that touches the hardware. The module's file, its `/sys`
  entries and its kernel messages keep the short name `zss`. What it does: it quiesces the driver through the driver's own sleep code, saves
  and restores PCI state, cuts and restores power through a backend, and
  watches for a card that goes silent. Loaded, it does nothing until the
  daemon hands it a device. Without it the daemon does a narrower version of
  the same from user space (NVIDIA and gmux only). See
  [`docs/kernel-module.md`](docs/kernel-module.md).
- **The NVIDIA driver patch** is separate and still needed for NVIDIA. It does
  two things. A caller arriving while the driver is suspended asks for a wake
  and sleeps instead of spinning for ever. And the driver can be *frozen*: shut
  to every caller without anything being asked of the card, for the moment a
  card vanishes, and resumed when the card is back.

Consequences worth knowing:

- Only applications started under ZSS_AirLock can be moved. Anything else that
  holds the GPU is frozen while it is off, or blocks a detach.
- ZSS_AirLock offers Vulkan 1.1 with swapchains and a short list of
  extensions. Chromium runs under it and can be moved between the NVIDIA and
  Intel GPUs while it draws (see
  [`docs/applications/chromium.md`](docs/applications/chromium.md)). OpenGL
  programs run under it through Zink (`zss-run --gl`, OpenGL 3.2 on this
  laptop's pair of GPUs). Programs that need Vulkan 1.2 or more are not
  covered yet.
- What a program is offered is what every GPU it may be moved to has, so that
  nothing it enables can hold it on one card. On the reference laptop that
  withholds seven features of the NVIDIA card. `ZSS_PROFILE=native` offers
  each GPU's own instead; a program that uses the difference is then parked
  rather than moved.
- A card that vanishes without warning is noticed by ZSS_Interceptor within
  about a tenth of a second, marked disconnected, and its driver told through
  the kernel's PCI error-recovery handlers if it has them, or frozen if it is
  the patched NVIDIA driver. What follows depends on whether the card was in
  use: see "When a card is lost without warning". None of it has been tried
  with a card physically pulled from real hardware; the losses so far were
  made by cutting the card's power rail.
- The module reports whether an IOMMU confines the GPU's memory access
  (`iommu=` in `zssctl status`). It does not add confinement of its own.

[`SPEC.md`](SPEC.md) describes the long-term design and says, subsystem by
subsystem, what was built, what was built differently, and what was dropped
and why (MMIO shadowing and a DMA isolator of our own are not planned).

---

## Project Structure

* [`SPEC.md`](SPEC.md) - Long-term architecture, multi-vendor design, and failure analysis. Not a description of what exists.
* [`openspec/changes/`](openspec/changes/) - Proposal, design, specs and tasks for each milestone: `zss-happy-path` (orderly detach and attach), `zss-device-loss` (a GPU that disappears), `zss-system-integration` (service, installer, power-off under a desktop), `zss-kernel-shim` (ZSS_Interceptor), `zss-vulkan-12-and-opengl` (wider Vulkan, OpenGL through Zink, applications), `zss-vm-passthrough` (lending a GPU to a virtual machine).
* [`tester/`](tester/) - The tester kit: a report of what a machine has for ZSS, with the moving tests, in a window or a terminal.
* [`src/layer/`](src/layer/) - ZSS_AirLock: a Vulkan driver shim that makes applications migratable.
* [`src/daemon/`](src/daemon/) - `zssd`, which runs detach, attach, off and on, and the power backends.
* [`src/zssctl/`](src/zssctl/) - Command-line client.
* [`kmod/`](kmod/) - ZSS_Interceptor (the `zss` kernel module) and its DKMS files.
* [`packaging/`](packaging/) - Installer, service units, the NVIDIA patch tool, and an Arch package.
* [`patches/`](patches/) - The wake-on-touch patch to the NVIDIA driver.
* [`tests/`](tests/) - Test application, frame comparison, and the host, QEMU and hardware harnesses.
* [`docs/`](docs/) - Wire protocol, test notes, hardware results, and what happens when a GPU is lost.
* [`include/`](include/) - Register maps for the reference platform.

## Installing

Each push to `main` publishes a release (GitHub Actions,
`.github/workflows/release.yml`) with two downloads, tested on Arch Linux and built
on Ubuntu 22.04:

- `zss-installer-VERSION.run`: `sh zss-installer-VERSION.run --check` says what
  the machine supports and changes nothing; without `--check` it installs.
- `zss-tester-VERSION.run`: the tester kit, with the moving tests ready to run.

They need glibc 2.34 or later: tried on Debian 12, Ubuntu 22.04 and 24.04,
Fedora 44 and openSUSE Tumbleweed (install, `--check`, uninstall and the
tester kit, in containers), and Arch Linux. The
version is `MAJOR.MINOR` from `meson.build` and a number counted up from the
last release; `packaging/make-bundle.sh VERSION DIR` makes the same files
locally. From source:

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
against (470.256.02 so far, patch revision 5); it then survives kernel updates, and a hook
re-applies it after a driver update. The stock modules are kept and restored at
boot if the patched driver does not load. `zss-nvidia-patch status` shows where
things stand, and `zss-nvidia-patch remove` puts the stock driver back.

At the end of an install the installer records what the machine has for ZSS
(the facts the tester kit collects, without the moving tests) together with
its own messages. When everything went well that is kept as `~/.zss/debug.log`;
when something failed, a full report is saved in `~/.zss/` as well and the
installer offers to open your mail program to send it. Computer and user names,
serial numbers and network addresses are removed first; nothing is sent
without you.

`--vm-passthrough` also prepares the GPU for lending to virtual machines: a
udev rule keeps the display server and the login manager off the card's display
nodes. The IOMMU has to be switched on by you (`intel_iommu=on` or
`amd_iommu=on` on the kernel command line); `--check` says whether it is, and
whether the card shares its isolation group with other devices.

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
zssctl on 0000:01:00.0        # power on and bring the applications back; --stay leaves them where they are
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
and the timer never freezes anything: a program outside ZSS_AirLock keeps the GPU on.

A monitor plugged in while the GPU is off is not noticed until something asks
about displays or you run `zssctl on`.

### On battery

`zss-power-event battery` switches the dedicated GPU off and moves its
applications, showing the progress ("Moving application 2 of 3") in
`zrn-warning-dialog` when that is installed; `zss-power-event ac` switches it
back on and returns the applications, if it was the battery event that
switched it off; a GPU switched off by hand stays off. The performance
manager (`zrn_perfd`) calls it on every charger change once it is installed
at `/usr/local/sbin/zss-power-event`. `--dry-run` shows the dialog and
switches nothing.

## Lending the GPU to a virtual machine

```sh
zssctl lend --check 0000:01:00.0   # what stands in the way, and what would be affected; changes nothing
zssctl lend 0000:01:00.0           # programs moved away, card handed to vfio-pci
zssctl reclaim 0000:01:00.0        # card reset by a power cycle, host driver back, programs returned
```

While it is lent the card belongs to the guest: ZSS does not watch, wake or
power it, and `off`, `on`, `detach` and `attach` are refused. `reclaim` is
refused while a virtual machine holds it, and `zssctl status` names the process
that does. A card that shares its IOMMU group with other devices is lent with
`--with-group`: they go with it and lose their host drivers until it comes back.

Before handing the card over, the kernel modules stacked on its driver (for
NVIDIA: `nvidia_uvm`, `nvidia_drm`, `nvidia_modeset`) are unloaded, and the
driver must have no user left; a driver asked to let go of a card still in use
waits for ever, so the lend is refused instead. The modules are loaded again on
reclaim.

On the reference laptop, 9 October 2026: lent in 2.9 s, used by a QEMU guest
booted from a CachyOS live image, and reclaimed in 11.2 s after the guest shut
down. What the laptop needed (the card out of X, the IOMMU, interrupts without
remapping because of a firmware bug) and how to start the guest:
[`docs/vm-handover.md`](docs/vm-handover.md).

## When a card is lost unexpectedly

When powering down via the ZSS GPU Power Down system, the GPU is safely powered down and recovered cleanly. If the GPU loses power or disconnects unexpectedly while something was actively rendering, the kernel and background system processes stay intact, but the display manager may hang. What happens today on the reference laptop (NVIDIA 470 with the patch, X running, kernel module loaded), from real power cuts:

| The card was | The desktop | Programs on the card | Getting the card back |
| :--- | :--- | :--- | :--- |
| idle | carries on | none | `zssctl on`: power, PCI state and driver restored, no reboot |
| being rendered on | carries on | stuck: frozen out of the driver, not moved to the other GPU | **a reboot.** `zssctl on` refuses while the stuck programs hold the card |

```sh
zssctl status                 # state=lost, driver=frozen
zssctl on 0000:01:00.0        # tries to restore power, then resumes the driver
```

Why the busy case fails: a program that is rendering calls into the driver
many times in the tenth of a second it takes to notice the loss, so the driver
finds out first; once the driver is frozen the program's calls sleep, so the
layer cannot move it; and a driver resumed under a program whose state on the
card is gone hung the machine in testing, which is why the daemon now refuses
to do it. Closing the stuck programs and then running `zssctl on` is the
obvious next thing and has not been tried.

Three of the tests that produced this table ended with the laptop reset or
hung. Details, timings and logs: [`docs/track-c.md`](docs/track-c.md).

## Help test it

The tester kit in [`tester/`](tester/) collects what a machine has for ZSS
(graphics cards, drivers, power control, IOMMU, Vulkan and OpenGL) and runs the
moving tests with a test daemon that switches nothing off. It changes nothing,
sends nothing by itself, and removes names and identifiers from the report.
`tester/zss-report-gui` opens it in a window; `tester/zss-report` runs it in a
terminal. See [`tester/README.md`](tester/README.md).

## Building and trying it

```sh
meson setup build && ninja -C build
meson test -C build                       # host and QEMU tests; nothing on this machine is powered off,
                                          # and ZSS_Interceptor is loaded only inside the QEMU guest

# Terminal 1: manage the dGPU without touching its power
build/src/daemon/zssd --socket "$XDG_RUNTIME_DIR/zss.sock" --gpu 0000:01:00.0=dry-run --allow-software
# Terminal 2: run something under ZSS_AirLock, then move it away and back
export ZSS_SOCKET="$XDG_RUNTIME_DIR/zss.sock"
build/src/layer/zss-run vkcube &
build/src/zssctl/zssctl detach 0000:01:00.0
build/src/zssctl/zssctl attach 0000:01:00.0
```

Applications run under ZSS_AirLock see a Vulkan 1.1 device with the extensions
it can carry across a move. Programs that need Vulkan 1.2 or more do not start
under it yet.

An OpenGL program. `--gl` runs it on Mesa's Zink, which draws OpenGL through
Vulkan, so that it is under ZSS_AirLock like any Vulkan program and can be moved
(OpenGL 3.2 and OpenGL ES 3.1 on this laptop's pair of GPUs):

```sh
build/src/layer/zss-run --gl glxgears
```

Zink needs two things the NVIDIA 470 driver has not got (dynamic rendering and
`VK_KHR_maintenance5`); on such a driver ZSS_AirLock provides them itself.

A browser, started on the dedicated GPU. `zss-run` recognises a program built
on Chromium (a browser, an Electron application) and adds the two switches
that make it draw through Vulkan; `--plain` leaves the arguments alone and
`--print` shows what would be run:

```sh
build/src/layer/zss-run chromium
```

| Variable | Effect |
| :--- | :--- |
| `ZSS_SOCKET` | Socket of the daemon to talk to (default `/run/zss/zssd.sock`) |
| `ZSS_DEBUG` | Makes ZSS_AirLock log what it loads and why a device is not migratable |
| `ZSS_ALLOW_SOFTWARE` | Lets a parked application resume on a software renderer |
| `ZSS_START_ON` | The GPU a program starts on: `dedicated` (what `zss-run` uses by default: the discrete card), a PCI address, part of a GPU's name, or `any`. `zss-run --on GPU` sets it. The other GPUs are not listed to the program but it can still be moved to them |
| `ZSS_PRESENT` | `direct`: every swapchain on the GPU the program draws on, as drivers do. `copy`: through the screen's GPU even for Mesa drivers. Default: through the screen's GPU only where needed |
| `ZSS_TRAP` | Debugging aid: a Vulkan command the program calls that ZSS_AirLock does not provide stops it with the command's name |
| `ZSS_PROFILE` | `portable` (default): each GPU reports what all GPUs the program may be moved to have. `native`: each reports its own |
| `ZSS_VULKAN` | `1.0` makes ZSS_AirLock present Vulkan 1.0 only, as it did before |
| `ZSS_REAL_DRIVER_FILES` | Colon-separated driver manifests for ZSS_AirLock to use instead of the system's |
| `ZSS_RETAIN` | Where uploaded textures are kept so they survive a lost GPU: `disk` (default), `ram`, or `off` |
| `ZSS_WRITE_WATCH` | `0` copies a program's mapped memory to the GPU whole at every submission, as before. By default only the pages it wrote since the last submission are copied (Linux 6.7 or later; older kernels fall back to the whole copy) |
| `ZSS_RETAIN_LIMIT_MB`, `ZSS_RETAIN_QUEUE_MB` | Size cap of the store (4096) and of data waiting to be written (256) |
| `ZSS_BIND_PCI` | Test aid: makes the software renderer pose as the PCI device at that address |
| `ZSS_TEST_LOSE_AT_SUBMIT` | Test aid: ZSS_AirLock behaves as if the GPU died at that submit |
| `ZSS_TEST_LOSE_AFTER_SUBMIT` | Test aid: the GPU dies just after that submit was accepted, with its work in flight |
| `ZSS_TEST_STUCK_MS`, `ZSS_LOSS_GRACE_MS` | Test aids: hold a thread inside ZSS_AirLock during a loss; how long recovery waits for such threads (3000) |

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

