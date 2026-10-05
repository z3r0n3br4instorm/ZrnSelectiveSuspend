## Context

See `proposal.md` for motivation. The repository currently holds `SPEC.md`, a README and one header; there is no build system and no code.

Facts about the development machine (MacBookPro9,1, Linux 7.2.2, X11) that shape the design:

- Three Vulkan devices are visible on the host: Intel HD 4000 (Vulkan 1.2, Mesa), NVIDIA GT 650M (Vulkan 1.2, proprietary 470.256.02) and llvmpipe (Vulkan 1.4).
- QEMU 11.1.1 with KVM is installed. It offers `pcie-root-port`, `bochs-display` and `secondary-vga`. No virtio-gpu device is installed, so a guest has no accelerated GPU.
- Xorg is the only process holding `/dev/nvidia*`.
- `apple_gmux` is loaded and owns I/O ports `0x700`–`0x7ff`. It exposes discrete-GPU power only through `vga_switcheroo`, which the proprietary NVIDIA driver does not join.
- The IOMMU is disabled and the dGPU is soldered.

An orderly detach means the GPU is healthy while applications leave it. That is what makes this milestone tractable: state can be copied off the live device, and once no client remains the kernel driver can be suspended or unbound by existing kernel interfaces.

## Goals / Non-Goals

**Goals:**

- One layer design that works unchanged across GPU vendors, relying only on the Vulkan API and the standard driver-loading interface.
- No custom kernel code in this milestone.
- Every stage before the laptop stage is safe to run repeatedly on the development machine.
- Tests that fail loudly when a migration silently loses content.

**Non-Goals:**

- Surprise removal, and with it the PCIe shield, MMIO shadow, DMA isolator and on-disk state store.
- OpenGL applications. Routing them through Zink is a later change.
- Full Vulkan coverage. The layer tracks a defined subset and declares everything else non-migratable.
- Migrating a display server. A display server on the GPU is a blocker.
- Freezing applications on focus loss or thawing on interaction.
- Making the weaker GPU look like the stronger one.

## Decisions

### D1. Three components, all userspace

```
 zssctl ──► zssd (root) ──► PCI sysfs remove / rescan
              ▲    │
   unix socket│    └──────► power backend
              │
 app + ZSS Vulkan layer ──► real Vulkan driver
```

`zssd` owns policy and privileged operations. The layer owns everything inside an application. `zssctl` is a thin client.

*Alternative considered:* a kernel module that parks the `pci_dev` as a phantom while the card is away. Rejected for this milestone because once clients have migrated nothing in userspace references the device, so existing suspend and remove interfaces are enough (D10).

### D2. The layer is a Vulkan driver shim that owns every handle

The layer is loaded by the Vulkan loader as the application's *only* driver (`zss-run` sets `VK_DRIVER_FILES` to its manifest). It loads the real vendor drivers itself and presents virtual GPUs, devices and objects on top of them. Every handle the application holds, including the device, queues and command buffers, is an object the layer owns; each maps to a real driver handle and stores the parameters used to create it. Migration is:

1. Hold the application's Vulkan entry points and wait for its submitted work to finish.
2. Read back the contents of every tracked buffer and image from the source device into system memory.
3. Create a device on the target, recreate each object in creation order from the stored parameters, and upload the contents. Command buffers are replayed from a recording.
4. Destroy the source's objects and device. The source stays intact until step 3 has succeeded, so a failed migration leaves the application where it was.
5. Unload the source's driver library, then release the held entry points.

Because the source is alive, step 2 captures GPU-generated content too, so no continuous recording of data is needed.

*Why a driver shim and not an implicit layer, as first planned.* Two findings during implementation forced this:

- An implicit layer sits below handles the loader and the real driver own. The application's device handle cannot outlive the real device, and the driver's own physical-device objects stay alive for the life of the instance.
- Real drivers keep device files open for as long as they are loaded. NVIDIA 470 holds `/dev/nvidia0`, `/dev/nvidiactl` and `/dev/nvidia-modeset` from instance creation until its library is unloaded, whatever is destroyed in between. The requirement that a migrated application holds no handle to the source GPU can only be met by unloading the driver, which only something above the driver can do.

A prototype confirmed that swapping the real device under a loader-visible handle works on all three host drivers, but it could not release the driver. The shim does both.

*Cost.* The shim must implement every entry point itself; nothing can be passed through untouched. That is what bounds the supported subset in D3.

