## 1. Skeleton

- [x] 1.1 Create `kmod/` with `Kbuild`, `Makefile` and `dkms.conf`; build an empty `zss.ko` for the running kernel with no warnings
- [x] 1.2 Add `/sys/kernel/zss/` with `version`, `manage` and `unmanage`; a managed device gets its directory and read-only files
- [x] 1.3 Find and hold all functions of the slot; refuse addresses that are not PCI devices
- [x] 1.4 Send a uevent on every state change
- [x] 1.5 Load the module in the QEMU guest and test manage, unmanage, the files and the events

## 2. Power sequencing

- [x] 2.1 Backend interface and the `test` backend, with switches to fail power-off, leave the device answering, and not come back
- [x] 2.2 Off and on with `quiesce=none`: save state, mark disconnected, backend, wait for the device, restore state
- [x] 2.3 `quiesce=pm`: run the bus's system-sleep and wake callbacks for each function, in order
- [x] 2.4 Unwind on every failure before the power cut; `failed` state and retry after
- [x] 2.5 Power every managed device on at module exit
- [x] 2.6 Test in the QEMU guest on the bochs card: full cycle, configuration space byte for byte, the driver usable afterwards, and each failure path

## 3. Loss guard

- [x] 3.1 Delayed work that reads the vendor ID of a device that should be on
- [x] 3.2 Mark functions disconnected, set `lost`, send the event; clear on a later `on`
- [x] 3.3 Test in the guest: silence detected within 300 ms, no report while off, return

## 3a. Protections added after review

- [x] 3a.1 Switch off bus mastering before power is cut, and restore it afterwards
- [x] 3a.2 Report whether the device is behind an IOMMU (`iommu` file)
- [x] 3a.3 Refuse `quiesce=pm` for a device with no driver bound: nothing would initialise it again
- [x] 3a.4 Report a driver that does not resume as `failed`, with the reason
- [x] 3a.5 Tell a driver that implements PCI error recovery that its device is gone, and that it is back; report `needs_rebind` otherwise, and have the daemon rebind
- [x] 3a.6 Show the IOMMU state in `zssctl status` and the installer's report

## 3b. Freezing a driver that cannot be told

- [x] 3b.1 Driver patch revision 4: freeze (gate only, nothing asked of the hardware), thaw (the ordinary resume), release; a proc file and exported hooks; builds for the running kernel
- [x] 3b.2 Module: freeze the driver the moment the guard sees the device go silent, before marking it; confirm a silent look after 10 ms instead of 100
- [x] 3b.3 Module: on taking a lost device back, try the backend's power-on, restore the configuration, thaw the driver; a failed thaw leaves it frozen and the device lost
- [x] 3b.4 Daemon: `zssctl on` for a lost device the module manages; `driver=frozen` in status; never rebind the NVIDIA driver
- [x] 3b.5 Test the module's side in the guest with a stand-in for the driver's hooks
- [ ] 3b.6 On the laptop, on a fresh boot: cut the rail under a running application, confirm the driver is frozen and never reports the GPU lost, `zssctl on`, confirm the driver resumes (run once: the driver reported the GPU lost before the freeze landed; the thaw then reported success; the device went silent again and the machine went down. See `docs/track-c.md`)
- [x] 3b.7 After that run: the layer checks a failing device against the bus before passing an error on; the daemon does not wait for a dead application or rescan the bus for a device the module took back; the module allows five seconds of silence after a return
- [x] 3b.8 Repeat 3b.6 with those changes, and once with the GPU idle at the moment of the cut (idle: full recovery with no reboot. Rendering: no crash, no evacuation, and the thaw hung the machine)
- [x] 3b.9 Daemon: refuse to bring back a lost device with a frozen driver while a program other than the display server and the listed services holds it
- [ ] 3b.10 Decide what a frozen driver does with callers that are not the display server: sleeping keeps them from being evacuated. Then try closing the programs before the thaw

## 4. Real backends

- [x] 4.1 `gmux` backend: probe for the classic gmux, power off, power on (runs on the laptop: see 7.2)
- [x] 4.2 `acpi` backend: probe for firmware power resources, D3cold and D0 through the firmware node (written and compiled; it has never run on any firmware)
- [ ] 4.3 Try the `acpi` backend in the guest against an injected firmware table, or record that this could not be done

## 5. Daemon

- [x] 5.1 `zss-kmod` backend: manage, power, state; choose `external` for the NVIDIA driver and `pm` otherwise
- [x] 5.2 Take loss and state from the module's events for devices it manages
- [x] 5.3 Fall back to the existing backends when the module is absent or refuses the device
- [ ] 5.4 Run the existing off/on and loss scenarios in the guest with the module loaded (two new scenarios run the daemon on the module: detach, attach, a failure, a loss and the return; the full existing set has not been repeated on it)

## 6. Packaging

- [x] 6.1 Installer step: register with DKMS and build, only after an explicit yes (run for real on the laptop)
- [x] 6.2 Load the module from `zssd.service`, never from the initial ramdisk
- [x] 6.3 Uninstaller: unload, remove from DKMS (written; tested for files in a staging root, never run for real)
- [x] 6.4 Test the installer and uninstaller steps in a staging root

## 7. Reference laptop (each step needs the user's go-ahead)

- [x] 7.1 Build and load the module; confirm nothing changed
- [x] 7.2 Manage the GPU with `backend=gmux quiesce=external` and cycle it (done through the daemon rather than by hand, twice; see `docs/track-c.md`)
- [ ] 7.3 Let the daemon use the module; repeat the off, close-an-application and on checks (the daemon uses it and off/on works; the close-an-application, hide and serve checks have not been repeated on it)
- [ ] 7.4 Unload the module with the GPU off; confirm it comes back

## 8. Documents

- [x] 8.1 README: what the module adds, what works without it, and the updated "what is universal" table
- [x] 8.2 Record the laptop results in `docs/track-c.md`
