## Context

See `proposal.md`. Facts established before writing this:

- The running kernel (7.2.2-arch) has module signing compiled in but not enforced, and lockdown is off, so an unsigned DKMS module loads.
- Every symbol the design needs is exported: `pci_save_state`, `pci_restore_state`, `pci_set_power_state`, `pci_device_is_present`, `pci_dev_trylock`, `acpi_device_set_power`, `bus_register_notifier`.
- The QEMU harness (`tests/track_b.py`, `tests/qemu/init.c`) boots the host's kernel and already loads modules with `finit_module`, so the module can be exercised there against the virtual `bochs-display` card and its `bochs` DRM driver.
- The reference laptop's GPU (`\_SB_.PCI0.P0P2.GFX0`) has no firmware power resources. Its power is the gmux. The `acpi` backend therefore cannot be tried on it.
- On the laptop the current user-space sequence works and stays as the fallback.

## Goals / Non-Goals

**Goals:**

- One module that can power any PCI GPU off and on, given a backend for the platform and a driver that supports system sleep.
- Loading the module changes nothing. Unloading it, or a failure at any step, leaves every managed device powered and usable.
- The daemon uses it when present and does not need it.
- Everything is proven in QEMU before it is loaded on hardware.

**Non-Goals:**

- Intercepting or shadowing a driver's access to hardware.
- Supporting kernels other than the one it is built against, beyond not using private symbols.
- A character device or ioctl interface.

## Decisions

### D1. Interface: sysfs and uevents

```
/sys/kernel/zss/
    manage            write "0000:01:00.0 backend=gmux quiesce=external"
    unmanage          write "0000:01:00.0"
    version
    0000:01:00.0/
        state         on | off | lost | failed            (read)
        power         write "off" or "on"; read = state
        backend       gmux | acpi | test                  (read)
        quiesce       pm | external | none                (read)
        functions     each PCI function handled, its driver, online|offline (read)
        last_error    text of the last failed step        (read)
        cycles        completed off/on cycles             (read)
        iommu         yes | no: is its DMA confined       (read)
        answers       1 | 0: does it answer on the bus now (read)
        needs_rebind  1 after a loss the driver could not be told about (read)
        test_fault    fault switches, test backend only
```

Every state change sends a `change` uevent on the device's directory with `ZSS_PCI=`, `ZSS_STATE=` and `ZSS_REASON=`. The daemon already has a netlink uevent socket.

Files are root-only. Access control for users stays in the daemon.

*Alternative considered:* a character device with ioctls. Rejected: nothing here needs more than a word written to a file, and sysfs is testable from a shell.

### D2. The sequence

```
 off:  lock functions (highest first)
       quiesce        pm: run each function's system-sleep callbacks
                      external: user space has already done it
       pci_save_state
       mark disconnected (so nothing reads a dead device)
       backend power off ─► confirm the device no longer answers
       state = off

 on:   backend power on ─► wait until the device answers (bounded)
       clear disconnected
       pci_restore_state, D0
       resume         pm: system-wake callbacks, lowest function first
       state = on
```

A failure before power is cut unwinds what was done and leaves the device on. A failure to come back leaves the state `failed`, with the reason in `last_error`, and another `on` may be tried.

All functions of the slot (GPU and its HDMI audio) are handled together, because they share one power rail.

### D3. Quiesce by the driver's own sleep callbacks

A driver's `struct dev_pm_ops` already knows how to stop its device and bring it back with nothing preserved in hardware: that is what a laptop suspend does. The module calls, through the bus's operations so that the PCI core's part runs too: `prepare`, `suspend`, `suspend_late`, `suspend_noirq`, and on the way back `resume_noirq`, `resume_early`, `resume`, `complete`.

This is done outside a system sleep, so user space is not frozen. That is the same situation the NVIDIA patch deals with, and the answer is the same division of labour: **the daemon makes the device idle first** (applications moved or frozen, display server not driving a display from it), and only then asks the module. The module does not judge whether the device is idle: that stays the daemon's rule. It refuses only when no driver is bound, because then nothing would initialise the device again after power returns (drivers replay the video BIOS themselves on resume; nothing else does).

