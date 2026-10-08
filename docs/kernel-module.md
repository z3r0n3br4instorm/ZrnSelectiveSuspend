# The `zss` kernel module

`zss.ko` is the part of ZSS that touches the hardware. It is optional: the
daemon uses it when it is loaded and falls back to its own, narrower power
backends when it is not. Source: [`kmod/zss.c`](../kmod/zss.c). Design and
tests: [`openspec/changes/zss-kernel-shim/`](../openspec/changes/zss-kernel-shim/).

## What it does

For a PCI GPU and every other function in its slot (its HDMI audio, usually):

1. **Quiesce.** Runs the driver's own system-sleep callbacks for that device
   alone (`quiesce=pm`), or leaves the driver to user space (`external`, used
   for the proprietary NVIDIA driver), or does nothing (`none`).
2. **Save state** with the PCI core's functions, and stop the device from
   starting transfers on the bus.
3. **Mark the device disconnected**, so the kernel's own accessors and
   well-behaved drivers leave the hardware alone.
4. **Cut power** through a backend, and check that the device really stopped
   answering.

Powering on is the reverse, with a bounded wait for the device to answer. The
driver's wake callbacks are what re-run the card's video BIOS initialisation;
the module does not execute a video BIOS itself, and for that reason refuses
`quiesce=pm` on a device with no driver bound.

While a device is supposed to be on, the module checks every 100 ms that it
still answers. Two silent checks mean it is gone: it is marked disconnected,
a driver that implements PCI error recovery is told, and user space gets an
event.

A driver that has no way of being told (the proprietary NVIDIA one) is frozen
instead, if it offers the hooks the ZSS driver patch adds: every caller is shut
out, and the driver is resumed, as after a sleep, when the device is back.
`driver_frozen` shows it. The freeze helps only if it lands before anything
calls into the driver: about 50 ms on an idle card, which is soon enough, and
never soon enough under a program that is rendering.

After a device comes back, silence is not taken for a new loss for five
seconds: a driver re-initialising its card may reset it.

## What it does not do

- It does not intercept a driver's access to the card (no MMIO shadowing).
- It does not confine the card's memory access; it reports whether an IOMMU
  already does (`iommu`).
- It does not decide whether a device may be powered off. That is the
  daemon's job (who holds it, what to move, freeze or hide).
- It does not replace the NVIDIA wake-on-touch patch.

## Interface

Everything is under `/sys/kernel/zss/`, root only.

| File | Use |
| :--- | :--- |
| `manage` | write `PCI [backend=NAME] [quiesce=pm\|external\|none]` to hand a device over |
| `unmanage` | write `PCI`; a device that is off is powered on first |
| `version` | module version |
| `PCI/state` | `on`, `off`, `lost` or `failed` |
| `PCI/power` | write `off` or `on` |
| `PCI/backend`, `PCI/quiesce` | what was chosen |
| `PCI/functions` | each function, its driver, and `online` or `offline` |
| `PCI/last_error` | why the last request failed |
| `PCI/cycles` | completed off/on cycles |
| `PCI/iommu` | `yes` or `no` |
| `PCI/answers` | `1` if the device answers on the bus right now |
| `PCI/needs_rebind` | `1` after a loss the driver could not be told about |
| `PCI/on_loss` | What happens to the driver when the device goes silent: `freeze` (default), `refuse` or `leave`. Frozen, the driver never learns of the loss and can be given the device back, but everything that calls it sleeps until then, the display server included. Refusing, it is frozen in the same way and turns callers away with an error at once: programs can be moved, the display server is not held, and the driver can still be thawed (needs a driver that offers `nv_zss_freeze_refusing()`, which is revision 5 of the NVIDIA patch; otherwise it is frozen plainly). Left, the driver finds out by itself, and one that cannot recover a lost device has to be reloaded. The daemon writes `freeze` while the device is idle and its `loss_while_busy` setting (`leave` unless configured) while anything is running on it |
| `PCI/driver_refusing` | `1` while the driver is frozen and refusing |
| `PCI/driver_frozen` | `1` while the driver is frozen because the device went silent |
| `PCI/test_fault` | fault switches for the `test` backend only |

Every state change sends a `change` uevent in subsystem `zss` with `ZSS_PCI`,
`ZSS_STATE` and `ZSS_REASON`.

By hand, with the `test` backend (which has no effect on hardware beyond what
the driver's own sleep code does):

```sh
echo "0000:03:00.0 backend=test quiesce=pm" > /sys/kernel/zss/manage
echo off > /sys/kernel/zss/0000:03:00.0/power
cat /sys/kernel/zss/0000:03:00.0/state
echo on > /sys/kernel/zss/0000:03:00.0/power
echo 0000:03:00.0 > /sys/kernel/zss/unmanage
```

## Backends

| Backend | Platform | State |
| :--- | :--- | :--- |
| `gmux` | Apple laptops with a classic gmux | runs on the MacBookPro9,1 |
| `acpi` | firmware power resources on the device or the port above it | **written, never run on any firmware** |
| `test` | none: a flag | QEMU tests |

Without `backend=`, the module picks the first real backend that recognises
the device and refuses if none does.

## Safety

- Loading the module changes nothing. It acts only on a device named in
  `manage`.
- A failure before power is cut is undone and leaves the device on. A device
  that does not come back is reported as `failed` and can be retried.
- Unloading powers every managed device on first. With `quiesce=external` it
  cannot resume the driver, which is why the daemon is stopped first: the
  daemon powers everything on as it stops.
- It is never in the initial ramdisk, and is loaded by `zssd.service`.

## What has been tested

| Where | What |
| :--- | :--- |
| QEMU (`tests/track_k.py`, 13 scenarios, clean kernel log) | load and unload; `pm`, `none` and `external`; ten cycles with identical PCI configuration; every failure path; silence noticed in under 350 ms; a card pulled through a hot-plug port and its replacement; freeze on loss and thaw on return against a stand-in for the driver's hooks; the daemon on top of the module |
| MacBookPro9,1, orderly | `gmux` backend with `quiesce=external` under a running X session, driven by the daemon: off/on cycles, power cut in 0.03 s, NVIDIA driver and HDMI audio working afterwards |
| MacBookPro9,1, power rail cut with the card idle | loss reported after 50 ms; NVIDIA driver frozen before it noticed; `zssctl on` restored power, PCI state and driver; the card rendered afterwards. No reboot |
| MacBookPro9,1, power rail cut under a rendering program | loss reported after about 100 ms, **after** the driver had found out; the driver was frozen all the same; resuming it later with the program still attached hung the machine |

Not tested: any driver other than `bochs` with `quiesce=pm`; the `acpi`
backend; a card physically pulled from real hardware; unloading the module
with a device off on real hardware; a driver's PCI error handlers being called
for real (`bochs` has none).
