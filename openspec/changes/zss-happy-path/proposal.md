## Why

Today a discrete or external GPU cannot be powered down or unplugged while applications are using it: they crash, or the driver refuses to let go. ZrnSelectiveSuspend's first usable milestone is the orderly ("happy") path: the user asks for a GPU to be detached, its applications move to another GPU, the device is released and powered off, and the reverse happens when it returns. This replaces the MMIO-shadow / AER-masking architecture in `SPEC.md` as the starting point, because an orderly detach needs none of it.

## What Changes

- Add a Vulkan layer that lets a running application be moved from one GPU to another without restarting, by reading its GPU state back from the old device and recreating it on the new one. It is loaded as the application's Vulkan driver (through a `zss-run` launcher) and drives the real drivers underneath. In this change it offers Vulkan 1.0 only.
- Add a system daemon and a `zssctl` command that run the detach and attach sequences: find the GPU's clients, migrate or park them, release the device from the kernel, power it off, and undo all of that on return.
- Add pluggable power backends that perform the power-off and state whether physical removal is electrically safe. Three ship in this change: hot-plug slot power, Apple gmux, and a dry-run backend that migrates without touching power.
- Report "safe to remove" only when no client remains, the kernel has released the device, and the backend confirms power is off.
- Add two test tracks: a QEMU harness that hot-removes and hot-adds a display card to exercise the full sequence, and a host harness that migrates real applications between this machine's Vulkan devices with nothing powered off. The full run on the reference laptop is a final, separately gated stage.
- Correct the Apple gmux port labels in `include/zrn_selective_suspend.h` (`0x728` is DDC, `0x740` is the external-port mux).
- Mark the surprise-removal subsystems in `SPEC.md` (PCIe shield, MMIO shadow, DMA isolator, resurrection engine) as deferred and not part of this milestone.

Not in this change: surprise removal, the on-disk state store, OpenGL applications, freezing on focus loss, and migrating a display server off the GPU.

## Capabilities

### New Capabilities

- `graphics-context-migration`: moving a running Vulkan application's graphics state between GPUs, parking it when no suitable GPU exists, and restoring it later.
- `gpu-detach-orchestration`: the user-facing detach and attach sequences, client discovery, blocker reporting, state reporting, and the safe-to-remove gate.
- `gpu-power-backend`: the contract a platform power backend fulfils, and the behaviour of the backends shipped in this change.

### Modified Capabilities

None. No specs exist yet.

## Impact

- **New code:** `src/layer/` (Vulkan layer), `src/daemon/` (daemon and power backends), `src/zssctl/`, `tests/` (QEMU and host harnesses, test application), and a build system at the repository root.
- **Changed files:** `include/zrn_selective_suspend.h`, `SPEC.md`, `README.md`.
- **Dependencies:** Vulkan headers (system or Meson wrap) and loader, a software Vulkan driver for the tests, and QEMU with KVM for the VM track. The guest uses the host's kernel and root file system, so no image is needed.
- **Privileges:** the daemon runs as root because it removes PCI devices and controls power. Applications talk to it as ordinary users.
- **Runtime cost:** applications started under the layer carry bookkeeping for every graphics object they create and one extra copy of mapped buffers per submit; applications not started under it cannot be migrated and block a detach.
- **Compatibility:** applications that need Vulkan 1.1 or later do not run under the layer yet.
- **Hardware risk:** the gated laptop stage cuts power to the dGPU and can hang the machine. Nothing before that stage touches power on the host.
