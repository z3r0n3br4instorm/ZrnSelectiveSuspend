## Why

Everything ZSS does to a device today is done from user space, and that has reached its limits:

- **Power-off works on one machine.** The daemon cuts power by writing the Apple gmux ports through `/dev/port`, and suspends the driver through a file only NVIDIA's driver has. Nothing else can be powered off.
- **PCI state is handled the hard way.** Configuration space is saved and written back register by register through sysfs, in an order found by trial. The kernel has `pci_save_state` and `pci_restore_state` for exactly this, and they are not reachable from user space.
- **A lost card is noticed late and nothing protects its driver.** The daemon polls the bus once a second. Until it notices, the kernel driver keeps talking to hardware that is gone, and nothing tells it to stop.
- **Root-only port pokes race the kernel's own gmux driver.** No collision has been seen, but it is not a design.

The project is meant to be universal. A small kernel module is the place where a GPU can be quiesced, powered off and restored through the kernel's own interfaces, for any driver and on any platform a backend exists for.

## What Changes

- **A kernel module, `zss.ko`** (GPL-2.0), inert when loaded: it touches nothing until a device is handed to it.
- **In-kernel power sequencing** for a managed PCI device and all its functions: quiesce the driver, save state, cut power through a backend, and the reverse, with every step able to fail back to "on".
- **Vendor-neutral driver quiesce.** The module runs the device's own system-sleep callbacks, which every driver that survives a laptop suspend already has: `amdgpu`, `i915`, `xe`, `nouveau`, `snd_hda_intel` for the audio function. Drivers that need their own path (NVIDIA's proprietary one) keep it: the module then does power and state only.
- **Kernel power backends**: `acpi` (firmware power resources, which covers most hybrid laptops made since about 2015), `gmux` (replacing the port writes from user space), and `test` (no hardware effect, for QEMU and host tests).
- **A loss guard.** While a managed device should be on, the module watches for it going silent, marks it disconnected in the kernel at once so its driver stops touching it, and tells user space.
- **Events instead of polling.** State changes are sent to user space as kernel events, which the daemon already listens for.
- **A daemon backend, `zss-kmod`**, used when the module is loaded. Without the module the daemon behaves exactly as it does today.
- **Packaging through DKMS**, optional, never in the initial ramdisk, removed cleanly.

Out of scope: shadowing MMIO, DMA isolation, and the rest of the in-kernel protection sketched in `SPEC.md`; waking a device for drivers other than NVIDIA's (the open drivers have runtime power management for that); replacing the NVIDIA driver patch.

## Capabilities

### New Capabilities

- `kernel-power-sequencing`: taking a PCI GPU through quiesce, save, power-off and back inside the kernel, with pluggable quiesce strategies and power backends.
- `kernel-loss-guard`: noticing in the kernel that a managed device has gone, stopping its driver from touching it, and reporting it.
- `kernel-module-packaging`: building, installing, loading and removing the module safely.

### Modified Capabilities

None. No specification has been archived yet; the daemon's existing behaviour is unchanged when the module is absent.

## Impact

- **New code**: `kmod/` (the module, its Kbuild and DKMS files), a `zss-kmod` backend in `src/daemon/backend.c`.
- **Changed code**: `packaging/install.sh` and `uninstall.sh` (optional module step), `tests/track_b*.py` and `tests/qemu/init.c` (load the module in the guest), README.
- **Risk**: this is kernel code on the user's work machine. It is developed and tested in QEMU first; on the laptop it is loaded only on request, starts inert, and every hardware step needs a go-ahead.
- **Not testable here**: the `acpi` backend on real firmware (this laptop's GPU has no firmware power resources) and a physically pulled card.
- **Kernel versions**: built against the running kernel (7.2). Uses only exported symbols; no kprobes, no patching of other modules.