*Alternative considered:* runtime power management (`pm_runtime_*`). It is the right mechanism for open drivers when they opt in, and the kernel then powers the device off by itself. It cannot be forced on a driver that does not implement it, which is the case that needs ZSS.

`quiesce=external` is for the proprietary NVIDIA driver, whose sleep callbacks expect a real system sleep; the daemon keeps using `/proc/driver/nvidia/suspend` and the wake-on-touch patch, and the module does state and power.

### D4. Backends

| Backend | How | Testable |
| :--- | :--- | :--- |
| `test` | Sets a flag; optionally makes the device "not answer" by a test hook | QEMU, host |
| `gmux` | The two power-port writes of the classic Apple gmux, from the kernel | Laptop |
| `acpi` | `acpi_device_set_power` to D3cold and D0 on the device's firmware node, which runs `_PR3`/`_PS3`/`_OFF` | QEMU with an injected SSDT (stretch); no real hardware here |

A backend is four functions, as in the daemon: probe, power off, power on, is it powered.

The `gmux` backend claims nothing the kernel's `apple-gmux` driver holds: on the classic gmux the power port is written directly, with no index protocol and no lock to share. That is what the daemon does today; doing it in the kernel removes `/dev/port` from the picture.

### D5. Loss guard

While a managed device's state is `on`, delayed work reads its vendor ID every 100 ms. Two all-ones reads in a row mean the device has gone. The module then marks every function disconnected (`pci_channel_io_perm_failure`), so that the kernel's own configuration accessors answer without touching hardware and drivers that check `pci_channel_offline()` stop. It calls the `error_detected` handler of a driver that implements PCI error recovery, which is the kernel's way of telling a driver its device is gone, sets the state `lost`, and sends the event. (Reads of the `config` file in sysfs by user space still go to the hardware; the mark does not cover them.)

When a lost device answers again the module says so once (`answers again`) and leaves the decision to user space. On `on` it restores the configuration saved when the device was handed over, and calls the driver's `slot_reset` and `resume` handlers; a driver without them is reported through `needs_rebind`, and the daemon unbinds and binds it.

A hot-plug port already does this in the kernel; the guard covers devices behind ports that do not report removal (a mux, a riser, a slot with hot-plug disabled).

What the guard does not do is make a driver that ignores the offline state safe. That needs the shadowing in `SPEC.md` and is out of scope.

### D6. Daemon backend `zss-kmod`

If `/sys/kernel/zss` exists and a device has no explicit backend, the daemon hands it to the module (`manage`), choosing `quiesce=external` for the `nvidia` driver and `pm` otherwise, and the module's own probing picks `gmux` or `acpi`. Off and on become writes to `power`. `state` and the uevents replace the daemon's bus polling for that device.

Everything above the backend is unchanged: holder rules, freezing, hiding, serving the display server, the watchdog.

### D7. Safety on the user's machine

- Loading does nothing. A device is touched only after a write to `manage`.
- `rmmod`, and module exit on shutdown, power every managed device on and restore it first.
- The module is never added to the initial ramdisk, and is loaded by `zssd.service`, not at early boot, so a faulty module cannot stop the machine from booting to a desktop.
- A kernel `WARN` or oops in the module is caught in QEMU: the guest tests fail on any kernel warning in its log.

### D8. Testing

| What | Where |
| :--- | :--- |
| Builds for the running kernel with no warnings | Host |
| Load, manage, unmanage, unload; files and events | QEMU guest |
| Full off/on with `quiesce=pm` on the bochs card: driver callbacks run in order, state restored byte for byte, console usable afterwards | QEMU guest |
| Failure at each step unwinds: a suspend callback that fails, a backend that will not cut power, a device that does not come back | QEMU guest, through the `test` backend's fault switches |
| Loss guard: device stops answering, state `lost` within 300 ms, event sent | QEMU guest, `test` backend |
| Daemon with the `zss-kmod` backend: existing off/on and loss scenarios | QEMU guest |
| `gmux` backend with `quiesce=external` in place of the user-space sequence | Laptop, each step with a go-ahead |

