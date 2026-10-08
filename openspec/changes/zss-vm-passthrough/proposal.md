## Why

ZSS can already take every application off a GPU and power the card down while the system runs. The same card, once nothing on the host uses it, could be given to a virtual machine and taken back afterwards, without logging out or rebooting. Today that is not possible: ZSS powers a card off but never lets go of it (`removal=not supported`), and nothing checks whether the machine can isolate the device for a guest.

## What Changes

- A third state for a managed GPU beside "attached" and "off": **lent**. The host's driver is unbound from every function of the card, the card is left powered and bound to the kernel's passthrough driver (`vfio-pci`), and ZSS stops watching and waking it.
- `zssctl lend <pci>` and `zssctl reclaim <pci>`. Lending moves or freezes applications exactly as `off` does today, then hands the card over. Reclaiming power-cycles the card, gives it back to the host's driver, and returns applications as `on` does.
- `zssctl lend --check <pci>`: says, without changing anything, whether this machine can lend the card and, if not, each reason and what would remove it (no IOMMU, the card shares an isolation group with something else, the display server holds the card, a function of the card has no passthrough driver).
- ZSS_Interceptor learns to release a device with its power on, and to refuse to take back a device a guest still holds.
- ZSS does **not** start, stop or configure virtual machines, and does **not** edit the bootloader. It produces a card that a VM manager can take, and takes it back.
- Status output and the D-Bus interface show the new state and who holds a lent card.

## Capabilities

### New Capabilities

- `vm-handover`: lending a managed GPU to a virtual machine and reclaiming it: the states, the order of operations, what happens to host applications, and how failures are undone.
- `vm-handover-preflight`: finding out, read-only, whether a given card on a given machine can be lent, and reporting every obstacle with its remedy.

### Modified Capabilities

<!-- None. No capability has been archived into openspec/specs yet; the power sequencing and loss guard this builds on are specified in the changes zss-kernel-shim and zss-system-integration and their requirements do not change. -->

## Impact

- **Daemon** (`src/daemon/`): a new device state and two operations; holder detection gains "a guest holds it"; refusal reasons.
- **ZSS_Interceptor** (`kmod/`): release with power on, driver override and rebinding for all functions of a card, loss guard and wake-on-touch suspended while lent, reset by power cycle on reclaim. A version bump and a module reload on install.
- **`zssctl`**: `lend`, `reclaim`, `lend --check`; status shows `lent`.
- **ZSS_AirLock**: none expected. Applications are moved off the card by the existing mechanism.
- **System requirements, not changed by ZSS**: an IOMMU enabled by a kernel boot parameter (on the reference laptop, `intel_iommu=on`), which the user sets; ZSS reports its absence and never edits the bootloader's configuration.
- **The display server**: on the reference laptop X holds the NVIDIA card for external displays, and earlier work showed X does not survive losing a card it has open. Lending therefore depends on X not holding the card, by a means still to be chosen (see design.md, Open Questions).
- **Tests**: a new track in the QEMU guest (which can emulate an IOMMU, so the whole hand-over can be tested without hardware), plus a hardware script for the laptop.
- **Docs**: `README.md`, `SPEC.md` (states and roadmap), a new `docs/vm-handover.md`.
