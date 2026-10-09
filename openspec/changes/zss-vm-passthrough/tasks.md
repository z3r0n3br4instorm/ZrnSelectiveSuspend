## 1. Measure the reference laptop (read-only, decides the rest)

- [x] 1.1 One boot with the IOMMU on (9 October 2026, through the added boot entry): 15 isolation groups. **The card is not alone:** group 2 holds both PEG root ports (`00:01.0`, `00:01.1`), the card (`01:00.0`, `01:00.1`) and the whole Thunderbolt controller (`05:00.0`, `06:0x.0` bridges and `07:00.0`, driver `thunderbolt`). The root ports have no ACS, so the kernel cannot tell the two apart. `zssctl lend --check` reports it
- [x] 1.2 In that boot: the desktop came up; X runs on the Intel GPU alone. Programs under ZSS_AirLock still draw from the NVIDIA card with it out of the X configuration (`glxgears` through Zink at 60 frames a second, `vkcube` 200 frames). `zssctl off`/`on` and suspend not yet tried in this boot
- [ ] 1.3 Record whether the card supports a bus reset and whether the gmux power cycle brings it back with no driver bound
- [ ] 1.4 Decide with the author whether to continue on this machine, from 1.1 to 1.3

## 2. Find out what the display server can do (Open Question 1)

- [ ] 2.1 From a text console with the desktop stopped: unbind `nvidia`, bind `vfio-pci`, hold the device open with a test program, release, power-cycle, rebind; record every step's result
- [x] 2.2 X started with the card not configured: the internal panel works. **But X still opens the card's display nodes at start-up**, through logind, and keeps them: `card0` (the firmware's framebuffer on the NVIDIA device, driver simpledrm) and `card2` (nvidia-drm). `AutoAddGPU off` stops X using them, not opening them. So the display server, logind and systemd all count as holding the card, and lending is refused. Taking the card out of the X configuration was not enough
- [ ] 2.3 Test whether X releases a GPU it uses only as an output provider with nothing connected, and takes it back; record exactly where it fails if it does
- [x] 2.4 Present 2.1 to 2.3 to the author and record the chosen way in design.md (decided by the author on 9 October 2026 before 2.1 to 2.3 were run: the card is out of the X configuration; see design.md)

## 3. Preflight

- [x] 3.1 One function in the daemon returning every obstacle for a card: IOMMU absent (and whether firmware offers one), isolation group shared, display server holds the card, passthrough driver unavailable for a function, no way to reset, programs that block (`lend_survey` in zssd.c with `lend_obstacles` in lend.c; one function for the check and the refusal)
- [x] 3.2 `zssctl lend --check PCI`: text and machine-readable output, exit status
- [x] 3.3 Tests: each obstacle produced in the guest or with the fake backend, and the clean case (in the guest: clean case, a program holding the card, the obstacle going away. Not produced in a test: IOMMU absent, a shared isolation group, a display server holding the card, the only-display case)

## 4. ZSS_Interceptor: release and take back