*Alternative considered:* record-and-replay of the full call stream including data. Rejected here because it costs memory or disk continuously and is only needed when the source device is already dead.

**Swapchains are not migrated.** Image counts and formats belong to one driver. After a migration the old swapchain is retired: its images are replaced by ordinary stand-ins so in-flight rendering has somewhere to go, and acquire and present return `VK_ERROR_OUT_OF_DATE_KHR`, which windowed applications already handle by building a new swapchain.

**Application memory is virtual.** Memory type numbers and sizes differ between drivers, so an application's `VkDeviceMemory` is never a real allocation. Each buffer and image gets its own real allocation on the current device. This uses one driver allocation per resource, which large applications may exhaust; pooling is future work.

### D3. Supported subset, with explicit refusal outside it

**The layer presents a Vulkan 1.0 device with `VK_KHR_swapchain` as its only device extension.** An application that requires a newer Vulkan version does not find a suitable device and does not start under the layer. This is the main limit of the milestone and the first thing to lift.

Within Vulkan 1.0, tracked and migratable: devices, queues, memory, buffers, images, image views, samplers, shader modules, render passes, framebuffers, descriptor set layouts, pools and sets, pipeline layouts, pipeline caches, graphics pipelines, command pools and primary command buffers, fences, semaphores, surfaces and swapchains.

Forwarded but not tracked: compute pipelines and dispatch, query pools, events, buffer views, secondary command buffers, sparse resources, and image formats whose texel size the layer does not know. The first use of any of these marks the device non-migratable with the feature named; the application keeps running. This keeps the first version honest and gives a growing list of what to add next.

Three narrower limits:

- Multisampled images are recreated empty, since their contents cannot be copied out. They are normally redrawn every frame.
- A linear image the application fills through mapped memory is laid out by one driver. It migrates once uploaded, but a migration requested while the upload is in progress fails with a "retry" reason.
- While any device in a process is parked, every Vulkan call in that process is held, including one that would destroy the device. A parked application must be resumed or killed; it cannot exit on its own.

Mapped memory needs special care, because applications commonly map a buffer once and keep the pointer (`vkcube` does this for its uniform data), and a new device cannot hand back the same address. The layer therefore never gives the application the driver's mapping. It returns a shadow allocation it owns, copies shadow to device memory at submit, flush and unmap, and copies device memory to shadow after the application waits on a fence or on queue or device idle. The pointer stays valid across migration because it never pointed at the device. The costs are one extra copy of mapped ranges per submit, and that an application reading GPU-written mapped memory without waiting first sees stale data; the second is recorded as a known limitation.

### D4. Capabilities are checked at migration time, not masked at start

The layer passes the source GPU's real capabilities to the application. When migration is requested it compares what the application enabled and used (features, extensions, formats, limits it exceeded) against the target. A mismatch means park.

*Alternative considered:* advertise only the intersection of all GPUs from the start. Rejected because on the reference laptop that would cap every application at the HD 4000's level and remove the reason to have a dGPU.

### D5. Parking reuses migration

Parking runs migration steps 1, 2 and 5 with no target: state stays in system memory and the application's rendering threads stay held inside the layer. Resume runs steps 3 to 5. `zssd` additionally freezes a parked process with the cgroup freezer so it uses no CPU.

A detach therefore has three per-application outcomes: migrate, park, or block.

Freezing moves the process into a child cgroup of its own (`zss-parked-<pid>`) and freezes that, so nothing else in its session is affected. If that is not permitted the application simply stays held inside the layer.

### D6. Mapping a Vulkan device to a PCI device

The layer reads the PCI address from `VK_EXT_pci_bus_info` (present on the NVIDIA 470 and Mesa drivers), and falls back to matching vendor and device IDs in sysfs.

For tests, `ZSS_BIND_PCI=<address>` adds a second view of the software renderer that poses as the PCI device at that address and holds the device's DRM node open while in use. This gives the QEMU guest, which has no accelerated GPU, a real client of the hot-plugged card.

The layer remembers on disk which PCI devices each driver library serves, and does not load a driver whose GPU the daemon reports as detached: loading it would touch hardware that is powered off.

### D7. Client discovery from `/proc`

