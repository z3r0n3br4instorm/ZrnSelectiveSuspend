## 1. Repository groundwork

- [x] 1.1 Add a Meson build at the repository root with targets for the layer, `zssd`, `zssctl` and tests, and confirm an empty build succeeds
- [x] 1.2 Create `src/layer/`, `src/daemon/`, `src/zssctl/` and `tests/` with placeholder sources wired into the build
- [x] 1.3 Fix the gmux port names and value comments in `include/zrn_selective_suspend.h` to match the kernel's `apple-gmux.h` (`0x728` DDC, `0x740` external)
- [x] 1.4 Add a note at the top of `SPEC.md` and `README.md` stating that the surprise-removal subsystems are deferred and pointing to this change for the current milestone

## 2. IPC protocol

- [x] 2.1 Write the message schema (register, migrate, park, resume, outcome, status, detach, attach, subscribe, event) as a document in `docs/`
- [x] 2.2 Implement a shared newline-delimited JSON encoder and decoder with unit tests
- [x] 2.3 Implement the `zssd` socket server at `/run/zss/zssd.sock` with `SO_PEERCRED` checks: registration from any user, detach and attach from root or the `zss` group
- [x] 2.4 Test that an unauthorised detach is rejected and changes nothing

## 3. Vulkan layer: interception and tracking

- [x] 3.1 Implement the layer as a Vulkan driver shim with its manifest and the `zss-run` launcher, loading the real drivers underneath, and confirm `vkcube` runs under it on each host Vulkan device (changed from an implicit layer; see design D2)
- [x] 3.2 Add handle virtualisation for instance, physical devices, surfaces, device and queues
- [x] 3.3 Track memory, buffers, images, image views and samplers with their creation parameters
- [x] 3.4 Track shader modules, render passes, framebuffers, descriptor layouts, pools and sets, pipeline layouts and graphics pipelines
- [x] 3.5 Track command pools and buffers, fences, semaphores, surface and swapchain
- [x] 3.6 Implement shadow mappings for `vkMapMemory` with copies at submit, flush, unmap and after waits
- [x] 3.7 Mark the application non-migratable, with the feature named, on first use of anything outside the tracked subset
- [x] 3.8 Resolve the device's PCI address from `VK_EXT_pci_bus_info`, with the `ZSS_BIND_PCI` override
- [x] 3.9 Register with `zssd` on device creation, and run normally when the daemon is unreachable

## 4. Vulkan layer: migration, parking, resume

- [x] 4.1 Implement quiescing: wait for submitted work and hold the application's entry points
- [x] 4.2 Implement readback of all tracked buffer and image contents into system memory
- [x] 4.3 Implement recreation of tracked objects on a target device in dependency order, with content upload
- [x] 4.4 Retire swapchains on migration: stand-in images on the target and out-of-date results until the application rebuilds its swapchain (changed from recreating the swapchain; see design D2)
- [x] 4.5 Destroy the source device, unload its driver library, and verify the process holds no open handle to the source GPU
- [x] 4.6 Implement the capability comparison between what the application enabled and used and what the target offers
- [x] 4.7 Implement parking (capture, release source, hold) and resume onto a named device
- [x] 4.8 Return `ZSSFailedResumeNoDRM` when a resume has no suitable device, leaving the application parked
- [x] 4.9 Report exactly one outcome per request, and leave the application on its source GPU when a migration fails
- [x] 4.10 Record each application's origin GPU and support migrating back to it

## 5. Test application and track A (host, no power changes)

- [x] 5.1 Write `zss-testapp`: a deterministic scene including an image accumulated across frames, with frame dumps to disk and an off-screen mode
- [x] 5.2 Write a frame comparison tool with exact and tolerance modes
- [x] 5.3 Test same-driver migration (llvmpipe to llvmpipe) with exact frame comparison
- [x] 5.4 Test NVIDIA to Intel and back with tolerance comparison
- [x] 5.5 Test NVIDIA to llvmpipe and back with tolerance comparison
- [x] 5.6 Test that an application enabling a feature absent on the target is parked and later resumes intact on its origin GPU
- [x] 5.7 Test that an application using an untracked feature is reported non-migratable with the feature named
- [x] 5.8 Run `vkcube` through a migrate and migrate-back cycle and confirm it keeps rendering

## 6. Daemon: orchestration