- [x] 4.1 Per-device "lent" state in sysfs; loss guard and wake handling suspended while it is set
- [x] 4.2 Release with power on: driver override, unbind host driver, bind the passthrough driver, for every function of the card; undo on failure (as changed in design.md decision 3: the module saves state and stops guarding (`lend`, `unlend`); the daemon does the override, unbind and bind through sysfs and undoes them on failure)
- [x] 4.3 Take back: refuse while a holder has the passthrough device open; power cycle (or bus reset); clear overrides; bind the host driver (the module refuses under a bound driver and power-cycles; the daemon checks for holders, unbinds the passthrough driver and binds the host's)
- [ ] 4.4 An interrupted hand-over is recognised on module load and reported (not in the module. The daemon recognises an interrupted hand-over at start-up from its record and the module's state, and undoes it; untested)
- [x] 4.5 Version bump, DKMS, installer text (0.3.0; built and installed through DKMS on the laptop, loaded at the next boot)

## 5. Daemon and `zssctl`

- [x] 5.1 States `lending`, `lent`, `reclaiming`; transitions from `attached` and `off`; undo paths (`lent` is a state; lending and reclaiming show as `detaching` and `attaching`)
- [x] 5.2 `lend`: detach sequence up to quiesce, then release through the module (or the user-space fallback); refusal uses the preflight function (only the part that moves applications is reused: the driver is not suspended. No user-space fallback: without the module a card cannot be lent, and the check says so)
- [x] 5.3 `reclaim`: holder check, take back, return applications (`--stay` as for `on`)
- [x] 5.4 A lent card is skipped by idle power-off, wake-on-touch, the loss policy, the power-source event and daemon shutdown (every one of them acts only on an attached or powered-off card; `off`, `on`, `detach`, `attach` are refused by name for a lent one)
- [x] 5.5 Start-up: read the lent state from the kernel; finish or undo an interrupted hand-over (tested for a restart under a running guest; the interrupted case is untested)
- [ ] 5.6 Status and D-Bus: state `lent`, holder process; programs started meanwhile recorded as wanting the card (status shows `lent` and the holder as class `guest`, tested. Not tested: a program started while the card is lent being recorded as wanting it. There is no D-Bus interface in ZSS; the spec no longer mentions one)

## 6. Tests without hardware

- [x] 6.1 Guest test bed boots with an emulated IOMMU; isolation groups present (`tests/track_v.py`: `-device intel-iommu`, `intel_iommu=on`, the vfio modules, and a second display device for the host to keep)
- [x] 6.2 Track: lend, hold the passthrough device from a test program, status shows holder, reclaim refused; release, reclaim, host driver back, an application returned
- [ ] 6.3 Track: failure injected at each step of lending and of reclaiming; state and applications as specified (done: binding fails after the host driver let go (undone); the reset fails on reclaim (stays lent, next attempt works); reclaim under a bound driver. Not done: a failure at every single step)
- [ ] 6.4 Track: daemon restarted while lent; module reloaded refused while lent (daemon restart while lent: done. Unmanage refused while lent: done. Module unload while lent: not tested)
- [ ] 6.5 A layered application moved away by `lend` and returned by `reclaim`, exact frames

## 7. The reference laptop (after 2.4)

- [x] 7.1 The display server is off the card: the card is out of the X configuration, and a udev rule puts its display nodes on a seat of their own (X and the login manager open every display node of their seat). Seen working after a reboot on 9 October 2026
- [x] 7.2 On the laptop, 9 October 2026 (`docs/lend-first-run.log`): lent with `--with-group` in 2.9 s; a QEMU guest (CachyOS live ISO, OVMF, `-display gtk`) took both functions and ran; the author reports passthrough worked in the guest; after the guest shut down `zssctl reclaim` took 11.2 s, with `nvidia`, `snd_hda_intel` and `thunderbolt` back, the NVIDIA driver answering, programs returned and X answering throughout. Needed on this machine: `vfio_iommu_type1 allow_unsafe_interrupts=Y`, because the firmware's interrupt-remapping table is broken ("ioapic 2 has no mapping iommu"); set for the run and set back after. No hardware script yet: it was run by hand
- [ ] 7.3 Guest behaviour with the "Mac Edition" card: it has no PCI option ROM (QEMU: "Device ROM size is zero"); the guest's display was QEMU's emulated card. What the guest's driver logged was not collected
- [ ] 7.4 Charger unplugged while lent: behaviour as decided in Open Question 3
- [x] 7.5 `--with-group`: devices sharing the card's isolation group go with it and come back with it. Worked on the laptop with the Thunderbolt controller; no test in the guest, where no device shares the card's group
- [x] 7.6 Seat rule so that X and the login manager do not open the card's display nodes: works on the laptop
- [x] 7.7 Found on the first runs, and built: before any unbind the modules stacked on the card's driver are unloaded (after their users have drained) and the driver must have no user left, else the lend is refused and undone; the unbind has a time limit. The first run without this left the NVIDIA driver waiting in its unbind for good, with the daemon stuck in it
- [x] 7.8 Found on the first runs, and built: the daemon marks the time it is moving programs (`<runtime dir>/moving`), and the author's power manager, which stops background programs, leaves them running for that long
- [ ] 7.9 Programs do not register again with a daemon that has been restarted; until they are restarted they block a lend and are frozen, not moved, for a power-off
- [x] 7.10 Found after the card left X: frames drawn on the NVIDIA card reached the Intel-driven screen out of order now and then (typing in a Chromium window). ZSS_AirLock now presents such frames itself on the screen's GPU (`src/layer/present.c`): the window's real swapchain is on a small device of the layer's on Intel, each frame is read back from NVIDIA, waited for, copied in and presented in order. vkcube 300 frames in 5.2 s (5.1 s direct), glxgears 60 fps, a VS Code window correct while typing and across a move; `ZSS_PRESENT=direct` turns it off. Whether the flicker is gone has to be judged by eye

## 8. Documents

- [x] 8.1 `docs/vm-handover.md`: requirements, how to enable the IOMMU for one boot and permanently (by the user), the commands, a QEMU and a libvirt example, what does not work
- [x] 8.2 `README.md` and `SPEC.md`: the lent state, commands, roadmap
- [x] 8.3 `docs/protocol.md` and `docs/kernel-module.md`: new requests, states and sysfs entries