### D5a. A driver that cannot be told: freeze

The first real power cut (`docs/track-c.md`) showed the limit of D5. The NVIDIA driver has no error handlers. About four seconds after the cut, when the X server next called into it, it found the device missing, logged "GPU has fallen off the bus", and from then on would not look at the device again; an attempt to force it through its suspend path hung in the kernel.

A suspend in that driver is two things: closing the gate every caller passes (the lock the wake-on-touch patch already uses), and saving the device's state. The second is what cannot be done with the device gone. Patch revision 4 adds a **freeze** that is the first alone, plus what a suspend does that does not touch hardware (stop the periodic check and the bottom half, mark the devices suspended), and a **thaw** that runs the ordinary resume and opens the gate. The module calls the freeze from its guard as soon as the device is silent, by run-time lookup so that it does not depend on the driver, and the thaw when the device is powered and its configuration restored.

The guard looks again 10 ms after a first silent look, so the freeze comes about 110 ms after the loss at worst. Anything that calls into the driver sooner still gets there first.

Whether the driver's resume works with no suspend before it is inside NVIDIA's closed code. If it does not, the driver stays frozen, `release` opens the gate, and the fallback is rebinding the driver once nothing holds the device.

`nvidia-uvm` is not frozen: its own suspend talks to the device.

## Results so far (QEMU, kernel 7.2.2, bochs card)

All twelve scenarios of `tests/track_k.py` pass, with a clean kernel log: load and unload; manage and unmanage; refusals; ten off/on cycles through the bochs driver's own sleep callbacks with the PCI configuration identical afterwards; cycles with `none` and `external`; a backend that fails, one that lies, a device that does not return, and the retry; silence noticed in under 350 ms and the return; a card pulled through the hot-plug port and its replacement managed; unloading with the device off; the daemon detaching, attaching, reporting a kernel-side failure in the module's words, hearing of a loss in under a second and taking the device back.

On the reference laptop (see `docs/track-c.md`): installed through DKMS, loaded by the service, picked by the daemon; `gmux` with `quiesce=external` cycled the GPU twice under a running X session, power cut in 0.03 s, driver and audio working afterwards.

Not yet run anywhere: the `acpi` backend, any driver other than `bochs` with `quiesce=pm`, a card physically pulled from real hardware, and unloading the module on real hardware with a device off.

## Risks / Trade-offs

- **Kernel code on a work machine** → QEMU first, inert on load, fallback to the user-space path by not loading it.
- **Sleep callbacks run outside a system sleep may assume a frozen user space** → the daemon makes the device idle first; drivers known not to cope use `external`; each newly supported driver is tried in a VM or on spare hardware before being listed as working.
- **Internal kernel interfaces change** (the PM callback order, `error_state`) → only exported symbols and public structures are used; the DKMS build fails loudly rather than misbehave; the daemon falls back.
- **The `acpi` backend cannot be proven on real firmware here** → shipped marked experimental until someone with such a laptop tries it.
- **Two things could write the gmux power port** (the module and the kernel's switcheroo code) → same as today; the daemon is the only caller.

## Migration Plan

Nothing changes until the module is installed and loaded. On the laptop: install through the installer's new optional step, load by hand once, check `state`, then let the daemon use it. Rollback is `rmmod zss` or uninstalling the DKMS package; the daemon returns to its own backends.

## Open Questions

- Can the gmux hot-plug interrupt be used from the module to notice a monitor being plugged in while the GPU is off? Deferrable; it would close a documented limitation.
- Should the module refuse `quiesce=pm` for drivers it has no record of? Proposed: allow, and log clearly; decide after trying more drivers.