`zssd` finds a GPU's device nodes from sysfs (DRM nodes under the PCI device, plus the NVIDIA character device when the `nvidia` driver is bound) and scans `/proc/*/fd` for processes holding them. A process is migratable if it has registered through the layer, a display server if its name is on a known list (`Xorg`, `Xwayland` and the common Wayland compositors), and non-migratable otherwise. The same scan is repeated after the applications report, as an independent check that nothing still holds the device.

### D8. IPC

A Unix stream socket at `/run/zss/zssd.sock` carrying newline-delimited JSON, specified in `docs/protocol.md`. `zssd` authenticates peers with `SO_PEERCRED`: registration from any local user; detach, attach and resume from root, the `zss` group, or the user the daemon itself runs as (which `--no-owner-access` turns off). Events are delivered to any connection that subscribes.

*Alternative considered:* D-Bus. Rejected to keep the layer free of dependencies, since it is loaded into every application.

### D9. Power backends

A backend is a small interface: power off, power on, is powered, removal is safe.

- **`pciehp-slot`**: writes the slot's `power` attribute under `/sys/bus/pci/slots/`. Declares removal safe. This is the backend the VM uses and it is a real backend for hot-plug-rated hardware.
- **`apple-gmux`**: port I/O from `zssd`, following the power sequence in the kernel's `apple-gmux` driver. Declares removal not supported. Uses suspend in place (D10).
- **`dry-run`**: changes nothing and skips the release step. Because it takes nothing away, processes outside the layer (a display server, for one) are listed but do not block it. That is what lets migration be tested on a GPU the desktop is using.

For `apple-gmux`, power is confirmed by reading the card's PCI vendor ID, which reads as all ones when the card is unpowered; the gmux itself has no documented power readback.

*Alternative considered for gmux:* a kernel module, or `vga_switcheroo`. Switcheroo is unavailable with the proprietary driver, and a module would add the only kernel code in the milestone for one register write. The trade-off is in Risks.

### D10. Two release strategies, chosen by whether the device can leave

How the kernel driver lets go of the device before power is cut depends on the hardware:

| Strategy | Used when | Before power-off | After power-on |
| :--- | :--- | :--- | :--- |
| **Suspend in place** | The device cannot be physically removed, so the same card always returns (`apple-gmux`) | Driver stays bound and is suspended as it would be for system sleep | Driver is resumed as it would be on wake |
| **Unbind** | The device can be removed or swapped (`pciehp-slot`) | PCI remove; no driver is bound | PCI rescan; the driver probes whatever is there |

Suspend in place is the primary strategy on the reference laptop because wake-from-sleep is the path the NVIDIA driver already exercises on this hardware: it re-initialises the card from state it cached at load, so it does not depend on a cold probe finding a usable video BIOS, and it does not require every kernel-side holder of the device to drop first.

Suspending one device outside a system sleep is driver-specific:

- Drivers with runtime power management that tolerates full power loss (the open DRM drivers) are suspended through runtime PM, where the PCI core saves and restores configuration space itself.
- The proprietary NVIDIA driver is suspended by writing `suspend` and `resume` to `/proc/driver/nvidia/suspend`; 470 has no runtime PM on Kepler. Because no system sleep is in progress, nothing saves or restores PCI configuration space, so `zssd` saves it through sysfs before the power cut and writes it back after the link is up. Newer NVIDIA drivers that do support runtime PM could use the first path; that choice is not implemented.

Unbind remains the only correct strategy for removable devices: a suspended driver cannot be resumed onto a different card, and a hot-plug port tears the device down when it leaves regardless.

*Alternative considered:* unbind everywhere. Rejected as the laptop's primary path because it bets on an untested cold probe and on clearing every holder of the device, when a known-good path exists.

### D11. Languages and build

C11 for the layer, daemon and `zssctl`; Meson for the build; Python for the test harnesses. C keeps the layer small and matches the project's kernel-side code. Vulkan headers come from the system when installed and from a Meson wrap otherwise.

### D12. Three test tracks, ordered by risk

| Track | Where | What it exercises | Hardware state changed |
| :--- | :--- | :--- | :--- |
| A | Host | Layer migration between real Vulkan devices, `dry-run` backend | None |
| B | QEMU | Full detach and attach with `pciehp-slot`, hot-remove and hot-add of `bochs-display` over QMP | Guest only |
| C | Host | Full sequence with `apple-gmux` | dGPU power |

In track B the guest renders on llvmpipe and the layer, told by `ZSS_BIND_PCI` to pose as the hot-plugged card, holds that card's device node open, so the card has a real client without needing an accelerated guest GPU. Track B proves sequencing and kernel behaviour; track A proves graphics migration. Neither substitutes for the other.

