## Context

See proposal.md for the motivation. What exists and constrains the design:

- A managed GPU has two resting states, "attached" and "off". Going to "off" already does most of a hand-over: applications under ZSS_AirLock are moved to another GPU, others are frozen or block the operation, the driver is quiesced, and ZSS_Interceptor saves the PCI state and cuts power. What it never does is unbind the driver; the daemon reports `removal=not supported`.
- ZSS_Interceptor guards a managed device: it polls for loss and tells the driver if the card stops answering. It also holds the device's power backend (on the reference laptop, the Apple gmux rail), which gives a true cold reset.
- Measured on the reference laptop (MacBookPro9,1, 9 October 2026): the firmware provides a DMAR table and the CPU has VT-x, but the kernel runs with no IOMMU (no isolation groups). The card has two functions, `01:00.0` (VGA, driver `nvidia`) and `01:00.1` (HDMI audio). QEMU, libvirt, OVMF and `vfio-pci` are installed.
- The X server holds the NVIDIA card (it drives the external displays), and every earlier experiment showed X does not survive losing a card it has open: a lost card needed a log-out to come back.
- Standing constraints from the author: the bootloader's configuration is not touched; external displays must keep working in normal use; no solution that is a workaround for something not understood.

## Goals / Non-Goals

**Goals:**
- A card that nothing on the host uses can be handed to a VM and taken back, with host applications moved away and returned as for off and on.
- The user is told beforehand, completely, why a hand-over would fail.
- Everything except the last step on real hardware is testable in the QEMU guest.

**Non-Goals:**
- Starting or configuring VMs, or integrating with libvirt beyond documenting a hook.
- Showing the guest's picture on the host (a capture card or Looking Glass is the user's business). On the laptop the guest's output appears on an external monitor only.
- Making a particular guest driver accept the card (the NVIDIA "code 43" class of problem); it is investigated and documented, not solved here.
- Enabling the IOMMU for the user.
- Lending the GPU the host's display server is running on, when it is the only one.

## Decisions

### 1. A third resting state, "lent", owned by the daemon
The daemon's state machine gains `lent` with transitions `attached → lending → lent` and `lent → reclaiming → attached`, plus `off → lending`. Lending reuses the detach sequence up to and including quiescing the driver, then diverges: instead of saving state and cutting power, the driver is unbound.

*Alternative considered:* treating "lent" as "off" plus a manual unbind by the user. Rejected: the loss guard and wake-on-touch would then act on a card a guest owns, and the daemon would power it off or rebind the host driver on shutdown.

### 2. The unit of hand-over is the isolation group restricted to the card's functions
All functions of the card (same domain, bus and device) are handed over together. If the isolation group contains anything else, lending is refused; ZSS does not unbind other devices' drivers.

*Alternative considered:* handing over the whole group whatever it contains. Rejected: on a laptop the group can include the root port's other children, and silently unbinding them is the kind of side effect ZSS exists to avoid.

### 3. Binding is done by ZSS_Interceptor, not by writing sysfs from the daemon
The module already owns the device across power transitions. It gains a "release with power on" operation: stop the loss guard and wake handling for the device, set the driver override for each function, unbind the host driver, bind the passthrough driver, and mark the device as lent in its own sysfs entry so that a restarted daemon can read the truth from the kernel. The user-space fallback path (no module) does the same through `driver_override` and `drivers_probe`, with the same checks.

*Alternative considered:* daemon-only, through sysfs. Kept as the fallback, not the main path, because between unbind and bind the module's guard would otherwise see a device with no driver and treat it as lost.

### 4. Reclaim resets by power cycle
After a guest has used the card its state is unknown. On reclaim ZSS_Interceptor cuts and restores power through the backend (the same operation as off then on) before the host driver is bound. Where the backend cannot cut power, a bus reset is used if the device supports one; if neither is available the preflight check says so and lending is refused, because the card could not be given back in a known state.

*Alternative considered:* rebinding without a reset. Rejected: an NVIDIA card left initialised by a guest driver is exactly the unknown state that produced the Xid 79 hangs seen earlier.

### 5. Holder detection for a lent card
A guest holds the card through an open handle on the passthrough device. The daemon finds holders the way it finds them for a host driver's device nodes, extended to the passthrough device's group and device files, and reports the process (normally `qemu-system-x86_64`). Reclaim is refused while there is one; ZSS never kills a VM.

### 6. Preflight is the same code as the refusal
`zssctl lend --check` and the refusal inside `lend` call one function that returns the full list of obstacles. This keeps the check honest: it cannot say "fine" and then have the operation fail for a reason it did not look at.

### 7. Tested in the QEMU guest with an emulated IOMMU
The existing guest test bed (track K) boots a kernel with ZSS_Interceptor and an emulated GPU. QEMU can emulate an Intel IOMMU, so the guest can exercise the whole sequence: isolation groups appear, the device is unbound and bound to the passthrough driver, a nested "guest" (or a small user-space program holding the passthrough device open) plays the VM, and reclaim brings the host driver back. The hardware script for the laptop covers what the guest cannot: the gmux power cycle and the real NVIDIA driver.

## Risks / Trade-offs

- **The display server holds the card** → lending is impossible in the normal desktop session until the Open Question below is settled. Mitigation: the preflight says so plainly; the first milestone is the mechanism, tested from a text console and in the guest.
- **The isolation group on the laptop may contain more than the card** → unknown until one boot with the IOMMU on. Mitigation: it is the first task; if the group is shared, the change stops there for this machine and the mechanism remains for others.
- **Apple's firmware and the card's ROM** → the guest may fail to initialise a "Mac Edition" card. Mitigation: investigated and documented (ROM file passed to the VM, UEFI guest); not a ZSS defect.
- **The IOMMU changes host behaviour** (DMA remapping costs a little, and some old firmware has broken tables) → Mitigation: nothing in ZSS requires it unless lending is used; the docs say how to try it for one boot from the boot menu before making it permanent, which the user does.
- **A crash between unbind and bind leaves a card with no driver** → Mitigation: the module records "lending" before it starts and the daemon, on start-up, finishes or undoes an interrupted hand-over; the guard is told first.
- **Module reload needed to install** → the new ZSS_Interceptor version replaces a module that is in use under a running desktop. Mitigation: installed at a boundary the user chooses (log-out or reboot), as before.

## Migration Plan

Additive. Existing states, commands and configuration are unchanged. A daemon without the new module keeps working and reports lending as unavailable. Rollback is reinstalling the previous module and daemon; no stored state changes format.

## Open Questions

1. **How does the display server come off the card on the reference laptop?** Lending needs X not to hold the NVIDIA card, but X holds it so that external displays work. The candidates, none chosen:
   - a. Lend only from a session started without the card in X (no external displays in that session); the session is chosen at log-in.
   - b. Lend only from a text console, with the desktop session stopped.
   - c. Find out whether X can be made to release and re-take a GPU it uses only for outputs with nothing connected (provider hot-unplug), which would also answer the parked "X does not survive a lost card" problem.
   This is the author's decision. The task list measures first (tasks 1.x and 2.x) and builds the mechanism independently of the answer; the session integration (task group 7) waits for it.
2. **Should reclaim be automatic when the VM exits?** A libvirt hook could call `zssctl reclaim`. Left out of scope until the manual path works; noted in the docs.
3. **The power-source integration:** when the charger is unplugged today the card is switched off. A lent card belongs to a guest. Proposed default: leave a lent card alone and say so in the dialog; to be confirmed.
