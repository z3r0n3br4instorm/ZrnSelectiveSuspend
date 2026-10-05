# Track B: how the QEMU guest behaves

Recorded from manual runs on QEMU 11.1.1 with the host's kernel (7.2.2) as the
guest kernel. `tests/track_b.py` depends on each point below.

## The guest

There is no disk image. The guest boots the host's kernel with an initramfs
that holds one static program (`tests/qemu/init.c`) and five modules
(`netfs`, `9pnet`, `9pnet_virtio`, `9p`, `bochs`). That program mounts the
host's root file system read-only over 9p, mounts fresh `/dev`, `/proc`,
`/sys`, `/tmp`, `/run` and a cgroup2 hierarchy on top, mounts a second,
writable 9p share at `/mnt` for results, and runs the test script as root.
Boot to script takes about one second.

Because the guest has its own `/tmp`, nothing under the host's `/tmp` is
visible to it. Files the guest must see go in the work share.

## The hardware

```
-machine q35  -global ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off
-device pcie-root-port,id=rp1,chassis=1,slot=1
-device bochs-display,id=gpu1,bus=rp1
```

The card appears as `0000:01:00.0` (`1234:1111`), bound to `bochs-drm`, with
`/dev/dri/card0`.

**The `-global` switch matters.** QEMU's default for q35 is ACPI hot-plug.
With it the slot is named `0`, `pciehp` is not involved, and writing `0` to
the slot's `power` attribute is an ACPI eject. Turning it off gives native
PCIe hot-plug: the slot is named `1` and the kernel logs

```
pciehp: Slot #1 AttnBtn+ PwrCtrl+ MRL- AttnInd+ PwrInd+ HotPlug+ Surprise+ Interlock+
```

which is the kind of slot the `pciehp-slot` backend is written for.

## Sequence observed

| Step | Result |
| :--- | :--- |
| Write `1` to the device's `remove` | Device, driver and `/dev/dri/card0` disappear at once. The slot still reads `power = 1`. |
| Write `0` to the slot's `power` | Returns after about one second. The slot reads `power = 0`. |
| QMP `device_del gpu1` after that | `DeviceNotFound`. QEMU has already dropped the card: it treats the guest powering the slot off as the end of an unplug. |
| Write `1` to the slot's `power` while it is empty | The write fails. There is no card to power. |
| QMP `device_add bochs-display,id=gpu1,bus=rp1` | `pciehp` logs "Button press", "Card present", "Link Up"; the card is enumerated and `bochs-drm` binds within about a second. The slot reads `power = 1`. |

## Consequences for the tests

- In this guest, cutting slot power also ejects the card. A real slot keeps
  the card until someone pulls it. The harness therefore accepts
  `DeviceNotFound` as "already gone" when it asks for an unplug, and a test
  cannot power the slot back on without the host re-adding the card first.
- A card that returns powers its own slot, so `zssd` sees it on its next
  one-second poll and attaches it. The test of an explicit `attach` runs the
  daemon with `--no-auto-attach`.
- A power-off failure is simulated by bind-mounting a read-only file that
  reads `1` over the slot's `power` attribute.
- The guest has no accelerated GPU. The test application renders on the
  software renderer and sets `ZSS_BIND_PCI` to the card's address, which makes
  the layer hold the card's device node open exactly as a driver would. The
  guest tests therefore prove sequencing, kernel behaviour and freezing, not
  cross-GPU rendering; that is what the host tests are for.
