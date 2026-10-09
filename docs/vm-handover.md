# Lending a GPU to a virtual machine

A GPU that ZSS manages can be handed to a virtual machine while the host keeps
running, and taken back afterwards. ZSS does the host's side: it moves
programs off the card, gives the card to the kernel's passthrough driver
(`vfio-pci`), leaves it alone while a guest has it, and on the way back resets
it by a power cycle and returns it to the host's driver. ZSS does not start or
configure virtual machines.

Status: tested in a QEMU guest with an emulated IOMMU (`tests/track_v.py`), and run on the
reference laptop on 9 October 2026: the card was lent and reclaimed with three programs
moved away and back (`lend-first-run.log`), and lent to a QEMU guest (CachyOS live ISO) that used
the card and was reclaimed after it shut down.

## What a machine needs

| Requirement | How to check |
| :--- | :--- |
| An IOMMU, enabled | `ls /sys/kernel/iommu_groups` is not empty |
| The card alone in its isolation group (bridges above it do not count) | `zssctl lend --check PCI` |
| The display server does not have the card open | `zssctl status` lists no display server for it |
| ZSS_Interceptor managing the card (it resets the card on its return) | `zssctl status` shows `backend=zss-kmod` |
| The host has another display device, if the card drives a monitor | `zssctl lend --check PCI` |

`zssctl lend --check PCI` reports all of these at once, each with what would
remove it, and changes nothing. `--json` gives the same as one document.

## Commands

```sh
zssctl lend --check 0000:01:00.0     # what stands in the way; changes nothing
zssctl lend 0000:01:00.0             # move programs away, hand the card over
zssctl status                        # state=lent, and the process holding the card
zssctl reclaim 0000:01:00.0          # power-cycle it, give it back to the host's driver, return programs
zssctl reclaim 0000:01:00.0 --stay   # the same, leaving programs where they are
```

`--with-group` (on `lend` and on `lend --check`): devices that share the card's
isolation group, other than bridges, are handed over with it. They have no
host driver while the card is lent and get it back on reclaim. On the reference
laptop that is the Thunderbolt controller (`07:00.0`): no Thunderbolt on the
host while the card is lent. Without the option such a device is an obstacle.

While a card is lent, `off`, `on`, `detach` and `attach` are refused for it,
and it is not watched for loss, woken or powered off. Reclaiming is refused
while a virtual machine holds the card; ZSS never stops a virtual machine.

A program that was not started under ZSS_AirLock and has the card open blocks
lending (it is not frozen, as it is for a power-off: a frozen program still
has the card open, and the host's driver could not let go). A card that is off
is switched on first: `zssctl on PCI --stay`, then `zssctl lend PCI`.

## A virtual machine that takes the card

Once the card is lent, any passthrough configuration works. With QEMU, for a
card whose functions are `01:00.0` and `01:00.1`:

```sh
qemu-system-x86_64 -machine q35,accel=kvm -cpu host -m 4G \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2-ovmf/x64/OVMF_CODE.4m.fd \
  -device vfio-pci,host=01:00.0,multifunction=on -device vfio-pci,host=01:00.1 \
  ...
```

With libvirt, add the two functions as host devices with `managed='no'`: ZSS
has already bound them, and libvirt must not rebind them itself.

## The reference laptop (MacBookPro9,1)

Set up on 9 October 2026:

- **IOMMU.** The boot menu (`/boot/grub/grub.cfg`, hand-written for the gmux; never
  regenerate it) has a sixth entry, "Arch -- Intel panel, nvidia POWERED, IOMMU on
  (for VM passthrough)". It starts the same kernel image with the usual command line
  plus `intel_iommu=on iommu=pt`. It is not the default; choose it at the menu. The
  previous file is `/boot/grub/grub.cfg.before-iommu`. If `/etc/kernel/cmdline`
  changes, the copy in that entry has to be changed by hand.
- **X server.** `/etc/X11/xorg.conf.d/10-prime-intel-primary.conf` no longer lists the
  NVIDIA card (it was an inactive render-offload device). The previous file is beside
  it as `10-prime-intel-primary.conf.with-nvidia`. Known costs: no PRIME offload
  through X, and nothing on outputs wired to the NVIDIA card. Whether programs under
  ZSS_AirLock can still put windows on the X screen from the NVIDIA card without it
  being in X is **not known** and is the first thing to check after the reboot.

- **Seat rule.** `/etc/udev/rules.d/72-zss-lend-seat.rules` puts the card's display
  nodes on a seat of their own, because X and the login manager open every display
  node of their seat at start-up, even for a card that is not in the X configuration.
  Delete the file and reboot to undo.
- **Found on the first boot with the IOMMU on:** the card shares isolation group 2 with
  the Thunderbolt controller, so lending needs `--with-group`.

First run, in order:

1. Boot the "IOMMU on" entry. `cat /proc/cmdline` shows `intel_iommu=on`; `ls /sys/kernel/iommu_groups | wc -l` is not zero.
2. Check the desktop: `zssctl status` (no Xorg under the card), a program with `zss-run`, `zssctl off` and `on`.
3. `zssctl lend --check 0000:01:00.0 --with-group` and read every line.
4. `zssctl lend 0000:01:00.0 --with-group`, then `zssctl status`, then `lspci -nnk -s 01:00` (both functions on `vfio-pci`).
5. `zssctl reclaim 0000:01:00.0`, then `lspci -nnk -s 01:00` (`nvidia` and `snd_hda_intel` back), then a program with `zss-run`.
6. Only then a virtual machine.

To undo the setup: choose any other boot entry (or delete the sixth), and copy the
`.with-nvidia` file back over the X configuration, then log in again.

## What lending does besides binding

- **Kernel modules stacked on the card's driver are unloaded**, and loaded again on
  reclaim (for NVIDIA: `nvidia_uvm`, `nvidia_drm`, `nvidia_modeset`). They keep the card
  open for themselves, and a driver asked to let go of a card in use does not refuse: it
  waits, for ever. The daemon first waits up to ten seconds for their users to go (the
  picture a program last showed is a buffer shared with the display server), then requires
  the driver to have no user left. Otherwise the lend is refused and undone.
- **The unbind has a time limit** (15 s), so that a driver that will not let go cannot take
  the daemon with it.
- **`<runtime dir>/moving` exists while the daemon is moving programs.** A power manager
  that stops background programs should leave them running while it does; a stopped
  program cannot answer the request to move.

## On the reference laptop, also needed

- **QEMU's window front-end.** Arch's `qemu-base` has no display; `qemu-ui-gtk` gives `-display gtk`.
- **Interrupts without remapping.** The MacBookPro9,1 firmware's table is broken ("ioapic 2 has
  no mapping iommu"), so the kernel cannot remap interrupts and the passthrough driver refuses
  a group until told `allow_unsafe_interrupts`. For one run:
  `echo Y | sudo tee /sys/module/vfio_iommu_type1/parameters/allow_unsafe_interrupts`
  (and `N` after). It lets a guest's device raise interrupts on the host that the IOMMU does
  not check: acceptable for a guest you trust, not for one you do not.
- **No option ROM.** The "Mac Edition" card has none; QEMU warns, and the guest's display is
  QEMU's emulated card.

## What is not done

- Lending a card straight from "off".
- Returning a card automatically when the virtual machine exits (a libvirt hook could call `zssctl reclaim`).
- The charger rule: the power-source helper asks for `off`, which is refused for a lent card, so the card simply stays lent.
- Anything inside the guest: the "Mac Edition" card's ROM, and what a guest's driver makes of it, are untested.
