## 1. Measure the reference laptop (read-only, decides the rest)

- [ ] 1.1 One boot with `intel_iommu=on` typed at the boot menu (nothing saved): record the isolation groups, the group of `01:00.0` and `01:00.1`, and anything else in it; keep the output in `docs/vm-handover.md`
- [ ] 1.2 In that boot, check the desktop works as before (X, external display, `zssctl off`/`on`, suspend) and record any difference
- [ ] 1.3 Record whether the card supports a bus reset and whether the gmux power cycle brings it back with no driver bound
- [ ] 1.4 Decide with the author whether to continue on this machine, from 1.1 to 1.3

## 2. Find out what the display server can do (Open Question 1)

- [ ] 2.1 From a text console with the desktop stopped: unbind `nvidia`, bind `vfio-pci`, hold the device open with a test program, release, power-cycle, rebind; record every step's result
- [ ] 2.2 Start X with the card not configured: confirm the internal panel works and record what is lost
- [ ] 2.3 Test whether X releases a GPU it uses only as an output provider with nothing connected, and takes it back; record exactly where it fails if it does
- [ ] 2.4 Present 2.1 to 2.3 to the author and record the chosen way in design.md

## 3. Preflight

- [ ] 3.1 One function in the daemon returning every obstacle for a card: IOMMU absent (and whether firmware offers one), isolation group shared, display server holds the card, passthrough driver unavailable for a function, no way to reset, programs that block
- [ ] 3.2 `zssctl lend --check PCI`: text and machine-readable output, exit status
- [ ] 3.3 Tests: each obstacle produced in the guest or with the fake backend, and the clean case

## 4. ZSS_Interceptor: release and take back

- [ ] 4.1 Per-device "lent" state in sysfs; loss guard and wake handling suspended while it is set
- [ ] 4.2 Release with power on: driver override, unbind host driver, bind the passthrough driver, for every function of the card; undo on failure
- [ ] 4.3 Take back: refuse while a holder has the passthrough device open; power cycle (or bus reset); clear overrides; bind the host driver
- [ ] 4.4 An interrupted hand-over is recognised on module load and reported
- [ ] 4.5 Version bump, DKMS, installer text

## 5. Daemon and `zssctl`

- [ ] 5.1 States `lending`, `lent`, `reclaiming`; transitions from `attached` and `off`; undo paths
- [ ] 5.2 `lend`: detach sequence up to quiesce, then release through the module (or the user-space fallback); refusal uses the preflight function
- [ ] 5.3 `reclaim`: holder check, take back, return applications (`--stay` as for `on`)
- [ ] 5.4 A lent card is skipped by idle power-off, wake-on-touch, the loss policy, the power-source event and daemon shutdown
- [ ] 5.5 Start-up: read the lent state from the kernel; finish or undo an interrupted hand-over
- [ ] 5.6 Status and D-Bus: state `lent`, holder process; programs started meanwhile recorded as wanting the card

## 6. Tests without hardware

- [ ] 6.1 Guest test bed boots with an emulated IOMMU; isolation groups present
- [ ] 6.2 Track: lend, hold the passthrough device from a test program, status shows holder, reclaim refused; release, reclaim, host driver back, an application returned
- [ ] 6.3 Track: failure injected at each step of lending and of reclaiming; state and applications as specified
- [ ] 6.4 Track: daemon restarted while lent; module reloaded refused while lent
- [ ] 6.5 A layered application moved away by `lend` and returned by `reclaim`, exact frames

## 7. The reference laptop (after 2.4)

- [ ] 7.1 Implement the chosen way for the display server to be off the card
- [ ] 7.2 Hardware script: lend, start a VM with the card, stop it, reclaim; desktop and external display afterwards
- [ ] 7.3 Guest behaviour with the "Mac Edition" card: UEFI guest, ROM file, what the guest driver says; documented, not solved
- [ ] 7.4 Charger unplugged while lent: behaviour as decided in Open Question 3

## 8. Documents

- [ ] 8.1 `docs/vm-handover.md`: requirements, how to enable the IOMMU for one boot and permanently (by the user), the commands, a QEMU and a libvirt example, what does not work
- [ ] 8.2 `README.md` and `SPEC.md`: the lent state, commands, roadmap
- [ ] 8.3 `docs/protocol.md` and `docs/kernel-module.md`: new requests, states and sysfs entries