The guest needs no disk image: it boots the host's kernel with a one-program initramfs and uses the host's root file system read-only over 9p. `docs/qemu-hotplug.md` records how the virtual slot behaves, including that QEMU ejects the card as soon as the guest cuts slot power.

Correctness of migration is checked by a test application that renders a deterministic scene, including an image accumulated across frames, and writes frames to disk. Frames after migration are compared with a run that never migrated. Comparison is exact when source and target are the same driver and tolerance-based across drivers.

Track C does not start without the user's explicit go-ahead in the session that runs it.

## Risks / Trade-offs

- **Xorg holds the NVIDIA device on the laptop** → It is a blocker by specification, so track C cannot complete until X releases the GPU. Track C begins with an investigation task for this. If X cannot release it mid-session, track C is run from a session whose X configuration does not load the NVIDIA driver, and that limitation is recorded.
- **Applications that render on NVIDIA may need the X server's NVIDIA driver to present** → If so, removing the driver from X prevents the very applications we want to test. The investigation task covers this; the fallback is to validate track C with an off-screen test application.
- **Suspend in place is driven from userspace, outside a real system sleep** → The kernel still believes the device is powered and present while it is off, so anything that touches it in that window (a configuration-space read, a monitoring tool, the audio function's driver) reaches absent hardware. The audio function is unbound before the cut and rebound after, the window is kept to the suspended period only, and task 9.4 checks the kernel log for errors. If this proves fragile, a small kernel helper that performs the save, power transition and restore inside the PCI core's own power-management path is the follow-up; it is the first place this project is likely to need kernel code.
- **The X server may not tolerate the NVIDIA driver being suspended under it** → System sleep normally switches away from the X session first. Task 9.3 tests whether that is needed here. A display server on the GPU stays a blocker in this milestone either way.
- **Unbind on the laptop is untested** → The driver's remove may wait on `nvidia_modeset`, `nvidia_drm` or the firmware framebuffer attached to the dGPU, and its probe may fail on a cold-powered card. It is no longer the laptop's primary path; task 9.5 tries it only to learn whether it works.
- **Port I/O to gmux can race the `apple-gmux` driver** → The driver writes the power register only during switcheroo power changes and resume, neither of which happens during a detach. The sequence is verified against the kernel source before first use, and the backend refuses to run if a classic gmux is not detected.
- **Powering off the dGPU may hang the laptop** → Track C runs from a text console with work saved, and its first run removes the device and powers off with no applications involved.
- **The supported Vulkan subset is too small for most real applications** → Non-migratable reasons are reported per feature, giving a measured list of what to add. `vkcube` and the test application are the acceptance bar for this milestone, and both pass.
- **Cross-driver migration can change rendering slightly** → Tests use a tolerance across drivers. Applications that depend on bit-exact output are out of scope.
- **Hot-removing `bochs-display` from QEMU while the guest has already powered the slot off may behave differently from a real unplug** → The harness asserts guest-side state after each QMP step and the first task of track B is a manual run that pins down the exact QMP sequence.
- **A held application can be killed as "not responding" by the desktop** → Migration holds entry points only for the duration of the copy. Parked applications are affected; this is recorded as a known limitation.
- **`ZSS_BIND_PCI` lets a process claim any GPU** → It only affects that process's own registration and cannot cause a detach; it is still documented as test-only.
- **Nothing that needs root has run on the host.** The session that built this had no root access. The `pciehp-slot` backend, PCI remove and rescan, and the cgroup freezer were exercised as root inside the QEMU guest. The `apple-gmux` backend, runtime-PM suspend, and the NVIDIA suspend-in-place sequence have been written and reviewed against the kernel source but never executed; track C is their first run.
- **Applications needing Vulkan 1.1 or later cannot use the layer** → Stated in D3. Lifting it means implementing the newer entry points in the shim, which is mechanical but large.

## Migration Plan

Nothing is deployed today, so there is nothing to migrate. Rollback at any stage is uninstalling the layer manifest and stopping `zssd`; with the layer absent applications run exactly as before. After a failed track C run, a reboot restores the dGPU.

## Open Questions

- Which desktop environments kill parked windows, and how to suppress that. Deferrable: parking still functions.
- Whether a Wayland surface migrates as an X11 one does. The code path is the same, but only X11 has been run.