- [x] 6.1 Implement the managed-device model and the state machine (attached, detaching, powered-off, safe-to-remove, attaching) with events on every transition
- [x] 6.2 Implement client discovery from sysfs device nodes and `/proc/*/fd`, with classification into migratable, non-migratable and display server
- [x] 6.3 Implement the blocker check returning `ZSSDetachBlocked` with every blocking process and its reason, changing nothing
- [x] 6.4 Implement the detach sequence: request migrate or park for each client, wait for all outcomes, abort on any failure
- [x] 6.5 Freeze parked processes with the cgroup freezer and thaw them before resume
- [x] 6.6 Implement the unbind strategy: PCI remove of every function through sysfs, verify no driver remains bound, and rescan on attach
- [x] 6.7 Implement the suspend-in-place strategy through runtime PM for drivers that support it, refusing the power cut if the driver does not reach the suspended state (written, never executed: no such driver is available without root; first run is track C)
- [x] 6.8 Implement the safe-to-remove gate from the three conditions in the spec
- [x] 6.9 Implement the attach sequence: power on, resume or rescan per strategy, wait for the driver to be ready with a timeout, migrate origin applications back, resume parked applications
- [x] 6.10 Treat a device reappearing while detached as an attach request
- [x] 6.11 Implement `zssctl status`, `detach`, `attach`, `resume` and `monitor`

## 7. Power backends

- [x] 7.1 Define the backend interface and selection from configuration or platform detection, failing clearly when none applies
- [x] 7.2 Have each backend declare its release strategy, and refuse power-off while a driver is bound and not suspended
- [x] 7.3 Implement the `dry-run` backend, which skips PCI release and reports that power was not changed
- [x] 7.4 Implement the `pciehp-slot` backend using the slot's `power` attribute
- [x] 7.5 Implement the `apple-gmux` backend with classic-gmux detection, after checking its power sequence against the kernel's `apple-gmux` driver source (sequence checked against the source; written, never executed: needs root; first run is track C)
- [x] 7.6 Implement suspend in place for NVIDIA 470: unbind the audio function, save PCI configuration through sysfs, write `suspend` to `/proc/driver/nvidia/suspend`; on return wait for the link, restore configuration, write `resume`, rebind audio (written, never executed: needs root; first run is track C)
- [x] 7.7 Run a full dry-run detach and attach on the host with `zss-testapp` on the NVIDIA device

## 8. Track B (QEMU)

- [x] 8.1 Build the guest without an image: the host's kernel, a one-program initramfs with the `9p` and `bochs` modules, and the host's root file system read-only over 9p for Mesa's software Vulkan driver
- [x] 8.2 Write the launch script: q35 machine with native PCIe hot-plug, KVM, a `pcie-root-port` holding a hot-pluggable `bochs-display`, a QMP socket, and the build directory visible in the guest
- [x] 8.3 Manually run remove and add of the display card over QMP, record the exact sequence and the guest's kernel messages in `docs/`
- [x] 8.4 Write the harness that drives the guest and QMP and asserts guest state after every step
- [x] 8.5 Test detach of an idle card through to safe-to-remove, that attach fails while the slot is empty, then re-add and attach
- [x] 8.6 Test detach with `zss-testapp` bound to the card: the application migrates, the card is removed and returned, and the application is bound to it again with frames matching
- [x] 8.7 Test that a process holding the card without the layer produces `ZSSDetachBlocked`
- [x] 8.8 Test that a card re-added without a command triggers the attach sequence
- [x] 8.9 Test that a failed power-off never yields safe-to-remove

## 9. Track C (reference laptop, gated)

- [ ] 9.1 Stop and obtain the user's explicit go-ahead before any task in this group
- [ ] 9.2 Check that `zssd`'s first root run on the host detects the gmux and reads the dGPU's power state correctly, before anything is switched
- [ ] 9.3 Investigate whether Xorg tolerates the NVIDIA driver being suspended under it or must release the device first, and whether NVIDIA-rendered applications can present without the X server's NVIDIA driver; record findings in `docs/`
- [ ] 9.4 From a text console with no clients, run suspend in place: suspend the driver, power the dGPU off through `apple-gmux`, power it on, resume the driver, and confirm the GPU works; capture the kernel log and check it for errors
- [ ] 9.5 Separately, try the unbind strategy on the dGPU (remove, power-cycle, rescan) to learn whether cold probe works on this hardware; record the result without depending on it
- [ ] 9.6 Run a full detach and attach with `zss-testapp` on the dGPU and compare frames
- [ ] 9.7 Measure battery draw with the dGPU powered off against the baseline and record it
- [ ] 9.8 Record the outcome, limitations found and follow-up changes needed in `docs/`
